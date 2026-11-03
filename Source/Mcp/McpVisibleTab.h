/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// P11.9 item 1: which canvases are on the artist's screen, and what focus_tab may say. JUCE-free (the GuiHost fills
// EditorView from the component tree on the message thread), unit-tested in Tests/mcp/visible_tab_test.cpp.
//
// Live check 2026-11-03: focus_tab answered "focused: X" and list_tabs marked X focused while the editor showed another
// tab; with flow mode on, free deletes landed on a tab the artist could not see. Rule: flow frees objects only on a
// canvas that is on screen now (a split view of a showing, not minimised window), and focus_tab says "focused" only
// when the asked tab became the editor's current canvas and its view is showing.
//
// Residue: the component tree is the truth here. A window covered by another app, or a GPU that stopped repainting
// (seen 2026-11-03 with Blender open), still reads as showing; nothing in-process can see either.

#include <vector>

namespace mcp::visible {

struct SplitView {
    void const* canvas = nullptr; // the t_canvas* the split shows
    bool viewShowing = false;     // its viewport component is showing (JUCE isShowing: visible up to a non-minimised peer)
};

struct EditorView {
    bool windowShowing = false;            // the editor is showing and its window not minimised
    void const* pluginModeCanvas = nullptr; // plugin mode shows only this canvas
    std::vector<SplitView> splits;          // the left and right split views of the tab component
};

inline bool onScreen(void const* canvas, std::vector<EditorView> const& editors)
{
    if (!canvas)
        return false;
    for (auto const& e : editors) {
        if (!e.windowShowing)
            continue;
        if (e.pluginModeCanvas) {
            if (e.pluginModeCanvas == canvas)
                return true;
            continue;
        }
        for (auto const& s : e.splits)
            if (s.canvas == canvas && s.viewShowing)
                return true;
    }
    return false;
}

enum class Focus {
    Switched,   // the asked tab is the editor's current canvas and on screen
    NotCurrent, // the editor's current canvas is another one
    NotShown,   // it is current, but its view is not on screen (window minimised or hidden)
};

inline Focus focusOutcome(void const* wanted, void const* current, bool currentShowing)
{
    if (!wanted || current != wanted)
        return Focus::NotCurrent;
    return currentShowing ? Focus::Switched : Focus::NotShown;
}

inline char const* focusWords(Focus f)
{
    switch (f) {
    case Focus::Switched: return "focused";
    case Focus::NotCurrent: return "not switched: the editor still shows another tab";
    case Focus::NotShown: return "not on screen: the tab is current but the PlugData window is minimised or hidden";
    }
    return "not switched";
}

} // namespace mcp::visible
