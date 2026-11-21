/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Phase 3 of the ai_widget plan (capability `widget_event`; the core and the rules: Source/Mcp/McpWidgetEvent.h). The `ai-event` receiver:
// while the bridge serves (start() / setReplySinkForTests, until stop()), a native CLASS_PD receiver is bound to the Pd name `ai-event`,
// exactly like the `ai-ask` receiver (MCPBridgeWidgetAsk.h): no canvas object, it coexists with any [r ai-event].
//
//   ai-event <widget> <rev> <paddr> use|no|back   ->   /pd/widget_ask <use|no|back> <canvas> <widget> <rev>   (sent by the pump)
//
// The receiver runs wherever Pd dispatches (the audio thread too), under the Pd lock: it only reads the atoms and calls
// mcp::WidgetEventQueue::offer (no lock, no allocation, no JUCE: scripts/test-bridge-safety.sh greps this file for it). The address is
// the sending object's address as TEXT (`p` and hex digits): compared and told apart per instance, never read as a pointer. Anything after
// the word is dropped; a word, a number or an address of the wrong shape is counted and not queued. The message-thread pump
// (MCPBridge::pumpWidgetAsks) pops the events. Included by MCPBridgeWidgetAsk.h only.

#pragma once

#include "Mcp/McpWidgetEvent.h"

#include <chrono>

struct McpEventReceiver {
    t_object obj; // ob_pd at offset 0: bindable
    t_symbol* sym = nullptr;
    mcp::WidgetEventQueue* queue = nullptr;
};

inline t_class* mcpEventReceiverClass = nullptr;

inline std::int64_t mcpEventNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void mcpEventReceiverFree(McpEventReceiver* x)
{
    if (x->sym) {
        pd_unbind(&x->obj.ob_pd, x->sym);
        x->sym = nullptr;
    }
}

inline char const* mcpEventSymbolText(t_atom const& a)
{
    return (a.a_type == A_SYMBOL && a.a_w.w_symbol) ? a.a_w.w_symbol->s_name : nullptr;
}

// `head` = the first word (the widget name); argv = the atoms after it: <rev> <paddr> <word>.
inline void mcpEventOffer(McpEventReceiver* x, t_symbol* head, int argc, t_atom* argv)
{
    if (!x || !x->queue) return;
    char const* widget = head ? head->s_name : nullptr;
    double const rev = (argc >= 1 && argv[0].a_type == A_FLOAT) ? static_cast<double>(argv[0].a_w.w_float) : -1.0;
    char const* addr = argc >= 2 ? mcpEventSymbolText(argv[1]) : nullptr;
    char const* word = argc >= 3 ? mcpEventSymbolText(argv[2]) : nullptr;
    x->queue->offer(widget, rev, addr, word, mcpEventNowMs());
}

// pd.send("ai-event", "list", { "ai.draw", 7, "p55d0", "use" }) arrives as a list (or a symbol, with the widget alone); [s ai-event] with
// `ai.draw 7 p55d0 use` arrives as the selector ai.draw. A bang or a float names no widget: counted as malformed.
inline void mcpEventReceiverAnything(McpEventReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    if (s == &s_list || s == &s_symbol) {
        if (argc >= 1 && argv[0].a_type == A_SYMBOL)
            mcpEventOffer(x, argv[0].a_w.w_symbol, argc - 1, argv + 1);
        else
            mcpEventOffer(x, nullptr, 0, nullptr);
        return;
    }
    mcpEventOffer(x, s, argc, argv);
}

inline void mcpEventReceiverList(McpEventReceiver* x, t_symbol*, int argc, t_atom* argv)
{
    mcpEventReceiverAnything(x, &s_list, argc, argv);
}

inline void mcpEventReceiverBang(McpEventReceiver* x)
{
    mcpEventOffer(x, nullptr, 0, nullptr);
}

inline void mcpEventReceiverFloat(McpEventReceiver* x, t_float)
{
    mcpEventOffer(x, nullptr, 0, nullptr);
}

// Caller holds the Pd lock. Idempotent.
inline void ensureMcpEventReceiverClass()
{
    if (mcpEventReceiverClass) return;
    mcpEventReceiverClass = class_new(gensym("mcp_ai_event"), nullptr, reinterpret_cast<t_method>(mcpEventReceiverFree),
        sizeof(McpEventReceiver), CLASS_PD, static_cast<t_atomtype>(0));
    class_addbang(mcpEventReceiverClass, mcpEventReceiverBang);
    class_addfloat(mcpEventReceiverClass, reinterpret_cast<t_method>(mcpEventReceiverFloat));
    class_addlist(mcpEventReceiverClass, reinterpret_cast<t_method>(mcpEventReceiverList));
    class_addanything(mcpEventReceiverClass, reinterpret_cast<t_method>(mcpEventReceiverAnything));
}

// Binds a receiver on `ai-event` for `queue`. Caller holds the Pd lock.
inline McpEventReceiver* mcpBindEventReceiver(mcp::WidgetEventQueue& queue)
{
    ensureMcpEventReceiverClass();
    auto* rec = reinterpret_cast<McpEventReceiver*>(pd_new(mcpEventReceiverClass));
    rec->sym = gensym("ai-event");
    rec->queue = &queue;
    pd_bind(&rec->obj.ob_pd, rec->sym);
    return rec;
}
