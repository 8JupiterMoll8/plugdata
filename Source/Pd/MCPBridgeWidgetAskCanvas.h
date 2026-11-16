/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The canvas of a widget ask (proof3 3c, MCPBridgeWidgetAsk.h): the ai-ask message names no canvas, so the pump finds the
// tab a widget sits in. Message thread, under the Pd lock. ONE walk of the open patches serves every widget the pump popped
// in a tick (a hello burst at load is one lock and one walk, not one per widget).
//
// A tab is a ROOT canvas (pd_this->pd_canvaslist) whose patch (subpatches and abstractions included) holds a box of the
// widget's class (`ai.tap`; pd-lua's shadow class `ai.tap:gfx`, the one a GUI widget really is; or an abstraction FILE
// `ai.tap` / `ai.tap.pd`). It is named by its identity wire key: the `key` of that tab's /pd/list_tabs row, the name every
// canvas verb resolves. Not found (a looper: its class is ai-loop2, its word ai.loop_<name>; a box not placed yet; a [clone]
// instance), found in more than one tab (which one asked is not known), or a walk that ran out of its box budget (another
// tab may hold it beyond what was seen): `main`, the focused tab, which is what an ask said before it named a canvas.
//
// Hidden roots (review 2026-11-17, finding 7): pd_canvaslist is every root Pd holds, shown in a tab or not (a patch opened by
// a message and never shown). Such a root holding the widget makes it two holders (`main`) or names a key no tab has (a key
// every canvas verb still resolves). The fork's only "root is a tab" test is McpGuiHost::listTabs, the editor's tab list: it is
// empty without an editor (the handler tests, a headless instance) and names the SUBPATCH, not the root, for a tab that shows one,
// so it is not used; no plugdata code opens a root without a tab, so the case stays rare and ends at the old `main`.

#pragma once

#include "MCPBridgeCmdCommon.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

inline constexpr int kAskCanvasWalkBudget = 20000; // boxes visited per tick, all roots and all widgets together
inline constexpr int kAskCanvasMaxDepth = 32;

// One widget of the tick. The strings are built once, outside the walk.
struct McpAskTarget {
    std::string cls;  // ai.tap
    std::string gfx;  // ai.tap:gfx
    std::string file; // ai.tap.pd
    bool inRoot = false; // seen under the root being walked
    int holders = 0;     // roots that hold it
    juce::String key;    // the key of the first root that held it
};

// The file name of an abstraction instance as Pd bound it (gl_name without any directory), null for a [pd] subpatch.
// Same name pd::getAbstractionFileName builds (its .m~ normalisation only drops a ".pd" that `file` spells out), without a
// juce::String per instance.
inline char const* mcpAskAbstractionName(t_canvas* c)
{
    if (!canvas_isabstraction(c) || !c->gl_name) return nullptr;
    char const* const name = c->gl_name->s_name;
    char const* const slash = std::strrchr(name, '/');
    return slash ? slash + 1 : name;
}

// Marks the targets the box (a class name, or an abstraction file name) belongs to. `pending` counts targets not yet seen in this root.
inline void mcpAskMark(std::vector<McpAskTarget>& targets, std::size_t& pending, char const* name, bool abstraction)
{
    for (auto& t : targets) {
        if (t.inRoot) continue;
        bool const hit = abstraction ? (t.cls == name || t.file == name) : (t.cls == name || t.gfx == name);
        if (!hit) continue;
        t.inRoot = true;
        --pending;
    }
}

// False: the budget (or the depth) ran out, the root was not seen whole.
inline bool mcpAskWalk(t_canvas* c, std::vector<McpAskTarget>& targets, std::size_t& pending, int depth, int& budget)
{
    if (!c) return true;
    if (depth > kAskCanvasMaxDepth) return false;
    for (t_gobj* g = c->gl_list; g && pending > 0; g = g->g_next) {
        if (--budget < 0) return false;
        t_class* const k = pd_class(&g->g_pd);
        if (k == canvas_class) {
            auto* const sub = reinterpret_cast<t_canvas*>(g);
            if (char const* const file = mcpAskAbstractionName(sub)) mcpAskMark(targets, pending, file, true);
            if (!mcpAskWalk(sub, targets, pending, depth + 1, budget)) return false;
            continue;
        }
        if (char const* const name = class_getname(k)) mcpAskMark(targets, pending, name, false);
    }
    return true;
}

// The tab key of each widget, in the order given (`main` where it cannot be told). CALLER HOLDS the Pd lock (a PdLockScope,
// which also selects this processor's Pd instance: pd_this is read here).
inline std::vector<juce::String> mcpWidgetAskCanvasesLocked(PluginProcessor& processor, std::vector<std::string> const& widgets, int budget)
{
    std::vector<juce::String> keys(widgets.size(), juce::String("main"));
    if (widgets.empty() || !pd_this) return keys;
    std::vector<McpAskTarget> targets;
    targets.reserve(widgets.size());
    for (auto const& w : widgets) {
        McpAskTarget t;
        t.cls = w;
        t.gfx = w + ":gfx";
        t.file = w + ".pd";
        targets.push_back(std::move(t));
    }
    for (t_canvas* root = pd_this->pd_canvaslist; root; root = root->gl_next) {
        for (auto& t : targets) t.inRoot = false;
        std::size_t pending = targets.size();
        if (!mcpAskWalk(root, targets, pending, 0, budget)) return keys; // not seen whole: nothing is claimed
        for (auto& t : targets) {
            if (!t.inRoot) continue;
            if (++t.holders == 1) t.key = juce::String(processor.getMcp()->identity.keyOf(root));
        }
    }
    for (std::size_t i = 0; i < targets.size(); ++i)
        if (targets[i].holders == 1 && targets[i].key.isNotEmpty()) keys[i] = targets[i].key;
    return keys;
}

// Takes the Pd lock. Never creates a name the AI could not resolve (the key is the identity wire key).
inline std::vector<juce::String> mcpWidgetAskCanvases(PluginProcessor& processor, std::vector<std::string> const& widgets)
{
    McpHost::PdLockScope const lock(processor);
    return mcpWidgetAskCanvasesLocked(processor, widgets, kAskCanvasWalkBudget); // pd_this is checked inside: after the lock selected the instance
}
