/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Proof plan step 3, part 3c (capability `widget_ask`): the `ai-ask` receiver. While the bridge serves (start() /
// setReplySinkForTests, until stop()), a native CLASS_PD receiver is bound to the Pd name `ai-ask`, the same way the
// armed control window binds its probe (MCPBridgeControlWindow.h): no canvas object, it coexists with any [r ai-ask].
//
//   ai-ask <widget> <rev>    ->  /pd/widget_ask ask   <canvas> <widget> <rev>
//   ai-ask hello <widget>    ->  /pd/widget_ask hello <canvas> <widget> 0
// Anything after the rev is dropped (review M4: no free text from a patch into the AI's prompt).
// <canvas> is the tab the widget sits in (mcpWidgetAskCanvas below: found by the pump, the message names none), or
// `main` (the focused tab) when it cannot be told.
//
// The receiver runs wherever Pd dispatches (the audio thread too), under the Pd lock: it only reads the atoms and
// calls mcp::WidgetAskQueue::offer (no lock, no allocation, no JUCE). The pump below runs on the message
// thread every kPumpMs, pops the events and sends them (MCPBridge::pumpWidgetAsks). Bounds and the rate limit
// (1 ask per widget per 2 s): Source/Mcp/McpWidgetAsk.h. Included by MCPBridge.cpp only.

#pragma once

#include "Mcp/McpWidgetAsk.h"

#include <chrono>

struct McpAskReceiver {
    t_object obj; // ob_pd at offset 0: bindable
    t_symbol* sym = nullptr;
    mcp::WidgetAskQueue* queue = nullptr;
};

inline t_class* mcpAskReceiverClass = nullptr;

inline std::int64_t mcpAskNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// `head` = the first word (a widget name, or "hello"); argv = the atoms after it.
inline void mcpAskOffer(McpAskReceiver* x, t_symbol* head, int argc, t_atom* argv)
{
    if (!x || !x->queue) return;
    std::int64_t const now = mcpAskNowMs();
    char const* first = head ? head->s_name : nullptr;
    if (first && std::strcmp(first, "hello") == 0) {
        char const* widget = (argc >= 1 && argv[0].a_type == A_SYMBOL && argv[0].a_w.w_symbol) ? argv[0].a_w.w_symbol->s_name : nullptr;
        x->queue->offer(mcp::WidgetAskKind::Hello, widget, 0, now);
        return;
    }
    double const rev = (argc >= 1 && argv[0].a_type == A_FLOAT) ? static_cast<double>(argv[0].a_w.w_float) : -1.0;
    x->queue->offer(mcp::WidgetAskKind::Ask, first, rev, now);
}

inline void mcpAskReceiverFree(McpAskReceiver* x)
{
    if (x->sym) {
        pd_unbind(&x->obj.ob_pd, x->sym);
        x->sym = nullptr;
    }
}

// [s ai-ask] with `ai.draw 7 bass` arrives as the selector ai.draw; pd.send("ai-ask", "list", {...}) and a message
// starting with a symbol as a list or symbol. A bang or a float names no widget: counted as malformed.
inline void mcpAskReceiverAnything(McpAskReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    if (s == &s_list || s == &s_symbol) {
        if (argc >= 1 && argv[0].a_type == A_SYMBOL)
            mcpAskOffer(x, argv[0].a_w.w_symbol, argc - 1, argv + 1);
        else
            mcpAskOffer(x, nullptr, 0, nullptr);
        return;
    }
    mcpAskOffer(x, s, argc, argv);
}

inline void mcpAskReceiverList(McpAskReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    (void)s;
    mcpAskReceiverAnything(x, &s_list, argc, argv);
}

inline void mcpAskReceiverBang(McpAskReceiver* x)
{
    mcpAskOffer(x, nullptr, 0, nullptr);
}

inline void mcpAskReceiverFloat(McpAskReceiver* x, t_float)
{
    mcpAskOffer(x, nullptr, 0, nullptr);
}

// Caller holds the Pd lock. Idempotent.
inline void ensureMcpAskReceiverClass()
{
    if (mcpAskReceiverClass) return;
    mcpAskReceiverClass = class_new(gensym("mcp_ai_ask"), nullptr, reinterpret_cast<t_method>(mcpAskReceiverFree),
        sizeof(McpAskReceiver), CLASS_PD, static_cast<t_atomtype>(0));
    class_addbang(mcpAskReceiverClass, mcpAskReceiverBang);
    class_addfloat(mcpAskReceiverClass, reinterpret_cast<t_method>(mcpAskReceiverFloat));
    class_addlist(mcpAskReceiverClass, reinterpret_cast<t_method>(mcpAskReceiverList));
    class_addanything(mcpAskReceiverClass, reinterpret_cast<t_method>(mcpAskReceiverAnything));
}

// ── The canvas of an ask ────────────────────────────────────────────────────────────────────────────────────────────
// The ai-ask message names no canvas, so the receiver cannot know the patch a widget sits in; the pump finds it (message
// thread, under the Pd lock): the open ROOT canvas (the tab) whose patch (subpatches and abstractions included) holds a
// box of the widget's class (`ai.tap`; pd-lua's shadow class `ai.tap:gfx`, the one a GUI widget really is; or an abstraction
// file `ai.tap` / `ai.tap.pd`), named by its identity wire key: the `key` of that tab's /pd/list_tabs row, the name every
// canvas verb resolves. Not found (a looper: its class is ai-loop2, its word ai.loop_<name>; a box not placed yet; a
// [clone] instance) or found in more than one tab (which one asked is not known): `main`, the focused tab, as before.
inline constexpr int kAskCanvasWalkBudget = 200000; // boxes visited per lookup
inline constexpr int kAskCanvasMaxDepth = 32;

inline bool mcpCanvasHoldsWidget(t_canvas* c, std::string const& cls, std::string const& gfx, std::string const& file, int depth, int& budget)
{
    if (!c || depth > kAskCanvasMaxDepth) return false;
    for (t_gobj* g = c->gl_list; g; g = g->g_next) {
        if (--budget < 0) return false;
        t_class* const k = pd_class(&g->g_pd);
        if (k == canvas_class) {
            juce::String abstraction;
            if (pd::getAbstractionFileName(g, abstraction)
                && (abstraction == juce::String(cls) || abstraction == juce::String(file))) return true;
            if (mcpCanvasHoldsWidget(reinterpret_cast<t_canvas*>(g), cls, gfx, file, depth + 1, budget)) return true;
            continue;
        }
        char const* const name = class_getname(k);
        if (name && (cls == name || gfx == name)) return true;
    }
    return false;
}

// Caller holds no Pd lock: takes it. Never creates a name the AI could not resolve (the key is the identity wire key).
inline juce::String mcpWidgetAskCanvas(PluginProcessor& processor, char const* widget)
{
    if (!widget || !pd_this) return "main";
    std::string const cls(widget);
    std::string const gfx = cls + ":gfx";
    std::string const file = cls + ".pd";
    McpHost::PdLockScope const lock(processor);
    juce::String key;
    int holders = 0;
    int budget = kAskCanvasWalkBudget;
    for (t_canvas* root = pd_this->pd_canvaslist; root && budget > 0; root = root->gl_next) {
        if (!mcpCanvasHoldsWidget(root, cls, gfx, file, 0, budget)) continue;
        if (++holders > 1) return "main";
        key = juce::String(processor.getMcp()->identity.keyOf(root));
    }
    return holders == 1 && key.isNotEmpty() ? key : juce::String("main");
}

// The bridge's part: the queue, the binding and the message-thread pump.
struct McpWidgetAskState {
    static constexpr int kPumpMs = 50;

    struct Pump final : juce::Timer {
        explicit Pump(MCPBridge& b) : bridge(b) {}
        void timerCallback() override { bridge.pumpWidgetAsks(); }
        MCPBridge& bridge;
    };

    explicit McpWidgetAskState(MCPBridge& b) : pump(b) {}

    mcp::WidgetAskQueue queue;
    McpAskReceiver* receiver = nullptr; // bound to `ai-ask` while the bridge serves
    Pump pump;
};
