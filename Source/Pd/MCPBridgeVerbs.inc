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

// ── pd: files ──
MCP_VERB(pd, "load", pdLoad)
MCP_VERB(pd, "save_content", pdSaveContent)
MCP_VERB(pd, "load_content", pdLoadContent)
MCP_VERB(pd, "save_patch", pdSavePatch)
MCP_VERB(pd, "load_patch", pdLoadPatch)
MCP_VERB(pd, "open_patch", pdOpenPatch)
MCP_VERB(pd, "list_tabs", pdListTabs)
MCP_VERB(pd, "focus_tab", pdFocusTab)
MCP_VERB(pd, "close_tab", pdCloseTab)

// ── pd: record and render ──
MCP_VERB(pd, "record_start", pdRecordStart)
MCP_VERB(pd, "record_stop", pdRecordStop)
MCP_VERB(pd, "render", pdRender)

// ── pd: analysis ──
MCP_VERB(pd, "diagnose", pdDiagnose)
MCP_VERB(pd, "clusters", pdClusters)
MCP_VERB(pd, "perf", pdPerf)
MCP_VERB(pd, "bounds", pdBounds)
MCP_VERB(pd, "collisions", pdCollisions)
MCP_VERB(pd, "wire_occlusions", pdWireOcclusions)
MCP_VERB(pd, "measure_text", pdMeasureText)

// ── pd: layout ──
MCP_VERB(pd, "tidy", pdTidy)
MCP_VERB(pd, "align", pdAlign)
MCP_VERB(pd, "zoom_to_fit", pdZoomToFit)
MCP_VERB(pd, "deoverlap", pdDeoverlap)
MCP_VERB(pd, "layout_guard", pdLayoutGuard)
MCP_VERB(pd, "compose", pdCompose)
MCP_VERB(pd, "pillars", pdPillars)
MCP_VERB(pd, "flow", pdFlow)

// ── pd: encapsulate ──
MCP_VERB(pd, "encapsulate", pdEncapsulate)
MCP_VERB(pd, "encapsulate_to_file", pdEncapsulateToFile)
MCP_VERB(pd, "encapsulate_gop", pdEncapsulateGop)

// ── pd: overlays and capture ──
MCP_VERB(pd, "select", pdSelect)
MCP_VERB(pd, "ai_overlay", pdAiOverlay)
MCP_VERB(pd, "ai_ghost", pdAiGhost)
MCP_VERB(pd, "ai_annotate", pdAiAnnotate)
MCP_VERB(pd, "ai_region", pdAiRegion)
MCP_VERB(pd, "ai_hud", pdAiHud)
MCP_VERB(pd, "get_overlays", pdGetOverlays)
MCP_VERB(pd, "set_overlays", pdSetOverlays)
MCP_VERB(pd, "moodboard", pdMoodboard)
MCP_VERB(pd, "screenshot_canvas", pdScreenshotCanvas)
MCP_VERB(pd, "gem_capture", pdGemCapture)

// ── pd: identity ──
MCP_VERB(pd, "identity_snapshot", pdIdentitySnapshot)
MCP_VERB(pd, "identity_version", pdIdentityVersion)
MCP_VERB(pd, "clear_ids", pdClearIds)
MCP_VERB(pd, "register_id", pdRegisterId)
MCP_VERB(pd, "rename_id", pdRenameId)
MCP_VERB(pd, "rename_id_batch", pdRenameIdBatch)
MCP_VERB(pd, "get_mappings", pdGetMappings)

// ── pd: legacy index and *_id forms ──
MCP_VERB(pd, "create", pdCreate)
MCP_VERB(pd, "create_batch", pdCreateBatch)
MCP_VERB(pd, "connect_batch", pdConnectBatch)
MCP_ALIAS(pd, "connect", pdConnectBatch)
MCP_VERB(pd, "disconnect", pdDisconnect)
MCP_VERB(pd, "disconnect_batch", pdDisconnectBatch)
MCP_VERB(pd, "delete", pdDelete)
MCP_VERB(pd, "delete_batch", pdDeleteBatch)
MCP_VERB(pd, "move_id", pdMoveId)
MCP_ALIAS(pd, "move_batch", pdMoveId)
MCP_ALIAS(pd, "move_batch_id", pdMoveId)
MCP_VERB(pd, "create_id", pdCreateId)
MCP_VERB(pd, "create_batch_id", pdCreateBatchId)
MCP_VERB(pd, "connect_id", pdConnectId)
MCP_VERB(pd, "connect_batch_id", pdConnectBatchId)
MCP_VERB(pd, "disconnect_id", pdDisconnectId)
MCP_VERB(pd, "disconnect_batch_id", pdDisconnectBatchId)
MCP_VERB(pd, "delete_id", pdDeleteId)
MCP_VERB(pd, "delete_batch_id", pdDeleteBatchId)
MCP_VERB(pd, "edit_id", pdEditId)

#undef MCP_SOURCE
#undef MCP_VERB
#undef MCP_ALIAS
#undef MCP_DOMAIN
