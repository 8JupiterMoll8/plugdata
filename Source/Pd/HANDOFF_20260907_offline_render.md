# HANDOFF — Offline Render Feature (`/pd/render`) — 2026-09-07 15:48

**Author:** opencode (z-ai/glm-5.3-free)
**For:** next agent (GLM 5.3 Pro or equivalent)
**Repos:** `~/Desktop/plugdata/plugdata-core` (C++ fork, branch `feat/v4-zero-dropout-copilot`, HEAD `30b680672`) + `~/Desktop/plugdata/mcp-server` (TS, HEAD `bfb4b81`)
**Mission:** implement + verify offline faster-than-realtime render. **Code is DONE and the happy path is LIVE-VERIFIED. One open bug remains (bridge wedge).**

---

## 1. What This Feature Is

`/pd/render <filePath> <durationSec> [analyze 0|1] [corrId]` — bakes the current patch to WAV on a detached background thread by looping `performDSP` without the audio device clock. 3000ms of audio rendered in **2ms wall-clock** (1114x realtime) in the live test. Live audio suspends for the render wall-time (brief mute, no dropouts — standalone player skips callbacks while suspended). Running sequencer jobs (`/seq/*`) advance inside the render via `audioTick()`, so the render captures the pattern as heard. Optional FFTW spectral analysis (`analyze:1`) reuses the `/meter/spectral` feature set on the loudest channel.

Design decision record: render lives in `record_performance` (action `render`), NOT a new tool — "get audio into a file" is one outcome, two speeds. NOT in monitor.ts (stethoscope ≠ file producer).

## 2. Files Changed (ALL UNCOMMITTED)

### plugdata-core (C++):
| File | Change |
|---|---|
| `Source/Pd/MCPBridge.cpp` | +/pd/render handler (~line 2121, after record_stop), +`runOfflineRender` + `renderSpectralJson` + `RenderGuard` + `mcpRenderActive` file-static (~lines 285-560), +includes `<thread> <vector> <atomic>`, +`"render"` in `/bridge/capabilities` list (~line 5792). NOTE: the git diff also contains PRE-EXISTING uncommitted work (screenshot labels, `AllGuis.h` includes, knob_get_snd/rcv externs) — commit carefully, do NOT blindly `git add -A` |
| `Source/Pd/MCPBridge_OfflineRender_IMPLEMENTATION_PLAN.md` | untracked — full PRD (architecture, threading rules, protocol, risks, verification plan) — READ FIRST |

Build: `cd plugdata-core/build && cmake --build . --target plugdata_standalone -j8` — **PASSES**, binary at `plugdata-core/Plugins/Standalone/plugdata`.

### mcp-server (TS):
| File | Change |
|---|---|
| `src/transport/osc-client.ts` | +`renderOffline(filePath, durationSec, analyze, timeout?)` after `recordStop` (~line 923). Awaits `/pd/render/reply/<corrId>`, parses JSON. Timeout = `min(65000, durationSec*1000 + 15000)` |
| `src/tools/recorder.ts` | `action` enum +`"render"`, +`renderMs` (100-60000, default 4000), +`analyze` params; handler renders via osc-client, V2 response with path/timings/analysis |
| `src/tools/performance.ts` | `RECORD_ALIASES` +`render`→"render" (capture_audio_wav routing, ~line 1241), `recordAction` enum +`"render"`, +`render_ms` +`analyze` schema fields, actions-description blob updated |
| `src/transport/osc-client.ts` (send path) | **CRITICAL FIX**: `PATH_FIRST_ARG_ACTIONS` Set +`"render"` (~line 1591) — without it, the WAV file path arg gets mangled to basename by canvas-name normalization |
| `docs/canonical-tools.json` | +offline-render feature line (Pillar 6) |
| `scripts/test-mcp-stdio.js` | +render integration test (id 60) after recorder stop |
| `AGENTS.md` | +2 tool-routing rows (instant render, bit-identical A/B) |

Build: `npm run build` — **PASSES**.

## 3. Verification Status

### ✅ PASSED (live, raw-OSC on port 9000, dedicated listener 19021):
1. `/bridge/connect 127.0.0.1 19021` → ack, sender retargeted
2. `/pd/render /tmp/opencode/render_test_1.wav 3.0 1 <corrId>` → started ack → reply JSON:
   `{"renderedMs":3000, "wallClockMs":2, "speedFactor":1114.4, "sampleRate":44100, "numChannels":2, "blocks":2068, "analysis":{...all zeros — empty canvas, expected}}`
3. WAV validated: 3.0s, stereo, 44100Hz, 16-bit PCM (python wave module)

Test script (reusable): `/tmp/opencode/test-pd-render.mjs` — encodes OSC 1.0 by hand, connects, renders, prints reply.

### ❌ OPEN BUG — BRIDGE WEDGE AFTER `/pd/load_content`:
Sequence that broke it: `test-pd-render-sound.mjs` sent `/pd/load_content pd-main /tmp/opencode/render_tone.pd <corr>` (to load a 220Hz tone patch for a sound-level test). The load **never replied** and the bridge's `/pd` domain then went dead — subsequent `/pd/ping` gets NO response (connect/ack still works, so the OSC socket itself is alive; `/pd/*` handlers never answer). Reproduced twice (2nd time with the live MCP server STOPped via kill -STOP, so it's NOT the heartbeat race).

**Suspicion (unconfirmed):** `load_content` on the artist's live `pd-main` wedged inside the handler — it calls `pd_typedmess(cnv, "clear")`, `pasteDirect`, `canvas_loadbang`, `canvas_update_dsp()`, then `enqueueFunctionAsync(synchroniseCanvases)` + `startDSP()`. Possibly a deadlock between sys_lock and the audio thread, OR `startDSP()` when suspended. NOTE: this may be a PRE-EXISTING load_content bug unrelated to my render code — my first render (test 1) ran clean before any load_content was sent. DO NOT assume it's the render feature until proven: `/pd/ping` works fine after a render alone.

**Recovery for artist:** PlugData needs restart (pid was 319656, started 15:27, binary at `plugdata-core/Plugins/Standalone/plugdata` — binary already contains ALL new code, restart preserves the feature).

**Next steps for the incoming agent:**
1. Restart PlugData (artist must reopen, or `kill 319656 && nohup plugdata &` pattern)
2. Re-run `/tmp/opencode/test-pd-render.mjs` → confirm ping+render still work post-restart
3. Isolate the load_content wedge: does `/pd/ping` die after a plain load_content WITHOUT any render? (i.e. pre-existing bug?) Check `s_inter`/`s_file` console for crash logs in `~/.local/share/plugdata` or wherever this fork logs
4. Complete the sound test: load tone patch → render 2s → assert `analysis.rmsDb > -60` (script exists: `/tmp/opencode/test-pd-render-sound.mjs`, fix its port to 9000 + add `/bridge/connect` first — it currently sends load before connecting properly)
5. Run full stdio gauntlet: `node scripts/test-mcp-stdio.js` (server must be running; contains render test id 60)
6. A/B determinism check: render same patch twice, `cmp a.wav b.wav` (claimed bit-identical — UNVERIFIED)

### ⏸ NOT YET RUN:
- stdio integration test (render case added, untested)
- Determinism check (two renders byte-identical)
- Sound-level render verification (blocked by the wedge)
- Live-sequencer-during-render test (render while beat plays — audioTick integration, designed but untested)

## 4. Architecture Notes for Reviewers

The render loop mirrors `processConstant` (PluginProcessor.cpp:993) exactly: `setThis → sendParameters → sendMessagesFromQueue → performDSP → audioTick → copy`. Key facts:
- `libpd_process_raw` takes `sys_lock` internally per block (verified in `Libraries/pure-data/src/z_libpd.c` PROCESS_RAW macro) — so the render thread and OSC mutations serialize on the same lock as live audio does.
- `suspendProcessing(true/false)` — standalone player skips the callback while suspended (juce_AudioProcessorPlayer.cpp:328 `isSuspended()` check). RAII `RenderGuard` guarantees resume + flag-clear on every exit path.
- Single-flight: file-static `std::atomic<bool> mcpRenderActive`, claimed on OSC thread, released by guard.
- WAV write happens on the render thread (not the lazy audio-thread writer path) — 16-bit PCM, stereo cap, `juce::WavAudioFormat`.
- `estimateFrequency` (autocorr fundamental) is forward-declared before my code, defined at ~line 6000 near ProbeManager.
- **JUCE trap hit during dev**: `juce::jmin<int64>` triggers juce_dsp `SIMDRegister<long long>` instantiation → incomplete-type error. Use `std::min<int64>`. (This is why the first build failed.)
- Memory: 60s stereo @44.1k ≈ 21MB renderBuf — bounded by duration clamp 0.1–60s.

Known design caveats (documented in plan §5/§7): trigger-before-render needs the receiver fired BEFORE render (it drains via sendMessagesFromQueue at loop start — a few blocks early); live recorder armed during render records silence (documented, Phase 2 fix); GUI meters freeze during the brief suspend.

## 5. Harness Rules Applied (AGENTS.md §8)

- Bridge LISTENS on UDP **9000** (not 19010 — 19010 is its send target for the server). Retarget via `/bridge/connect <ip> <port>` before raw-OSC tests, expect `/bridge/connect/ack`.
- The live MCP server's 2.5s heartbeat can steal the bridge sender mid-test → pause it (`kill -STOP <pid>`) for raw-OSC scripts, resume after (`kill -CONT`). (In the wedge case this did NOT fix it — the wedge is real, not the race.)
- "Schema ships with the feature" — done: enums, describes, aliases, capabilities all updated.
- Two-repo commit protocol: C++ (plugdata fork) first, then TS (mcp-server). NEVER `git add -A` in plugdata-core — the working tree carries unrelated uncommitted submodule churn + pre-existing screenshot-label work.

## 6. Suggested Commit Plan

1. `plugdata-core`: stage ONLY `Source/Pd/MCPBridge.cpp` (render hunks — carefully split from the pre-existing screenshot-label hunks if needed) + the new plan MD. Suggested msg: `feat(bridge): /pd/render offline faster-than-realtime WAV bake with optional FFTW analysis`
2. `mcp-server`: stage the 6 files listed in §2. Suggested msg: `feat(recorder): offline render action + capture_audio_wav render alias + /pd/render OSC client`
3. Only commit AFTER the wedge is root-caused (or proven pre-existing) and the stdio gauntlet passes.

## 7. Quick Reference — Repro/Test Commands

```bash
# Raw render test (after PlugData restart):
node /tmp/opencode/test-pd-render.mjs

# Sound test (needs the port fix noted in §3):
node /tmp/opencode/test-pd-render-sound.mjs

# Step-debug ping:
node /tmp/opencode/test-pd-step.mjs ping

# C++ build:
cd ~/Desktop/plugdata/plugdata-core/build && cmake --build . --target plugdata_standalone -j8
# TS build:
cd ~/Desktop/plugdata/mcp-server && npm run build
# Stdio gauntlet (server running):
node scripts/test-mcp-stdio.js
```

## 8. Silicon-Truth Summary

- Feature: COMPLETE (C++ + TS + docs + tests), compiles clean both sides, happy path live-verified with real timings.
- Blocker: bridge `/pd`-domain wedge after `/pd/load_content` (possibly pre-existing, unrelated to render — ping survives render alone). PlugData restart required to continue.
- Unverified claims: determinism (bit-identical renders), sequencer-during-render, stdio render case, sound-level analysis.
- The artist's PlugData (pid 319656) is currently WEDGED — restart it before any further testing.
