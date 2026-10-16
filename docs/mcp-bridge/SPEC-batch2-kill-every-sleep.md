# SPEC — Batch-2: Kill Every `sleep` in `src/` (C++ reply enrichment)

**Goal:** every TypeScript time-fence becomes a C++ handshake. Ordered by artist-facing UX impact — do top-down, ship incrementally, verify live on the 128 BPM beat after each item.

**Standing rule (from the wire fix):** change WHAT runs where, not the architecture. One rebuild + one reopen covers items 1–3 together; verify all in one live session.

---

## 1. Red-box failures in `batch_atomic` reply — kills the 80ms wait (UX: snappier builds, exact failures)

**Artist symptom today:** every build that creates boxes secretly costs +80ms while the harness eavesdrops on the console trying to guess which box broke.

**Change** (`MCPBridge.cpp`, batch PHASE 4 create loop ~line 1100): after each object is placed via `pasteDirect`, detect the red-box state (creation failed but box placed — the path that prints `couldn't create`) and append `{tempId, type}` to the existing `createFailures` vector. Same shape as the current hard-failure entries (`pendingCreates` tail, ~line 1130) — no reply-format break, additive only.

**TS counterpart** (mcp-server, no C++ needed after): `transaction-v4.ts` drops the 80ms wait and the split-print pairing regex to legacy-bridge fallback (no `diagnose`/`batch-facts` caps → old road). Immediate console read stays as belt-and-braces.

**Accept:** fake type ×2 → trip names it with zero added wait; receipt `executionMs` on clean creating builds drops ~80ms; 128 BPM beat unbroken.

## 2. Conditional X-ray in `batch_atomic` reply — kills the extra `getDiagnose` round trip (UX: synth builds return faster, no 3s timeout exposure)

**Artist symptom today:** every synth build pays a second round trip (~10–50ms healthy, 3000ms timeout when the bridge is sick — exactly when it hurts most).

**Change:** when the batch created signal objects (`type` contains `~`), run the existing `/pd/diagnose` logic (handler ~line 2129) inline in the batch lambda and append its facts (`dsp_cycles`, `zeroed_vcgs`, `dangling_main_sig`, `mismatched`) to the reply. Gate on signal-creates only — control-only batches pay nothing. Read-only, same thread, same lock discipline as the standalone handler.

**TS counterpart:** delete the separate `getDiagnose` call on the creating path; keep `analyze_patch(diagnostics)` (explicit user action) untouched.

**Accept:** creating build with bare `*~` names it in the same receipt; round-trip count per synth build drops 2→1; beat unbroken.

## 3. Reply on `/pd/rename_id` — kills the 20ms settle waits (UX: undo/rename feels instant)

**Artist symptom today:** renames and post-undo remaps sprinkle 20ms blind waits; undo occasionally beats the rename and shows stale `gui_*` names.

**Change** (`MCPBridge.cpp:3151`): mirror the `rename_id_batch` pattern directly below it (inline `mcpStableObjectMap` mutation, ~line 3170) — mutate inline and `sendReply("/pd/rename_id/reply/" + correlationId, renamed)`. The `correlationId` arg is already accepted and currently ignored. ~10 lines.

**TS counterpart:** `undo-v4.ts` + post-edit remap await the reply instead of sleeping; drop the 20ms settle.

**Accept:** edit→undo→redo cycle shows zero `gui_*` names with no sleeps; rename latency <5ms in receipt.

## 4. Audit the 9s `/pd/clear` handshake — diagnosis before prescription (UX: rebuilds stall 9s)

**Artist symptom today:** measured twice — backup 2–7ms, clear **9005ms** against a 3000ms timeout. Rebuilds stall while the beat (if running) plays over a dead canvas.

**Not yet specified:** unknown whether the reply is lost, the correlated wait mismatches, or the retry policy doubles the damage (logs show `retrying (1/1)`). Instrument first: timestamp send vs reply-arrival in C++ (`/pd/clear` handler ~line 2611). Fix follows evidence — do NOT bundle blind changes here.

## 5. TS-side companion (no C++): retire the legacy paths

- Migrate v2 mutations onto `batch_atomic` (kills the 25ms appearance-poll loop `transaction-v2.ts:1386`, `CLEAR_SETTLE_DELAY`, `POST_TRANSACTION_SETTLE`).
- Cap `detectedConnections` in v4 receipts (~10 + count; full topology stays one `census` away).
- GOP 200ms registration wait dies with the migration; the 100ms fs wait becomes a real `fsync`.
- Leave alone (correct fences): 2.5s heartbeat, identity/census debounces, retry backoff.

---

## Live verification protocol (after the single rebuild + reopen)

1. Fresh boot, rebuild standard 33-patch, start 128 BPM beat.
2. Fake `~` type ×2 → trip names it, no added wait, `executionMs` down ~80ms vs baseline.
3. Bare `*~` build → named in same receipt, single round trip.
4. Edit → undo → redo → zero `gui_*`, no sleeps.
5. `tap_audio_signal` RMS unchanged throughout; `read_pd_console` zero errors.
6. TempIds stable, undo stack cleared at end.
