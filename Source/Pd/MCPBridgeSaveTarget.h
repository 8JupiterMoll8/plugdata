// P11.8: what a /pd/save_patch of a canvas into a file would overwrite (shared with the bridge tests). CALLER HOLDS
// sys_lock for every function here.
#pragma once

#include "MCPBridgeCmdCommon.h"
#include "Mcp/McpPdFile.h"

#include <vector>

namespace mcp {

// P11.8: the file a ROOT canvas is bound to on the Pd side (its dir + name), or an empty File (a subpatch, a never-saved
// patch with a relative dir). CALLER HOLDS sys_lock.
inline juce::File pdRootFile(t_canvas* c)
{
    if (!c || c->gl_owner || !c->gl_name || !c->gl_name->s_name) return {};
    auto* dir = canvas_getdir(c);
    if (!dir || !dir->s_name) return {};
    return mcp::fileInPdDir(juce::String::fromUTF8(dir->s_name), juce::String::fromUTF8(c->gl_name->s_name));
}

// P11.8: what a save of `cnv` into `dest` would overwrite. `ownFile`: dest is the file this canvas is bound to (only a
// root has one: the Pd side's dir + name, or the file its tab saves to); a subpatch has none, so its root's file is
// "another open patch's" file like any other tab's. `otherOwner`: the name of another open patch whose file dest is
// (an open tab, or a root on the Pd side); a save there would destroy it, so it is refused whatever the caller says.
// `tabs` were read on the message thread before the lock: a tab's canvas is dereferenced only while it is live; a tab
// that is gone counts as another patch (fail closed). CALLER HOLDS sys_lock.
struct SaveTarget {
    bool isRoot = false;
    bool ownFile = false;
    juce::String otherOwner;
};

// Delta review HIGH: the file of an abstraction (its instance's dir + name), or an empty File.
inline juce::File pdAbstractionFile(t_canvas* c)
{
    if (!c || !c->gl_owner || !canvas_isabstraction(c) || !c->gl_name || !c->gl_name->s_name) return {};
    auto* dir = canvas_getdir(c);
    if (!dir || !dir->s_name) return {};
    return mcp::fileInPdDir(juce::String::fromUTF8(dir->s_name), juce::String::fromUTF8(c->gl_name->s_name));
}

// A tab showing `p` shows `cnv` itself or a plain subpatch inside it: no abstraction on the way up (an abstraction's
// canvas, and anything inside one, belongs to the abstraction's file, never to the parent).
inline bool isPlainPartOf(t_canvas* p, t_canvas* cnv)
{
    for (t_canvas* c = p; c; c = c->gl_owner) {
        if (c == cnv) return true;
        if (canvas_isabstraction(c)) return false;
    }
    return false;
}

// V1 tabs: does `c` (any canvas) hold, at any depth, an abstraction instance whose file is `file`? CALLER HOLDS sys_lock.
inline bool holdsInstanceOf(t_canvas* c, juce::File const& file, int depth = 0)
{
    if (!c || depth > 64) return false;
    for (t_gobj* g = c->gl_list; g; g = g->g_next) {
        if (pd_class(&g->g_pd) != canvas_class) continue;
        auto* sub = reinterpret_cast<t_canvas*>(g);
        if (canvas_isabstraction(sub) && pdAbstractionFile(sub) == file) return true;
        if (holdsInstanceOf(sub, file, depth + 1)) return true;
    }
    return false;
}

// V1 tabs: the names of the OTHER open root patches that hold an instance of `root`'s own file (an abstraction file
// open as its own tab: an edit there changes every instance). Empty for an unsaved root. CALLER HOLDS sys_lock.
inline std::vector<juce::String> rootsUsingFileOf(t_canvas* root)
{
    std::vector<juce::String> out;
    auto const file = pdRootFile(root);
    if (file == juce::File()) return out;
    for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next) {
        if (c == root || c->gl_owner || !c->gl_name || !c->gl_name->s_name) continue;
        if (holdsInstanceOf(c, file)) out.push_back(juce::String::fromUTF8(c->gl_name->s_name));
    }
    return out;
}

// `p` is an abstraction instance whose file is `file`, or a canvas inside one.
inline bool insideAbstractionOf(t_canvas* p, juce::File const& file)
{
    for (t_canvas* c = p; c; c = c->gl_owner)
        if (canvas_isabstraction(c) && pdAbstractionFile(c) == file) return true;
    return false;
}

// The name of a loaded abstraction instance (anywhere in the open patches) whose file `dest` is, or "".
inline juce::String abstractionUsing(juce::File const& dest)
{
    std::vector<t_canvas*> todo;
    for (t_canvas* r = pd_this ? pd_this->pd_canvaslist : nullptr; r; r = r->gl_next) todo.push_back(r);
    while (!todo.empty()) {
        t_canvas* c = todo.back();
        todo.pop_back();
        if (pdAbstractionFile(c) == dest) return juce::String::fromUTF8(c->gl_name->s_name);
        for (t_gobj* y = c->gl_list; y; y = y->g_next)
            if (pd_class(&y->g_pd) == canvas_class) todo.push_back(reinterpret_cast<t_canvas*>(y));
    }
    return {};
}

inline SaveTarget saveTargetOf(t_canvas* cnv, juce::File const& dest, std::vector<McpGuiHost::TabInfo> const& tabs)
{
    SaveTarget t;
    t.isRoot = cnv && cnv->gl_owner == nullptr;
    // Final delta M1: a ROOT bound to dest on the Pd side is dest's own patch, also when dest is an abstraction other
    // patches use (the artist edits abs.pd in its own tab): its instances are copies of this file, not other owners.
    bool const rootOwn = t.isRoot && pdRootFile(cnv) == dest;
    if (rootOwn) t.ownFile = true;
    for (auto const& tab : tabs) {
        if (tab.file == juce::File() || tab.file != dest) continue;
        bool const live = tab.patch && mcpCanvasIsLive(tab.patch);
        // Delta review HIGH: own only when the tab shows this root or a plain subpatch of it, never an abstraction.
        if (t.isRoot && live && isPlainPartOf(tab.patch, cnv)) {
            t.ownFile = true;
            continue;
        }
        if (rootOwn && live && insideAbstractionOf(tab.patch, dest)) continue; // an instance of this very file
        if (t.otherOwner.isEmpty()) t.otherOwner = tab.title.isNotEmpty() ? tab.title : dest.getFileName();
    }
    for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c && t.otherOwner.isEmpty(); c = c->gl_next) {
        if (c == cnv || c->gl_owner) continue;
        if (pdRootFile(c) == dest) t.otherOwner = juce::String::fromUTF8(c->gl_name->s_name);
    }
    // An abstraction in use (an instance is loaded, open in a tab or not): its file is the abstraction's, unless this
    // root IS that file (rootOwn).
    if (t.otherOwner.isEmpty() && !rootOwn) {
        auto const abs = abstractionUsing(dest);
        if (abs.isNotEmpty()) t.otherOwner = "abstraction " + abs;
    }
    if (t.otherOwner.isNotEmpty()) t.ownFile = false;
    return t;
}

} // namespace mcp
