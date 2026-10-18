// The mcp_* Pd system messages (Plan 3 P3.3 S7): one row per selector, the single
// list. It is included, with MCP_SYS defined, by
//   McpHost.h                 declares each handler as a private member function
//   McpSysCommands.cpp        fills the selector table that McpHost::handleSysMessage reads
//   Tests/mcp/registry_test.cpp  checks the rows without JUCE
//
//   MCP_SYS("mcp_selector", handler)
//
// A handler is `void handler(SmallString const& selector, SmallArray<pd::Atom> const& list)`
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
MCP_SYS("mcp_move_batch", sysMoveBatch)            // bridge: /pd/move_batch (verb move_id, index form)
MCP_SYS("mcp_clear_ids", sysClearIds)              // bridge: /pd/clear, clear_ids, load_content
MCP_SYS("mcp_clear_undo", sysClearUndo)            // bridge: /pd/clear_undo, encapsulate_gop, resetCanvasUndo (batch_atomic and others)
MCP_SYS("mcp_suspend_dsp", sysSuspendDsp)          // bridge: /pd/delete_batch_id
MCP_SYS("mcp_resume_dsp", sysResumeDsp)            // bridge: /pd/delete_batch_id

// ── identity ──
MCP_SYS("mcp_register_id", sysRegisterId)          // bridge: /pd/register_id, open_patch
MCP_SYS("mcp_rename_id", sysRenameId)              // Pd only
MCP_SYS("mcp_get_mappings", sysGetMappings)        // bridge: /pd/get_mappings
MCP_SYS("mcp_census", sysCensus)                   // bridge: /pd/census

// ── id-addressed forms of the legacy /pd verbs (McpSysIdCommands.cpp; removed with them, S9) ──
MCP_SYS("mcp_create_batch_id", sysCreateBatchId)           // bridge: /pd/create_batch_id
MCP_SYS("mcp_create_id", sysCreateId)                      // bridge: /pd/create_id
MCP_SYS("mcp_connect_id", sysConnectId)                    // bridge: /pd/connect_id
MCP_SYS("mcp_disconnect_id", sysDisconnectId)              // bridge: /pd/disconnect_id
MCP_SYS("mcp_delete_id", sysDeleteId)                      // bridge: /pd/delete_id, delete_batch_id
MCP_SYS("mcp_move_id", sysMoveId)                          // bridge: /pd/move_id (and its aliases)
MCP_SYS("mcp_edit_id", sysEditId)                          // bridge: /pd/edit_id
MCP_SYS("mcp_move_batch_id", sysMoveBatchId)               // bridge: /pd/move_id as move_batch_id (the undo/redo replay verb)
MCP_SYS("mcp_connect_batch_id", sysConnectBatchId)         // bridge: /pd/connect_batch_id
MCP_SYS("mcp_disconnect_batch_id", sysDisconnectBatchId)   // bridge: /pd/disconnect_batch_id

#undef MCP_SYS
