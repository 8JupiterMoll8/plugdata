# RESUME — Zero-Dropout Live DSP Hot-Patching

> Handoff note to resume this feature in a new session. Read this first.

## Branches (both based on `feat/native-cpp-bridge`)
- `plugdata-core` → **`feat/zero-dropout-hot-patching`**
- `mcp-server`   → **`feat/zero-dropout-hot-patching`**

(Neither is pushed yet; push when ready.)

## What this feature is
When the AI (or an artist) adds/removes/rewires signal objects (`~`) during
playback, Pure Data calls `canvas_update_dsp()`, which rebuilds the DSP graph
on the audio thread and causes a 10–30ms audible drop/stutter.

**Goal:** make live patching drop-out-free by building the new DSP graph in a
shadow/staging buffer, then atomically swapping the active graph pointer at a
sample-block boundary.

Full spec: `plugdata-core/PRD-zero-dropout-live-dsp-hot-patching.md` (committed).

## Foundation already in place (do NOT redo)
- Native C++ MCP bridge (`Source/Pd/MCPBridge.{h,cpp}`) is solid and verified:
  raw create/delete/disconnect via `pd::Patch`/`pd::Interface`, array
  auto-create, `/pd/update_dsp`, settings/status, server heartbeat.
- `mcp-server` side is verified against every tool action.

## What failed the first time (reverted — learn from this)
An earlier attempt implemented a staging DSP chain directly:
- `Libraries/pure-data/src/d_ugen.c` — added `u_dspchain_staging`,
  `u_staging_mode`, `u_swap_pending`; `dsp_add`/`dsp_addv` write to staging;
  `dsp_tick()` swaps pointers.
- `g_canvas.c` — `canvas_update_dsp()` → `canvas_update_dsp_hot()`.
- `m_obj.c` — `dsp_update_deferred` flag to defer per-wire recompiles.
- `m_pd.h` — exported the new symbols.
- `PluginProcessor.cpp` / `MCPBridge.cpp` — set `dsp_update_deferred` around
  batch mutations.

**Why it was reverted:** it did not actually produce glitch-free swapping and
was rolled back in full (d_ugen.c, g_canvas.c, m_pd.h, PluginProcessor.cpp,
MCPBridge.cpp restored). `m_obj.c` still has the inert `dsp_update_deferred`
global — harmless dead code, clean it up or reuse it.

**Key lesson:** the swap boundary is subtle — the naive "swap in `dsp_tick()`"
didn't account for (a) the pd signal-object sort, (b) `canvas_update_dsp` being
called from many places, and (c) the fact that PlugData's GUI + JUCE
`processBlock` drive DSP differently from vanilla pd's `dsp_tick`.

## Proposed approach for the next attempt (revise before coding)
Instead of patching pd's `dsp_add`/`dsp_tick` globally, consider a more
targeted design:

1. **Suspend-compile-swap at the block boundary we control** — the swap point
   should be inside `PluginProcessor::processBlock()` (JUCE audio callback),
   not inside pd's `dsp_tick()`. PlugData's audio path is `processBlock`, which
   is where we already know block boundaries.

2. **Staging graph owned by the bridge** — when the MCP server issues a
   mutation batch, the bridge: suspends DSP, applies mutations into a staging
   canvas (or marks a pending recompile), and sets `dsp_swap_pending`. The
   `processBlock` prologue checks the flag and performs the swap with an
   equal-power micro-fade to kill DC clicks.

3. **Validate the swap semantics first** — before writing DSP code, confirm how
   `canvas_update_dsp` / `canvas_start_dsp` / `canvas_stop_dsp` interact with
   PlugData's `Instance::performDsp` / the `pd::Patch` message queue. There may
   already be a suspend/resume API (`canvas_suspend_dsp`/`canvas_resume_dsp`)
   to build on instead of a custom staging chain.

4. **Phase 1 (server) first** — the PRD's Phase 1 is a cheap win: pre-allocate
   a dynamic summing bus (`[throw~]`/`[catch~]` to N `mcp_bus_*` channels) so
   new voices attach to an existing bus without tearing down `[dac~]` wires.
   This reduces dropouts without any core changes — ship it first, measure.

## Acceptance / measurement (from the PRD)
- 0 samples dropped on mutation (loopback + sample-diff).
- DSP swap latency < 2ms.
- DC-step / click < -90 dBFS on the swap boundary.
- 10k mutations stress without crash.

## Suggested next-session first steps
1. Read the PRD + this note.
2. Investigate the actual DSP rebuild path in `PluginProcessor::processBlock`
   and `pd::Instance` (grep `performDsp`, `canvas_update_dsp`, `dsp_tick`).
3. Implement Phase 1 (server summing bus) and verify it reduces stutter.
4. Prototype the suspend/swap in `processBlock` on a throwaway branch first.
