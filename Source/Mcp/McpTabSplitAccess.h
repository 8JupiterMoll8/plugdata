/*
 // The AI's split view, the GUI half (PARKED 2026-11-22; capability tab_split): the moves the tab menu's "Split left / Split right"
 // make, reached through ONE friend declaration in TabComponent.h (MCP-HOOK(tabSplit)) so the logic lives here, not in the upstream
 // file. The decision (what is allowed, what the reply says) is mcp::tabsplit in McpTabSplit.h. Message thread only: every call
 // touches components.
 */
#pragma once

#include "Mcp/McpTabSplit.h"

class Canvas;
class TabComponent;

struct McpTabSplitAccess {
    // The bars of this tab component: is the right split showing, and how many tabs each holds.
    static mcp::tabsplit::Layout layoutOf(TabComponent const& tabs);
    // 0 = the tab is in the left bar, 1 = in the right bar, -1 = not a tab here.
    static int barOf(TabComponent const& tabs, Canvas const* cnv);
    // Puts the tab on the left (bar 0) or right (bar 1) the way the tab menu does, then closes a split that ended up empty. The artist's
    // active view stays the active one when it is still showing. The caller has planned the move (mcp::tabsplit::planMove).
    static void moveToBar(TabComponent& tabs, Canvas* cnv, int bar);
    // Every tab back to the left bar, one view; the left view keeps showing what it showed.
    static void unsplit(TabComponent& tabs);
};
