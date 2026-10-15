# Bridge V1 Retirement Plan

## Context

This is plan 7, the last of the seven-plan fix program from the assessment `PlugData-MCP-Server/docs/review/REFACTOR_VS_REWRITE_16938db.md`. The program order and the binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md).

**Prerequisites:** plans 1–6 are applied. In particular, the TS server now speaks only v2 (`PlugData-MCP-Server/docs/plans/ts-protocol-v2-and-release-plan.md`) and provides the automated live release gate.

**Scope:** this fork only.

**Why:** v1 can only be deleted from the bridge after the TS client has moved to v2. Decision 3 says no other OSC clients exist.

This is a single-phase plan, kept in the program so the whole sequence runs under one runner.

## Summary

Delete v1 from the bridge, then pass the full release gate on the final build.

Default execution profile: lean.

Default capability class: C3.

Estimated cost: about 6 worker runs (1 phase, lean, before repairs).

## Phases

- [ ] **Phase 1: v1 retirement and final qualification**
  1. **Delete v1 from the bridge:**
     1. v1 decoding and the v1 adapter
     2. the remaining positional parsing
     3. the v1 capability-flag forks
     4. the Lua-parity shims
  2. **Reject v1 clients** with a clear error naming the required protocol version.
  3. **Release gate.** Run the TS repo's release gate against this build, from the TS repo's main checkout.
  4. **Dev build.** Install the verified app as the dev build.

  Report: F4, §4.2.

## Interfaces And Internal Boundaries

1. **Wire contract.** `docs/protocol/v2-schema.json` is the only wire contract left. The TS repo's vendored copy must have the same hash.
2. **Persisted data.** Identity sidecars and saved patches still load.

## Test Plan

1. **C++ tests.** The registry and codec test targets pass.
2. **v1 rejection.** A v1 message gets the clear rejection error.
3. **Release gate.** The TS repo's release gate passes against this build.
4. **Live.** The `PlugData-MCP-Server/DEV.md` standard protocol passes: zero dropouts, zero console errors, stable tempIds.

## Assumptions

The binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md). This plan relies on decision 3.

**Autonomy rules (decisions 13–15, 18):**
1. **Runner.** Execute with `$implement-phased-plan` in this repo's worktree pool. Create no other branches. The owner lands the plan with the apply command the runner prints.
2. **Remotes.** Never write to any remote. Read-only fetches are allowed.
3. **Live checks.**
   1. Quit any running PlugData and MCP server first.
   2. Build the worktree with CMake and Ninja (`-DENABLE_GEM=ON`, target `plugdata_standalone`).
   3. Drive it with the TS server from the TS repo's main checkout.
   4. Verify by measurement, not by ear.
4. **The other repo.** Use the TS repo read-only, and write outputs outside both repos.
5. **Dev build.** After the phase passes, copy the worktree's verified `Plugins/Standalone/plugdata.app` to `~/Documents/plugdata/mcp-dev-build/plugdata.app`.
6. **Blockers.** If a step needs the owner, skip it, continue with independent work, and list it in the final report.
