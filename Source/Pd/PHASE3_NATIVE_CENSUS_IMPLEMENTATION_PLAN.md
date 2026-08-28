# Phase 3: Native In-Memory Canvas Census (`/pd/census`) Implementation Plan

## Overview
Phase 3 eliminates the slowest remaining bottleneck in patch inspection: **file-based dumps and regex parsing**.

* **Current Architecture**: When inspecting canvas state (for audits, reconciliation, census, or diagnostics), the MCP server sends `/pd/dump`. C++ writes a `.pd` text file to `/dev/shm`, the server awaits a file system watcher, reads the file, and runs 10+ regex passes to reconstruct the object graph (~50–150 ms).
* **Phase 3 Architecture**: C++ traverses Pure Data's native `gl_list` and `linetraverser` directly in memory under `sys_lock()`, maps stable IDs, and returns a compact structured JSON string over OSC in **< 2 ms** with **0 file I/O**.

---

## 1. OSC Protocol Contract

### Request:
```
/pd/census [canvasName, correlationId]
```
* `canvasName` (string): e.g. `"main"` or subpatch symbol.
* `correlationId` (string): correlation tracking ID.

### Reply:
```
/pd/census/reply/<correlationId> [jsonString]
```
* `jsonString` (string): Single JSON payload:
```json
{
  "canvas": "main",
  "objectCount": 3,
  "connectionCount": 2,
  "objects": [
    {
      "index": 0,
      "id": "osc1",
      "kind": "obj",
      "type": "osc~",
      "args": ["440"],
      "x": 100,
      "y": 60,
      "inlets": 2,
      "outlets": 1
    },
    {
      "index": 1,
      "id": "vca1",
      "kind": "obj",
      "type": "*~",
      "args": ["0.3"],
      "x": 100,
      "y": 140,
      "inlets": 2,
      "outlets": 1
    },
    {
      "index": 2,
      "id": "dac1",
      "kind": "obj",
      "type": "dac~",
      "args": [],
      "x": 100,
      "y": 220,
      "inlets": 2,
      "outlets": 0
    }
  ],
  "connections": [
    {
      "srcIndex": 0,
      "srcOut": 0,
      "destIndex": 1,
      "destIn": 0,
      "srcId": "osc1",
      "destId": "vca1"
    },
    {
      "srcIndex": 1,
      "srcOut": 0,
      "destIndex": 2,
      "destIn": 0,
      "srcId": "vca1",
      "destId": "dac1"
    }
  ]
}
```

---

## 2. C++ Implementation Details (`plugdata-core`)

### `PluginProcessor.cpp` / `MCPBridge.cpp`:
1. In `processMcpSysMessage()`, add case `hash("mcp_census")`.
2. Under `sys_lock()`:
   * Fetch target `t_canvas* canvas = getCanvasBySymbol(canvas_symbol)`.
   * Create reverse mapping from `t_gobj*` pointer to semantic `tempId` string using `mcpStableObjectMap[canvas_symbol.toStdString()]`.
   * Walk `canvas->gl_list`:
     * Read `text->te_xpix`, `text->te_ypix`, `text->te_width`.
     * Extract class name and arguments from `text->te_binbuf` via `binbuf_getnatom()` and `binbuf_getvec()`.
     * Read `obj_ninlets(text)` and `obj_noutlets(text)`.
     * Support IEM GUIs (`bng`, `tgl`, `nbx`, `vsl`, `hsl`, `vradio`, `hradio`) and tables.
   * Walk connections using `t_linetraverser`:
     * Read `srcIndex`, `srcOut`, `sinkIndex`, `sinkIn`.
     * Attach `srcId` and `destId` if found in stable map.
3. Serialize to `juce::var` dynamic object and convert to JSON string with `juce::JSON::toString()`.
4. Reply with `sendMCPReply(String("/pd/census/reply/" + correlationId), atoms)`.

---

## 3. TypeScript Integration (`mcp-server`)

1. **Advertise Capability**: Add `"census"` to `/bridge/capabilities`.
2. **Context Fast Path**: Update `getAnalysis()` in `src/index.ts` and `src/analysis/patch-analyzer.ts`:
   * If `oscClient.hasCap("census")`: call `/pd/census`, parse JSON directly.
   * If not: fallback to legacy `/pd/dump`.
3. **Audit Acceleration**: `construct_patch_v2` preflight & audit rules consume the parsed in-memory census in **< 1 ms**.

---

## 4. Verification Checklist
- [ ] C++ builds without errors: `cmake --build build -j$(nproc)`
- [ ] TypeScript builds and passes test suite: `npm run build && npm test`
- [ ] Live benchmark `tests/live/test-phase3-census-live.ts`:
  - 100% object, wire, and argument accuracy vs legacy dump.
  - Execution time: **< 2 ms** (down from 50–150 ms).
