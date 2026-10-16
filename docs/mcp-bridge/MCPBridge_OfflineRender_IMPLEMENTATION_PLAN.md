# MCPBridge Offline Render — Implementation Plan

**Author:** AI Architecture Agent
**Date:** 2026-09-07
**Target:** `plugdata-core/Source/Pd/MCPBridge.cpp` + `mcp-server/src/tools/recorder.ts` + `mcp-server/src/transport/osc-client.ts`
**Prerequisite:** Zero-dropout WAV recorder (`/pd/record_start`), native probe/meter (all shipped and live-verified).

---

## 1. Goal

Add **offline (faster-than-realtime) rendering** of the current patch to a WAV file: run the existing Pd DSP graph in a tight loop on a background thread — no audio device clock, no `writesf~`, no canvas objects, no DSP recompile.

**Why:** today, hearing a 10s result costs 10s of realtime (`record_performance` + live playback). Offline render bakes the same 10s in ~0.3s, enabling:

- Instant A/B preview after AI edits (render before/after, compare)
- Bit-identical regression renders (deterministic — no clock jitter, no device timing)
- Fast AI iteration loops (render → analyze → fix → render), ~30x faster than realtime probing

**Non-goals (Phase 2 candidates, out of scope):**

- Null-test endpoint (subtraction of two renders) — trivial TS follow-up, separate PR
- Offline spectrogram/annotated waveform images
- MIDI event injection during render (renders start from current patch state; `[r]`-driven patterns need a pre-render trigger — see §5 Protocol)

---

## 2. Architecture Overview

```
/pd/render <filePath> <durationSec> [analyze] <corrId>
     │  OSC receiver thread (Realtime callback — must NOT render here)
     ▼
Validate path + duration → create parent dir
Send immediate "started" reply (corrId) — pattern proven by /seq/start
Launch JUCE background thread (std::thread via Thread::launch / detached)
     │
     ▼
RENDER THREAD (one-shot, owns the graph for its whole lifetime)
  1. processor->suspendProcessing(true)   ← live audio mutes briefly (honest blip)
     [standalone: AudioProcessorPlayer checks isSuspended() → skips callback]
  2. Zero the Pd input vectors (audioVectorIn already sized by prepareToPlay)
  3. N-block tight loop, mirroring processConstant exactly:
       for each of totalBlocks:
         setThis()
         sendParameters()
         sendMessagesFromQueue()      ← OSC fires / loadbangs drain correctly
         performDSP(in, out)          ← libpd_process_raw: sys_lock INSIDE
         mcpBridge->audioTick()       ← native transport + sequencer advance!
         copy out-vector → interleaved render buffer
  4. processor->suspendProcessing(false) ← live audio resumes (blanket priority)
  5. Write WAV (reuse /pd/record_* WAV-writer conventions: 16-bit PCM, stereo cap)
  6. Optional FFTW analysis on the baked buffer (same features as /meter/spectral:
     rmsDb, peakDb, fundamental, centroid, flatness, rolloff, crest, top-8 peaks)
  7. Reply /pd/render/reply/<corrId> with path + duration + analysis JSON (one message)
```

**Threading rules (the whole safety story):**

| Concern | Rule |
|---|---|
| Who may run the graph | Exactly one thread at a time. `libpd_process_raw` takes `sys_lock` internally per block; OSC handler thread already serializes everything else under the same lock, so a background thread calling `performDSP` is as safe as `processBlock` itself. |
| Live audio during render | `suspendProcessing(true)` → standalone `AudioProcessorPlayer` skips the callback (juce_AudioProcessorPlayer.cpp:328 checks `isSuspended()`). The render thread becomes the sole graph runner. |
| MCP mutations during render | Rejected: render entry checks `mcpRenderActive` atomic; `batch_atomic`/edit handlers already take `sys_lock` per-block via `performDSP` serialization — but we refuse NEW renders and return "render in progress" to /pd/render while one runs. Sequencer/transport OSC sends during render are SAFE: they enqueue into `functionQueue`, drained by our own `sendMessagesFromQueue()` inside the loop (same queue `processBlock` drains — no new code path). |
| GUI | `createComponentSnapshot`/canvas sync NOT touched — render is pure audio; `synchroniseCanvases` never called. |
| Telemetry (`telemetry/*`, probes) | Probes tap `t_outconnect` signals on the AUDIO thread via `audioTick()` — our render loop calls `audioTick()` per block, so live probes keep working DURING the render (they'll meter the rendered stream — a feature). |
| Reentrancy | Single-flight: one render at a time, enforced by `std::atomic<bool> mcpRenderActive`. Concurrent /pd/render gets an immediate error reply. |
| Audio-device config | Render uses `getSampleRate()` + `Instance::getBlockSize()` — matches whatever the artist's device is set to. Device-independent accuracy is Phase 2 (configurable SR). |

---

## 3. C++ Changes — MCPBridge.cpp/h

### 3.1 New handler in `handlePdDomain` (after `record_stop`, ~line 2114)

```cpp
if (action == "render") {
    // /pd/render <filePath> <durationSec> [analyze 0|1] <corrId>
    // Replies:
    //   /pd/render/started/<corrId>            (immediate ack)
    //   /pd/render/reply/<corrId> <json>       (on completion: path, renderedMs, speedFactor, analysis?)
    //   /pd/render/error/<corrId> <reason>    (on failure)
}
```

Parse: `filePath` (string), `durationSec` (float, clamp 0.1–60.0), `analyze` (int, optional, default 0), `corrId` (string).

Immediate validation + replies happen on the OSC thread:

1. `mcpRenderActive.exchange(true)` — if it returns `true`, reply error `"render already in progress"`, return.
2. Parent directory creation (same as `record_start`, MCPBridge.cpp:2082).
3. Send `/pd/render/started/<corrId>` ack, then launch the detached render thread.

### 3.2 Render thread body (file-static function, captures `processor`, params by value)

```cpp
static void runOfflineRender(PluginProcessor* processor, juce::String filePath,
                             float durationSec, bool analyze, juce::String corrId,
                             MCPBridge* bridge)
```

**Step 1 — suspend live audio** (order matters: suspend BEFORE first performDSP):

```cpp
processor->suspendProcessing(true);
```

RAII guard struct ensures `suspendProcessing(false)` + `mcpRenderActive = false` even on exception/early return.

**Step 2 — capture graph geometry** (before the loop, from the OSC thread context — safe under its implicit serialization):

- `pdBlockSize = Instance::getBlockSize()` (64)
- `sampleRate  = processor->getSampleRate()` (fallback 44100)
- `numCh       = jmin(2, max(in,out) channels)` — stereo cap like the recorder tap
- `totalBlocks = (int)std::ceil(durationSec * sampleRate / pdBlockSize)`
- Interleaved accumulation buffer `std::vector<float> renderBuf(numCh * totalSamples, 0)` — for 60s stereo 44.1k ≈ 21 MB, acceptable; clamp duration keeps it bounded.

**Step 3 — the loop.** Mirror `processConstant` (PluginProcessor.cpp:993) exactly, minus FIFO/MIDI plumbing we don't need:

```cpp
for (int b = 0; b < totalBlocks; ++b) {
    processor->setThis();
    processor->sendParameters();
    processor->sendMessagesFromQueue();   // drains OSC fires + loadbangs
    std::fill(audioVectorOut.begin(), audioVectorOut.end(), 0.0f); // (paranoia; libpd clears st_soundout)
    processor->performDSP(audioVectorIn, audioVectorOut);
    bridge->audioTick();                  // probes + native transport + seq advance
    // de-interleave out-vector → renderBuf, per processConstant's copy pattern
}
```

**Note on `audioVectorIn`**: zeroed once before the loop (`std::fill`) — `[adc~]` reads silence, correct for offline semantics. It's pre-sized `maxChannels * pdBlockSize` by `prepareToPlay` (PluginProcessor.cpp:745) and never resized while suspended, so indexing is stable.

**Note on dropouts**: none possible — no device callback running (`isSuspended`), no clock. The only audible effect: live output mutes for the render duration (typically 0.1–0.5s wall-clock). With sequencer jobs running, `audioTick()` advances them exactly as `processBlock` would, so a render **captures the pattern as heard** — same bars, same swing.

**Step 4 — resume live audio** (blanket priority — resume even on failure paths):

```cpp
processor->suspendProcessing(false);
```

**Step 5 — WAV write.** Reuse the recorder conventions but on the render thread (not lazy/audio-thread — we own the file exclusively): `juce::WavAudioFormat` → `createWriterFor(stream, sampleRate, numCh, 16, {}, 0)` → `writeFromAudioSampleBuffer`. Then flush.

**Step 6 — optional FFTW analysis.** If `analyze` flag: run the EXISTING spectral feature code path (the one `/meter/spectral` uses at MCPBridge.cpp:5825) on the final 1024-sample window of `renderBuf` channel 0 (or the loudest channel — pick max-RMS channel for robustness). Reuse `estimateFrequency` (MCPBridge.cpp:5737) for `fundamental`. Build the same JSON keys the server already knows how to parse (rmsDb, peakDb, fundamental, spectralCentroid, spectralFlatness, spectralRolloff, crestFactor, peakFrequency, fftSize, peaks) — plus render facts: `renderedMs`, `speedFactor = durationSec*1000 / wallClockMs`, `sampleRate`, `numChannels`.

**Step 7 — reply.** One OSC message: `/pd/render/reply/<corrId>` with the JSON string. On any failure after the started-ack: `/pd/render/error/<corrId>` with reason.

### 3.3 MCPBridge.h additions

```cpp
// in class MCPBridge (private section):
static std::atomic<bool> mcpRenderActive;   // single-flight guard

// file-static in .cpp — no header exposure needed beyond the guard
```

Actually simpler: `static std::atomic<bool>` as file-static in MCPBridge.cpp — zero header changes except nothing. Keep the header untouched. (Decision: file-static, single translation unit, OSC thread + render thread are the only touchers.)

### 3.4 Sequencer interaction (important design decision)

Native sequencer jobs (`/seq/*`) advance in `audioTick()` — our render loop calls it. Consequence: **a running drum pattern renders correctly** (its steps fire on the render timeline, identical spacing to live). But note the step counter advances per render block — so a 10s render of a 128bpm pattern renders ~21 bars, exactly like 10s of live playback would. No divergence, no drift. This is the desired semantics: **render = what you'd hear, minus the waiting.**

Transport `sampleCounter` also advances → `/transport/status` stays truthful after a render.

### 3.5 What we deliberately do NOT touch

- `processBlock` / `processConstant` / `processVariable` — zero changes to live path
- Probe `t_outconnect` taps — they meter the render stream automatically (safe: same graph, same thread-of-record semantics)
- `mcpWavWriter` / recorder state — render uses its own writer, never the live recorder's
- GUI/`synchroniseCanvases` — not called

---

## 4. TS Changes — mcp-server

### 4.1 `src/transport/osc-client.ts` — new method next to `recordStart` (~line 888)

```typescript
async renderOffline(filePath: string, durationSec: number, analyze: boolean,
                    timeoutMs = 65000): Promise<RenderResult | null>
```

- Sends `/pd/render [filePath, durationSec, analyze?1:0, corrId]`
- Awaits TWO messages: first `/pd/render/started/<corrId>` (ack), then `/pd/render/reply/<corrId>` (JSON payload) — same two-phase pattern as `/seq/start` (osc-client.ts:1045) but awaiting a second reply address. Implementation: `sendAndAwait` on the FINAL reply address with generous timeout `min(65000, durationSec*1000 + 15000)` (render is fast but the graph walk + WAV write add latency); the started-ack is fire-and-forget (logged).
- Error address: `/pd/render/error/<corrId>` — the existing error-surfacing convention routes it (see probe error pattern).
- Returns parsed JSON `{ path, renderedMs, speedFactor, sampleRate, numChannels, analysis? }` or `null` on failure/timeout.

### 4.2 `src/tools/recorder.ts` — extend `record_performance` (NOT a new tool)

Rationale: AGENTS.md §Tool-Routing — "get audio into a file" is one outcome; render is a second *speed* of the same tool. AI mental model: `record` = live tape (what you hear), `render` = instant bake (what the patch computes).

Schema additions (compact, per authoring skill):

```typescript
action: z.enum(["start", "stop", "status", "render"]),
// existing
durationMs: z.number().optional().describe("Auto-stop timer for live recording (ms)"),
// new
renderMs: z.number().min(100).max(60000).optional().describe("Offline render length (ms)"),
analyze: z.boolean().optional().describe("Include spectral analysis in render reply"),
```

Handler `action === "render"`:

1. Compute `fullPath` (same filename logic as `start`)
2. `const result = await ctx.oscClient.renderOffline(fullPath, renderMs/1000, analyze ?? false)`
3. On success return V2 response: meta `{ ok: true, action: "render", returned: result.path, ... }` + human text: `⚡ Offline render complete: <path> (<renderedMs>ms baked in <wall>ms, <speedFactor>x realtime) + analysis JSON block` + suggestedNext: `probe the file's numbers, or render again after an edit for A/B`
4. On null → helpful error string (per skill #6), never throw raw.

### 4.3 `src/tools/performance.ts` — routing aliases

- `capture_audio_wav` gains `render` in its `RECORD_ALIASES` map (performance.ts:1241): `render: "render"` → routes to `record_performance({action:'render', ...})` with `renderMs` mapped from `duration_ms`.
- The big `actions:` `.describe()` blob (performance.ts:300) — append one clause: `capture_audio_wav (recordAction:'render'): offline faster-than-realtime WAV bake, faster A/B previews + deterministic regression renders`.

### 4.4 Docs (AGENTS.md §9 workmap)

- AGENTS.md: `record_performance` row gains the render clause; Diagnostic Playbook unchanged (render is a creator tool, not a diagnostic).
- `docs/canonical-tools.json`: `record_performance` entry — append `render` to actions list.

---

## 5. Protocol Summary (C++ ↔ TS contract)

```
Request:
  /pd/render <filePath:str> <durationSec:float> [analyze:int] <corrId:str>

Replies (in order):
  /pd/render/started/<corrId>                       ← immediate, OSC thread
  /pd/render/reply/<corrId> <jsonString>            ← render thread, on success
  /pd/render/error/<corrId> <reason:str>            ← render thread, on failure

JSON payload:
  {
    "path": "/full/path.wav",
    "renderedMs": 10000,
    "wallClockMs": 312,
    "speedFactor": 32.05,
    "sampleRate": 44100, "numChannels": 2, "pdBlockSize": 64,
    "blocks": 6891,
    "analysis": {                       // only when analyze=1
      "rmsDb": -18.3, "peakDb": -6.2,
      "fundamental": 55.0, "peakFrequency": 55.0,
      "spectralCentroid": 812.4, "spectralFlatness": 0.031,
      "spectralRolloff": 5200.0, "crestFactor": 4.8,
      "fftSize": 1024, "peaks": [...]
    }
  }
```

**Trigger-before-render pattern** (documented for the AI, not coded): to render a `[r]`-driven voice that isn't currently playing, the AI fires the receiver first (`trigger_musical_events` → `/param` or `/trigger`), then immediately calls render. The message lands in `functionQueue` and is drained by the render loop's first `sendMessagesFromQueue()` — the sound event starts inside the render. Time-zero alignment is inherently a few blocks early; acceptable for v1, noted as known behavior.

---

## . Verification Plan

1. **C++ build**: `cd plugdata-core/build && cmake --build . --target plugdata_standalone -j8`
2. **TS build**: `cd mcp-server && npm run build` then artist reloads server + reopens PlugData (per AGENTS.md §9)
3. **Live test protocol** (standard patch from AGENTS.md, beat running):
   - `record_performance(action:'render', renderMs:4000)` → file exists, duration ≈ 4s, speedFactor > 5
   - Play rendered WAV back → identical musical content to live (same beat, same swing)
   - Render twice → byte-identical files (deterministic claim) — `cmp a.wav b.wav`
   - `analyze: true` → JSON parses, rmsDb matches a live `tap_audio_signal` within tolerance
   - Beat keeps playing live after render completes (no dropout on resume — listen)
   - Concurrent render attempt → clean "already in progress" error, no crash
   - Running sequencer job during render → rendered file contains the pattern
4. **stdio integration test**: add render case to `scripts/test-mcp-stdio.js`

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `suspendProcessing` in standalone — player thread keeps its own FIFO? | Verified: juce_AudioProcessorPlayer.cpp:328 checks `isSuspended()` and skips. Risk retired. |
| Reentrant render / mutation mid-render | Single-flight atomic + sys_lock-per-block serialization (mutations take the same lock inside `performDSP` windows). Worst case: mutation lands between blocks — graph recompile happens via normal canvas paths, which already handle recompile-under-lock. |
| Memory: 60s stereo @44.1k | ~21 MB heap — bounded by duration clamp. Fine. |
| DSP cycle in patch during render | Same behavior as live: Pd flags the cycle once, unscheduled objects output silence. `/pd/diagnose` facts unchanged. |
| GUI meter widgets during render | GUI polls statusbar peaks, which live on the audio device path — suspended → meters freeze briefly, then resume. Cosmetic, acceptable. |
| Artist has DSP OFF | Render still works (graph compiled by performDSP via SCHED_TICK) — `startDSP()` not required; document: render works with DSP toggled off. |
| Render while a live recording (`record_start`) is armed | Recorder taps in `processBlock`, which is suspended → the tap writes silence for the render window. Document as known interaction; mitigation: render entry pauses the live recorder and restarts it after resume (Phase 2 nicety). |

---

## 8. Implementation Order

1. C++ `/pd/render` handler + render thread (§3.1–3.2) — build, launch PlugData, raw-OSC test via port 19021 (per AGENTS.md harness rule 4)
2. TS `renderOffline` in osc-client (§4.1) + recorder action (§4.2) + aliases (§4.3)
3. Docs: AGENTS.md + canonical-tools.json (§4.4)
4. Verification gauntlet (§6) — full protocol above
5. Commit C++ first (plugdata repo), then TS (mcp-server repo) — matching the two-repo workflow
