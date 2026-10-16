# BUG — `construct_patch_v4`: move+rename silently drops the move; receipts over-report; failed creates leave red-box ghosts

**Date:** 2026-09-10 (live session)
**Branch:** `feat/v4-zero-dropout-copilot`
**Severity:** Medium — silent data loss of a requested edit (move) + misleading receipts. No crash.
**Status:** OPEN (verified live, reproducible)
**Verification method:** independent `analyze_patch(census)` read-back of the live Pd `gl_list` after each mutation (receipts alone are not trustworthy).

---

## Summary

A full `construct_patch_v4` action/guard matrix passed live (create all kinds, edit,
move, rename, connect, disconnect, delete, clear, dspOn, playNote, rate/cycle
pre-guards, silent-killer post-guard). Three receipt/behaviour defects were found by
comparing the C++/TS receipt against an independent census:

1. **Move + rename the same object in one call → the move is silently dropped.**
2. **`moved` / `renamed` counts are *requested*, not *applied*.** A dropped move still
   reports `moved: 1`.
3. **A failed `create` leaves a red-box ghost object on the canvas** (not rolled back).
4. Minor: `reconcile.detectedConnections` is empty on move/rename-only calls.
5. Minor: `connectFailures` "src not found, " has a trailing comma + empty detail.

---

## 1. Move + rename same object in one call → move silently dropped

### Repro (high confidence)

```
construct_patch_v4({ clear:true, create:[{tempId:"vco", type:"osc~", x:300, y:100}] })
construct_patch_v4({ move:[{tempId:"vco", x:300, y:400}] })      // census: vco @ (300,400)  OK
construct_patch_v4({ move:[{tempId:"vco", x:300, y:80}],
                     rename:[{from:"vco", to:"vco2"}] })          // receipt: moved:1, renamed:1
```

**Census after the combined call:** `vco2` still at **(300,400)** — the move did not land.
The rename did. Doing the two operations in separate calls works (census: y=80).

### Root cause

`mcp-server/src/tools/patching/transaction-v4.ts`:

- renames execute first (`:888-895`, `renameIdBatch`)
- moves execute afterwards (`:901-917`), resolving `mv.tempId`

So a move targeting the **pre-rename** tempId no longer resolves → skipped. Worse, the
collision preflight (`:921-928`) fetches bounds **after** the rename, so `mv.tempId`
(old) never matches `selfObj` (new) → the object is treated as its own obstacle and
gets a spurious "auto-nudged … to avoid collision" nudge.

### Suggested fix

- Either execute **moves before renames**, or translate move tempIds through the rename
  map (`newId = renameMap.get(mv.tempId) ?? mv.tempId`) before the bounds check and the
  move OSC batch.
- Report **applied** counts (see #2) and add a warning when a move/rename tempId did not
  resolve.

---

## 2. `moved` / `renamed` are requested counts, not applied

`transaction-v4.ts:1178-1179` and `:1213`:

```ts
moved: moves.length,
renamed: renames.length,
```

These are the number of entries in the request, not the number that landed. Combined
with #1 this produces a receipt that claims success for a mutation that never happened.
`createFailures` / `connectFailures` / `editFailures` / `deleteFailures` are accurate;
move/rename have no equivalent failure channel.

### Suggested fix

Track per-move/per-rename success (the C++ move/rename calls can return success, or
compare census/bounds before/after) and emit `moveFailures` / `renameFailures` +
accurate counts.

---

## 3. Failed `create` leaves a red-box ghost (not rolled back)

### Repro

```
construct_patch_v4({ create:[{tempId:"fake_obj", type:"nope_fake_xyz~", x:900, y:100}] })
```

Receipt: `createFailures:[{tempId:"fake_obj", type:"nope_fake_xyz~", reason:"couldn't
create — a red-box ghost remains on canvas; it is auto-adopted as a gui_* tempId …"}]`

A red-box (uninstantiable) object **remains on the canvas**. On the next call PHASE 0
reports `adopted: 1` — i.e. the ghost becomes a real, selected canvas object. The reason
string is honest, but the artifact is left behind.

### Contrast

The GOP-encapsulate path **does** roll back a failed red box:
`MCPBridge.cpp` GOP handler → `if (redBox) { if (newObj) glist_delete(cnv, newObj); … }`.

### Suggested fix

In `batch_atomic` PHASE 4, after aligning `pendingCreates` to created gobjs, delete any
created object that resolved to a red box (`te_type == T_OBJECT && pd_class == text_class`)
and keep it named in `createFailures`. That makes a failed create a pure no-op.

---

## 4. (Minor) `reconcile.detectedConnections` empty on move/rename-only calls

Move-only / rename-only receipts return `detectedConnections: []`. Census shows the
wires are actually intact — the reconcile wire-scan simply isn't refreshed on those
paths. Could mislead an agent into thinking wires were lost. Consider running the same
PHASE 0 connection scan (or omitting the field) on move/rename-only calls.

---

## 5. (Minor) `connectFailures` "src not found, " trailing comma

`MCPBridge.cpp:2146-2150` builds `"src not found, " + (dg ? "" : "dest not found")`.
When only the source is missing, the reason renders as `"src not found, "` with a
dangling comma and empty detail. Cosmetic; trim before concatenation.

---

## What passed (verified real via census)

| Action | Receipt | Census read-back |
|---|---|---|
| create (obj/msg/floatatom/symbolatom/text) | 18 created | 18 objects, all kinds ✅ |
| edit args | `edited:1` | `mg` `*~ 0.75` (was 0.5) ✅ |
| move | `moved:2` | ff @ (700,20), lbl @ (30,340) ✅ |
| rename | `renamed:1` | `sym_note` present ✅ |
| connect | `connected:1` | 16 → 17 wires ✅ |
| disconnect | `disconnected:1` | 17 → 16 wires ✅ |
| delete | `deleted:1` + `undoReset:true` | 17 objects, target gone ✅ |
| clear | `cleared:true` + `undoReset:true` | 0 objects ✅ |
| dspOn + playNote | `audioVerified:true` | probe -2.5 dB @ 261.6 Hz ✅ |
| R1 rate guard | `connectFailures` named | bad wire absent ✅ |
| R2 cycle guard | `connectFailures` named | bad wire absent ✅ |
| silent-killer X-ray | `zeroed_vcgs:[z1]`, `audioVerified:false` | chokePoint named ✅ |

**Bottom line:** the mutation engine is real (census agrees with receipts for
create/edit/delete/connect/disconnect/clear). The defects are in **move/rename receipt
accuracy + ordering** and **failed-create cleanup**.
