// The OSC verb table of the MCP bridge (Plan 3 P3.3). One row per verb; this
// file is the single list. It is included, with the macros below defined, by
//   MCPBridge.h           declares each handler as a private member function
//   MCPBridgeRegistry.cpp fills the command table
//   Tests/mcp/registry_test.cpp  checks the rows without JUCE
// and parsed as text by the TS wire-inventory extractor
// (scripts/protocol/extract-wire-inventory.ts), so keep one row per line and
// write the verb as a string literal.
//
//   MCP_SOURCE(domain, Path|PathOrArg0|Arg0)  where the verb of a message comes from
//   MCP_VERB(domain, "verb", handler)         one handler per verb
//   MCP_ALIAS(domain, "verb", handler)        another name for an existing handler
//   MCP_DOMAIN(domain, handler)               a verb-agnostic domain (param, chat)
//
// A handler is `void handler(const juce::String& action, const juce::OSCMessage& msg)`
// and keeps the body it had inside handle<Domain>Domain; `action` is the verb
// the message was dispatched under. An includer defines only the macros it
// needs; the rest expand to nothing. Rows move in here one domain at a time
// (P3.3 S2-S7); until a verb has a row, handle<Domain>Domain still serves it.

#ifndef MCP_SOURCE
#define MCP_SOURCE(domain, source)
#endif
#ifndef MCP_VERB
#define MCP_VERB(domain, verb, handler)
#endif
#ifndef MCP_ALIAS
#define MCP_ALIAS(domain, verb, handler)
#endif
#ifndef MCP_DOMAIN
#define MCP_DOMAIN(domain, handler)
#endif

// ── Domains: how oscMessageReceived finds the verb ──
MCP_SOURCE(pd, Path)
MCP_SOURCE(param, Path)
MCP_SOURCE(trigger, Path)
MCP_SOURCE(telemetry, Arg0)
MCP_SOURCE(array, PathOrArg0)
MCP_SOURCE(bridge, PathOrArg0)
MCP_SOURCE(morph, PathOrArg0)
MCP_SOURCE(meter, PathOrArg0)
MCP_SOURCE(transport, PathOrArg0)
MCP_SOURCE(seq, PathOrArg0)
MCP_SOURCE(chat, PathOrArg0)

// ── Verbs (rows move in per domain, P3.3 S2-S7) ──

// ── pd: system ──
MCP_VERB(pd, "ping", pdPing)
MCP_VERB(pd, "clear_console", pdClearConsole)
MCP_VERB(pd, "mcp_reload_lua", pdMcpReloadLua)
MCP_VERB(pd, "dsp", pdDsp)
MCP_VERB(pd, "update_dsp", pdUpdateDsp)
MCP_VERB(pd, "vis", pdVis)
MCP_VERB(pd, "clear", pdClear)
MCP_VERB(pd, "clear_undo", pdClearUndo)
MCP_VERB(pd, "clear_selection", pdClearSelection)
MCP_VERB(pd, "undo", pdUndo)
MCP_VERB(pd, "redo", pdRedo)

// ── pd: objects ──
MCP_VERB(pd, "obj_set", pdObjSet)
MCP_VERB(pd, "obj_relabel", pdObjRelabel)
MCP_VERB(pd, "obj_set_batch", pdObjSetBatch)
MCP_VERB(pd, "param_get", pdParamGet)
MCP_VERB(pd, "params", pdParams)
MCP_VERB(pd, "param_set_name", pdParamSetName)
MCP_VERB(pd, "obj_get_batch", pdObjGetBatch)
MCP_VERB(pd, "obj_get", pdObjGet)
MCP_VERB(pd, "census", pdCensus)
MCP_VERB(pd, "typeof", pdTypeof)
MCP_VERB(pd, "ports", pdPorts)
MCP_VERB(pd, "connections", pdConnections)
MCP_VERB(pd, "dump", pdDump)

// ── pd: docs ──
MCP_VERB(pd, "doc_get", pdDocGet)
MCP_ALIAS(pd, "doc", pdDocGet)
MCP_VERB(pd, "doc_search", pdDocSearch)
MCP_VERB(pd, "doc_dump", pdDocDump)
MCP_VERB(pd, "library_info", pdLibraryInfo)
MCP_VERB(pd, "ui_open_reference", pdUiOpenReference)
MCP_VERB(pd, "ui_open_help", pdUiOpenHelp)
MCP_VERB(pd, "viewport", pdViewport)

// ── pd: batch ──
MCP_VERB(pd, "batch_atomic", pdBatchAtomic)

#undef MCP_SOURCE
#undef MCP_VERB
#undef MCP_ALIAS
#undef MCP_DOMAIN
