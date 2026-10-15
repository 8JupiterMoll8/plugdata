# Bridge Protocol V2 Plan

## Context

This is plan 5 of the seven-plan fix program from the assessment `PlugData-MCP-Server/docs/review/REFACTOR_VS_REWRITE_16938db.md`. The program order and the binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md).

**Prerequisites:** plans 1–4 are applied. In particular, `bridge-restructure-plan.md` provides the command registry, the single execution model and the identity service.

**Scope:** this fork only. Paths are relative to it; TS paths carry a `PlugData-MCP-Server/` prefix.

**Why:** the v1 envelope has structural defects:
1. no version
2. datagram size limits
3. retries that are not idempotent
4. uncorrelated replies
5. five error conventions
6. no authentication

In addition, native undo is flushed after every AI mutation, and the wire format doubles as the undo log.

## Summary

The bridge serves a versioned, typed, authenticated v2 protocol next to v1, and owns one shared undo history. v1 keeps working unchanged until the TS client has moved to v2 (plan 6). The fork's final plan then deletes it.

Default execution profile: risky.

Default capability class: C4.

Estimated cost: about 30 worker runs (3 phases, risky, before repairs).

## Phases

- [ ] **Phase 1: v2 envelope beside v1**
  1. **Canonical schema.** Write the canonical v2 schema at `docs/protocol/v2-schema.json`. The C++ codec is generated from it, or checked against it in tests.
  2. **The envelope:**
     1. a handshake `{protocol, schemaHash, bridgeVersion}`
     2. explicit session ownership, so no client can silently take over the reply port
     3. an id on every reply
     4. typed `error{code, message}` values
     5. typed outcomes: `applied`, `partial`, `rejected` or `unknown`
     6. idempotency keys, with de-duplication for every mutating verb
     7. one timeout policy
  3. **Listener.** Loopback TCP with length-prefixed frames (delegated rule). It accepts loopback connections only, with no LAN mode (decision 2), and requires a token.
  4. **v1 stays untouched** on its existing port.

  Report: F4, F5, F6, §4.2.

- [ ] **Phase 2: Every verb over v2**
  1. **Typed v2 arguments.** Every registry verb is reachable over v2 with typed arguments. That means no positional parsing and no float32 coercion of replies (`Source/Pd/MCPBridge.cpp:12645-12646`).
  2. **v1 adapter.** v1 is served by an adapter onto the same handlers.
  3. **Typed undo log.** The undo log stores typed inverse commands instead of `OSCMessage`s (`MCPBridge.cpp:5245`).
  4. **Orphans.** Clean up the orphan messages listed in the v1 wire schema.

  Report: F4, §4.2, §4.4.

- [ ] **Phase 3: Shared undo history**
  1. **One undo history** across AI and manual edits, in chronological order. Each AI tool call is one undo step (decision 9).
  2. **No wholesale flush.** Stop flushing native undo after MCP mutations (`MCPBridge.cpp:5226-5240`). Phase review chooses the mechanism: register MCP mutations as proper undo actions, or keep one unified stack.
  3. **v2 undo.** Undo and redo are single idempotent v2 requests.
  4. **v1 undo** keeps its current behaviour until the v1-retirement plan.

  Report: §4.2, §4.3.

## Interfaces And Internal Boundaries

1. **Wire contract.** `docs/protocol/v2-schema.json` is the canonical v2 wire contract. The TS repo vendors a hash-checked copy in plan 6, so the schema freezes when this plan lands; later changes must stay compatible.
2. **v1 compatibility.** v1 stays fully compatible with `PlugData-MCP-Server/docs/protocol/v1-wire-schema.json`, so the current TS `main` keeps working.
3. **Old sessions.** Identity sidecars and saved patches from before this plan still load.

## Test Plan

1. **Codec:**
   1. C++ round-trip tests generated from the schema
   2. a census reply larger than 64 KB
   3. replay of each idempotency key
   4. lost-reply simulation for every mutating verb
2. **v2 smoke tests.** A minimal v2 test client in the C++ test target exercises every verb.
3. **v1 regression.** Golden replay through the TS repo's replay CLI must stay identical over v1.
4. **Undo.** Interleaved AI and manual edit scenarios via the v2 test client, with no heap corruption under AddressSanitizer.
5. **Live.** The `PlugData-MCP-Server/DEV.md` standard protocol over v1, driven by the TS server from the TS repo's main checkout, after every phase.

## Assumptions

The binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md). This plan relies on decisions 2, 3 and 9.

**Assumption:** loopback TCP works on macOS and Linux without firewall prompts.

**Autonomy rules (decisions 13–15, 18):**
1. **Runner.** Execute with `$implement-phased-plan` in this repo's worktree pool. Create no other branches. The owner lands the plan with the apply command the runner prints.
2. **Remotes.** Never write to any remote. Read-only fetches are allowed.
3. **Live checks.**
   1. Quit any running PlugData and MCP server first.
   2. Build the worktree with CMake and Ninja (`-DENABLE_GEM=ON`, target `plugdata_standalone`).
   3. Drive it with the TS server from the TS repo's main checkout.
   4. Verify by measurement, not by ear.
   5. Run only one PlugData at a time.
4. **The other repo.** Use the TS repo read-only, and write outputs outside both repos.
5. **Dev build.** After the final phase passes, copy the worktree's verified `Plugins/Standalone/plugdata.app` to `~/Documents/plugdata/mcp-dev-build/plugdata.app`.
6. **Blockers.** If a step needs the owner, skip it, continue with independent work, and list it in the final report.
