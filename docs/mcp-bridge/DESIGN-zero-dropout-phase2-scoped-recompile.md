# DESIGN — Zero-Dropout Phase 2: Scoped Recompile + Block-Boundary Splice

> Supersedes the "double-buffered atomic pointer swap" design in the original
> PRD. That design was reverted because it does not survive contact with how
> Pure Data actually schedules DSP. This document corrects it.

## Status

- **Replaces**: the Phase 2 "Pillar 2: Double-Buffered DSP Graph Swapping"
  section of `PRD-zero-dropout-live-dsp-hot-patching.md`.
- **Phase 1** (server-side summing bus) is unchanged and is the correct first
  step. This document is the core-engine follow-up.
- **Evidence**: derived from reading the real rebuild path (see "Facts" below),
  not from the original PRD's mental model.

---

## 1. Facts — how the DSP chain actually works in this fork

These are the constraints any design must respect. All file/line references are
against `Libraries/pure-data/src/` on `feat/zero-dropout-hot-patching`.

1. **The "graph" is one flat array, not a swappable list.**
   `THIS->u_dspchain` (`d_ugen.c:38`) is a `t_int*` of `<perfroutine, args…>`
   entries. `dsp_add`/`dsp_addv` (`d_ugen.c:360`, `:385`) append into it with
   `t_resizebytes` as each object's `dsp()` method runs. There is no second
   buffer.

2. **Execution is a straight walk.** `dsp_tick()` (`d_ugen.c:396`) is
   `for (ip = u_dspchain; ip; ) ip = (*perfroutine)(*ip);`. The audio path is
   `PluginProcessor::processBlock` → `processConstant`/`processVariable` →
   `Instance::performDSP` (`Instance.cpp:796`) → `libpd_process_raw`
   (`z_libpd.c:252`) → `dsp_tick`.

3. **Any signal-topology change rebuilds the whole thing.**
   `m_obj.c:736` and `:788` call `canvas_update_dsp()` when a signal connection
   changes (already gated by the inert `dsp_update_deferred` global left over
   from the reverted attempt). `canvas_update_dsp` (`g_canvas.c:1460`) is
   `canvas_stop_dsp()` + `canvas_start_dsp()`.

4. **The rebuild is destructive and stateful.** `canvas_stop_dsp`
   (`g_canvas.c:1430`) → `ugen_stop` (`d_ugen.c:671`) frees `u_dspchain` and
   calls `signal_cleanup()` (frees all signal buffers). `canvas_start_dsp`
   (`g_canvas.c:1415`) → `ugen_start` (`d_ugen.c:698`) allocates a fresh chain,
   then `canvas_dodsp` (`g_canvas.c:1365`) walks **every root canvas** calling
   every object's `dsp()` method. Those methods **mutate object state**
   (reset delay lines, filters, phase) and allocate new signal buffers.

5. **The drop is the rebuild, and it blocks audio.** The MCP bridge runs on the
   JUCE message thread (`MCPBridge::oscMessageReceived`); audio runs on the
   realtime thread. `libpd_process_raw` holds `sys_lock` across `dsp_tick`
   (`z_libpd.c:240–249`). While the message thread holds `sys_lock` inside
   `canvas_start_dsp`, `dsp_tick` cannot run → the block period (1.45 ms @ 64
   samples / 44.1 kHz) is exceeded → the audible drop.

6. **Batching already exists.** `MCPBridge.cpp` `mcp_create_batch_id` (line 2153)
   already wraps batches in `canvas_suspend_dsp()` / `canvas_resume_dsp()`
   (`g_canvas.c:1447/1454`), so N mutations collapse to **one** rebuild. The
   remaining cost is that single full-canvas rebuild.

---

## 2. Why the atomic swap is unworkable (and stays reverted)

The original design wanted: build a shadow DSP list on a worker thread, then
`active_dsp_list = staging_dsp_list` at sample 0 of `processBlock` with a
32-sample equal-power fade.

Three hard blockers, confirmed by the reverted attempt:

1. **The build is not side-effect-free.** Building a shadow list means running
   every object's `dsp()` method *again*, on the *live* objects. That resets
   live filter/delay state — the very thing we're trying not to disturb — and
   reallocates signal buffers. There is no way to "pre-build" without mutating.

2. **Swapping invalidates buffers.** The old chain's signal buffers are freed by
   `signal_cleanup`; the new chain references freshly allocated buffers. The
   fade assumes continuity that pd does not provide.

3. **The swap hides nothing.** The cost is the *synchronous* `canvas_start_dsp`
   under `sys_lock`, not the assignment of a pointer. A pointer swap cannot
   remove a synchronous rebuild.

**Conclusion:** abandon the swap. The lever that actually matters is *scope*:
make a mutation rebuild only the subgraph it touches, not the whole canvas.

---

## 3. Corrected design — scoped recompile + block-boundary splice

### 3.1 The key insight

A **new** voice (osc~ → filter → throw~) has **no live state**. Its `dsp()`
methods can run at any time without corrupting what is playing. The only reason
pd drops is that `canvas_start_dsp` recompiles **everything**, including the
already-running objects.

So: for a *new* subgraph, recompile only the new nodes (plus the consumer that
must re-register them), then splice the new chain fragment into the active
`u_dspchain` at a block boundary. Existing objects keep their state and their
buffers. The kick/snare never re-run `dsp()`.

### 3.2 The one real coupling: `throw~`/`catch~`

`catch~` sums all `throw~` of the same name, but it discovers its sources during
`dsp()`. Adding a new `throw~` therefore requires the matching `catch~` to
re-run its `dsp()`. Crucially, `catch~` is a **pure sum** — re-running it has no
stateful side effects. And in the Phase 1 bus topology its downstream is just
`catch~ → protection → dac~`, a 2–3 node slice.

This is why **Phase 1 (bus) and Phase 2 (scoped recompile) are complementary**:
the bus makes the "affected slice" of any new-voice mutation tiny and
state-free.

### 3.3 Components

1. **Scope computation** (in `canvas_dodsp` / a new `canvas_dodsp_scoped`).
   Given the set of changed objects (the bridge knows this — it issued the
   mutation batch), mark the dirty subgraph: changed nodes, their signal
   consumers, and any named-bus peer (`catch~`) that must re-register. Leave the
   rest of the graph untouched.

2. **Fragment build** (`d_ugen.c`). Add a build mode that redirects
   `dsp_add`/`dsp_addv` into a *fragment* buffer `u_dspchain_frag` while
   `u_building_fragment` is set, instead of the live `u_dspchain`. The new
   objects append into the fragment. The `catch~` re-run also appends its
   (small) updated slice into the fragment. Existing objects are never asked to
   re-run `dsp()`.

3. **Signal-buffer reuse** (`signal_cleanup` → scoped). Do **not** free the
   existing live signal buffers. Only allocate buffers for new nodes. This is
   what turns "recompile" into "append".

4. **Splice at the block boundary** (in `PluginProcessor::processBlock` /
   `processConstant`, *not* in `dsp_tick`). The bridge sets a flag
   `dsp_splice_pending` with the fragment ready. At the top of a block — before
   `performDSP` is called — splice the fragment into `u_dspchain` at its correct
   sort position under `sys_lock`, then clear the flag. The splice is a bounded
   memmove/copy, not a full rebuild.

5. **DC-step suppression** (optional, cheap). Because the *new* objects start
   from zero state, a hard splice usually has no DC step. Where a node's
   re-registration could step (e.g. a live `catch~` re-run), apply a per-block
   linear crossfade over the first 32 samples of the *new* signal only — the old
   signal buffers were never freed, so we can fade between old and new copies.

### 3.4 What this does NOT do (and why that is correct)

- **Editing the topology of an already-running voice** (rewiring a live osc~)
  still requires that object's `dsp()` to re-run, which resets its state. This
  remains an inherent, audibly-resetting operation — the PRD's non-goal list is
  consistent with this. Parameter edits (cutoff, gain) already avoid
  `canvas_update_dsp` entirely and are unaffected.

- **Deletion** is the inverse of creation: mark the removed nodes dirty, rebuild
  their former consumers (again `catch~`/`dac~` only), splice out.

### 3.5 Thread-safety contract

- Fragment build runs on the **message thread** (where the bridge already runs),
  under `sys_lock`, writing only to `u_dspchain_frag`.
- The splice runs on the **audio thread** inside `processBlock`, under `sys_lock`,
  guarded by an `std::atomic<bool>` pending flag written by the message thread.
- No `free()` of live buffers on either path; freed memory (removed nodes) is
  deferred to a small quarantine list drained at the next splice.

---

## 4. Deliverable order (replaces PRD §7)

1. **Phase 1** — summing bus in `mcp-server` (unchanged, in progress).
2. **Phase 2a** — fragment build mode + scoped `canvas_dodsp_scoped` + signal
   buffer reuse in `pure-data` (no splice yet). Validate no drop on *creation*
   by measuring rebuild time drop.
3. **Phase 2b** — block-boundary splice in `PluginProcessor::processBlock` +
   bridge flag `dsp_splice_pending`.
4. **Phase 2c** — optional DC-step fade; stress gauntlet (10k mutations).

## 5. Acceptance (unchanged from PRD §6)

- 0 samples dropped on **creation** (loopback + sample-diff).
- Splice latency < 2 ms.
- DC-step / click < -90 dBFS on the splice boundary.
- 10k mutations without crash.

## 6. Risks / rollback

- Phase 2 touches `d_ugen.c` and `g_canvas.c` — the exact files the reverted
  attempt touched. The difference this time is **scoped, fragment-based, and
  non-destructive** rather than a global swap. Prototype on a throwaway branch
  first (per the RESUME note), and keep `feat/native-cpp-bridge` as the
  known-good fallback.
- `dsp_update_deferred` (inert global in `m_obj.c`) should be removed or
  repurposed when we touch that file — do not leave dead code behind.
