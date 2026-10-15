# Bridge Restructure Plan

## Context

This is plan 4 of the seven-plan fix program from the assessment `PlugData-MCP-Server/docs/review/REFACTOR_VS_REWRITE_16938db.md`. The program order and the binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md).

**Prerequisites:** plans 1–3 are applied. That gives:
1. a reproducible fork
2. the dev build
3. golden traces, the OSC-level fake bridge and the replay CLI in the TS repo
4. the v1 wire schema at `PlugData-MCP-Server/docs/protocol/v1-wire-schema.json`
5. a consolidated TS server

**Scope:** this fork only. Paths are relative to it; TS paths carry a `PlugData-MCP-Server/` prefix.

**Why:** the assessment found three problems:
1. The fork sits 845+ upstream commits behind, with 214 interleaved hunks in 11 core upstream files.
2. The bridge lives in one ~6,435-line function with no tests.
3. Its execution model has thread-ownership defects in every handler.

The chosen path is "restructure in place, then re-host". Domain algorithms are ported as they are; there is no clean-room rewrite.

## Summary

The end state:
1. The fork sits on current upstream behind a small hook patch series.
2. The bridge is a command registry with one execution model, realtime-safe probes, and one identity service.

Every phase keeps the v1 wire format compatible, so the current TS `main` keeps working.

Default execution profile: risky.

Default capability class: C4.

Estimated cost: about 48 worker runs (5 phases: four risky, one review-heavy override; before repairs).

## Phases

- [ ] **Phase 1: Upstream catch-up**
  1. **Bring the fork onto current upstream `develop`** (845+ commits; 36 conflicting paths, 24 ignoring whitespace). This includes:
     1. the submodule bumps, with the fork's patch files re-based onto the new upstream pins
     2. replacing the deleted `Source/Objects/AllGuis.h` dependency (`Source/Pd/MCPBridge.cpp:17`)
  2. **Method.** Choose between merging and replaying onto fresh upstream, using the delegated rule: fewer re-resolved hunks wins.
  3. **Keep the macOS and Linux standalone builds green** (decision 12).
  4. **Keep every current feature working** (decision 6).

  Report: F10, §4.5.

- [ ] **Phase 2: Host seam and local patch series**
  1. **Hooks.** Move the interleaved hunks in core upstream files (98 in `Source/Canvas.cpp`) behind a small hook set:
     1. an audio-block tap
     2. a Pd-thread command entry point
     3. a save callback
     4. telemetry
     5. an overlay paint hook
  2. **Decouple the bridge from `PluginProcessor`.**
     1. Move MCP state out of `Source/PluginProcessor.h:471-735`.
     2. End the `PluginProcessor`/`MCPBridge` mutual friendship (`Source/Pd/MCPBridge.h:298-299`).
     3. Remove the bridge's GUI-header includes.
  3. **Upstream behaviour changes.** Restore or justify the ones the fork made:
     1. `setConnectionPath` no longer records undo (`Source/Pd/Interface.h:377`)
     2. the hand-rolled `glist_delete` (`Interface.h:244-309`)
  4. **Pd core patches.** Carry the `obj_disconnect` `oc2` fix and `dsp_update_deferred` in the local pure-data patch set, separate from the bridge hooks. Nothing is submitted upstream (decision 15).

  Report: F10, §4.1, §4.5.

- [ ] **Phase 3: Bridge command registry**
  Override: review-heavy
  Capability override: C3
  1. **Registry.** Split `handlePdDomain` (`MCPBridge.cpp:3136-9570`) and the second dispatch layer in `PluginProcessor::receiveSysMessage` into a command registry with one handler per verb, grouped in per-domain files.
  2. **Unknown verbs** get an explicit error reply.
  3. **Clean-up.**
     1. Remove duplicate branch labels.
     2. Remove the legacy `*_id` mutation path wherever `batch_atomic` covers it, and the TS repo does not use it.
  4. **Tests.** Add a C++ test target that drives handlers through the registry.

  Report: F2, §4.1.

- [ ] **Phase 4: Single execution model and realtime safety**
  1. **One owner context.** All Pd mutations run in one context under the Pd lock.
     1. The OSC thread only decodes and enqueues.
     2. The function queue is never drained without the lock (`Source/PluginProcessor.cpp:2405-2407`, `:2454`, `:2625`, `:1845`).
     3. One reply queue owns `OSCSender`.
     4. No multi-second waits happen on the network thread.
  2. **Realtime hygiene.**
     1. `midiInputTick` takes no locks, allocates nothing and makes no `callAsync` calls (`MCPBridge.cpp:10795-10858`).
     2. No seqLock is held across `sendReply`.
     3. Probes and taps use validated handles, re-resolved after delete or DSP recompile (`MCPBridge.h:136`, `MCPBridge.cpp:11657-11660`, `PluginProcessor.cpp:1080`).
     4. Fix the teardown races (`PluginProcessor.cpp:876`, and detached threads that capture `this`).
     5. Stop interning census JSON and console lines as permanent Pd symbols.
  3. **Open bug #83.** Reproduce it or close it (armed capture reads silence).

  Report: F3, §4.2, §4.3.

- [ ] **Phase 5: Identity service**
  1. **One minter.** C++ becomes the single tempId minter, replacing the four C++ minting sites (`MCPBridge.h:86-121`, `PluginProcessor.cpp:2097`, `:2210`, `:3227`).
  2. **Versioning.** Give each canvas its own identity version, bumped by GUI edits too. The v1 reply shape is kept.
  3. **Sidecars.** Validate them on load (`MCPBridge.cpp:2538-2559`), and write them so a crash cannot leave a mismatched pair.
  4. **Resolution and locking.**
     1. Apply strict canvas resolution everywhere, with no first-canvas fallback.
     2. Lock the identity map against UI-thread mutation (`Source/Canvas.cpp:2402-2404`).
     3. Check adoption for name collisions (`PluginProcessor.cpp:2101`).

  Report: §4.1, §4.2, §4.3.

## Interfaces And Internal Boundaries

1. **OSC v1 wire format.** It stays compatible with `PlugData-MCP-Server/docs/protocol/v1-wire-schema.json` throughout. Additions only: error replies for unknown verbs, and per-canvas identity values in the existing reply fields.
2. **Identity sidecars.** The `.mcpids.json` format stays readable. Any format change needs a loader that accepts the old format.
3. **Host seam.** The seam is the only allowed coupling between the bridge and upstream PlugData. New fork changes to upstream files go through it.
4. **Ownership.** C++ owns identity and execution; the TS mirror adapts in the TS v2 plan.

## Test Plan

1. **Golden replay.** After every phase, replay the goldens against this worktree's build with the TS repo's replay CLI.
   1. Byte-identical after Phase 3.
   2. Semantically identical after Phases 1, 2, 4 and 5.
2. **C++ unit tests.** From Phase 3, the registry test target runs as part of verification.
3. **Sanitizer stress runs** after Phase 4:
   1. ThreadSanitizer and AddressSanitizer builds
   2. a forced audio-thread stall
   3. delete during DSP
   4. probe-then-delete
4. **Identity chaos tests** after Phase 5:
   1. #38: a create steals an existing tempId
   2. #62: competing canvas keys
   3. manual GUI edits detected by `sync`
5. **Live.** Run the `PlugData-MCP-Server/DEV.md` standard test patch and protocol after every phase, driven by the TS server from the TS repo's main checkout: zero dropouts, zero console errors, stable tempIds.

## Assumptions

The binding owner decisions are recorded in [`fork-stabilize-plan.md`](fork-stabilize-plan.md). This plan relies on decisions 6, 12, 15 and 16.

**Assumptions:**
1. **Hooks.** The hooks are carried as a local patch series. Upstream acceptance is not required.
2. **Sanitizer builds.** A sanitizer-enabled debug build of PlugData is feasible on macOS.

**Autonomy rules (decisions 13–15, 18):**
1. **Runner.** Execute with `$implement-phased-plan` in this repo's worktree pool. Create no other branches. The owner lands the plan with the apply command the runner prints.
2. **Remotes.** Never write to any remote. Read-only fetches, including the upstream fetch for Phase 1, are allowed.
3. **Live checks.**
   1. Quit any running PlugData and MCP server first.
   2. Build the worktree with CMake and Ninja (`-DENABLE_GEM=ON`, target `plugdata_standalone`).
   3. Drive it with the TS server from the TS repo's main checkout.
   4. Verify by measurement, not by ear.
   5. Run only one PlugData at a time.
4. **The other repo.** Use the TS repo read-only, and write outputs outside both repos.
5. **Dev build.** After the final phase passes, copy the worktree's verified `Plugins/Standalone/plugdata.app` to `~/Documents/plugdata/mcp-dev-build/plugdata.app`.
6. **Blockers.** If a step needs the owner, skip it, continue with independent work, and list it in the final report.
