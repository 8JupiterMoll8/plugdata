/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The chip -> selection pill plumbing, the parts that need neither Pd nor JUCE (unit-tested by Tests/mcp/widget_focus_test.cpp
// through scripts/test-bridge-safety.sh).
//
// The artist's decision (2026-11-18): there is ONE way to talk to the AI about something on the canvas: select it, the selection
// pill opens, type, Enter. An ai.* widget's ask chip ("AI...") is a shortcut for exactly that:
//   chip press            the widget sends `ai-ask focus <widget> <rev> <ptr>` (McpWidgetAsk.h); the bridge finds the box the pointer
//                         belongs to in the open patches, selects it on its canvas and opens the pill with the cursor in it
//                         (MCPBridgeWidgetFocus.h, CanvasMcpLayer::focusPillOn); it tells the widget `focused <ptr>` when it did
//   typed text + Enter    a normal pill message with that selection (the server attaches the widget's state: widgets=ai.tap@<rev>)
//   Enter on no text      "complete it": the selected widget is told `ask <ptr>` and asks as its chip did (`ai-ask <widget> <rev>`,
//                         the existing /pd/widget_ask path, the note `complete <widget> (rev n)`)
//   Escape                close, nothing sent
// A chip may add `<x> <y> <w> <h> [subject <text...>]` to its focus word (McpPillDock.h): the pill then opens DOCKED in the widget's own strip,
// with the subject as its first chip, and grows downward when the words wrap. Without them the pill floats under the selection as before.
// A widget that hears no `focused` within a moment (an older fork, a canvas that cannot show the pill) asks at once, as before.

#pragma once

#include "McpPillDock.h"
#include "McpWidgetState.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace mcp {

enum class PillKey { Enter, Escape };
enum class PillOutcome { SendPrompt, CompleteWidget, Dismiss };

// What a key on the selection pill does. `promptEmpty`: no text typed (after trimming); `selectionIsWidget`: the one selected box is an
// askable ai.* widget (only asked for when selectedCount is 1).
inline PillOutcome pillAction(PillKey key, bool promptEmpty, int selectedCount, bool selectionIsWidget)
{
    if (key == PillKey::Escape) return PillOutcome::Dismiss;
    if (!promptEmpty) return PillOutcome::SendPrompt;
    if (selectedCount == 1 && selectionIsWidget) return PillOutcome::CompleteWidget;
    return PillOutcome::Dismiss;
}

// The widgets that show an ask chip and have one box per instance: the fixed names of the history book except the loopers (a looper's
// word names an instance: ai.loop_<name>, so it cannot be told from a box's class).
inline bool widgetPillAskable(std::string const& word)
{
    if (word.compare(0, 7, "ai.loop") == 0) return false;
    for (char const* fixed : kWidgetStateFixed)
        if (word == fixed) return true;
    return false;
}

// The widget a box is, by its class or abstraction name: `ai.tap`, pd-lua's shadow class `ai.tap:gfx`, the legacy `ai-tap` /
// `ai-tap:gfx`, or the wrapper abstraction's file `ai.dice.pd`. "" when it is none of the askable widgets.
inline std::string widgetOfBoxName(char const* name)
{
    if (!name || !*name) return {};
    std::string n(name);
    auto const endsWith = [&n](char const* suffix) {
        std::size_t const len = std::strlen(suffix);
        return n.size() > len && n.compare(n.size() - len, len, suffix) == 0;
    };
    if (endsWith(":gfx")) n.resize(n.size() - 4);
    else if (endsWith(".pd")) n.resize(n.size() - 3);
    if (n.compare(0, 3, "ai-") == 0) n[2] = '.';
    if (!widgetAskNameOk(n.c_str()) || !widgetPillAskable(n)) return {};
    return n;
}

// Is a box of this class name the pd-lua object of `widget` (ai.dice <- ai-dice, ai-dice:gfx, ai.dice, ai.dice:gfx)? Used to find the
// object inside a wrapper abstraction.
inline bool isWidgetObjectName(char const* name, char const* widget)
{
    if (!name || !widget) return false;
    std::string n(name);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ":gfx") == 0) n.resize(n.size() - 4);
    if (n.compare(0, 3, "ai-") == 0) n[2] = '.';
    return n == widget;
}

// The widget the pill acts on: its word and the pd-lua object's address (compared with `ask <ptr>` / `focused <ptr>` words, never
// dereferenced by anything that reads it from a message).
struct WidgetPillTarget {
    std::string widget;
    std::uint64_t luaPtr = 0;
    PillDock dock; // what the chip's focus word asked for: dock the pill in this widget-local strip, with this subject chip (docked = false: the floating pill)
    bool valid() const { return !widget.empty() && luaPtr != 0; }
};

} // namespace mcp
