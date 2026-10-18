# MCP bridge threading contract

Plan 3 P3.4 (2026-10-18) gave the bridge one execution model. This is the contract for every new handler, hook
or posted job. The reasons and the measurements behind each rule are in the TS repo,
`docs/plans/p3.4-steps.md` (Design and Status).

## Contexts

| Context | Thread | Entry | Allowed |
|---|---|---|---|
| Net | JUCE OSC receiver thread | `MCPBridge::oscMessageReceived` | decode and push a copy to the command worker. Nothing else. |
| Worker | the bridge's command worker (`mcp::SerialWorker`, `MCPBridgeWorker.h`) | the command table (`MCPBridgeVerbs.inc`) | runs handlers one at a time in arrival order; reads and non-structural Pd changes under `sys_lock`; may wait. |
| Pd | the **message thread** under `sys_lock` | `runInPdContext`, `McpHost::enqueuePd` / `runPd` / `pdWait` | every STRUCTURAL edit: create, delete, connect, disconnect, retype, paste, load. plugdata edits Pd here itself. |
| Gui | the message thread | `McpHost::onMessageThread(fn, timeoutMs, where)`, `postMessage` | editors, components, viewport, repaint. |
| Audio | the audio callback | the `MCP-HOOK` lines in `PluginProcessor.cpp` -> `McpAudio.cpp` | no blocking lock, no allocation, no `callAsync`; Pd state only under a `ScopedTryLock` on the Pd lock. |
| Jobs | `MCPBridge::jobPool` | offline render, voice analysis | joined by `MCPBridge::shutdown`; stop on `isShuttingDown()`. |

## Rules

1. **Structural edits run in the Pd context.** A handler that creates, deletes, connects, retypes, pastes or loads
   starts with `if (runInPdContext(&MCPBridge::<self>, action, msg)) return;`. Reads and parameter sets may stay on
   the worker, under `sys_lock`.
2. **Never wait for another context while holding `sys_lock`.** Fetch GUI facts first (`mcpFetchGuiFacts`), then
   lock. `onMessageThread` logs `MCP_AUDIT wait-under-lock` when the audit switch is on.
3. **Waits use `mcp::OneShotHandoff`** (`MCPBridgeHandoff.h`): a job that has not begun when the caller gives up
   never runs, so it can never touch the caller's freed frame. A started job is always waited for.
4. **Posted work revalidates its pointers.** Anything that crosses a hop with a `t_canvas*` starts with
   `if (mcpCanvasGone(cnv)) return;`; objects are looked up again by tempId (`resolveStableId` checks `gl_list`).
   Use `MCPBridge::postMessage` / `McpHost::postMessage` (weak-reference guarded), never a bare `callAsync`.
5. **Replies go through `sender`** (`ReplyQueue`, one sender thread). Any thread may post a reply; the audio thread
   never does. Do not hold a lock while sending.
6. **GUI only on the message thread.** No `getEditors()` off it; `getCanvasBySymbol("main")` reads the focused root
   the message thread publishes (`refreshFocusedRoot`).
7. **No per-request Pd symbols.** Correlation ids, canvas names and JSON travel as strings (`McpHost::callSys` with
   a `mcp::SysList`); `gensym` is for user patch names and fixed vocabulary only.
8. **Probes and taps hold a topology epoch** (`ugen_getsortno()` at resolve): the audio thread reads a pointer only
   while the epoch is current and DSP is on.
9. **No Pd `post()` outside the Pd context.** plugdata's print hook (console buffer, the MCP console tap's
   `gensym`) is not thread-safe; from the worker or a timer use `mcpLogLine` (`MCPBridgeCmdCommon.h`: the console
   queue plus the same `/pd/log` OSC line). Found by TSan in S11.
10. **Lock order: the Pd lock before `mcpOverlayLock`** (the GUI's sync and paint take them in that order). Build
   overlay data under the Pd lock, swap it in under the overlay lock.
11. **An MCP mutation empties Pd's native undo queue through `resetNativeUndo`** (free, then `canvas_undo_init`):
   the UNDO_INIT head must stay (finding L6).
12. **Identity state only through `McpHost::identity`, only under the Pd lock** (P3.5). Take `McpHost::PdLockScope`
   (recursive) around any identity access; `getStableId`, `getOrAdoptStableId`, `resolveStableId` and
   `reconcileIdentity` take it themselves. Never hold a reference into an id map past the lock. Only
   `IdentityCore::version()` is lock-free (atomic). With `PLUGDATA_MCP_AUDIT=1` every unlocked access logs
   `MCP_AUDIT identity-unlocked <where> role=... at <frames>`.

## Test tools

- `PLUGDATA_MCP_AUDIT=1`: lock-hold timing per command and thread role, `wait-under-lock` reports, the symbol count in
  `/pd/perf`, and the test-only verb `/pd/debug_stall_audio <ms> <corrId>` (the next audio block holds the Pd lock
  and sleeps).
- `-DMCP_RT_GUARD=ON`: counts allocations and blocking locks inside the audio hooks (`rtAllocs`, `rtLocks` in
  `/pd/perf`).
- `-DMCP_BRIDGE_TESTS=ON`: `mcp_bridge_tests`, real handlers on a headless processor
  (`scripts/run-mcp-bridge-tests.sh`).
- TSan: launch with `LD_PRELOAD=<tsan_clockwait_shim.so>` (`Tests/mcp/tsan_clockwait_shim.c`; GCC 11's libtsan does
  not intercept `pthread_cond_clockwait`) and `suppressions=Tests/mcp/tsan.supp`, under `setarch x86_64 -R`.
- Sanitizer clones and the live stress, stall and race scripts: `scripts/live/p34-*.ts` and `san-triage.py` in the
  TS repo.
