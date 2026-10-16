# PRD: True Zero-Dropout Live Patching Engine (Phase 4)

**Document Version:** 1.0.0  
**Target Repositories:** `plugdata-team/plugdata` (`plugdata-core`) & `8JupiterMoll8/PlugData-MCP-Server`  
**Author:** Jupiter Moll & Antigravity  
**Date:** 2026-08-29  
**Status:** PROPOSED / ARCHITECTURE SPECIFICATION  

---

## 1. Executive Summary & Problem Statement

### 1.1 The Issue
When an AI agent or artist adds a new synth voice, effect chain, or modulation routing live while a beat is playing (e.g. via `construct_patch_v2`), the audio stream experiences a **500ms to 4,000ms pause/dropout** instead of the theoretical 1.3ms DSP recompile time.

### 1.2 Root Cause Analysis (Forensic Audit)
Line-by-line inspection of `MCPBridge.cpp` (C++) and `transaction-v2.ts` (TypeScript) identified four compounding latency bottlenecks:

1. **The "Triple DSP Stop" Cycle (`MCPBridge.cpp:600-702` + `transaction-v2.ts:2748`):**
   * `canvas_suspend_dsp()` calls `ugen_stop()`, completely disabling audio processing (`THISGUI->i_dspstate = 0`).
   * Objects and wires are constructed.
   * `canvas_resume_dsp()` calls `canvas_start_dsp()`, rebuilding and restarting DSP.
   * `MCPBridge.cpp:702` calls `canvas_update_dsp()`, stopping and restarting DSP a second time.
   * `transaction-v2.ts:2748` sends an OSC `/pd/update_dsp` message, stopping and restarting DSP a third time.
   * *Impact:* 3 sequential oscillator phase resets and buffer flushes.

2. **Incomplete C++ `batch_atomic` Protocol (`MCPBridge.cpp:576`):**
   * The native C++ endpoint `/pd/batch_atomic` currently only processes **Creates**, **Connects**, and **Edits**.
   * **Disconnects** and **Deletes** are omitted from the atomic C++ packet.
   * Whenever a patch modification requires rewiring an existing audio path (e.g. inserting sidechain ducking, bus sum redirect), the server falls back to sequential OSC roundtrips (`/pd/disconnect_batch_id` → `/pd/batch_atomic` → `/pd/update_dsp`), creating multi-roundtrip thread lock contention.

3. **Synchronous UI Layout on Main Message Thread (`MCPBridge.cpp:706`):**
   * `processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });` locks the JUCE message manager and forces full UI layout synchronization across all open viewports, blocking real-time IPC message dispatching for 100ms–300ms on large patches.

4. **Post-Mutation Roundtrip Bloat in TypeScript (`transaction-v2.ts`):**
   * After C++ execution, `transaction-v2` executes a post-mutation `/pd/census` call, runs CPU-intensive Dagre graph positioning in JavaScript, and sends `/pd/move_batch` to reposition boxes.

---

## 2. Target Architecture & Specifications

### 2.1 Unified 5-Operation Atomic C++ Protocol
Extend `/pd/batch_atomic` in `MCPBridge.cpp` to execute all 5 fundamental patch operations in a single `sys_lock()` pass:

```
OSC Pattern: /pd/batch_atomic
Arguments:
  [0] canvasName      (string)  - e.g. "pd-main"
  [1] correlationId   (string)  - UUID/nonce
  [2] deleteCount     (int)     - N deletes
  [3] disconnectCount (int)     - N disconnects
  [4] editCount       (int)     - N edits
  [5] createCount     (int)     - N creates
  [6] connectCount    (int)     - N connects
  [7..N] payload data...
```

#### C++ Execution Sequence inside `sys_lock()`:
```cpp
sys_lock();
// 1. Execute all Deletes by stable ID (clean up dangling pointers)
// 2. Execute all Disconnects (source ID/out -> dest ID/in)
// 3. Execute all In-place Edits/Renames
// 4. Execute all Creates (allocate gobj, register stable ID & serial)
// 5. Execute all Connects (wire newly created and existing objects)
// 6. Single DSP graph update: canvas_update_dsp() EXACTLY ONCE
sys_unlock();
```

---

### 2.2 Single-Recompile Rule (Eliminate DSP Stop/Resume Thrashing)
* **Remove** `canvas_suspend_dsp()` and `canvas_resume_dsp()` from `batch_atomic`.
* **Remove** redundant `canvas_update_dsp()` at `MCPBridge.cpp:702`.
* **Remove** post-mutation `/pd/update_dsp` from `transaction-v2.ts:2748` when `batch_atomic` was used.
* **Result:** Audio DSP is recalculated exactly once in a single <1.5ms operation without zeroing active voice buffers.

---

### 2.3 Decoupled Asynchronous UI Synchronization
* Replace immediate full canvas sync with throttled/debounced UI viewport updates:
```cpp
// Defer UI sync to idle thread; do not block audio engine or OSC return
juce::MessageManager::callAsync([p = processor, cnv] {
    if (p && cnv) p->synchroniseCanvases();
});
```

---

### 2.4 Server-Side Zero-Roundtrip Post-Processing
* In `transaction-v2.ts`:
  * If `batch_atomic` succeeds, **skip** post-mutation `/pd/census`.
  * Update in-memory shadow cache directly using the returned inline ID mappings (`reply.mappings`).
  * If `layout: "dagre"` is requested, compute positions offline or during idle, never blocking the audio return.

---

## 3. Implementation Roadmap

### Phase 4.1: C++ Bridge Expansion (`plugdata-core`)
1. Update `Source/Pd/MCPBridge.cpp` and `MCPBridge.h`:
   * Expand `/pd/batch_atomic` argument parsing for `deletes` and `disconnects`.
   * Implement pointer cleanup for deleted objects in `mcpStableObjectMap` and `mcpStableSerialMap`.
   * Consolidate DSP recompile into a single atomic `canvas_update_dsp()` invocation.
2. Verify with standalone unit test harness in `tests/`.

### Phase 4.2: TypeScript Transaction Engine Upgrade (`mcp-server`)
1. Update `src/tools/patching/transaction-v2.ts`:
   * Bundle `deletes`, `disconnects`, `edits`, `creates`, and `connects` into the expanded `oscClient.batchAtomic()` payload.
   * Eliminate fallback to `/pd/disconnect_batch_id` and `/pd/update_dsp`.
   * Use returned inline mappings to synchronize `IdentityMap` instantly with 0 disk I/O and 0 post-census queries.
2. Update `src/transport/osc-client.ts`:
   * Update `batchAtomic()` signature to send 5-phase arguments.

### Phase 4.3: Live Audio Benchmark & TVC Certification
1. Run automated gauntlet while 134 BPM techno loop is playing.
2. Measure audio stream continuity via `monitor_signal_flow(action: 'tap_audio_signal')` and native FFT meter.
3. Target: Maximum audio interruption < 2.0ms (0 audible dropouts, 0 xruns).

---

## 4. Success Criteria

| Metric | Current State | Target State |
| :--- | :--- | :--- |
| **Live Mutation Latency** | 4,000 ms (4.0s) | **< 15 ms (0.015s)** |
| **Audio Dropout / Silence** | 500 – 1,000 ms | **< 1.5 ms (Imperceptible)** |
| **OSC Roundtrips per Transaction** | 4 – 6 messages | **1 single atomic message** |
| **DSP Recompiles per Transaction** | 3 sequential stops/starts | **1 single recompile** |
