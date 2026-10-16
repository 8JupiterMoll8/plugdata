# HANDOFF — MCP Undo System (PlugData + MCP bridge)

**For the next agent.** What the MCP undo system is, what shipped, what is missing,
and the open design question about knob/slider value undo.
Branch: `feat/v4-zero-dropout-copilot` (both `plugdata-core` and `mcp-server`).

---

## 1. What "our undo system" is

The **MCP transaction stack** — `mcpUndoStack` / `mcpRedoStack` in
`Source/PluginProcessor.*`. It stores, per canvas, a list of `McpTransaction`
(forward + inverse `juce::OSCMessage` deltas + `replayAction`). Undo replays the
inverse through the bridge; redo replays the forward.

- **GUI `Ctrl+Z` and MCP `undo_redo_patch_transaction` both use it**:
  `Canvas::undo()` checks `hasMcpTransaction()` first, else falls back to Pd's
  native `patch.undo()` (`Source/Canvas.cpp:2834`). `/pd/undo` mirrors that
  (`Source/Pd/MCPBridge.cpp:7447`).
- **It is NOT Pd's `g_undo.c`.** Pd's native undo handles *manual GUI gestures*;
  MCP mutations deliberately **flush** Pd's queue (`resetCanvasUndo`) to avoid the
  index-based dangling-pointer crash. The two coexist by the dual-stack priority.
- **Message-delta only** — never raw pointers/indices. This is why it is crash-safe.

---

## 2. Status — SHIPPED & VERIFIED LIVE (no crashes)

| Operation | Undo behaviour | Commit |
| :--- | :--- | :--- |
| build (create + connect) | removes the build | (pre-existing) |
| edit / `retype` | restores prior class + args | `6fe6affc6` |
| delete | restores the object **+ all its wires** | `6fe6affc6` |
| move | restores prior position | `b8f9c1b2e` |
| lock + canvas lifetime | stacks locked, cleared on canvas destroy/load/clear | `63e074791` |

Verified live: `saw~ 2` → undo → `osc~ 220`; delete `*~ 0.05` → undo → object + 3
wires restored; move (100,100)↔(600,420) undo/redo. Zero segfaults.

Also shipped (related, this session):
- Core `18181af3c` — relabel label sync in `TextBase` render path.
- Server `7dea5cf` — `edit` → `retype` rename (alias kept).
- Server `b9f1e76` — optional `relabel` on `plugdata_send action:'object'`.

---

## 3. What is MISSING

### 3a. Auto-layout undo — FEASIBLE, recipe below
`layout_patch` (`pillars`/`tidy`/`deoverlap`/`compose`/`align`) moves objects through
their **own handlers**, not `batch_atomic`, so those moves are not recorded.
Manual `move` **is** covered; auto-layout is not.

**Recipe (reuses the move machinery):** in each layout/deoverlap handler, capture
each object's old `(x,y)` before moving (see the move handler pattern at
`Source/PluginProcessor.cpp` `case hash("mcp_move_batch_id")`, and the deoverlap
handler in `MCPBridge.cpp` ~`mcp_move_batch_id`), then `pushMcpTransaction(cnv,
canvasSymbol, fwd, inv, "move_batch_id")` with inverse = old positions.
Handlers to touch: `/pd/align`, `/pd/deoverlap`, `/pd/pillars`, `/pd/flow`,
`/pd/compose`, `/pd/tidy`.

### 3b. Knob / slider value undo — OPEN DESIGN QUESTION (see §4)
`set[]` goes through `/pd/obj_set_batch`, which **pushes no transaction at all**.
So a value turn is invisible to undo today.

### 3c. `#5` pop-before-verify — DEFERRED
`undoMcpTransaction` moves the tx to the redo stack **before** the inverse is
confirmed applied (`PluginProcessor.cpp`). On a rare timeout the history can
corrupt (redo duplicates). Proper fix needs `handlePdDomain` to return success
(invasive signature change). Low visible benefit.

---

## 4. THE OPEN QUESTION — should a knob/slider turn be undoable?

**Direct answer to the artist's question:** *today, a knob/slider turn is NOT
undoable, and it is NOT in our undo system.* `set` pushes no transaction.

Can it be? **Only for GUI widgets** (knob, hsl, vsl, nbx, tgl, hradio, vradio,
button) — because those are the only objects whose current value is **readable**
(`analyze_patch action:'props'` → `props.value`; also `manage_snapshots
save_params` reads exactly these). For DSP objects (`lop~`, `osc~`, `*~` etc.) the
prior value is **not readable** — `props` returns `{}` — so it is **impossible**
to undo generically without a full snapshot.

### Options (needs an artist/product decision, not just code)
1. **Never** — treat value turns as live-performance gestures (like a real knob).
   Simplest; undo stays structural-only.
2. **Discrete sets only** — record a transaction per widget `set[]` call. Risk:
   a fast sweep = many undo steps; `Ctrl+Z` forever. Could coalesce by
   time+object (e.g. one undo step per knob per ~500 ms).
3. **Values-only snapshot (save_params style)** — before a `set`, capture the old
   readable values (cheap map `tempId→value`); inverse re-`set`s them. No reload,
   no pop, identity preserved. **This is the recommended shape** if we do it.
4. **Full-canvas snapshot** — REJECTED (reload = pop/click, destroys tempIds).

**Recommendation:** Option 3, **widgets only**, with time-coalescing (Option 2's
guard) so a sweep collapses to one undo step. DSP-object `set` remains a live
gesture by design.

---

## 5. Verification (crash gauntlet — every change)
1. `cmake --build build --target plugdata_standalone -j8`
2. Restart PlugData **manually** (do NOT launch a second instance — it fights over
   UDP 9000 and makes the bridge go deaf).
3. Via MCP: build → edit → delete → move → layout → undo/redo each.
4. Expect: correct rollback/redo, **zero segfault** (`journalctl -k | grep -i segfault`),
   master audio still audible.

---

## 5b. Design ethos (why this exists — keep this)

This MCP layer is **made by an artist, for artists.** The bar is not "works on
average" — it is **live-stable**: you can patch and change values *while the music
plays* with no dropouts, no recompiles, and no crashes. Every design choice serves
that:
- **message lane (`set`)** over recreation, so nothing interrupts the sound;
- **transaction deltas** instead of Pd's fragile index-based undo;
- **flush-and-fence** around `g_undo`, because artist-grade stability means never
  letting a known crash vector run.

"Super stable" is a property you **maintain**, not one you have: run the crash
gauntlet after every change (§5), and keep the ground rules (§6). The old crashes
were real — they are fenced now, not forgotten.

## 6. Ground rules (do NOT violate)
- **Never touch Pd's `g_undo.c` / `canvas_undo_free` threading** — that is the
  crash vector. Stay in message deltas. `resetCanvasUndo` stays as-is.
- The undo stacks are shared across the OSC thread and message thread — always hold
  `mcpUndoLock` for map ops, and **never** across the nested `batch_atomic` call
  (execute outside the lock).
- `McpTransaction` has **no default ctor** (OSCMessage needs an address) — hold by
  `std::unique_ptr`, not a stack local.
- Replay goes through `handlePdDomain(tx->replayAction, msg)` — keep new
  transaction types message-delta and action-name based.

---

## 7. Commit map
`plugdata-core`:
- `18181af3c` relabel render-path sync
- `63e074791` undo lock + canvas lifetime
- `6fe6affc6` undo edits + deletes
- `b8f9c1b2e` undo moves (action-aware replay)

`mcp-server`:
- `7dea5cf` edit→retype rename
- `b9f1e76` relabel on send lane
