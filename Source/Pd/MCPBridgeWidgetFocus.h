/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The Pd side of the widget chip -> selection pill shortcut (Source/Mcp/McpWidgetFocus.h has the story). Message thread, CALLER HOLDS
// the Pd lock (a PdLockScope) for every function here. Included by MCPBridge.cpp and the handler tests.
//
//   mcpWidgetFocusChainLocked   the boxes a pd-lua object sits in, from the open patches: [(canvas holding it, the object), (the
//                               canvas holding that canvas, the box that is that canvas), ... up to a root]. The pointer a widget
//                               sends is only COMPARED with the boxes found (never dereferenced), so a stale or forged one finds
//                               nothing. Innermost first: the first canvas that has a view shows the artist's box.
//   mcpWidgetTargetOfBoxLocked  which askable widget a box is (a pd-lua object of the family, or the wrapper abstraction that holds
//                               one) and the address of its pd-lua object: what the pill needs to tell THAT instance to ask.
//   mcpTellWidgetLocked         one word and the object's address, as a list on the widget's in-name (`focused <ptr>`, `ask <ptr>`):
//                               every instance of the widget hears it, the one with that address acts.

#pragma once

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeWidgetAskCanvas.h" // mcpAskAbstractionName
#include "Mcp/McpWidgetFocus.h"

#include <cstdint>
#include <string>
#include <vector>

inline constexpr int kFocusWalkBudget = 20000; // boxes visited, all roots together
inline constexpr int kFocusWalkMaxDepth = 32;

struct McpFocusLink {
    t_canvas* cnv = nullptr; // the canvas that holds `box`
    t_gobj* box = nullptr;
};

// true: `target` was found and `path` holds the links from the root down to it (outermost first). false: not under `c`, or the budget ran out.
inline bool mcpFocusWalk(t_canvas* c, t_gobj const* target, std::vector<McpFocusLink>& path, int depth, int& budget)
{
    if (!c || depth > kFocusWalkMaxDepth) return false;
    for (t_gobj* g = c->gl_list; g; g = g->g_next) {
        if (--budget < 0) return false;
        path.push_back({ c, g });
        if (g == target) return true;
        if (pd_class(&g->g_pd) == canvas_class && mcpFocusWalk(reinterpret_cast<t_canvas*>(g), target, path, depth + 1, budget)) return true;
        path.pop_back();
    }
    return false;
}

// Innermost first; empty when the object is in no open patch (or the budget ran out before it was seen). The chain climbs from a canvas to
// the box that holds it only while that canvas is graph-on-parent (a wrapper abstraction, a GOP subpatch): a plain [pd foo] shows its
// widgets only in a view of its own, so a view of the parent could not show the widget.
inline std::vector<McpFocusLink> mcpWidgetFocusChainLocked(std::uint64_t ptr, int budget)
{
    std::vector<McpFocusLink> out;
    if (ptr == 0 || !pd_this) return out;
    auto const* const target = reinterpret_cast<t_gobj const*>(static_cast<std::uintptr_t>(ptr)); // compared only
    for (t_canvas* root = pd_this->pd_canvaslist; root; root = root->gl_next) {
        std::vector<McpFocusLink> path;
        if (mcpFocusWalk(root, target, path, 0, budget)) {
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                out.push_back(*it);
                // `it->cnv` holds the box; the NEXT link up exists only if `it->cnv` itself is shown on its parent (graph-on-parent)
                if (it->cnv && it->cnv->gl_owner && !it->cnv->gl_isgraph) break;
            }
            return out;
        }
        if (budget < 0) break;
    }
    return out;
}

inline mcp::WidgetPillTarget mcpWidgetTargetOfBoxLocked(t_gobj* box)
{
    mcp::WidgetPillTarget t;
    if (!box) return t;
    // The box comes from the GUI, which may hold a box the AI deleted a moment ago: it is read only after it is found in an open patch (a
    // comparison walk), never on the GUI's word.
    if (mcpWidgetFocusChainLocked(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(box)), kFocusWalkBudget).empty()) return t;
    t_class* const k = pd_class(&box->g_pd);
    if (k == canvas_class) {
        // a wrapper abstraction (ai.dice.pd holds [ai-dice] and its in-name receiver): the widget's object is a box inside it
        auto* const c = reinterpret_cast<t_canvas*>(box);
        char const* const file = mcpAskAbstractionName(c);
        std::string const widget = file ? mcp::widgetOfBoxName(file) : std::string();
        if (widget.empty()) return t;
        for (t_gobj* g = c->gl_list; g; g = g->g_next) {
            if (!mcp::isWidgetObjectName(class_getname(pd_class(&g->g_pd)), widget.c_str())) continue;
            t.widget = widget;
            t.luaPtr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g));
            return t;
        }
        return t;
    }
    std::string const widget = mcp::widgetOfBoxName(class_getname(k));
    if (widget.empty()) return t;
    t.widget = widget;
    t.luaPtr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(box));
    return t;
}

// false: no receiver is bound to the widget's in-name (nobody to tell).
inline bool mcpTellWidgetLocked(char const* widget, char const* word, std::uint64_t ptr)
{
    char inName[mcp::kWidgetAskNameLen + 8] = {};
    if (!mcp::widgetAskInName(widget, inName, sizeof(inName))) return false;
    t_symbol* const in = gensym(inName);
    if (!in->s_thing) return false;
    char hex[mcp::kWidgetFocusPtrChars + 1] = {};
    mcp::widgetFocusPtrText(ptr, hex, sizeof(hex));
    if (!hex[0]) return false;
    t_atom a[2];
    SETSYMBOL(&a[0], gensym(word));
    SETSYMBOL(&a[1], gensym(hex));
    pd_typedmess(in->s_thing, &s_list, 2, a);
    return true;
}
