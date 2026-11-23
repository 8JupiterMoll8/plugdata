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
//   ai-ask focus <widget> <rev> <ptr>  ->  no event for the server: the widget's chip "AI..." selects that widget on its canvas and opens the
//                            selection pill (MCPBridge::focusWidgetPill, MCPBridgeWidgetFocus.h; the pump below waits for the mouse button
//                            to go up first). The ask itself comes later, from the pill: Enter on no text tells the widget `ask <ptr>`.
// Anything after the rev is dropped (review M4: no free text from a patch into the AI's prompt).
// <canvas> is the tab the widget sits in (mcpWidgetAskCanvases: found by the pump, the message names none), or
// `main` (the focused tab) when it cannot be told.
//
// The receiver runs wherever Pd dispatches (the audio thread too), under the Pd lock: it only reads the atoms and
// calls mcp::WidgetAskQueue::offer (no lock, no allocation, no JUCE). The pump below runs on the message
// thread every kPumpMs, pops the events and sends them (MCPBridge::pumpWidgetAsks). Bounds and the rate limit
// (1 ask per widget per 2 s): Source/Mcp/McpWidgetAsk.h. Included by MCPBridge.cpp only.
//
// The same pump also carries the live widget history (MCPBridgeWidgetState.h): a receiver on each widget's state name, and the
// `/pd/widget_ask state ...` events it makes after 1 s of quiet (kind `state`: the kind word is free, the event address is the same),
// and the artist's answers to a proposal (MCPBridgeWidgetEvent.h, capability widget_event): a receiver on `ai-event` and the
// `/pd/widget_ask <use|no|back|another> ...` events it makes.

#pragma once

#include "Mcp/McpWidgetAsk.h"
#include "Mcp/McpWidgetFocus.h"
#include "MCPBridgeWidgetAskCanvas.h"
#include "MCPBridgeWidgetEvent.h"
#include "MCPBridgeWidgetState.h"

#include <chrono>
#include <functional>
#include <string>

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
    if (first && std::strcmp(first, "focus") == 0) {
        // focus <widget> <rev> <ptr>: the pointer is a hex symbol (a float or a missing one names no object: malformed)
        char const* widget = (argc >= 1 && argv[0].a_type == A_SYMBOL && argv[0].a_w.w_symbol) ? argv[0].a_w.w_symbol->s_name : nullptr;
        double const rev = (argc >= 2 && argv[1].a_type == A_FLOAT) ? static_cast<double>(argv[1].a_w.w_float) : -1.0;
        std::uint64_t ptr = 0;
        if (argc >= 3 && argv[2].a_type == A_SYMBOL && argv[2].a_w.w_symbol) mcp::widgetFocusPtrParse(argv[2].a_w.w_symbol->s_name, ptr);
        x->queue->offer(mcp::WidgetAskKind::Focus, widget, rev, now, ptr);
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

// The canvas of an ask (the tab the widget sits in) is found by the pump: MCPBridgeWidgetAskCanvas.h.

// The bridge's part: the queue, the binding and the message-thread pump.
struct McpWidgetAskState {
    static constexpr int kPumpMs = 50;
    static constexpr std::int64_t kStatePostGapMs = 1000; // the state refusals reach the console at most this often

    struct Pump final : juce::Timer {
        explicit Pump(MCPBridge& b) : bridge(b) {}
        void timerCallback() override { bridge.pumpWidgetAsks(); }
        MCPBridge& bridge;
    };

    explicit McpWidgetAskState(MCPBridge& b) : pump(b) {}

    mcp::WidgetAskQueue queue;
    McpAskReceiver* receiver = nullptr; // bound to `ai-ask` while the bridge serves
    // The artist's answers to a proposal (capability widget_event): `ai-event <widget> <rev> <paddr> use|no|back`, bound while the bridge serves.
    mcp::WidgetEventQueue events;
    McpEventReceiver* eventReceiver = nullptr;
    mcp::WidgetEventLogGate eventLog; // the pump's console line for refused events: what is wrong only, at most one in 5 s
    // The chip's focus (`ai-ask focus ...`): the newest one waits here until the mouse button is up (the chip fires on the press; the
    // canvas must not change its mode under a held button), at most kFocusWaitMs; then it is carried out once, answered or not.
    static constexpr std::int64_t kFocusWaitMs = 1200;
    struct PendingFocus {
        bool active = false;
        std::string widget;
        std::uint64_t ptr = 0;
        std::int64_t sinceMs = 0;
    } pendingFocus;
    // Tests: stands in for the GUI (McpGuiHost::focusWidgetPill); it is told the canvas and box each candidate view would select.
    std::function<bool(t_canvas*, t_gobj*, mcp::WidgetPillTarget const&)> focusSinkForTests;
    // Tests: stands in for "a mouse button is down" (the headless instance has no mouse).
    std::function<bool()> mouseHeldForTests;
    // Live widget history: one receiver per claimed slot of the book (the fixed widgets at bind, a named looper at its hello).
    mcp::WidgetStateBook states;
    McpStateReceiver* stateReceivers[mcp::kWidgetStateSlots] = {};
    std::int64_t statePostedMs = 0;
    Pump pump;
};
