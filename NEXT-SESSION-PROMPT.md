# NEXT SESSION PROMPT

> Paste this at the start of a fresh session to resume the feature immediately.

---

Continue the "Zero-Dropout Live DSP Hot-Patching" feature for PlugData + MCP server.

Two repos, both on branch `feat/zero-dropout-hot-patching`:

```bash
cd /home/alphi/Desktop/plugdata/plugdata-core && git checkout feat/zero-dropout-hot-patching
cd /home/alphi/Desktop/plugdata/mcp-server     && git checkout feat/zero-dropout-hot-patching
```

Read first:
- `plugdata-core/RESUME-zero-dropout-hot-patching.md` (state + lessons + rollback)
- `plugdata-core/PRD-zero-dropout-live-dsp-hot-patching.md` (full spec)

Context you can trust as DONE (do not redo):
- Native C++ MCP bridge (`Source/Pd/MCPBridge.{h,cpp}`) is implemented + verified.
- Bridge enable/disable toggle, configurable ports, live status (Advanced settings).
- Server heartbeat ping (`osc-client.ts`) keeps the bridge status accurate.
- Base branch `feat/native-cpp-bridge` is the known-good fallback.

Start here (in order):
1. Phase 1 (server side, low risk): pre-allocate a dynamic summing bus so new
   voices attach without tearing down `[dac~]` wires. Verify it reduces stutter.
2. Investigate the real DSP rebuild path before touching core code: grep
   `processBlock` / `performDsp` / `canvas_update_dsp` / `canvas_suspend_dsp`
   in `PluginProcessor.cpp` + `pd::Instance`. The first attempt failed because
   it swapped in pd's `dsp_tick()` instead of the JUCE block boundary.
3. Prototype the suspend/compile/swap in `PluginProcessor::processBlock()` on a
   throwaway branch before committing to it.

Environment:
- Bridge listens UDP 9000, replies to 19010.
- Build plugdata: `cmake --build build --target plugdata_standalone`.
- Build server: `npm run build`.
- Verify via MCP tools (`verify_connection`, `analyze_spectrum`).

If the feature fails: `git checkout feat/native-cpp-bridge` in both repos (safe
rollback) and abandon/delete `feat/zero-dropout-hot-patching`.
