# Fork Stabilize Plan

## Context

Source: the assessment `PlugData-MCP-Server/docs/review/REFACTOR_VS_REWRITE_16938db.md` in the sibling TS repo (`../PlugData-MCP-Server`). The verdict is hybrid and refactor-led, with no spec-first rewrite.

**Program.** Seven plans across two repos, run strictly in this order. Each plan starts only after the previous one has been applied.

1. **Fork: `docs/plans/fork-stabilize-plan.md` (this plan)**
2. TS: `docs/plans/stabilize-and-baseline-plan.md`
3. TS: `docs/plans/ts-consolidation-plan.md`
4. Fork: `docs/plans/bridge-restructure-plan.md`
5. Fork: `docs/plans/bridge-protocol-v2-plan.md`
6. TS: `docs/plans/ts-protocol-v2-and-release-plan.md`
7. Fork: `docs/plans/bridge-v1-retirement-plan.md`

**Repos.**
1. **This fork:** branch `feat/multimodal-studio-canvas`. Paths are relative to it.
2. **TS server:** the sibling `../PlugData-MCP-Server`. Its paths are written with a `PlugData-MCP-Server/` prefix.

This plan changes only this repo.

**What this plan does:**
1. Makes the fork rebuildable from a fresh clone.
2. Closes the bridge's network exposure and lifetime bugs.
3. Installs the dev build the TS plans test against.

Issues are tagged per phase as `Report: F<n>` (finding) or `§<n>` (section) of the assessment.

**Resolved owner decisions (2026-10-16).** These bind all seven plans. An identical copy lives in the TS repo's `docs/plans/stabilize-and-baseline-plan.md`. No phase may reopen them; a phase that finds one impossible reports it under decision 18.

1. `approvalToken:"auto"` is a bug. It never approves outside the env-gated test path.
2. The bridge listens on loopback only. There is no LAN mode.
3. No OSC clients exist besides the MCP server. v1 is retired outright once the TS client speaks v2.
4. The minimum supported bridge is the one built by this program. Older bridges and the Lua bridge are dropped.
5. `POST /api/tool/:name` exposes public tools only.
6. Every current feature survives the upstream move, including overlays, sketch, voice/MIDI capture and the Copilot panel.
7. "Audible" means a peak above −60 dBFS.
8. Jam mode always shows failures, as one compact line.
9. Ctrl+Z follows one chronological history across AI and manual edits. Each AI tool call is one undo step.
10. If no dropout-free method exists for heavy-abstraction creates in Compiled Mode, keep the brief DSP-off workaround and report it in the receipt.
11. Record and render write to `~/Documents/plugdata/mcp-renders`. The folder is configurable.
12. The macOS and Linux standalone builds must stay green. The agent verifies macOS; Linux is checked when the owner pushes.
13. The agent may build, launch, restart and drive PlugData on this Mac.
14. Each plan runs in its repo's worktree pool. The owner lands it with the one apply command the runner prints, and the next plan starts only after that.
15. Never write to any remote: no push, no remote repositories, no upstream pull requests or issues. Read-only fetches from upstream and submodule remotes are allowed.
16. Fork submodule commits are kept as patch files in this fork, applied by a script. No new repositories are created.
17. The program runs with its planned profiles, about 202 worker runs before repairs.
18. Blockers that need the owner are skipped and reported at the end, while independent work continues.

**Technical choices delegated to the agent, with these rules:**

1. **Upstream method.** Choose between merging and replaying onto fresh upstream; the option that leaves fewer hunks to re-resolve wins.
2. **v2 framing.** Use loopback TCP with length-prefixed frames, unless phase review finds a blocker.
3. **Bridge internals.** The executor design, registry layout and identity-service internals are decided in phase review.
4. **Test runner.** Keep plain `tsx` with `node:assert`, unless review shows it cannot do the job.

## Summary

Make the fork reproducible and close its urgent safety bugs, without changing its architecture.

Default execution profile: review-heavy.

Default capability class: C3.

Estimated cost: about 18 worker runs (2 phases: one review-heavy, one risky override; before repairs).

## Phases

- [ ] **Phase 1: Reproducibility and repo hygiene**
  1. **Export the submodule commits.** Export every fork-only submodule commit (pure-data, pd-else, pd-lua, JUCE, pd-iemguts, plus any other changed submodule) as patches under `patches/submodules/`. Six of the twelve are currently missing there.
  2. **Patch script.** Add `scripts/apply-submodule-patches.sh`, which applies the patches idempotently on top of the pinned commits.
  3. **Upstream pins.** Pin each patched submodule to the upstream commit its patches sit on, so a fresh clone can fetch every submodule.
  4. **Keep the main checkout clean** for the worktree pool:
     1. Patched submodules must not show as modified in `git status`, for example via `ignore = dirty` in `.gitmodules`.
     2. Update `scripts/setup-parallel-worktree.sh` to reset submodule working trees before updating, then apply the patches.
  5. **Fresh-clone build.** Prove that a fresh local clone builds the standalone app using only the upstream submodules plus the patch script.
  6. **Owner follow-up.** The final report prints the commands the owner runs once in the main checkout after applying the plan.
  7. **Line endings.** Convert the 20 files still in CRLF to LF, matching upstream (`39edb767`). Do it in a whitespace-only commit.
  8. **Hygiene.**
     1. Move planning docs out of `Source/Pd/` and the repo root into `docs/planning/`.
     2. Delete the committed `Resources/Scripts/__pycache__/*.pyc` and the stale patch snapshots (`patches/mcp_native_bridge.patch` and its siblings).
     3. Delete `Source/Pd/ai_midi.c` and `Source/Pd/ai_voice.c` if the build does not compile them.
     4. Correct stale statuses: `BUG-v4-move-rename-receipt-and-create-ghost.md`, and PRDs marked "PROPOSED" for shipped code.

  Report: F10, §4.1, §4.4.

- [ ] **Phase 2: Bridge exposure and lifetime fixes**
  Override: risky
  Capability override: C4
  1. **Exposure.**
     1. Bind the OSC receiver to 127.0.0.1, and accept `/bridge/connect` targets on loopback only (decision 2) (`Source/Pd/MCPBridge.cpp:353`, `:10059-10066`).
     2. Restrict `/pd/record_start` and `/pd/render` to `~/Documents/plugdata/mcp-renders` by default, configurable (decision 11) (`MCPBridge.cpp:5628-5638`, `:5683-5700`).
  2. **Lambda lifetimes.**
     1. Give the `batch_atomic` and layout lambdas owned, reference-counted state, so a timeout can never leave them pointing at a dead stack frame (`MCPBridge.cpp:4412-4419`, `:5112-5124`, `:5155-5159`).
     2. Serialize `OSCSender` access, and skip reconnects when the target is unchanged.
  3. **Audio-callback I/O.** Move recorder-tap file creation and disk writes off the audio callback (`Source/PluginProcessor.cpp:1254-1286`).
  4. **Small fixes.**
     1. Fix the self-comparing identity serial check (`PluginProcessor.cpp:2124-2137`).
     2. Correct the `batch_atomic` reply-format comment (`MCPBridge.cpp:4151`).

  Report: F3, F4, F6.

## Interfaces And Internal Boundaries

1. **OSC v1 wire format.** It stays message-compatible. Phase 2 changes who may connect and where replies and files may go, not message shapes. The current TS `main` must keep working against this build.
2. **Identity.** The sidecar format (`.mcpids.json`) and identity ownership in C++ are unchanged.
3. **Upstream.** This plan aligns line endings only. The catch-up merge is in `bridge-restructure-plan.md`.
4. **Dev build.** The app at `~/Documents/plugdata/mcp-dev-build/plugdata.app` becomes the build every TS plan tests against. This plan installs it first.

## Test Plan

1. **Build:**
   1. a clean-clone macOS build after Phase 1
   2. a worktree build after Phase 2
2. **Live:** the `PlugData-MCP-Server/DEV.md` standard test patch and protocol (`DEV.md:58-87`), driven by the TS server started from the TS repo's main checkout, after both phases. Phase 2 adds three more checks:
   1. a forced audio-thread timeout that must not crash
   2. a non-loopback connection attempt that must be refused
   3. a record/render path outside the allowed folder that must be refused
3. **Dev build install:** the verified app is installed at `~/Documents/plugdata/mcp-dev-build/plugdata.app`, and it launches.

## Assumptions

The owner decisions in Context are settled facts, not assumptions.

**Assumption:** a local PlugData build toolchain (CMake, Ninja, Xcode command-line tools) is available on this Mac.

**Autonomy rules (decisions 13–15, 18):**
1. **Runner.** Execute with `$implement-phased-plan` in this repo's worktree pool; `scripts/setup-parallel-worktree.sh` prepares the submodules. Create no other branches. The owner lands the plan with the apply command the runner prints.
2. **Remotes.** Never write to any remote. Read-only fetches are allowed.
3. **Live checks.**
   1. Quit any running PlugData and MCP server first.
   2. Build the worktree with CMake and Ninja (`-DENABLE_GEM=ON`, target `plugdata_standalone`). The app lands at `Plugins/Standalone/plugdata.app` inside the worktree.
   3. Drive it with the TS server from the TS repo's main checkout.
   4. Verify by measurement, not by ear.
   5. Run only one PlugData at a time.
4. **The other repo.** Use the TS repo read-only: run its server and scripts from its main checkout, never change it, and write outputs outside both repos.
5. **Dev build.** After the final phase passes, copy the worktree's verified `Plugins/Standalone/plugdata.app` to `~/Documents/plugdata/mcp-dev-build/plugdata.app`, replacing the previous one.
6. **Blockers.** If a step needs the owner, skip it, continue with independent work, and list it in the final report.
