/*
 // The AI's split view, the GUI half: see McpTabSplitAccess.h. Message thread only.
 */
#include "PluginEditor.h"
#include "Toolbar.h"
#include "Canvas.h"
#include "TabComponent.h"
#include "Mcp/McpTabSplitAccess.h"

mcp::tabsplit::Layout McpTabSplitAccess::layoutOf(TabComponent const& tabs)
{
    mcp::tabsplit::Layout layout;
    layout.split = tabs.splits[1] != nullptr;
    layout.leftTabs = tabs.tabbars[0].size();
    layout.rightTabs = tabs.tabbars[1].size();
    return layout;
}

int McpTabSplitAccess::barOf(TabComponent const& tabs, Canvas const* cnv)
{
    if (!cnv)
        return -1;
    for (int bar = 0; bar < 2; ++bar)
        for (int i = 0; i < tabs.tabbars[bar].size(); ++i)
            if (tabs.mcpCanvasOfTab(bar, i) == cnv)
                return bar;
    return -1;
}

void McpTabSplitAccess::moveToBar(TabComponent& tabs, Canvas* cnv, int const bar)
{
    auto const* button = static_cast<decltype(tabs.tabbars[0][0])>(nullptr); // the tab's button (its class is private to TabComponent.cpp)
    for (int b = 0; b < 2; ++b)
        for (int i = 0; i < tabs.tabbars[b].size(); ++i)
            if (tabs.mcpCanvasOfTab(b, i) == cnv)
                button = tabs.tabbars[b][i];
    if (!button)
        return;

    auto* const artistsView = tabs.getCurrentCanvas(); // whatever the artist is working in stays the active view
    if (bar == 0)
        tabs.moveToLeftSplit(button);
    else
        tabs.moveToRightSplit(button);
    tabs.closeEmptySplits(); // as the menu does after a move: a bar that ended up empty closes its split
    if (artistsView && (artistsView == tabs.splits[0] || artistsView == tabs.splits[1]))
        tabs.setActiveSplit(artistsView);
}

void McpTabSplitAccess::unsplit(TabComponent& tabs)
{
    if (!tabs.splits[1])
        return;
    auto* const leftShows = tabs.splits[0].getComponent();
    // Each call moves the first right tab to the left bar (and shows what is left on the right, or nothing); bounded by the count the
    // right bar started with, so a bar the move cannot empty ends the loop instead of spinning.
    for (int n = tabs.tabbars[1].size(); n > 0 && tabs.tabbars[1].size() > 0 && tabs.splits[1]; --n)
        tabs.moveToLeftSplit(tabs.tabbars[1][0]);
    tabs.closeEmptySplits();
    if (leftShows)
        tabs.showTab(leftShows, 0); // the left view keeps showing what it showed (a no-op when it still does)
}
