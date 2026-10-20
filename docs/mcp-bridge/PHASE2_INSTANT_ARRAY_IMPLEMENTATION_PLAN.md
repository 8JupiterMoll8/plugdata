# Phase 2: Instant Array/Sample Access — Implementation Plan

**Goal:** Eliminate the 10ms inter-chunk delays and 128-float chunking for array read/write. A 1-second buffer (44100 samples) should transfer in <50ms instead of 3.5 seconds.

**Key Insight:** The C++ MCPBridge ALREADY has direct `t_word*` memory access (`handleArrayDomain` at MCPBridge.cpp:946-1087). The bottleneck is entirely in the protocol: tiny 128-float OSC packets with 10ms sleeps between them.

---

## 1. The Problem (What's Slow)

### Write: 44100 samples currently takes ~3.5 seconds

```
TypeScript: for each 128-float chunk {
    send("/array/write", [...128 floats...])
    await sleep(10ms)    ← THIS IS THE KILLER
}
// 345 chunks × 10ms = 3.45 seconds just in delays
```

The C++ side already writes directly to `vec[offset].w_float` — it's instant. But it receives 345 separate UDP packets with artificial delays.

### Read: 44100 samples takes ~500ms

```
C++: for each 128-float chunk {
    sender.send(chunkMsg with 128 floats)  ← sends all chunks back-to-back
}
TypeScript: assembles chunks via callback, waits for /done signal
```

The C++ sends chunks without delay, but the 128-float chunk size means 345 UDP packets for 1 second of audio. UDP packet overhead + JS callback assembly adds up.

---

## 2. The Solution: Large-Chunk Direct Transfer

### Strategy: Increase chunk size from 128 to 4096+ floats

OSC over UDP has a practical packet limit of ~8192 bytes (Ethernet MTU is 1500 for fragmented, but localhost UDP can handle up to 65535 bytes). Each float is 4 bytes in OSC. So:

- **4096 floats = 16KB per packet** — well within localhost UDP limits
- 44100 samples ÷ 4096 = **11 packets** instead of 345
- With zero delay: 11 packets × ~0.1ms = **~1ms** instead of 3450ms

For writes: **remove the 10ms inter-chunk delay entirely** and increase chunk size.
For reads: **increase the C++ chunk size** from 128 to 4096.

---

## 3. C++ Changes (MCPBridge.cpp)

### 3.1 New action: `/array/write_bulk`

Add a new bulk write handler that accepts larger payloads and doesn't expect chunked sequencing:

**File:** `MCPBridge.cpp` in `handleArrayDomain()`

```cpp
if (arrayAction == "write_bulk") {
    // Format: [arrayName, subpatch, correlationId, offset, ...floats]
    // All data in one message — no chunk index, no total chunks
    if (msg.size() < 4) { sys_unlock(); return; }
    
    auto correlationId = getArgString(msg[2]);
    int offset = static_cast<int>(getArgFloat(msg[3]));
    int dataStart = 4;
    int dataCount = msg.size() - dataStart;
    
    // Auto-resize array if needed
    int requiredSize = offset + dataCount;
    if (requiredSize > size) {
        garray_resize_long(garray, requiredSize);
        // Re-fetch after resize
        if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
            sys_unlock();
            sendReply("/array/write_bulk/error/" + correlationId, "Resize failed");
            return;
        }
    }
    
    // Direct memory write — the hot loop
    int written = 0;
    for (int i = dataStart; i < msg.size() && (offset + written) < size; ++i) {
        vec[offset + written].w_float = getArgFloat(msg[i]);
        written++;
    }
    
    garray_redraw(garray);
    sys_unlock();
    
    // Reply with [written, totalSize]
    juce::OSCMessage reply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
    reply.addArgument(static_cast<int32>(written));
    reply.addArgument(static_cast<int32>(size));
    sender.send(reply);
    return;
}
```

### 3.2 Increase read chunk size

**Current** (line 1024): `int const CHUNK = 128;`

**Change to:**
```cpp
int const CHUNK = 4096;  // ~16KB per UDP packet, well within localhost limits
```

This is backward-compatible — the TypeScript chunk handler already assembles by `chunkIndex`, not by assumed size.

### 3.3 New action: `/array/read_bulk`

A single-reply read for arrays ≤ 8192 samples (fits in one UDP packet on localhost):

```cpp
if (arrayAction == "read_bulk") {
    // Format: [arrayName, subpatch, correlationId, offset, limit]
    auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
    int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
    int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : size;
    
    int readStart = std::max(0, std::min(offset, size - 1));
    int readCount = std::min(limit, size - readStart);
    
    // Cap at 8192 samples per single reply (32KB OSC payload)
    readCount = std::min(readCount, 8192);
    
    juce::OSCMessage reply { juce::OSCAddressPattern("/array/read_bulk/reply/" + correlationId) };
    reply.addArgument(arrayName);
    reply.addArgument(static_cast<int32>(size));         // total array size
    reply.addArgument(static_cast<int32>(readStart));    // actual start
    reply.addArgument(static_cast<int32>(readCount));    // actual count
    for (int i = readStart; i < readStart + readCount; ++i) {
        reply.addArgument(vec[i].w_float);
    }
    sys_unlock();
    sender.send(reply);
    return;
}
```

### 3.4 Capability advertisement

In `handleBridgeDomain("capabilities")`, add:
```cpp
reply.addArgument(juce::String("array_bulk"));
```

---

## 4. TypeScript Changes (array-tools.ts)

### 4.1 Write — Use bulk path with large chunks, no delay

**File:** `src/tools/array-tools.ts`

Replace the chunked write loop (lines 44-63) with:

```typescript
const BULK_CHUNK_SIZE = 4096;  // 4096 floats per packet

if (oscClient.hasCap("array_bulk")) {
    // Fast bulk write: large chunks, no inter-chunk delay, await final reply
    const totalChunks = Math.ceil(data.length / BULK_CHUNK_SIZE);
    const id = String(oscClient.nextCorrelationId());
    
    for (let c = 0; c < totalChunks; c++) {
        const offset = c * BULK_CHUNK_SIZE;
        const slice = data.slice(offset, offset + BULK_CHUNK_SIZE);
        
        if (c === totalChunks - 1) {
            // Await only the final chunk's reply
            await oscClient.sendAndAwait(
                "/array/write_bulk",
                [targetArray, subpatch, id, offset, ...slice],
                `/array/write_bulk/reply/${id}`,
                5000,
                { retries: 0 }
            );
        } else {
            // Fire-and-forget for intermediate chunks (C++ processes in order)
            await oscClient.send("/array/write_bulk", [targetArray, subpatch, id, offset, ...slice]);
        }
        // NO delay between chunks!
    }
    return; // done
}

// ... existing chunked fallback for bridges without array_bulk cap ...
```

### 4.2 Read — Use bulk path for small/medium arrays

```typescript
if (oscClient.hasCap("array_bulk") && limit <= 8192) {
    // Single-reply bulk read
    const id = String(oscClient.nextCorrelationId());
    const reply = await oscClient.sendAndAwait(
        "/array/read_bulk",
        [targetArray, subpatch, id, offset, limit],
        `/array/read_bulk/reply/${id}`,
        5000,
    );
    // reply.args = [name, totalSize, actualStart, actualCount, ...floats]
    const floats = (reply.args as number[]).slice(4);
    return floats;
}

// For arrays > 8192 samples: use the improved chunked read (C++ now sends 4096/chunk)
// ... existing chunk-assembly code (already works, just faster with bigger chunks) ...
```

### 4.3 Remove the 10ms delay from existing chunked write (backward compat improvement)

Even for the old `/array/write` path, remove the delay when talking to the C++ bridge:

```typescript
// Old code:
if (c < totalChunks - 1) {
    await new Promise(r => setTimeout(r, 10));  // ← DELETE THIS
}

// New: no delay needed. The C++ bridge processes messages in order on its OSC thread.
// The 10ms delay was needed for the Lua bridge which could drop UDP under load.
// The C++ bridge (JUCE OSCReceiver) handles back-to-back packets correctly.
```

---

## 5. Performance Comparison

| Operation | Current | After Phase 2 |
|-----------|---------|---------------|
| Write 44100 samples | 345 chunks × 10ms = **3450ms** | 11 chunks × 0ms = **~5ms** |
| Write 4096 samples | 32 chunks × 10ms = **320ms** | 1 chunk = **~1ms** |
| Read 44100 samples | 345 chunk callbacks = **~200ms** | 11 chunk callbacks = **~15ms** |
| Read 512 samples | 4 chunk callbacks = **~20ms** | 1 bulk reply = **~2ms** |
| Read 4096 samples | 32 chunk callbacks = **~50ms** | 1 bulk reply = **~2ms** |

---

## 6. Files to Modify

| File | What to change |
|------|---------------|
| `plugdata-core/Source/Pd/MCPBridge.cpp` | Add `write_bulk` and `read_bulk` handlers in `handleArrayDomain()`. Increase existing read CHUNK from 128 to 4096. Add `"array_bulk"` to capabilities. |
| `mcp-server/src/tools/array-tools.ts` | Add bulk write path (no delay, 4096 chunks). Add bulk read path (single reply for ≤8192). Remove 10ms delay from existing chunked write. |

---

## 7. Testing

| Test | How to verify |
|------|---------------|
| Small write (64 samples) | `manage_pd_tables({ action: "access_table_data", mode: "write", ... })` with 64 floats. Verify instant. |
| Large write (44100 samples) | Generate 1 second of sine wave, write to array. Time it. Should be <50ms. |
| Read back | Read the same array, verify data matches what was written (no quantization — float precision preserved). |
| Auto-resize | Write to an array that doesn't exist yet. Verify it auto-creates and resizes. |
| Stats after write | Call `analyze` on the written array. Verify RMS/pitch match expected values. |
| Backward compat | With a bridge that doesn't report `array_bulk`, verify the old chunked path still works. |
| Bulk read boundary | Read exactly 8192 samples (the cap). Verify it works. Read 8193 — verify it falls back to chunked. |

---

## 8. Edge Cases & Risks

| Risk | Mitigation |
|------|-----------|
| UDP packet too large (>65535 bytes) | 4096 floats × 4 bytes + OSC overhead = ~16.5KB. Well under 65KB limit. Safe on localhost. |
| Network fragmentation on non-localhost | Not a concern — MCP bridge is always localhost (127.0.0.1). |
| JUCE OSCReceiver message size limit | JUCE's OSC implementation handles messages up to the UDP buffer size. On Linux, default receive buffer is 212992 bytes. 16KB is fine. |
| Concurrent writes to same array | `sys_lock()` serializes all access. Safe. |
| Array resize during write | `garray_resize_long` is called under `sys_lock`. After resize, `garray_getfloatwords` is re-called to get the new pointer. Safe. |
| Float precision | Direct `w_float` access preserves full 32-bit float precision. No 16-bit PCM quantization (unlike the WAV fallback). |
| OSC argument count limits | JUCE OSCMessage has no hard limit on argument count. 4096 float arguments is fine. |

---

## 9. Future Extension: Binary Blob Protocol (Phase 2b, optional)

For truly massive arrays (>100K samples), OSC float arguments have per-float overhead (4 bytes type tag + 4 bytes value = 8 bytes/sample). An OSC blob argument would be more efficient (4 bytes header + 4 bytes/sample raw). This is optional and can be done later:

```cpp
// Optional future: OSC blob for massive transfers
juce::MemoryBlock blobData(readCount * sizeof(float));
memcpy(blobData.getData(), &vec[readStart].w_float, readCount * sizeof(float));
reply.addArgument(juce::OSCArgument(blobData));
```

This halves the bandwidth for large transfers but requires changes to the osc.js parser on the TypeScript side. Not needed for Phase 2 — the 4096-float chunk approach already achieves <50ms for 1-second buffers.

---

## 10. Summary

The implementation is straightforward because:
1. C++ already has direct `t_word*` access (existing `handleArrayDomain`)
2. We just need larger chunks and no delays
3. The bulk endpoints are simple variations of existing code
4. TypeScript changes are minimal (capability check + new send pattern)

**Net result:** Loading a 1-second sample goes from 3.5 seconds to ~5ms. This makes granular synthesis, wavetable morphing, live tape loops, and sample-based instruments practical in real-time.
