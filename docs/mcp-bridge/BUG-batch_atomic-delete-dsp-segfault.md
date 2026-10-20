# BUG — Segfault: `batch_atomic` delete of a live object while DSP runs

**Date:** 2026-09-03 (live session, ~23:50 local)
**Bridge:** `feat/v4-zero-dropout-copilot`, incl. `1f0c7bd0f` (wire safety fix)
**Severity:** Crash (PlugData process gone, bridge timeout, canvas lost if unsaved)
**Status:** VERIFIED FIXED (2026-09-04 live session) — `Interface::removeObjectsAudioThread` + batch PHASE 1 reroute. Live tested deleting control and wired signal objects mid-playback (-17 dBFS RMS tone); 2ms-18ms execution, 0 errors, zero audio dropouts, clean DSP toggle.
**Related:** earlier same-session crash after a `nope_fake_xyz~` fake-create timeout (unknown `~`-type create path — possibly the same lifetime class, unconfirmed)

---

## Repro (high confidence)

1. Canvas: 33-object drum patch (kick/hh/snare → `throw~`/`catch~` bus → `hip~` → `clip~` → `dac~`), DSP on.
2. 128 BPM drum pattern running (`play_drum_pattern`, kick/hh/snare receivers firing).
3. Mid-beat: `construct_patch_v4({ delete: ["live_probe_tag"] })` where `live_probe_tag` is a live `[print]` control object created minutes earlier via the same path.
4. Bridge died mid-call: tool result `aborted`, `/pd/batch_atomic` no reply, `verify_connection` → timeout, PlugData PID gone. Fresh launch shows empty canvas.

Control case: the identical session performed ~15 `create`/`move`/`probe`/`tap`/`load` ops mid-beat with zero dropouts (RMS 36→38 dBFS unbroken). The single `delete` immediately preceded the crash.

## Suspected mechanism

Same use-after-free class as the curved-wire crash fixed in `1f0c7bd0f` (`setConnectionPath` in-place update): the `batch_atomic` delete path likely destroys the `t_gobj` while the running DSP graph still references it. The wire fix covered connect/disconnect; object destruction appears unprotected. Untouched: adds, moves, passive metering, save/load — all survived DSP-on use all session.

## Suggested hardening (C++ side)

1. Delete path: unlink + defer actual free until the next safe point (same pattern as the wire fix — never destroy under a live graph reference).
2. Audit the unknown-`~`-type create path (crash #1 correlation): a red-box `~` object may register a broken DSP node.
3. Consider a `/pd/batch_atomic` dry-run or pre-flight validity check that cannot take the process down — a failed mutation should be a receipt, never a segfault.

## Second symptom (same night): GUI window destroyed, engine alive

- After a DSP-off toggle with the beat job running: PlugData window disappeared, but PID survived, bridge ping 1ms, full 33-object canvas intact.
- Distinct from the segfaults (PID death). Suspect: GUI thread walking the corrupted undo stack (`canvas_undo_free: unsupported undo command 1281530090` was already in the console buffer — garbage command int = heap/stack corruption predating the visible symptom), detonated by editor/menu refresh during the DSP state change.
- Corroborates the heap-corruption theory over a single delete-path bug: something earlier (pre-fix audio-thread `undo_add`? red-box `~` create?) poisoned shared state; later operations trip over it.

## Harness-side mitigations already in place (no C++ change needed)

- Emergency pre-clear backup restored the full 33-object patch (+ semantic names via `.ids.json` stash) in ~10s after relaunch.
- Rule going forward: no `delete`/`disconnect` with DSP on until the above lands.

## Evidence retained

- `mcp-server/patches/.emergency_pre_clear_main_2026-09-03T20-08-28.pd` (+ `.ids.json`) — pre-crash canvas, restored.
- MCP receipts: mid-beat create 22ms clean → delete call `aborted` → ping timeout → fresh PID, empty canvas.
