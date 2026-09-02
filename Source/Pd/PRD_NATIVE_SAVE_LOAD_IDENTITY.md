# PRD: Phase 3 — C++-Native Save/Load with Identity Trailer

**Document Version:** 0.1.0 (DRAFT)
**Target Repos:** `plugdata-core` (C++ bridge) & `PlugData-MCP-Server` (TS)
**Depends on:** Phase 1 (shipped) + Phase 2 (shipped, live-verified 2026-09-02)
**Author:** Jupiter Moll & Antigravity
**Status:** PROPOSED

---

## 1. Motivation — Found by the Full Gauntlet

The post-Phase-2 tool gauntlet tested every MCP tool family. One serious bug surfaced:

**`manage_patch_files load` scrambles identity.** The load path clears the C++ stable map, then
re-registers the OLD tempIds **by position** onto the freshly loaded canvas. Because the loaded
object order is a permutation of the saved order, every semantic name landed on the wrong object
(`rm_main_34` → a message box; `[r kick]` → named `k_ptrig`). The C++-truth probe exposed it
immediately (`Outlet 0 is not a signal outlet` on what should be a signal module) — the honest-error
design worked, but the underlying identity decision happened **outside the engine**.

Root cause is architectural, identical to what Phases 1–2 eliminated elsewhere: **identity crosses
the TS boundary and gets re-invented there.** Save = TS-mediated text dump; load = TS positional
re-registration.

The proof that C++-native file operations are the answer already exists in the codebase:
`/pd/encapsulate_to_file` (C++ writes the file, swaps the canvas, preserves identity — verified
perfect in live testing, 6ms, zero dropout).

---

## 2. Design — Identity Never Leaves the Engine

### 2.1 Save: `canvas_saveto` + Identity Trailer

New bridge action `/pd/save_patch <canvas> <filePath> <corrId>` (runs under `sys_lock`):

1. Serialize the canvas with Pd's own `canvas_saveto(x, binbuf)` — the battle-tested native
   serializer that handles every object type, GOP, arrays, subpatches.
2. Append an **identity trailer** to the binbuf before writing: for each object, in `gl_list`
   order, a `#X text` comment line:
   ```
   #X text 10 10 ;
   #X text 10 30 ; MCP-IDENTITY V1 <n_objects> <identity_version>
   #X text 10 50 ; MCP-ID <index> <tempId>
   ...
   ```
   (Comment lines are ignored by Pd's loader — zero compatibility risk with plain Pd.)
3. Write atomically (tmp + rename), reply success.

Renamed/user tempIds are captured exactly as they are in `mcpStableObjectMap`. Objects without a
registration simply have no trailer line and will be auto-adopted on load (Phase 2 naming).

### 2.2 Load: `binbuf_evalfile` + Direct Re-registration

New bridge action `/pd/load_patch <canvas> <filePath> <corrId>`:

1. Clear the target canvas (existing `clear` path) and `canvas_setfilepath`-style open via
   `binbuf_evalfile` — **the same trusted path that loads abstractions**.
2. Walk `gl_list`; parse the MCP-IDENTITY trailer; for each `<index> <tempId>` whose object still
   exists at that index, register directly: `mcpStableObjectMap[canvas][tempId] = gobj` +
   serial map + one version bump.
3. Objects without trailer entries → Phase 2 native auto-adopt mints readable names at next census
   (or immediately, same walk).
4. `canvas_update_dsp()`, restart DSP, reply with object/connection counts.

**No positional guessing. No TS involvement. Renames survive save/load exactly.**

### 2.3 TS: `manage_patch_files` Demoted to Pass-Through

- `save` / `load` actions forward to the new endpoints and format the reply. No text handling,
  no identity restoration, no store juggling.
- The legacy dump/dumped file-watch machinery in `getAnalysis`'s fallback path becomes a further
  candidate for retirement (native census already covers it on modern bridges).
- The load-scramble code path (positional re-registration) is deleted.
- Cap gate: `save_patch_v2` / `load_patch_v2` in the caps list; legacy bridges keep the old path.

### 2.4 Bonus Unlocks (same mechanism)

- **Snapshots with exact identity**: `manage_snapshots` scene recall rebuilds canvases through the
  same loader — semantic names survive scene changes mid-performance.
- **Autosave/backup**: the trailer makes every autosave identity-complete.
- **Pd-native compatibility**: a MCP-saved file opens fine in vanilla Pd/PlugData (trailer is
  comments); identity "lights up" only when loaded through the bridge.

### 2.5 Multi-Tab Support — Tabs as Identity-Tracked Canvases

Today `main` is a moving pointer to the focused tab, root canvases share one identity bucket, and
MCP cannot open a patch in a new tab. Extension:

1. **`/pd/open_patch <filePath> [corrId]`** — the engine opens a patch in a NEW editor tab
   (enqueued to the message thread, same path as the UI's File→Open). The sidecar next to the file
   is read and its identities pre-registered under the tab's own canvas key. The patch opens
   fully named — ready to probe without any adoption step.
2. **`/pd/list_tabs [corrId]`** — enumerate open root tabs: canvas key, file name, object count,
   connection count, identity version, sidecar-present flag. TS `list` gains a `tabs` view.
3. **Per-tab identity keys by file name.** Root canvases loaded from a file use their file name as
   the identity bucket key (e.g. `pd-mybeat.pd`) instead of the shared `pd-main`. Switching tabs
   never blurs identities; untitled patches keep `pd-main` until saved.
4. **`load` / `save_patch` gain an explicit canvas argument** — target any open tab by its key, not
   just the focused one (defaults to the focused tab = today's behavior).
5. **Tab-close eviction** — when a tab closes, its identity bucket is evicted (canvas-destroyed
   hook), keeping the map clean across a session with many tabs opened/closed.

---

## 3. In Scope / Out of Scope

**In:** `/pd/save_patch` + trailer writer, `/pd/load_patch` + re-registration, caps gating, TS
pass-through demotion, deletion of the positional-restore path, subpatch traversal (nested
canvases) in the trailer, tests. **Multi-tab (§2.5):** `/pd/open_patch`, `/pd/list_tabs`,
per-tab identity keys by file name, tab-close eviction, explicit canvas targeting.
**Out:** editing the Pd file format itself (trailer is comments only), migrating
`surgical_load`/canvas-reconstruction internals (separate review), hardware export paths.

---

## 4. Acceptance Criteria

1. Save → load round-trip: **every tempId identical** after load, including user renames
   (`dc_block` stays `dc_block`, `rm_main_34` stays `rm_main_34`).
2. Save → load → `probe` on any tempId works immediately; zero "Object not found".
3. A MCP-saved file loads in plain PlugData with no errors (trailer invisible).
4. Hand-added objects saved without registration are auto-adopted with readable names on load.
5. Subpatch canvases (e.g. `pd-hh_voice`) round-trip with their internal identities.
6. Legacy bridge (no new caps) → old save/load path unchanged.
7. Full gauntlet re-run: zero regressions; the load-scramble bug is gone.
8. **Multi-tab:** open two patches in two tabs → `list_tabs` reports both with correct object
   counts; each tab's identities are independent; `load`/`save_patch` can target either tab by
   canvas key; closing a tab evicts its bucket; switching tabs never cross-resolves tempIds.

## 5. Task Breakdown (est. ~1.5 developer-days)

| # | Task | File(s) | Effort |
|---|---|---|---|
| 1 | Trailer writer (save): canvas_saveto + MCP-ID lines, atomic write | `MCPBridge.cpp` | M |
| 2 | Loader: binbuf_evalfile + trailer parse + re-register + auto-adopt | `MCPBridge.cpp`, `PluginProcessor.cpp` | M |
| 3 | Caps: `save_patch_v2`, `load_patch_v2` | `MCPBridge.cpp` | S |
| 4 | TS pass-through + delete positional restore | `manage-patch-files`/`file-io.ts` | S |
| 5 | Nested-canvas trailer sections (subpatches) | `MCPBridge.cpp` | M |
| 6 | Tests: round-trip identity, vanilla-Pd compat, subpatch nesting, rename survival | `__tests__` + live gauntlet | M |
| 7 | §2.5 multi-tab: `/pd/open_patch` (new tab + sidecar pre-register) | `MCPBridge.cpp`, `PluginEditor` glue | M |
| 8 | §2.5 `/pd/list_tabs` + per-tab identity keys + tab-close eviction | `MCPBridge.cpp`, `PluginProcessor.cpp` | M |
| 9 | §2.5 TS: `list` tabs view + explicit canvas targeting in save/load | `file-io.ts` | S |

**Estimate:** ~1.5 dev-days (tasks 1–6) + ~1 dev-day (multi-tab, tasks 7–9).

## 6. Verification Protocol

Standard beat + hand-drawn MERDA module. Save → close PlugData → reopen → load → assert:
census tempIds byte-identical to pre-save; probe works on every previously registered tempId;
console clean; zero dropout during load.
