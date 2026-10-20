# C++ MCPBridge Acceleration Roadmap

**Owner:** alphi (artist/architect)  
**Status:** Living document — update as features ship  
**Philosophy:** Stop using files and multi-step OSC as communication. Read memory directly in C++.

---

## Shipped

| # | Feature | Speedup | Date |
|---|---------|---------|------|
| M1 | Native zero-dropout signal meter/probe | 400x | 2026-08-28 |
| P1 | Inline Identity Mappings (Kill Polling Loop) | 50-100x | 2026-08-28 |
| P2 | Instant Array/Sample Access (array_bulk) | 40-70x | 2026-08-28 |
| P3 | Native In-Memory Canvas Census (/pd/census) | 40-100x | 2026-08-28 |
| P4 | Lightweight Object Inspection (/pd/typeof + /pd/ports) | 500x | 2026-08-29 |
| P5 | C++ Identity Authority (/pd/identity_snapshot + /pd/identity_version) | 50-100x | 2026-08-29 |
| P6 | Direct Connection Topology (/pd/connections) | 100x | 2026-08-29 |
| P7 | Bridge-Side Spectral Analysis (/meter/spectral) | 8-10x | 2026-08-29 |
| P8 | Atomic Batch Operations (/pd/batch_atomic) | 3x fewer glitches | 2026-08-29 |

---

## All Phases Shipped & Integrated

All 8 phases are fully implemented in C++ (`plugdata-core`) and active in TypeScript (`mcp-server/src`).

---

### ~~Phase 6 — Direct Connection Topology~~ ✓ SHIPPED

**Feature:** `/pd/connections` endpoint  
**Shipped:** 2026-08-29  
**What was built:** A C++ handler that walks `t_outconnect` linked lists via `linetraverser`, builds a ptr→index map with a single O(n) canvas walk, and returns structured JSON with all wires including tempId labels for both endpoints. No file dump, no regex parsing.  
**Protocol:**
- `/pd/connections <canvas> <corrId>` → `/pd/connections/reply/<corrId> <jsonString>`
- JSON: `{ "canvas": "pd-main", "count": N, "connections": [{ "srcIndex", "srcOut", "destIndex", "destIn", "srcId", "destId" }, ...] }`
- ~0.56ms measured latency for full topology query

---

### ~~Phase 5 — C++ as Identity Authority~~ ✓ SHIPPED

**Feature:** `/pd/identity_snapshot` and `/pd/identity_version` endpoints  
**Shipped:** 2026-08-29  
**What was built:**  
1. **`/pd/identity_version`** — Ultra-cheap (one atomic read, no locks, no canvas walk). Returns a monotonic version counter that increments on every identity mutation (create, delete, rename, register, edit, clear). Node.js polls this to decide if a full snapshot fetch is needed. ~0.6ms round-trip.

2. **`/pd/identity_snapshot`** — Returns the COMPLETE identity state as structured JSON in one shot. Uses an O(n+m) algorithm: single gl_list walk builds ptr→index map, then iterates stable identity map. Includes class name, binbuf text, inlet/outlet count for every registered object. Automatically evicts stale entries (deleted objects) during traversal. ~0.5ms for 10 objects.

3. **Monotonic version counter** (`mcpIdentityVersion`) — Atomic uint64_t in PluginProcessor, incremented at every identity mutation site (create_batch_id, create_id, register_id, delete_id, rename_id, edit_id, clear_ids). Enables the "dirty check" pattern: if version hasn't changed, skip the snapshot entirely.

**Protocol:**
- `/pd/identity_version <corrId>` → `/pd/identity_version/reply/<corrId> <version:int32>`
- `/pd/identity_snapshot <canvas> <corrId>` → `/pd/identity_snapshot/reply/<corrId> <jsonString>`
- JSON schema: `{ version, canvas, totalObjects, entryCount, entries: [{ id, index, class, text, inlets, outlets }], evicted? }`

**How Node.js uses this (future integration):**
```typescript
// Instead of O(n²) reconciliation:
const ver = await getIdentityVersion();
if (ver === lastKnownVersion) return cachedMap; // zero work
const snapshot = await getIdentitySnapshot(canvas);
// Trust it unconditionally — C++ is the authority
lastKnownVersion = snapshot.version;
return rebuildMapFromSnapshot(snapshot);
```

---

### ~~Phase 4 — Lightweight Object Inspection~~ ✓ SHIPPED

**Feature:** `/pd/typeof` and `/pd/ports` endpoints  
**Shipped:** 2026-08-29  
**Protocol:**
- `/pd/typeof <canvas> <tempId> <corrId>` → `/pd/typeof/reply/<corrId> <className> <objectText> <x> <y> <w> <h>`
- `/pd/ports <canvas> <tempId> <corrId>` → `/pd/ports/reply/<corrId> <numInlets> <numOutlets> <inletTypes> <outletTypes>`

---

### ~~Phase 7 — Bridge-Side Spectral Analysis~~ ✓ SHIPPED

**Feature:** `/meter/spectral` endpoint with native FFTW3  
**Shipped:** 2026-08-29  
**What was built:** A new meter action that starts a probe, captures audio into the ring buffer for ~200ms, then applies Hann window + real FFT (FFTW3 `fftwf_plan_dft_r2c_1d`) on the 1024-sample buffer. Returns structured JSON with spectral features: centroid, flatness, rolloff, crest factor, fundamental, peak frequency, top 8 harmonic peaks, plus standard RMS/peak dB.  
**Protocol:**
- `/meter/spectral <canvas> <tempId> [outletIndex] [durationMs] <corrId>` → `/meter/spectral/result/<corrId> <jsonString>`
- JSON: `{ rmsDb, peakDb, fundamental, spectralCentroid, spectralFlatness, spectralRolloff, crestFactor, peakFrequency, sampleRate, fftSize, binHz, blocks, peaks: [{freq, dB}] }`
- ~207ms total (200ms capture + <1ms FFT) vs 3-5 seconds (old WAV rig approach)

---

### ~~Phase 8 — Atomic Batch Operations~~ ✓ SHIPPED

**Feature:** `/pd/batch_atomic` endpoint  
**Shipped:** 2026-08-29  
**What was built:** A single OSC endpoint that accepts create + connect + edit operations in one message. Executes under one `sys_lock()` with `canvas_suspend_dsp()` / `canvas_resume_dsp()` wrapping the entire batch, then exactly ONE `canvas_update_dsp()` at the end. Returns inline identity mappings for all created objects.  
**Protocol:**
- `/pd/batch_atomic <canvas> <corrId> <createCount> <connectCount> <editCount> [creates...] [connects...] [edits...]`
- Creates: `[tempId, x, y, kind, type, nargs, ...args]` per object
- Connects: `[srcId, srcOut, destId, destIn]` per wire
- Edits: `[tempId, newType, nargs, ...args]` per edit
- Reply: `/pd/batch_atomic/reply/<corrId> <created> <connected> <edited> [tempId, index, ...]`
- ~1.5ms for 3 creates + 2 connects + 1 edit (single DSP recompile)

---

## The Vision

When all 8 phases are complete:

| Operation | Today | After All Phases |
|-----------|-------|-----------------|
| Build a 10-object synth | ~800ms (polls + file I/O) | ~20ms (direct memory) |
| Load a 1-second sample | ~3500ms (chunked OSC) | ~2ms (memcpy) |
| Scan canvas state | ~50ms (dump + parse file) | ~3ms (memory walk) |
| Check one object's type | ~~50ms (full dump)~~ DONE | ~0.1ms (struct read) |
| Count object ports | ~~50ms (full dump)~~ DONE | ~0.1ms (struct read) |
| Probe signal during playback | ~~300ms + glitch~~ DONE | ~5ms, zero dropout |
| Full acoustic analysis | ~4000ms (WAV rig) | ~500ms (native FFT) |
| Trace 6-object silence bug | ~~3 seconds + 6 glitches~~ DONE | ~15ms, seamless |

The MCP server becomes a thin orchestration layer. All the heavy lifting happens at C++ speed inside the PlugData process.

---

## How to Use This Document

1. Pick the next phase (start with Phase 1)
2. Give this document + the implementation plan pattern from the meter probe to your coding agent
3. The agent follows the same workflow: research internals → design protocol → implement C++ → update TypeScript → test live
4. Mark it shipped in the table above
5. Move to the next phase

Each phase is independent — you can ship them one at a time and get value immediately.

---

## Reference: The Pattern (What Worked for Meter Probe)

Every phase follows the same recipe:

1. **Identify the slow path** — what file I/O or multi-round-trip pattern exists
2. **Research Pd internals** — what struct/pointer gives direct access to the data
3. **Add a handler to MCPBridge.cpp** — new OSC domain or action
4. **Add audioTick or timer logic** — if it needs audio-thread access
5. **Update PluginProcessor** — if new hooks needed (usually not after Phase 1)
6. **Advertise capability** — add to `/bridge/capabilities` reply
7. **Add TypeScript fast path** — `if (oscClient.hasCap("new_cap"))` → use native, else fallback
8. **Update tool descriptions** — canonical-tools.json + tool registration strings
9. **Test live** — build C++, restart server, verify with real audio
