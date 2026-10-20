# Pending local submodule changes (NOT applied by the build)

Uncommitted edits found in the owner's submodule checkouts during Plan 1
Phase 2 (2026-10-16). They are **not needed to build** (the two files that
are, `iemguts.c` and `particlepad.c`, are real patches in
`../submodules/`), so `scripts/apply-submodule-patches.sh` ignores this
folder. They are kept here so nothing is lost. **Owner decision needed:**
for each item, promote it to a patch under `../submodules/<name>/`, or delete it.

| Item | What it is | Effect on the built app |
|---|---|---|
| `JUCE/juce_TextEditor-scrollbar.diff` | `TextEditor::checkLayout` keeps the text holder at least viewport height and shows scrollbars by content height | Text editor scrolling/scrollbar behaviour differs without it |
| `pd-else/tracked-modifications.diff` (23 files) | `#X coords` (graph-on-parent size/margins) of the MERDA `*.m~.pd` modules | MERDA module GOP panels look different |
| `pd-else/tracked-modifications.diff` (`Source/Control/lua/pdlua.c`) | Guards `typedmess` on an unbound send | Probably not compiled (the build uses the pd-lua submodule); verify |
| `pd-else/untracked/*.pd_lua` (5) | boid-flock, canvas2d, flock, pong, xypad abstractions | Extra demo/abstraction objects missing from the object browser |

To promote one: apply it in the submodule, commit, and `git format-patch`
into `../submodules/<name>/` (use `--start-number` after the last patch).
