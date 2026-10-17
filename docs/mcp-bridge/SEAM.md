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

## Baseline (P3.2 S0, fork `060f3383c`, base `5c6587314`)

215 hunks in 35 upstream-owned files, +7,351 / -83 lines, 0 tagged (`Canvas.cpp` 47, `PluginProcessor.cpp` 18, `OverlayDisplaySettings.h` 17, `LuaObject.h` 15, `Interface.h` 11, `Sidebar.cpp` 11, `Instance.cpp` 11, then 28 files with 1 to 8). The gate is expected to fail until P3.2 S9. Target at the end: at most 60 hunks, `Canvas.cpp` at most 12.

## Known upstream behaviour

- **Silent transparent window when no OpenGL context can be created (finding L2).** The "OpenGL 3.0 support is required" dialog fires only when `nvgCreateContext` fails inside `NVGSurface::newOpenGLContextCreated()`. When JUCE cannot create a GL context at all (for example an NVIDIA userspace upgraded under the running kernel module: `nvidia-smi` says "Driver/library version mismatch"), that callback never runs, so the window stays transparent with no error. This is upstream code with no fork diff and is not patched; the MCP repo's `scripts/live/gl-preflight.sh` turns it into "reboot needed" for the live scripts.
