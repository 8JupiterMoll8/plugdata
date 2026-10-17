# The MCP seam

The fork's AI bridge (`Source/Pd/MCPBridge*`, the overlay layer, the moodboard) lives next to plugdata's own code. Every upstream merge (P3.1 cost: 19 text files, 53 hunks, four submodules, and a long porting pass) is paid for by the hunks the fork leaves **inside upstream-owned files**. The seam is the rule that keeps that number small and visible.

`scripts/check-seam.sh` measures it against `docs/mcp-bridge/UPSTREAM_BASE` (the upstream `develop` commit the fork was last merged with). Run `--report` any time; `--gate` fails when the rules below are broken.

## Rules

1. **New fork changes to upstream files go through the seam.** A change in an upstream-owned file is one hook call, tagged `// MCP-HOOK(<name>)` on the added line, into code under `Source/Mcp/`. No new logic inline.
2. **Every hunk is accounted for.** A hunk in an upstream file either carries an `MCP-HOOK(` tag or its file is listed under "Carried patches" below with the reason it cannot be a hook.
3. **The bridge sees no GUI.** `Source/Pd/MCPBridge*` includes none of the 13 plugdata GUI headers (PluginEditor, Toolbar, Sidebar, Canvas, CanvasViewport, TabComponent, Object, Connection, ObjectBase, AllGuis, GemCapture, Fonts, Dialogs). GUI work goes through `McpGuiHost`, implemented in `Source/Mcp/GuiHost.cpp`.
4. **Hooks are no-ops without a host.** With no MCP host attached, every hook returns at once and changes nothing.
5. **Pure moves stay pure.** Moved code is checked with a line-multiset comparison: old minus new is only hook lines; new minus old is only wrappers, includes and prefix rewrites.

## Hook set (target of P3.2, declared in `Source/Mcp/McpHooks.h`)

| Area | Hooks |
|---|---|
| audio | `prepare(sampleRate, blockSize)`, `audioBlock(buffer, midiHistory, sampleRate)`, `pdBlock(in, out, blockSize, numChannels)` |
| Pd-thread command | `handleSysMessage(selector, list) -> bool` |
| save | `patchSaved(t_glist*, path)` |
| telemetry | `consoleMessage(text, isError)`, `selectionChanged(selector, list)` |
| overlay paint | `CanvasMcpLayer::paintUnder/OverObjects`, `decorateObject`, `isTargeted(object\|connection)` |
| canvas input | mouse, key and modifier events, each returning `consumed` |
| canvas lifecycle | created, destroyed, synchronised, settingsChanged, `undo/redo -> handled` |

## Carried patches

Upstream-file hunks that stay as they are, with the reason. Empty until P3.2 S9; the gate reads the first column.

| File | Why it cannot be a hook |
|---|---|

## Submodule patches (`patches/submodules/`)

Not part of the hunk count above (that measures `Source/`). Applied by `scripts/apply-submodule-patches.sh` on top of the pinned upstream submodules; the reason and the dependants of each patch are in its commit message.

| Patch | What | Why it is carried |
|---|---|---|
| `pure-data/0001` | `obj_disconnect` releases the signal of `oc2` instead of `oc` | Upstream bug still present at the pin (2002d63): dangling weak reference, heap corruption. Needed by every wire removal on a signal outlet |
| `pure-data/0002` | `dsp_update_deferred` global and its two guards in `obj_connect`/`obj_disconnect` | Fork feature, no upstream equivalent: the bridge's `batch_atomic` defers the per-wire DSP recompile. Dropping it breaks the link |
| `pure-data/0003` | null guards in `g_rtext.c`, `canvas_displaceselection`, and the undo queue (`g_undo.c`) | Not fixed upstream at the pin (every hunk applies to the unpatched pin). Per hunk reasons are in the patch message; includes a plain upstream bug (`canvas_undo_rebranch` leaves DSP suspended). Note: `glist_getrtext(gl, obj, 0)` means "create only if visible" and returns 0 otherwise, it does not mean "do not create" |
| `pd-else/0001-0003` | `ai_midi`, `ai.voice`, `particlepad` externals | Fork-only objects (new files) |
| `pd-iemguts/0001` | `iemguts_setup()` aggregator | Build glue for plugdata |
| `pd-lua/0001` | sequential array indexing for atom tables | Fork fix |
| `JUCE/0001` | nullptr check for `XGetImage` in `createSnapshotOfNativeWindow` | Fork fix (GEM capture) |

## Baseline (P3.2 S0, fork `060f3383c`, base `5c6587314`)

215 hunks in 35 upstream-owned files, +7,351 / -83 lines, 0 tagged (`Canvas.cpp` 47, `PluginProcessor.cpp` 18, `OverlayDisplaySettings.h` 17, `LuaObject.h` 15, `Interface.h` 11, `Sidebar.cpp` 11, `Instance.cpp` 11, then 28 files with 1 to 8). The gate is expected to fail until P3.2 S9. Target at the end: at most 60 hunks, `Canvas.cpp` at most 12.

## Progress

- **S3 (`Interface.h` back to upstream).** `Source/Pd/Interface.h` and `Source/Pd/Patch.h` match the base byte for byte. The fork's Pd operations live in `Source/Mcp/PdOps.h` (`mcp::PdOps`): `findOwnerCanvas`, `unlinkGobj`, `removeObjectsAudioThread`, `clearCanvasAudioThread`, `clearCanvas`, `canUndo`, `canRedo`, the `garray_getname` extern, plus two wrappers that keep the old guards for the bridge's callers only: `removeObjects` (a canvas without an editor goes to the audio-thread delete) and `renameObject` (no editor or no rtext: skip). `createObjectFast` (Interface and Patch) had no caller and is deleted. `Patch::canUndo/canRedo` keep the fork's live read of Pd's undo queue (tagged `MCP-HOOK(undoState)`; an unlocked read from the message thread, P3.4).
  - **`setConnectionPath` is upstream again**, so a path edit is undoable. The fork's in-place version (`1f0c7bd0f`, "live curved connection segfault") is NOT carried: every object drag runs the path updater (remove + reconnect of each of its wires), and with DSP on a file-loaded voice did 10 drag + undo cycles, undo restoring the position each time; the bridge repro (`mcp-server/scripts/live/s3-path-update-repro.ts`) ran 20 drag + `batch_atomic` delete races and 20 bridge `tidy` runs, all alive. The likely cause of the old segfault is the `oc2` disconnect bug, fixed by `pure-data/0001`.
  - **Finding L6 (pre-existing, not S3):** on objects built by `batch_atomic`, GUI drag, undo, drag segfaults in `canvas_undo_init` (from `Patch::startUndoSequence` in `Object::mouseDrag`). Same crash on the S2 build (in-place `setConnectionPath`) and with no wires at all. For P3.4/P3.5 (the fork's dual undo stack and the bridge's undo-queue handling).
  - **Finding L7 (pre-existing):** `screenshot_canvas` crops at the viewport's offset inside the editor but grabs the whole X window, which has a client-side shadow margin, so the image starts ~20 logical px up and left of the viewport (it shows a strip of the tab bar).

## Known upstream behaviour

- **Silent transparent window when no OpenGL context can be created (finding L2).** The "OpenGL 3.0 support is required" dialog fires only when `nvgCreateContext` fails inside `NVGSurface::newOpenGLContextCreated()`. When JUCE cannot create a GL context at all (for example an NVIDIA userspace upgraded under the running kernel module: `nvidia-smi` says "Driver/library version mismatch"), that callback never runs, so the window stays transparent with no error. This is upstream code with no fork diff and is not patched; the MCP repo's `scripts/live/gl-preflight.sh` turns it into "reboot needed" for the live scripts.
