# MCPBridge Zero-Dropout Signal Meter — Implementation Plan

**Author:** AI Architecture Agent  
**Date:** 2026-08-28  
**Target:** `/home/alphi/Desktop/plugdata/plugdata-core/Source/Pd/MCPBridge.cpp`  
**Prerequisite:** MCPBridge.cpp is already integrated into the PlugData core (done).

---

## 1. Goal

Add **passive signal metering** to MCPBridge so the MCP server can read RMS/peak/frequency from any object's outlet **without injecting temporary objects** and **without causing DSP graph recompilation** (zero audio dropout).

Currently, the MCP server's `probe` tool deploys a temporary `[throw~]`/`[catch~]`/`[writesf~]` rig on canvas, which forces `canvas_update_dsp()` and causes 20-50ms audio glitches. This feature eliminates that entirely.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  AUDIO THREAD (PluginProcessor::processBlock)               │
│                                                             │
│  performDSP()  ─── sys_lock/unlock ───► DSP chain runs     │
│       │                                                     │
│       ▼                                                     │
│  mcpBridge->audioTickCallback()                             │
│       │   (called AFTER performDSP, same audio callback)    │
│       │                                                     │
│       ├─► For each active probe target:                     │
│       │     outconnect_get_signal(oc) → t_signal*           │
│       │     Compute RMS/peak over 64-sample block           │
│       │     Write result to lock-free ProbeResult queue     │
│       │                                                     │
│       ▼                                                     │
│  [continues to statusbarSource->peakBuffer.write(buffer)]   │
└─────────────────────────────────────────────────────────────┘
                          │
              lock-free SPSC queue
              (moodycamel::ReaderWriterQueue)
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  MCP BRIDGE THREAD (OSC receiver / Timer callback)          │
│                                                             │
│  On /probe/start:                                           │
│    - Resolve tempId → canvas index → object → outlet        │
│    - Find t_outconnect* for the target outlet               │
│    - Store in activeProbes[] (atomic pointer write)         │
│                                                             │
│  On timer tick (15ms / 66Hz):                               │
│    - Dequeue ProbeResults from lock-free queue              │
│    - Accumulate into per-probe accumulators                 │
│    - When requested duration elapsed:                       │
│       Send OSC reply with RMS/peak/freq to MCP server       │
│                                                             │
│  On /probe/stop:                                            │
│    - Remove from activeProbes[]                             │
│    - Send final accumulated result                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Key Design Decisions

### 3.1 Signal Access Pattern (Proven by ConnectionMessageDisplay)

The pattern at `Source/Components/ConnectionMessageDisplay.h:106-134` is the only safe way to read signal buffers. We replicate it:

```cpp
// Called from audio thread AFTER performDSP() completes
void audioTickCallback()
{
    for (auto& probe : activeProbes) {
        if (!probe.active.load(std::memory_order_relaxed)) continue;
        
        auto* oc = probe.outconnect.load(std::memory_order_acquire);
        if (!oc) continue;
        
        auto* signal = outconnect_get_signal(oc);
        if (!signal || !signal->s_vec) continue;
        
        // signal->s_n = samples per channel (typically 64)
        // signal->s_nchans = number of channels
        // signal->s_vec = interleaved float buffer
        
        float rms = 0.0f, peak = 0.0f;
        int n = signal->s_n;
        float* vec = signal->s_vec; // read channel 0
        
        for (int i = 0; i < n; i++) {
            float s = vec[i];
            float abs_s = std::fabs(s);
            rms += s * s;
            if (abs_s > peak) peak = abs_s;
        }
        rms = std::sqrt(rms / (float)n);
        
        ProbeResult result { probe.probeId, rms, peak, signal->s_n };
        probe.resultQueue.try_enqueue(result);
    }
}
```

### 3.2 Debugging Flag Requirement

`outconnect_get_signal()` returns NULL unless `plugdata_debugging_enabled()` is true. The bridge must **auto-enable** this when probes are active:

```cpp
void activateProbing()
{
    if (!plugdata_debugging_enabled()) {
        processor->lockAudioThread();
        set_plugdata_debugging_enabled(1);
        // Force DSP recompile so signals get stored on outconnects
        canvas_update_dsp();
        processor->unlockAudioThread();
        debugWasEnabledByUs = true;
    }
}
```

**Note:** This causes ONE DSP recompile when the first probe activates. After that, all subsequent probes are zero-cost. When all probes stop, optionally restore the flag.

### 3.3 Resolving Object → Outlet → t_outconnect*

The MCP server identifies objects by semantic `tempId`. The bridge must resolve:

```
tempId → canvas index → t_gobj* → t_object* → outlet chain → t_outconnect*
```

The identity map (tempId → index) already exists in the Lua layer (`mcp_id_map`). For the C++ bridge, we need a callback to the processor's identity system:

```cpp
t_outconnect* resolveProbeTarget(const juce::String& canvasName, 
                                  const juce::String& tempId, 
                                  int outletIndex)
{
    // Get canvas
    t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
    if (!cnv) return nullptr;
    
    // Resolve tempId to index (via the existing identity map)
    int objIndex = processor->resolveTempoId(canvasName, tempId);
    if (objIndex < 0) return nullptr;
    
    // Walk to object
    t_gobj* gobj = glistObjectAt(cnv, objIndex);
    if (!gobj) return nullptr;
    
    t_object* obj = pd_checkobject(&gobj->g_pd);
    if (!obj) return nullptr;
    
    // Walk to the requested outlet
    t_outlet* outlet = obj->ob_outlet;
    for (int i = 0; i < outletIndex && outlet; i++) {
        outlet = outlet->o_next;
    }
    if (!outlet) return nullptr;
    
    // Check it's a signal outlet
    if (outlet->o_sym != &s_signal) return nullptr;
    
    // Return the first outconnect (the signal buffer is shared across all)
    return outlet->o_connections;
}
```

### 3.4 Thread Safety Summary

| Operation | Thread | Lock | Notes |
|-----------|--------|------|-------|
| Register probe target (resolve tempId → outconnect*) | OSC thread | `sys_lock()` | Needs Pd lock to walk canvas safely |
| Read signal buffer | Audio thread | None (called after performDSP, lock released) | Buffer is stable until next DSP recompile |
| Write ProbeResult to queue | Audio thread | None (lock-free enqueue) | `moodycamel::ReaderWriterQueue` |
| Read ProbeResult from queue | Timer/OSC thread | None (lock-free dequeue) | Timer at 15-66ms interval |
| Send OSC reply | Timer/OSC thread | None | JUCE OSCSender is thread-safe |

---

## 4. Data Structures

### 4.1 ProbeResult (audio → timer thread)

```cpp
struct ProbeResult {
    uint32_t probeId;       // which probe this belongs to
    float rms;              // RMS of this 64-sample block
    float peak;             // peak absolute value
    int blockSize;          // number of samples (usually 64)
};
```

### 4.2 ActiveProbe (shared between threads)

```cpp
struct ActiveProbe {
    std::atomic<bool> active { false };
    std::atomic<t_outconnect*> outconnect { nullptr };
    uint32_t probeId = 0;
    juce::String correlationId;
    int outletIndex = 0;
    int durationMs = 500;           // how long to accumulate
    juce::int64 startTimeMs = 0;
    
    // Lock-free queue: audio thread writes, timer thread reads
    moodycamel::ReaderWriterQueue<ProbeResult> resultQueue { 256 };
    
    // Accumulator (timer thread only)
    float accRms = 0.0f;
    float accPeak = 0.0f;
    int accBlocks = 0;
};
```

### 4.3 ProbeManager (owned by MCPBridge)

```cpp
static constexpr int MAX_PROBES = 16;  // max simultaneous probes

class ProbeManager {
public:
    ActiveProbe probes[MAX_PROBES];
    
    // Called from audio thread after performDSP()
    void audioTick();
    
    // Called from OSC/timer thread
    int startProbe(t_outconnect* oc, const juce::String& correlationId, int durationMs);
    void stopProbe(uint32_t probeId);
    void collectResults();  // called by timer, sends OSC replies
    
private:
    std::atomic<uint32_t> nextProbeId { 1 };
    bool debugWasEnabledByUs = false;
};
```

---

## 5. OSC Protocol Extension

### 5.1 New Domain: `/meter/`

Add to `MCPBridge::oscMessageReceived()`:

```cpp
} else if (domain == "meter") {
    auto meterAction = parts.size() > 1 ? parts[1] : "";
    handleMeterDomain(meterAction, message);
}
```

### 5.2 Inbound Messages (MCP Server → PlugData)

| Address | Arguments | Description |
|---------|-----------|-------------|
| `/meter/start` | `canvasName, tempId, outletIndex, durationMs, correlationId` | Start passive probe on object outlet |
| `/meter/stop` | `probeId, correlationId` | Stop a running probe early |
| `/meter/stop_all` | `correlationId` | Stop all probes |
| `/meter/query` | `canvasName, tempId, outletIndex, correlationId` | One-shot: read current block, reply immediately |

### 5.3 Outbound Messages (PlugData → MCP Server)

| Address | Arguments | Description |
|---------|-----------|-------------|
| `/meter/start/reply/<corrId>` | `probeId` (int) | Probe registered, returns ID |
| `/meter/result/<corrId>` | `rmsDb` (float), `peakDb` (float), `dominantFreqHz` (float), `blockCount` (int) | Final accumulated result |
| `/meter/query/reply/<corrId>` | `rmsDb` (float), `peakDb` (float), `nChans` (int), `blockSize` (int) | Instant single-block result |
| `/meter/error/<corrId>` | `errorMessage` (string) | Probe failed (object not found, not signal outlet, etc.) |

### 5.4 Capability Advertisement

Add to `handleBridgeDomain("capabilities")`:

```cpp
reply.addArgument(juce::String("meter"));           // passive signal metering
reply.addArgument(juce::String("meter_query"));     // instant one-shot query
```

---

## 6. Frequency Estimation (Zero-Crossing + Autocorrelation)

For a 64-sample block at 44100 Hz, zero-crossing gives rough pitch. For better accuracy, accumulate multiple blocks and run autocorrelation:

```cpp
float estimateFrequency(const float* buf, int n, float sampleRate)
{
    // Simple autocorrelation pitch detection
    int minLag = (int)(sampleRate / 5000.0f);  // max freq 5kHz
    int maxLag = std::min(n / 2, (int)(sampleRate / 20.0f));  // min freq 20Hz
    
    float bestCorr = 0.0f;
    int bestLag = 0;
    
    for (int lag = minLag; lag <= maxLag; lag++) {
        float corr = 0.0f;
        for (int i = 0; i < n - lag; i++) {
            corr += buf[i] * buf[i + lag];
        }
        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag = lag;
        }
    }
    
    return bestLag > 0 ? sampleRate / (float)bestLag : 0.0f;
}
```

For accurate frequency detection across multiple blocks, the ProbeManager accumulates samples into a ring buffer (512-1024 samples) and runs autocorrelation on the full buffer at reply time.

---

## 7. Implementation Steps (Ordered)

### Phase 1: Infrastructure (no audio thread changes yet)

| Step | File | What to do |
|------|------|-----------|
| 1.1 | `MCPBridge.h` | Add `#include <Libraries/readerwriterqueue/readerwriterqueue.h>`. Add `ProbeManager probeManager;` member. Add `void handleMeterDomain(...)` declaration. |
| 1.2 | `MCPBridge.h` | Define `ProbeResult`, `ActiveProbe`, `ProbeManager` structs (see Section 4). |
| 1.3 | `MCPBridge.cpp` | Add `handleMeterDomain()` with `start`, `stop`, `stop_all`, `query` cases. |
| 1.4 | `MCPBridge.cpp` | In `oscMessageReceived()`, add the `"meter"` domain routing. |
| 1.5 | `MCPBridge.cpp` | Add `"meter"` and `"meter_query"` to the capabilities reply. |

### Phase 2: Probe Resolution

| Step | File | What to do |
|------|------|-----------|
| 2.1 | `MCPBridge.cpp` | Implement `resolveProbeTarget()` — resolve tempId to `t_outconnect*` using `glistObjectAt()` + outlet walk. Initially use index-based resolution (the MCP server sends the canvas index alongside tempId). |
| 2.2 | `MCPBridge.cpp` | In `handleMeterDomain("start")`: call `resolveProbeTarget()` under `sys_lock()`, validate it's a signal outlet, store in `probeManager.probes[]`. |
| 2.3 | `MCPBridge.cpp` | Handle the `plugdata_debugging_enabled()` gate: if false, enable it + trigger one `canvas_update_dsp()`. Track this state in `probeManager.debugWasEnabledByUs`. |

### Phase 3: Audio Thread Integration

| Step | File | What to do |
|------|------|-----------|
| 3.1 | `MCPBridge.h` | Add `void audioTick()` public method. |
| 3.2 | `MCPBridge.cpp` | Implement `ProbeManager::audioTick()` — iterate active probes, read signal, compute RMS/peak, enqueue `ProbeResult`. |
| 3.3 | `PluginProcessor.cpp` | After `performDSP()` (line ~1025), add: `if (mcpBridge) mcpBridge->audioTick();` — same location where `connectionListener->updateSignalData()` is already called. |

### Phase 4: Result Collection & Reply

| Step | File | What to do |
|------|------|-----------|
| 4.1 | `MCPBridge.cpp` | In the existing `timerCallback()` (currently used for MorphJobs), add a call to `probeManager.collectResults()`. |
| 4.2 | `MCPBridge.cpp` | `ProbeManager::collectResults()`: dequeue all `ProbeResult`s, accumulate RMS/peak, check if duration elapsed, send OSC reply when done. |
| 4.3 | `MCPBridge.cpp` | Convert linear amplitude to dBFS for the reply: `20.0f * log10f(value + 1e-10f)`. |

### Phase 5: Frequency Estimation

| Step | File | What to do |
|------|------|-----------|
| 5.1 | `MCPBridge.h` | Add a small ring buffer (1024 floats) to `ActiveProbe` for frequency analysis. |
| 5.2 | `MCPBridge.cpp` | In `audioTick()`, also copy raw samples into the ring buffer. |
| 5.3 | `MCPBridge.cpp` | In `collectResults()`, when duration elapses, run autocorrelation on the ring buffer to estimate dominant frequency. Include in reply. |

### Phase 6: One-Shot Query

| Step | File | What to do |
|------|------|-----------|
| 6.1 | `MCPBridge.cpp` | `handleMeterDomain("query")`: resolve target, read current signal buffer directly (under `ScopedTryLock` on audioLock), compute RMS/peak, reply immediately. No background probe needed. |

### Phase 7: Cleanup & Hardening

| Step | File | What to do |
|------|------|-----------|
| 7.1 | `MCPBridge.cpp` | When all probes stop and `debugWasEnabledByUs == true`, restore `set_plugdata_debugging_enabled(0)` (optional — may decide to leave it on for performance). |
| 7.2 | `MCPBridge.cpp` | Handle DSP recompilation events: when `canvas_update_dsp()` is called externally, all stored `t_outconnect*` pointers may be invalidated. Use the weak reference pattern (`oc_signal_reference`) or re-resolve on next audioTick if signal is NULL. |
| 7.3 | `MCPBridge.cpp` | Timeout: if a probe's duration elapses but no samples were collected (DSP off?), reply with error. |
| 7.4 | `MCPBridge.h` | Add `/meter/status` query that returns number of active probes and whether debugging is enabled. |

---

## 8. MCP Server Integration (Node.js side)

The MCP server (`/home/alphi/Desktop/plugdata/mcp-server/src/`) needs to use the new `/meter/` protocol instead of deploying probe rigs. Changes needed:

### 8.1 New OSC Protocol Handler

In the bridge communication layer (likely `src/bridge/` or `src/comms/`):

```typescript
// Register listener for /meter/result/<corrId> and /meter/query/reply/<corrId>
// When received, resolve the pending Promise for that correlation ID
```

### 8.2 Update `monitor_signal_flow` tool (`src/tools/observe.ts`)

For the `probe` and `tap_audio_signal` actions:

```typescript
// OLD: deploy throw~/catch~/writesf~ rig → wait → read file → delete rig
// NEW: send /meter/query (one-shot) or /meter/start (duration) → wait for /meter/result reply
```

### 8.3 Capability Check

When the bridge reports `"meter"` in its capabilities, use the native meter path. Fall back to the old rig-based approach for legacy bridges (Lua-only installations without the C++ bridge).

---

## 9. Files to Modify (Summary)

| File | Changes |
|------|---------|
| `Source/Pd/MCPBridge.h` | Add ProbeResult, ActiveProbe, ProbeManager structs. Add `audioTick()`, `handleMeterDomain()`. Include readerwriterqueue. |
| `Source/Pd/MCPBridge.cpp` | Implement handleMeterDomain, ProbeManager::audioTick, ProbeManager::collectResults, resolveProbeTarget, frequency estimation. Add "meter" to OSC router and capabilities. |
| `Source/PluginProcessor.cpp` | Add `mcpBridge->audioTick()` call after `performDSP()` (line ~1025). |
| `mcp-server/src/tools/observe.ts` | (Later) Switch probe/tap_audio_signal to use `/meter/` protocol when available. |
| `mcp-server/src/bridge/` | (Later) Add OSC listener for `/meter/result/*` and `/meter/query/reply/*`. |

---

## 10. Testing Strategy

| Test | How |
|------|-----|
| Basic probe | Create `[osc~ 440]` → `[dac~]`, send `/meter/query` on the osc~ outlet. Verify ~440Hz frequency, ~0.707 RMS (-3dB). |
| Zero dropout | Start a drum pattern via MCP, probe multiple objects while playing. Verify no audio glitch (no `canvas_update_dsp()` called). |
| Multi-probe | Probe 8 objects simultaneously. Verify all return results within timeout. |
| DSP off | Probe with DSP off. Verify graceful error reply (not hang). |
| Invalid target | Probe a non-existent tempId. Verify error reply. |
| Control outlet | Probe a control-rate outlet (e.g. `[metro]`). Verify error "not a signal outlet". |
| Stress | Run 16 probes at once for 10 seconds. Verify no memory leak, no crash. |

---

## 11. Performance Budget

| Metric | Budget | Notes |
|--------|--------|-------|
| Audio thread overhead per probe | < 2 microseconds | Just RMS/peak over 64 floats + one enqueue |
| Max probes before audible impact | 16 | At 16 probes × 2us = 32us overhead per 64-sample block (1.45ms at 44.1kHz). Well under 5% CPU budget. |
| Memory per probe | ~5 KB | 256-entry queue × 16 bytes + 1024-float ring buffer |
| Reply latency | < 30ms | Timer runs at 15ms; worst case is 2 timer ticks. |

---

## 12. Migration Path

1. **Phase 1-4:** Get basic meter working (RMS/peak). Ship it.
2. **Phase 5:** Add frequency estimation. Ship it.
3. **Phase 6:** Add instant query. Ship it.
4. **MCP Server update:** Switch observe.ts to use native meter when available.
5. **Remove old probe rig code:** Once all users have the C++ bridge, deprecate the canvas-injecting probe approach.

The old probe method remains as fallback for installations still using the Lua bridge without the C++ MCPBridge.

---

## 13. Dependencies

- `moodycamel::ReaderWriterQueue` — already in the project at `Libraries/readerwriterqueue/readerwriterqueue.h`
- `outconnect_get_signal()` — already declared in `m_pd.h`, PlugData extension
- `plugdata_debugging_enabled()` / `set_plugdata_debugging_enabled()` — already in `s_inter.h`
- `glistObjectAt()` — already implemented as a static helper in MCPBridge.cpp

No new external dependencies required.

---

## 14. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| DSP recompile invalidates stored `t_outconnect*` | Probe reads garbage / crashes | Use weak reference validation (`is_reference_valid`). If NULL, set probe to "stale" state, attempt re-resolve on next timer tick. |
| `plugdata_debugging_enabled` has CPU cost | Slight overhead storing signals on every outconnect during DSP compilation | Acceptable. The overhead is at compilation time, not per-sample. The per-sample cost is just the `outconnect_set_signal` pointer store which only happens during DSP graph setup. |
| Object deleted while probe active | Dangling pointer | The weak reference pattern in `outconnect_get_signal` handles this — returns NULL when reference is invalid. |
| Audio thread blocks on full queue | Glitch | `try_enqueue` never blocks — it silently drops. Meter may report stale data for one frame, which is acceptable. |
| MCP server sends probe requests faster than we can process | Queue overflow | Limit to MAX_PROBES=16. Reject with error if full. |
