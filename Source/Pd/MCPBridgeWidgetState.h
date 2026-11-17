/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Thread phase 2, live widget history (capability `widget_state`; the core and the rules: Source/Mcp/McpWidgetState.h). The state
// receiver: a native CLASS_PD object bound to a widget's state name (`ai-tap-state`, `ai-loop_kick-state`, ...), the same way the
// `ai-ask` receiver and the armed control window bind theirs (MCPBridgeWidgetAsk.h, MCPBridgeControlWindow.h): no canvas object, it
// coexists with the widget's own [r] / pd.Receive and with an armed control window on the same name (Pd keeps a bind list).
//
// It runs wherever Pd dispatches (the audio thread too), under the Pd lock. A widget's state frames all pass here; only a message
// whose selector is `end` is looked at (one pointer compare for every other frame), and only while the server has switched the history
// on (one atomic read). No lock, no allocation, no JUCE. Included by MCPBridgeWidgetAsk.h only.

#pragma once

#include "Mcp/McpWidgetState.h"

#include <chrono>

struct McpStateReceiver {
    t_object obj; // ob_pd at offset 0: bindable
    t_symbol* sym = nullptr;
    mcp::WidgetStateBook* book = nullptr;
    int slot = -1;
};

inline std::int64_t mcpStateNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline t_class* mcpStateReceiverClass = nullptr;
inline t_symbol* mcpStateEndSym = nullptr;

inline void mcpStateReceiverFree(McpStateReceiver* x)
{
    if (x->sym) {
        pd_unbind(&x->obj.ob_pd, x->sym);
        x->sym = nullptr;
    }
}

// `pd.send("ai-tap-state", "end", {rev})` arrives as the selector `end` with one float. Bangs, floats and lists reach the
// anything method too (Pd's defaults), with their own selector: none of them is `end`.
inline void mcpStateReceiverAnything(McpStateReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    if (!x || !x->book || s != mcpStateEndSym) return;
    if (!x->book->watching()) return;
    if (argc < 1 || argv[0].a_type != A_FLOAT) {
        x->book->noteMalformed();
        return;
    }
    x->book->noteEnd(x->slot, static_cast<double>(argv[0].a_w.w_float), mcpStateNowMs());
}

// Caller holds the Pd lock. Idempotent.
inline void ensureMcpStateReceiverClass()
{
    if (mcpStateReceiverClass) return;
    mcpStateEndSym = gensym("end");
    mcpStateReceiverClass = class_new(gensym("mcp_ai_state"), nullptr, reinterpret_cast<t_method>(mcpStateReceiverFree),
        sizeof(McpStateReceiver), CLASS_PD, static_cast<t_atomtype>(0));
    class_addanything(mcpStateReceiverClass, reinterpret_cast<t_method>(mcpStateReceiverAnything));
}

// Binds a receiver to the state name of `widget` for `slot` (the book's claim). Null for a name that has no state name. Caller holds the Pd lock.
inline McpStateReceiver* mcpBindStateReceiver(mcp::WidgetStateBook& book, int slot, char const* widget)
{
    char name[48];
    if (!mcp::widgetStateSymbolName(widget, name, sizeof name)) return nullptr;
    ensureMcpStateReceiverClass();
    auto* rec = reinterpret_cast<McpStateReceiver*>(pd_new(mcpStateReceiverClass));
    rec->sym = gensym(name);
    rec->book = &book;
    rec->slot = slot;
    pd_bind(&rec->obj.ob_pd, rec->sym);
    return rec;
}
