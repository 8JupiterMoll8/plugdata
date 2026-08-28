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

---

## Roadmap (Ordered by Value)

### NEXT: Phase 3 — Native Canvas Census

**Feature:** Direct JSON canvas state via `/pd/census`  
**Why it's #3:** Right now, to "see" the patch, the server tells C++ to write a file, waits for the file, reads it, then parses Pd text format with regex. This happens constantly.  
**What to build:** C++ walks the `gl_list` linked list, emits object types/positions/connections as structured data in one OSC reply. No file, no parsing.  
**Artist impact:** Everything feels more responsive. The AI "sees" the canvas 10-20x faster. Audit, lint, reconcile — all snappier.  
**Effort:** Medium (need to walk Pd internal structures and format the reply)

---

### Phase 4 — Lightweight Object Inspection

**Feature:** `/pd/typeof` and `/pd/ports` endpoints  
**Why it's #4:** Currently, to check if one object is a `dac~` or count its inlets, the server dumps and parses the ENTIRE patch. Just to read one object's class name.  
**What to build:** Two simple endpoints that read `ob_binbuf` (object text) and count outlets/inlets directly from the struct.  
**Artist impact:** Faster error checking, port validation, connection safety. Fewer "wrong inlet" mistakes.  
**Effort:** Low (reading a single struct field)

---

### Phase 5 — C++ as Identity Authority

**Feature:** Single source of truth for tempId mapping  
**Why it's #5:** The identity map exists in both Node.js AND C++. After every mutation, an expensive O(n²) reconciliation runs to match them. Fragile and slow.  
**What to build:** C++ maintains the sole authoritative registry. Every batch reply includes the full state. Node.js only caches locally, never reconciles.  
**Artist impact:** Fewer identity bugs ("object not found"), faster transactions, more reliable patching overall.  
**Effort:** High (architectural change spanning both codebases)

---

### Phase 6 — Direct Connection Topology

**Feature:** `/pd/connections` endpoint  
**Why it's #6:** To know what's wired to what, the server dumps the entire patch to a file and parses it. Even just to check one wire.  
**What to build:** Walk `t_outconnect` linked lists in C++, return connection list directly.  
**Artist impact:** Connection validation, topology analysis, and disconnect detection all become instant.  
**Effort:** Low (similar pattern to census but focused on connections only)

---

### Phase 7 — Bridge-Side Spectral Analysis

**Feature:** FFT inside the C++ bridge for `analyze_acoustics`  
**Why it's #7:** Full acoustic analysis currently deploys 10 objects, records WAV, reads file, runs FFT in JavaScript. Takes 3-5 seconds.  
**What to build:** Read signal buffer directly (like the meter), run KISS FFT or similar, return MFCCs + spectral centroid + crest factor in one reply.  
**Artist impact:** "What does this sound like?" answers in 500ms instead of 5 seconds. Acoustic fingerprinting becomes practical for live use.  
**Effort:** Medium (integrate an FFT library into the C++ build)

---

### Phase 8 — Atomic Batch Operations

**Feature:** Single-recompile compound transactions  
**Why it's #8:** Creating 10 objects + connecting them + toggling DSP can trigger 3 separate DSP recompiles = 3 audio glitches during live performance.  
**What to build:** A `/pd/batch_atomic` endpoint that accepts create+connect+edit in one message and does exactly ONE `canvas_update_dsp()` at the end.  
**Artist impact:** Building new voices during a live set with only one brief click instead of three. Smoother live patching.  
**Effort:** Medium (needs careful transaction ordering in C++)

---

## The Vision

When all 8 phases are complete:

| Operation | Today | After All Phases |
|-----------|-------|-----------------|
| Build a 10-object synth | ~800ms (polls + file I/O) | ~20ms (direct memory) |
| Load a 1-second sample | ~3500ms (chunked OSC) | ~2ms (memcpy) |
| Scan canvas state | ~50ms (dump + parse file) | ~3ms (memory walk) |
| Check one object's type | ~50ms (full dump) | ~0.1ms (struct read) |
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
