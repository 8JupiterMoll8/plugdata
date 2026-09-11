# PRD — Unified Transactional Undo/Redo Engine for PlugData + MCP

**Status:** Implemented & Verified Live (Stage 1 & Stage 2)  
**Author:** AI Pair Programmer & Jupiter Moll  
**Target:** `plugdata-core` (C++) + `mcp-server` (TypeScript)  
**Depends on:** `/pd/batch_atomic`, `mcpStableObjectMap`, JUCE `Canvas::undo()`

---

## 1. Problem Statement & Root Cause

Pure Data's internal undo implementation (`Libraries/pure-data/src/g_undo.c`) was written in 1996 for single-threaded, manual, single-object GUI edits. It suffers from three fatal architectural flaws when paired with modern AI automation:

1. **Positional Array Indexing Instead of Identities**:
   Pd records undo actions by raw position (`canvas_getindex`), e.g., "object #4". When an atomic batch creates, deletes, or moves multiple objects at once, positional indices desynchronize. The next undo action targets the wrong object or walks off the array.
2. **Destructive Object Re-creation (`UNDO_APPLY`)**:
   To undo properties or positions, Pd calls `canvas_doclear` (destroying the object in C memory) and pastes it anew (`canvas_dopaste`). If the object is a subpatch (`[pd name]`), **it frees the entire subpatch canvas glist**, leaving JUCE's C++ UI components with dangling pointers → **instant segfault / crash**.
3. **Thread Mismatch**:
   MCP mutations run asynchronously and execute on the Audio DSP thread for zero-dropout performance, whereas Pd's `canvas_undo_add` is strictly single-threaded on the scheduler/message thread.

When an artist uses PlugData alongside an AI assistant, pressing `Ctrl+Z` or clicking "Undo" in the toolbar either:
- Segfaults PlugData (if stale entries remain on the stack), or
- Does nothing (if the queue was flushed as a workaround), frustrating the artist's natural muscle memory.

---

## 2. Goals & Success Metrics

- **G1 — Zero Crashes**: Clicking "Undo" in the PlugData GUI toolbar or pressing `Ctrl+Z` must **never** crash PlugData under any circumstances.
- **G2 — AI Actions Are Undoable in GUI**: If the MCP agent builds 5 subpatches, pressing `Ctrl+Z` in the GUI must cleanly undo the AI's action.
- **G3 — Full Redo Parity**: Pressing `Ctrl+Y` (or Redo in the toolbar) must cleanly re-apply the undone action.
- **G4 — Zero Audio Dropouts**: Undoing or redoing must use the audio-thread `batch_atomic` pipeline without stopping DSP or causing audio dropouts.
- **G5 — Identity Continuity**: Semantic IDs (`kick_voice`, `master_gain`) must remain intact across undo/redo cycles.

---

## 3. Comprehensive Evaluation of All 5 Possible Solutions

To solve this fundamentally, there are five architectural paths:

### Option 1: The Hardened Circuit Breaker (Defensive Queue Neutralization)
- **Mechanism**: Every time MCP mutates the canvas (batch_atomic, moves, alignment, layout), execute `canvas_undo_free()` and set `u_queue = u_last = u_cleanstate = nullptr` across all root canvases and subpatches.
- **How it behaves when clicking Undo in GUI**: In `Libraries/pure-data/src/g_undo.c` (line 262), `canvas_undo_undo()` checks `if (udo->u_queue && udo->u_last != udo->u_queue)`. Because the queue is null, Pd exits immediately without touching any memory. Clicking Undo or pressing `Ctrl+Z` becomes an instant, safe **no-op**.
- **Pros**: 100% crash-proof. Zero audio dropout. Solves the emergency crash in 10 minutes with ~15 lines of code.
- **Cons**: Pressing `Ctrl+Z` in the GUI does not undo the AI's action. The user must use MCP tools or prompt to undo.

---

### Option 2: Pure Data C Engine Overhaul (`Libraries/pure-data/src/g_undo.c`)
- **Mechanism**: Rewrite Miller Puckette's 1996 Pure Data undo engine in C. Replace positional array indexing (`canvas_getindex()`) with pointer/stable-ID hashing. Replace destructive `UNDO_APPLY` (`canvas_doclear` + `canvas_dopaste`) with in-place property modification.
- **How it behaves**: Pure Data itself becomes capable of multi-object atomic undo natively.
- **Pros**: Upstream Pd vanilla compatibility; fixes the bug at the lowest possible layer.
- **Cons**: Extremely high risk of regressions across vanilla Pd patches; modifies vendored git submodule (`Libraries/pure-data`); does not solve the thread-safety problem (Pd's undo still assumes single-threaded scheduler access, while MCP executes on the real-time audio thread).

---

### Option 3: Unified C++ Transaction Engine in JUCE (Recommended)
- **Mechanism**: Intercept `Canvas::undo()` and `Canvas::redo()` in JUCE (`Source/Canvas.cpp`). Maintain a bidirectional Transaction Stack (`std::vector<PatchTransaction>`) in `PluginProcessor`. 
  - Every `/pd/batch_atomic` automatically computes an exact mathematical inverse delta:
    - `create(id)` $\leftrightarrow$ `delete(id)`
    - `connect(src, out, dest, in)` $\leftrightarrow$ `disconnect(src, out, dest, in)`
    - `move(id, x, y)` $\leftrightarrow$ `move(id, oldX, oldY)`
    - `edit(id, args)` $\leftrightarrow$ `edit(id, oldArgs)`
  - When the user presses `Ctrl+Z` or clicks "Undo" in the toolbar, `Canvas::undo()` checks if an MCP transaction exists. If yes, it pops the inverse delta and executes it via the audio-thread `batch_atomic` pipeline.
- **Pros**: 
  - 100% crash-proof (no raw C pointer dereferences).
  - Full GUI parity: pressing `Ctrl+Z` or clicking toolbar Undo cleanly rolls back AI creations.
  - Zero audio dropouts: runs on the existing lock-free `batch_atomic` DSP thread.
  - Maintains semantic IDs (`kick_voice`, `master_gain`) intact.
- **Cons**: Requires ~150-200 lines of clean C++ in JUCE / MCPBridge.

---

### Option 4: In-Memory Snapshot Binbuf Stack (RAM Checkpointing)
- **Mechanism**: Before every mutation, serialize the target canvas to a binary buffer in RAM using `canvas_savetofile()`. Store a stack of 10-20 in-memory snapshots. On GUI Undo, clear the canvas and reload from the previous binbuf.
- **How it behaves**: Full rollback of canvas state on `Ctrl+Z`.
- **Pros**: Conceptual simplicity; captures 100% of canvas state including canvas properties.
- **Cons**: Reloading an entire patch (`canvas_doclear` + `binbuf_eval`) briefly suspends DSP, causing an audible pop/click. Destroys stable C++ UI handles.

---

### Option 5: Dual-Stack Event Bridge (Synthetic JUCE Actions)
- **Mechanism**: For every MCP operation, generate a synthetic JUCE UI `UndoableAction` and push it onto JUCE's native `juce::UndoManager`.
- **How it behaves**: Bridges MCP directly into JUCE's undo manager.
- **Pros**: Uses standard JUCE undo patterns.
- **Cons**: PlugData's canvas does not use `juce::UndoManager` for patch objects; it delegates all object operations to `patch.undo()` / Pd's C layer. Subclassing `juce::UndoableAction` for all Pd object types would require rewriting PlugData's canvas architecture.

---

## 4. Architectural Comparison Matrix

| Criteria | Option 1: Circuit Breaker | Option 2: C Engine Rewrite | Option 3: Unified C++ Tx Engine | Option 4: RAM Snapshots | Option 5: JUCE UndoManager |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Crash Safety** | 100% Crash-Proof | Uncertain (Submodule risk) | **100% Crash-Proof** | 100% Crash-Proof | 90% |
| **GUI Ctrl+Z Works?** | ❌ No-Op | ✅ Yes | ✅ **Yes** | ✅ Yes | ✅ Yes |
| **Audio Dropouts** | **Zero** | DSP suspended | **Zero** | ⚠️ Pop/Click on reload | Zero |
| **Submodule Pollution** | **None** | High (`Libraries/pure-data`) | **None** | None | High |
| **Implementation Effort** | **10 Minutes** | Weeks / Fragile | **1-2 Hours** | ~3 Hours | Days |
| **Recommendation** | **Stage 1 (Immediate)** | Reject | **Stage 2 (Permanent)** | Reject | Reject |

---

## 4. The Recommended Architecture: The 2-Stage Strategy

### Stage 1 (Immediate Hotfix — 10 Minutes): Hardened Circuit Breaker
Seal the 3 leak points identified during the live session so that clicking Undo in the GUI is **immediately and permanently crash-proof**:
1. **`Source/Pd/MCPBridge.cpp` (Line 5504)**: Add `resetCanvasUndo(proc, canvasName)` immediately after `canvasComp->alignObjects(alignMode)`.
2. **`Source/Pd/MCPBridge.cpp` (Line 3013)**: Include `|| layoutSanitizedMoved > 0` in the `batch_atomic` reset condition.
3. **`Source/PluginProcessor.cpp` (Line 2253)**: In `mcp_clear_undo`, clear `cnv` AND also clear the root canvas `pd_this->pd_canvaslist` so subpatch mutations never leave root canvas undo records armed.

### Stage 2 (Permanent Feature): The Unified C++ Transaction Engine
Replace Pd's 1996 `g_undo.c` with a modern C++ Transaction Stack directly inside PlugData:

1. **The Transaction Struct in C++ (`Source/Pd/PatchTransaction.h`)**:
```cpp
struct AtomicMutationDelta {
    std::vector<PdCreatePayload> creates;
    std::vector<PdConnectPayload> connects;
    std::vector<PdEditPayload> edits;
    std::vector<PdDeletePayload> deletes;
    std::vector<PdDisconnectPayload> disconnects;
    std::vector<PdMovePayload> moves;
};

struct PatchTransaction {
    juce::String id;
    juce::String canvasName;
    juce::String description;
    AtomicMutationDelta forward;
    AtomicMutationDelta inverse;
};
```

2. **Automatic Inverse Generation in `MCPBridge.cpp`**:
   Before executing `/pd/batch_atomic`:
   - For every `create(id, type, args, x, y)` → inverse generates `delete(id)`.
   - For every `connect(src, out, dest, in)` → inverse generates `disconnect(src, out, dest, in)`.
   - For every `delete(id)` → inverse generates `create(id, originalType, originalArgs, originalX, originalY)` + restores previous connections.
   - For every `move(id, newX, newY)` → inverse generates `move(id, oldX, oldY)`.

3. **Hooking `Canvas::undo()` in `Source/Canvas.cpp`**:
```cpp
void Canvas::undo()
{
    if (processor && processor->hasMcpTransaction(getCanvasSymbol())) {
        processor->popAndExecuteInverseMcpTransaction(getCanvasSymbol());
        synchronise();
        return;
    }

    // Fallback: If no MCP transaction exists, run native safe handler
    patch.undo();
    synchronise();
}
```

---

## 5. Verification Plan

### Automated / C++ Verification
1. **Compilation**:
   `cmake --build /home/alphi/Desktop/plugdata/plugdata-core/build --target plugdata_standalone -j8`
2. **Crash Gauntlet**:
   - Create 5 modular subpatches via `construct_patch_v4`.
   - Run `layout_patch(pillars)`.
   - Run `construct_patch_v4(move)`.
   - Simulate/Trigger GUI `Canvas::undo()` → verify **zero segfaults, zero console errors**.

### Manual Verification with Jupiter
1. Launch PlugData Standalone.
2. Build 5 subpatches and start the 132 BPM audio groove.
3. Click "Undo" in the PlugData GUI toolbar → confirm the audio stays playing and the app does NOT crash.
4. With Stage 2 enabled: Confirm `Ctrl+Z` reverses the last AI build, and `Ctrl+Y` restores it.

---

## 6. Implementation Notes & Dual-Stack Priority

### The Dual-Stack Priority Rule
In a hybrid patching environment where both the AI agent and the human artist interact on the same canvas:
1. **Manual GUI edits always take immediate undo priority**: If the artist manually drags or edits an object after an AI build, pressing `Ctrl+Z` / clicking Undo MUST undo the manual gesture first (`patch.undo()` via `g_undo.c`).
2. **AI rollback follows once manual queue is clean**: Only when the native undo queue has reached its clean state (`udo->u_last == udo->u_queue`) does `Canvas::undo()` pop the C++ transactional stack (`mcpUndoStack`) and execute the inverse batch delta.
3. **Symmetrical Redo**: During Redo, the AI transaction is redone first (recreating the objects in memory) before re-applying manual positions or edits on top.
4. **Synchronous Queue Ground Truth**: `Patch::canUndo()` directly queries Pure Data's C engine queue (`pd::Interface::canUndo(cnv)`) rather than relying exclusively on asynchronous GUI callback flags.
5. **Live Verification Result**: Verified live with Jupiter Moll — zero crashes on subpatch move, single-step move rollback on 1st Undo, AI subpatch deletion on 2nd Undo, and full restoration on Redo.

