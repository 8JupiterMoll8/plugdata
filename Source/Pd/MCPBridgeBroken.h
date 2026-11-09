/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Pd's red boxes: objects whose class could not be created. Moved out of MCPBridgeCmdFiles.cpp (P11.2 V10, the load
// report) so /pd/broken and the census can use the same walk. Read only; caller holds the Pd lock.
#pragma once

#include "MCPBridgeCmdCommon.h"

namespace mcp::broken {

/** A red box: a text_class object of type T_OBJECT with atoms (review L8: an empty "[ ]" box is not one). */
inline bool isRedBox(t_gobj* g)
{
    return g && pd_class(&g->g_pd) == text_class && reinterpret_cast<t_text*>(g)->te_type == T_OBJECT
        && reinterpret_cast<t_text*>(g)->te_binbuf && binbuf_getnatom(reinterpret_cast<t_text*>(g)->te_binbuf) > 0;
}

/**
 * Every red box under `c`. Top-level entries carry their index; ones inside subpatches/abstractions/clone instances
 * the top-level container's index and a path ("<index>:<name>/.../<index>"). Clone instances are walked.
 */
inline void collectFailedCreates(t_canvas* c, int topIndex, juce::String const& path, juce::Array<juce::var>& out, int depth)
{
    if (!c || depth > 32) return;
    int index = 0;
    for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
        int const top = depth == 0 ? index : topIndex;
        if (isRedBox(g)) {
            char* buf = nullptr;
            int len = 0;
            binbuf_gettext(reinterpret_cast<t_text*>(g)->te_binbuf, &buf, &len);
            auto* o = new juce::DynamicObject();
            o->setProperty("index", top);
            o->setProperty("text", buf && len > 0 ? juce::String::fromUTF8(buf, len).trim() : juce::String());
            if (depth > 0) o->setProperty("path", path + "/" + juce::String(index));
            if (buf) freebytes(buf, len);
            out.add(juce::var(o));
        } else if (pd_class(&g->g_pd) == canvas_class) {
            auto* sub = reinterpret_cast<t_canvas*>(g);
            juce::String const name = sub->gl_name ? juce::String::fromUTF8(sub->gl_name->s_name) : juce::String("pd");
            collectFailedCreates(sub, top, (path.isEmpty() ? juce::String() : path + "/") + juce::String(index) + ":" + name, out, depth + 1);
        } else if (pd_class(&g->g_pd) == clone_class) {
            int const n = clone_get_n(g);
            for (int i = 0; i < n; ++i)
                if (auto* inst = mcpCloneInstance(g, i))
                    collectFailedCreates(reinterpret_cast<t_canvas*>(inst), top,
                        (path.isEmpty() ? juce::String() : path + "/") + juce::String(index) + ":clone#" + juce::String(i), out, depth + 1);
        }
    }
}

/** How many red boxes sit inside container `g` (a subpatch, an abstraction or a clone), all levels down. */
inline int countInside(t_gobj* g)
{
    juce::Array<juce::var> found;
    if (!g) return 0;
    if (pd_class(&g->g_pd) == canvas_class)
        collectFailedCreates(reinterpret_cast<t_canvas*>(g), 0, {}, found, 1);
    else if (pd_class(&g->g_pd) == clone_class)
        for (int i = 0, n = clone_get_n(g); i < n; ++i)
            if (auto* inst = mcpCloneInstance(g, i))
                collectFailedCreates(reinterpret_cast<t_canvas*>(inst), 0, {}, found, 1);
    return found.size();
}

} // namespace mcp::broken
