// The mcp_* Pd system messages (Plan 3 P3.3 S7): one row per selector, the single
// list. It is included, with MCP_SYS defined, by
//   McpHost.h                 declares each handler as a private member function
//   McpSysCommands.cpp        fills the selector table that McpHost::handleSysMessage reads
//   Tests/mcp/registry_test.cpp  checks the rows without JUCE
//
//   MCP_SYS("mcp_selector", handler)
//
// A handler is `void handler(SmallString const& selector, mcp::SysList const& list)` (P3.4 S7)
// and keeps the body it had as a case of McpHost::dispatchSysMessage. Every row is
// reachable from Pd (a message to `pd` or to `mcp_host` ends in
// PluginProcessor::receiveSysMessage); "bridge" names the OSC verbs of MCPBridge that
// call receiveSysMessage with the selector themselves, "Pd only" means nothing in the
// fork sends it.

#ifndef MCP_SYS
#define MCP_SYS(selector, handler)
#endif

// ── selection, housekeeping ──
MCP_SYS("mcp_selection_gate", sysSelectionGate)    // Pd only
MCP_SYS("mcp_move", sysMove)                       // Pd only
MCP_SYS("mcp_clear_selection", sysClearSelection)  // bridge: /pd/clear_selection
MCP_SYS("mcp_move_batch", sysMoveBatch)            // bridge: /pd/move_batch (index form)
MCP_SYS("mcp_move_batch_id", sysMoveBatchId)       // bridge: /pd/move_batch_id
MCP_SYS("mcp_clear_ids", sysClearIds)              // bridge: /pd/clear, clear_ids, load_content
MCP_SYS("mcp_clear_undo", sysClearUndo)            // bridge: /pd/clear_undo, encapsulate_gop, resetCanvasUndo (batch_atomic and others)

// ── identity ──
MCP_SYS("mcp_register_id", sysRegisterId)          // bridge: /pd/register_id, open_patch
MCP_SYS("mcp_rename_id", sysRenameId)              // Pd only
MCP_SYS("mcp_get_mappings", sysGetMappings)        // bridge: /pd/get_mappings
MCP_SYS("mcp_census", sysCensus)                   // bridge: /pd/census

#undef MCP_SYS
