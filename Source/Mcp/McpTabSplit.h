// The split view for the AI (PARKED 2026-11-22, "split view for the AI"; capability tab_split), the JUCE-free part. PlugData already has
// a split view (TabComponent: the tab menu's "Split left / Split right"); the AI had no way to use it, so a tab it opened landed in
// whichever side was active. The verb /pd/tab_split <corrId> <left|right|none> [tab] puts a tab on one side or goes back to one view;
// the moves are the menu's own (TabComponent::mcpMoveToBar / mcpUnsplit, message thread), and this header decides what is allowed
// and what the reply says. Tested by Tests/mcp/tab_split_test.cpp.
//
//  - Request: the word the server sends. left / right = put the named tab on that side; none (also unsplit, off) = one view again;
//  - planMove mirrors the menu (canSplitTab = a split exists, or the tab's bar holds two tabs): with no split either side needs two
//    tabs (the tab goes to its side, the others to the other); with a split a tab already on the asked side stays and one on the
//    other side moves over; the only tab of a bar can still move (the bars then merge, as the menu does);
//  - the reply reads the layout back AFTER the move (the caller never assumes the move happened): per tab its side, plus whether a split shows.
#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::tabsplit {

enum class Request { Left, Right, Unsplit };

/** left / right / none (also unsplit, off), any case, blanks around trimmed; anything else: no request. */
inline std::optional<Request> parseRequest(std::string_view word)
{
    auto const isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!word.empty() && isBlank(word.front())) word.remove_prefix(1);
    while (!word.empty() && isBlank(word.back())) word.remove_suffix(1);
    std::string lower;
    for (char const c : word) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "left") return Request::Left;
    if (lower == "right") return Request::Right;
    if (lower == "none" || lower == "unsplit" || lower == "off") return Request::Unsplit;
    return std::nullopt;
}

/** What the tab component shows: is there a right split, and how many tabs each bar holds. */
struct Layout {
    bool split = false;
    int leftTabs = 0;
    int rightTabs = 0;
};

enum class Plan { MoveLeft, MoveRight, AlreadyThere, TooFewTabs, NothingToUnsplit, Unsplit };

/** What to do for `wanted` with a tab that sits in bar `currentBar` (0 left, 1 right). */
inline Plan planMove(Request wanted, Layout const& layout, int currentBar)
{
    if (wanted == Request::Unsplit) return layout.split ? Plan::Unsplit : Plan::NothingToUnsplit;
    int const bar = wanted == Request::Left ? 0 : 1;
    if (layout.split) {
        if (currentBar == bar) return Plan::AlreadyThere;
        return bar == 1 ? Plan::MoveRight : Plan::MoveLeft;
    }
    if (layout.leftTabs <= 1) return Plan::TooFewTabs;
    return bar == 1 ? Plan::MoveRight : Plan::MoveLeft;
}

/** The side of a tab in the reply: bar 1 is right, everything else left (a view with no split has one side). */
inline char const* sideWord(int bar) { return bar == 1 ? "right" : "left"; }

/** The sentence for a refusal (empty for a move and for the unsplit). */
inline char const* planWords(Plan plan)
{
    switch (plan) {
    case Plan::TooFewTabs: return "a split needs two tabs: open a second tab first";
    case Plan::NothingToUnsplit: return "there is no split to close";
    case Plan::AlreadyThere: return "the tab is already on that side";
    case Plan::MoveLeft:
    case Plan::MoveRight:
    case Plan::Unsplit: return "";
    }
    return "";
}

} // namespace mcp::tabsplit
