/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Everything a bridge command file needs (Plan 3 P3.3): the includes the handlers
// were written against, the Pd C declarations they use, and the few helpers that
// more than one command file shares. MCPBridge.cpp and every MCPBridgeCmd<Domain>.cpp
// include this and nothing else of the bridge's own.

#pragma once

#include "MCPBridge.h"
#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Mcp/McpGuiHost.h" // all GUI work (Plan 3 P3.2 S6): no plugdata GUI header here
#include "Mcp/McpGuiFields.h" // t_fake_knob & co. reads (generated AllGuis.h)
#include "Constants.h" // overlay bits (Origin ... AIHud)
#include <g_all_guis.h>       // t_slider live fields (x_min/x_max/x_fval) for obj_get
#include "Pd/Interface.h"
#include "Mcp/PdOps.h" // fork Pd ops (removeObjects routing, audio-thread delete, clear)
#include "McpIemArgs.h" // IEM GUI short-form creation-arg guard (shared)
#include "MCPBridgeSafety.h" // loopback + output-path guards (Report F6)
#include "MCPBridgeHandoff.h" // safe timed waits on queued lambdas (Report F3)
#include "MCPIdentity.h" // renameStableId (finding C6)
#include "MCPJson.h" // jsonEscape for hand-built JSON (finding C6)
#include "MCPBridgeInternal.h" // getArgString/getArgFloat/jsonEscape/... shared with the command files
#include "Utility/SettingsFile.h" // compiledMode (hvcc_mode) in /pd/perf
#include "Utility/Config.h"
#include "../../Libraries/fftw3/api/fftw3.h"
#include "Pd/Library.h"

#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <map>

#include <algorithm>

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
#include <s_inter.h>
#include <g_all_guis.h> // t_iemgui x_snd/x_rcv for screenshot GUI-widget labels
#include <m_imp.h>      // obj_starttraverseoutlet/obj_nexttraverseoutlet (duplicate-wire check)

extern "C" {
extern t_class* clone_class; // defined in g_clone.c (pd core)
extern int clone_get_n(t_gobj*);
extern t_glist* clone_get_instance(t_gobj*, int);
}

extern t_class *text_class;

struct _outlet
{
    t_object *o_owner;
    struct _outlet *o_next;
    t_outconnect *o_connections;
    t_symbol *o_sym;
};
}

// ── Undo-stack safety ──────────────────────────────────────────────
// Free a canvas's Pd undo queue after an MCP mutation that removes or
// replaces objects (delete / clear / load).
//
// Why: MCP mutation paths (pasteDirect for create, removeObjectsAudioThread
// for delete) and the wholesale clear/load paths intentionally do NOT
// register Pd undo actions. But Pd's undo entries reference their target
// objects by *index* (canvas_undo_apply stores u_index, resolved via
// glist_nth; create/recreate/cut do the same). Deleting or clearing objects
// shifts or frees those targets, leaving the queue holding stale
// t_undo_action nodes whose data/indices no longer match the live list. The
// next undo/redo walks them and dereferences freed memory — the observed
// "unsupported undo command <garbage int>" heap-corruption crash.
//
// Since MCP never contributes correct undo actions, the only safe state for
// the queue after such a mutation is EMPTY. canvas_undo_free() must run on
// the Pd scheduler thread, so this routes through
// receiveSysMessage("mcp_clear_undo") — the exact proven-safe path used by
// the /pd/clear_undo action (PluginProcessor mcp_clear_undo case).
inline void resetCanvasUndo(PluginProcessor* proc, juce::String const& canvasName)
{
    if (!proc) return;
    proc->enqueueFunctionAsync([proc, canvasName]() {
        SmallArray<pd::Atom> atoms;
        atoms.add(pd::Atom(proc->generateSymbol(canvasName)));
        proc->receiveSysMessage("mcp_clear_undo", atoms);
    });
}

// ── PRD_OBJ_GET_SET P4: shadow of values posted to named buses (/param) ──────
// Written by the /param domain, read by /pd/param_get.
inline std::mutex s_paramShadowMutex;
inline std::unordered_map<std::string, std::vector<t_atom>> s_paramShadow;
