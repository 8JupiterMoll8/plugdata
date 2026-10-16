# Phase 1: Kill the Identity Polling Loop — Implementation Plan

**Goal:** Eliminate the 100-500ms post-create polling loop by returning tempId→index mappings directly in the `create_batch_id` reply.

**Prerequisite:** MCPBridge.cpp is already integrated. The meter probe pattern is the reference.

---

## 1. The Problem (What's Slow)

After `construct_patch_v2` creates objects via `create_batch_id`, the Node.js server needs to know what canvas index each new object got. Currently:

1. C++ creates objects, stores `tempId → t_gobj*` in `mcpStableObjectMap`
2. C++ replies with ONLY the count: `(float)created` — no indices
3. Node.js enters a **polling loop** (25ms × up to 20 attempts = 500ms max):
   - Sends `/pd/get_mappings`
   - Gets back `[tempId, index, tempId, index, ...]`
   - Checks if ALL new tempIds have valid indices
   - If not, waits 25ms and tries again
4. If polling fails: does a full `/pd/dump` + file parse (another 50-80ms)

**Location of polling loop:** `src/tools/patching/transaction-v2.ts` lines 1244-1322

---

## 2. The Solution

### C++ Side: Include mappings in the `create_batch_id` reply

**File:** `/home/alphi/Desktop/plugdata/plugdata-core/Source/PluginProcessor.cpp`  
**Function:** The `mcp_create_batch_id` handler (search for `case hash("mcp_create_batch_id")`)  
**Current reply (around line 2233-2236):**

```cpp
SmallArray<pd::Atom> atoms;
atoms.add(pd::Atom((float)created));   // Just the count!
sendMCPReply(String("/pd/create_batch_id/reply/" + correlation_id), atoms);
```

**Change to:**

```cpp
SmallArray<pd::Atom> atoms;
atoms.add(pd::Atom((float)created));

// Append inline identity mappings: [tempId, index, tempId, index, ...]
// The objects were just created and are still in scope.
// mcpStableObjectMap already has them (populated at line ~2216).
// NOTE: sys_lock is still held here, so getObjectIndex is safe.
for (int o = 0; o < created; o++) {
    auto& objId = objectIds[o];  // The tempId string for this object
    t_gobj* gobj = createdObjects[o];  // The t_gobj* returned by createObject
    int index = getObjectIndex(canvas, gobj);  // Walk gl_list to find index
    if (index >= 0) {
        atoms.add(pd::Atom(generateSymbol(objId)));
        atoms.add(pd::Atom((float)index));
    }
}

sendMCPReply(String("/pd/create_batch_id/reply/" + correlation_id), atoms);
```

**Important:** You need to collect `objectIds[]` and `createdObjects[]` during the creation loop. Look at the existing loop (around lines 2178-2230) — it already iterates through the objects. You need to save the tempId string and the returned `t_gobj*` for each successful creation so you can emit them in the reply.

Currently the code does:
```cpp
mcpStableObjectMap[canvas_symbol.toStdString()][object_id.toStdString()] = targetObj;
```

So `object_id` (the tempId) and `targetObj` (the t_gobj*) are already available in the loop. Just collect them into arrays:

```cpp
// Before the creation loop:
std::vector<std::string> createdIds;
std::vector<t_gobj*> createdPtrs;

// Inside the loop, after successful creation:
createdIds.push_back(object_id.toStdString());
createdPtrs.push_back(targetObj);

// After the loop, in the reply:
for (size_t o = 0; o < createdIds.size(); o++) {
    int index = getObjectIndex(canvas, createdPtrs[o]);
    if (index >= 0) {
        atoms.add(pd::Atom(generateSymbol(createdIds[o])));
        atoms.add(pd::Atom((float)index));
    }
}
```

---

### Node.js Side: Parse the extended reply and skip polling

**File:** `/home/alphi/Desktop/plugdata/mcp-server/src/tools/patching/transaction-v2.ts`  
**Location:** Around line 1244-1322 (the polling loop)

**Current flow:**
```typescript
// 1. Send create_batch_id
const result = await oscClient.sendAndAwaitCorrelated("/pd/create_batch_id", [...], "/pd/create_batch_id/reply", 5000);
// result.args = [createdCount]  ← only a number

// 2. Poll for mappings (the slow loop)
let resolved = false;
for (let attempt = 0; attempt < pollMaxAttempts && !resolved; attempt++) {
    const mappingsReply = await oscClient.sendAndAwaitCorrelated("/pd/get_mappings", [canvas], ...);
    // Parse [tempId, index, tempId, index, ...]
    // Check if all createdTempIds have valid indices
    // If yes: resolved = true; break;
    await sleep(pollIntervalMs);
}
```

**Change to:**
```typescript
// 1. Send create_batch_id
const result = await oscClient.sendAndAwaitCorrelated("/pd/create_batch_id", [...], "/pd/create_batch_id/reply", 5000);

// 2. Parse inline mappings from the reply (NEW — skip polling)
const createdCount = Number(result.args[0]);
const inlineMappings: Record<string, number> = {};
// Reply format: [count, tempId1, index1, tempId2, index2, ...]
for (let i = 1; i + 1 < result.args.length; i += 2) {
    const tempId = String(result.args[i]);
    const index = Number(result.args[i + 1]);
    if (tempId && index >= 0) {
        inlineMappings[tempId] = index;
    }
}

// 3. Apply mappings immediately (same code that previously ran after polling)
for (const [tempId, index] of Object.entries(inlineMappings)) {
    if (createdMap[tempId]) {
        createdMap[tempId].index = index;
    }
    mappings[tempId] = index;
}

// 4. Check if all tempIds resolved
const allResolved = createdTempIds.every(tid => inlineMappings[tid] !== undefined);

// 5. ONLY poll if inline mappings are incomplete (fallback for edge cases)
if (!allResolved) {
    // Keep the existing polling loop as a fallback, but it should rarely trigger
    // ... existing polling code ...
}
```

---

### Capability Advertisement

**File:** `MCPBridge.cpp` in `handleBridgeDomain("capabilities")`

Add to the capabilities reply:
```cpp
reply.addArgument(juce::String("inline_mappings"));
```

**File:** Node.js side — optionally check `oscClient.hasCap("inline_mappings")` before skipping the poll. This makes it backward-compatible with older C++ builds that don't have this feature.

---

## 3. Testing

| Test | How to verify |
|------|---------------|
| Basic create | `construct_patch_v2` with 5 objects. Check `_v2meta.unknownTempIds` is empty. Verify all objects appear on canvas. |
| Speed | Time the transaction. Should be ~50-100ms faster than before (no 25ms × N polls). |
| Connections after create | Create objects + wire them in one transaction. Verify connections succeed (they need valid indices). |
| Edge case: failed create | Create an invalid object (e.g. `[nonexistent_external~]`). Verify it doesn't appear in inline mappings but other objects do. |
| Backward compat | If `inline_mappings` cap is absent, verify the old polling loop still works. |

---

## 4. Files to Modify

| File | What to change |
|------|---------------|
| `plugdata-core/Source/PluginProcessor.cpp` | In `mcp_create_batch_id` handler: collect objectIds + pointers during creation loop, append `[tempId, index]` pairs to reply atoms |
| `mcp-server/src/tools/patching/transaction-v2.ts` | Parse extended reply after `create_batch_id`, apply mappings inline, skip polling if all resolved |
| `plugdata-core/Source/Pd/MCPBridge.cpp` | Add `"inline_mappings"` to capabilities reply |

---

## 5. Risks & Edge Cases

| Risk | Mitigation |
|------|-----------|
| `getObjectIndex` is O(n) per object — could be slow on huge canvases | For a batch of 10 objects on a 200-object canvas: 10 × 200 comparisons = 2000 ops. Still <0.1ms. Only matters at 1000+ objects. |
| Object creation fails mid-batch (some succeed, some fail) | Only append mappings for objects that actually succeeded (check `targetObj != nullptr`). The count field already reflects partial success. |
| Race with GUI synchronization | `synchroniseCanvases()` is called async AFTER the reply. Indices are valid at reply time because sys_lock is still held during index lookup. |
| Subpatch objects may have different index semantics | The existing `getObjectIndex` already handles subpatches — it walks the specific canvas's `gl_list`. |

---

## 6. Expected Result

**Before:** Create 10 objects → reply in 5ms → poll 4-8 times × 25ms = **100-200ms** before connections can fire.

**After:** Create 10 objects → reply in 5ms WITH all indices → connections fire immediately = **~5ms total**.

**Net savings:** 100-500ms per `construct_patch_v2` call that creates objects.

Since `construct_patch_v2` is called on virtually every AI action (build a synth, add effects, create a voice), this compounds into a dramatically faster creative loop.
