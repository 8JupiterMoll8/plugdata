/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Phase 3 of the ai_widget plan (capability `widget_call`; the core and the rules: Source/Mcp/McpWidgetCall.h). The collector: a native
// CLASS_PD object bound to a widget's state name (`ai-draw-state`, `ai-loop_kick-state`) for the length of ONE /pd/widget_call, the same
// way the armed control window and the live history's state receivers bind theirs (MCPBridgeControlWindow.h, MCPBridgeWidgetState.h): no
// canvas object, it coexists with the widget's own pd.Receive (a "pdlua proxy receive") and [r] on that name, because Pd keeps a bind list.
//
// It is bound and unbound INSIDE the handler (MCPBridge::pdWidgetCall, MCPBridgeCmdControl.cpp), under the Pd lock, by a guard whose
// destructor unbinds: no frame can reach it from the audio thread before or after the call, and an exception cannot leave it bound. While
// it is bound it only appends to the fixed storage of the call's WidgetCallLog (reserved before the bind): no lock, no allocation, no JUCE
// (scripts/test-bridge-safety.sh greps this file for it). It records every selector the widget sends, the way the control window does:
// `d1 ...` as selector d1, a list as `list`, a bang as `bang`.
//
// Included by MCPBridgeOwnReceivers.h (the verbs that ask who listens on a name leave the bridge's own receivers out).

#pragma once

#include "Mcp/McpWidgetCall.h"

struct McpCallCollector {
    t_object obj; // ob_pd at offset 0: bindable
    t_symbol* sym = nullptr;
    mcp::WidgetCallLog* log = nullptr;
};

inline t_class* mcpCallCollectorClass = nullptr;

inline void mcpCallCollectorFree(McpCallCollector* x)
{
    if (x->sym) {
        pd_unbind(&x->obj.ob_pd, x->sym);
        x->sym = nullptr;
    }
}

// One message of the widget: its selector and its atoms. A pointer atom (nobody sends one) is a question mark. The atoms past the 33rd are
// not walked: the 33rd already says `truncated`.
inline void mcpCallCollectorRecord(McpCallCollector* x, char const* selector, int argc, t_atom* argv)
{
    if (!x || !x->log) return;
    x->log->beginFrame(selector);
    int const n = argc < mcp::kWidgetCallMaxAtoms + 1 ? argc : mcp::kWidgetCallMaxAtoms + 1;
    for (int i = 0; i < n; ++i) {
        if (argv[i].a_type == A_FLOAT) x->log->addNumber(static_cast<double>(argv[i].a_w.w_float));
        else if (argv[i].a_type == A_SYMBOL) x->log->addSymbol(argv[i].a_w.w_symbol ? argv[i].a_w.w_symbol->s_name : "");
        else x->log->addSymbol("?");
    }
}

inline void mcpCallCollectorBang(McpCallCollector* x)
{
    mcpCallCollectorRecord(x, "bang", 0, nullptr);
}

inline void mcpCallCollectorFloat(McpCallCollector* x, t_float f)
{
    t_atom a;
    SETFLOAT(&a, f);
    mcpCallCollectorRecord(x, "float", 1, &a);
}

inline void mcpCallCollectorSymbol(McpCallCollector* x, t_symbol* s)
{
    t_atom a;
    SETSYMBOL(&a, s);
    mcpCallCollectorRecord(x, "symbol", 1, &a);
}

inline void mcpCallCollectorList(McpCallCollector* x, t_symbol*, int argc, t_atom* argv)
{
    mcpCallCollectorRecord(x, "list", argc, argv);
}

inline void mcpCallCollectorAnything(McpCallCollector* x, t_symbol* s, int argc, t_atom* argv)
{
    mcpCallCollectorRecord(x, s && s->s_name ? s->s_name : "", argc, argv);
}

// Caller holds the Pd lock. Idempotent.
inline void ensureMcpCallCollectorClass()
{
    if (mcpCallCollectorClass) return;
    mcpCallCollectorClass = class_new(gensym("mcp_widget_call"), nullptr, reinterpret_cast<t_method>(mcpCallCollectorFree),
        sizeof(McpCallCollector), CLASS_PD, static_cast<t_atomtype>(0));
    class_addbang(mcpCallCollectorClass, mcpCallCollectorBang);
    class_addfloat(mcpCallCollectorClass, reinterpret_cast<t_method>(mcpCallCollectorFloat));
    class_addsymbol(mcpCallCollectorClass, reinterpret_cast<t_method>(mcpCallCollectorSymbol));
    class_addlist(mcpCallCollectorClass, reinterpret_cast<t_method>(mcpCallCollectorList));
    class_addanything(mcpCallCollectorClass, reinterpret_cast<t_method>(mcpCallCollectorAnything));
}

// The collector, bound from the constructor to the destructor. Caller holds the Pd lock for the whole life of the guard, and made `state`
// (gensym) and the log's room BEFORE it: nothing is made while the collector is bound.
struct McpCallCollectorGuard {
    McpCallCollectorGuard(mcp::WidgetCallLog& log, t_symbol* state)
    {
        ensureMcpCallCollectorClass();
        collector = reinterpret_cast<McpCallCollector*>(pd_new(mcpCallCollectorClass));
        collector->sym = state;
        collector->log = &log;
        pd_bind(&collector->obj.ob_pd, state);
    }
    ~McpCallCollectorGuard()
    {
        if (collector) pd_free(&collector->obj.ob_pd); // unbinds (mcpCallCollectorFree)
    }
    McpCallCollectorGuard(McpCallCollectorGuard const&) = delete;
    McpCallCollectorGuard& operator=(McpCallCollectorGuard const&) = delete;

    McpCallCollector* collector = nullptr;
};
