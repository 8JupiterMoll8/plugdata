/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Who listens on a Pd name (P11.4 V19, moved out of MCPBridgeCmdBound.cpp's anonymous namespace for truth slice 3:
// the stop facts' `names`, send_checked's receivers). Read only; caller holds the Pd lock.
//
// The binding: a single binder is s_thing; several binders are Pd's bindlist, detected by its class NAME ("bindlist",
// the class is static in m_pd.c) and read through McpBindlistView, whose layout copies m_pd.c's t_bindlist
// (Tests/mcp/bridge_handler_tests.cpp runBoundCases pins it with three [r] on one name: a Pd update that changes the
// struct fails there, loudly). The symbol is looked up without creating it (a name nothing ever used stays out of
// Pd's symbol table).
#pragma once

#include "MCPBridgeCmdCommon.h"

#include <cstring>
#include <unordered_set>
#include <vector>

namespace mcp::bound {

// The layout of t_bindlist in Libraries/pure-data/src/m_pd.c (static there). Pinned by runBoundCases.
struct McpBindlistView {
    t_pd b_pd;
    t_pd** b_vec;
    int b_n;
};

/** gensym's lookup (m_class.c dogensym: djb2 over the bytes, SYMTABHASHSIZE buckets) without the insert. */
inline t_symbol* findSymbolNoCreate(char const* s)
{
    if (!s || !pd_this || !pd_this->pd_symhash)
        return nullptr;
    unsigned int hash = 5381;
    for (char const* p = s; *p; ++p)
        hash = ((hash << 5) + hash) + static_cast<unsigned int>(*p); // as dogensym: a char added (sign included)
    for (t_symbol* sym = pd_this->pd_symhash[hash & (SYMTABHASHSIZE - 1)]; sym; sym = sym->s_next)
        if (std::strcmp(sym->s_name, s) == 0)
            return sym;
    return nullptr;
}

/** Everything bound to `sym` (empty for a null or unbound symbol). */
inline std::vector<t_pd*> bindersOf(t_symbol* sym)
{
    std::vector<t_pd*> out;
    if (!sym || !sym->s_thing)
        return out;
    t_pd* thing = sym->s_thing;
    char const* cls = class_getname(*thing);
    if (cls && std::strcmp(cls, "bindlist") == 0) {
        auto const* list = reinterpret_cast<McpBindlistView const*>(thing);
        if (list->b_vec && list->b_n > 0 && list->b_n < 1000000)
            for (int i = 0; i < list->b_n; ++i)
                if (list->b_vec[i])
                    out.push_back(list->b_vec[i]);
        return out;
    }
    out.push_back(thing);
    return out;
}

/** What placedAmong found: the binders that are placed boxes, and whether the walk stopped at its cap. */
struct Placed {
    std::unordered_set<t_pd const*> found;
    bool walkLimit = false; // depth > 64 or more than maxObjects visited: a binder not found may be a real box
};

/**
 * Review H5 (proof3): which of `binders` are boxes placed on a canvas (found in some glist, every clone instance
 * included, whatever its -s: mcpCloneInstance). Only these may be read as a t_object. A class check is not enough:
 * pd-lua's `pd.Receive` proxy has a class made with flags 0 (CLASS_PATCHABLE, so pd_checkobject passes) on a 24-byte
 * struct; reading its outlets or binbuf overreads the heap. Pointers are compared, never dereferenced, until found.
 * A walk cut by its caps says so (walkLimit): the caller refuses with that word, not "non-object binder".
 * Caller holds the Pd lock.
 */
inline Placed placedAmong(std::vector<t_pd*> const& binders, int maxObjects = 500000, int maxDepth = 64)
{
    std::unordered_set<t_pd const*> wanted(binders.begin(), binders.end());
    Placed out;
    if (wanted.empty() || !pd_this)
        return out;
    int visited = 0;
    auto walk = [&](auto&& self, t_canvas* c, int depth) -> void {
        if (!c)
            return;
        if (depth > maxDepth) {
            out.walkLimit = true;
            return;
        }
        for (t_gobj* g = c->gl_list; g && out.found.size() < wanted.size(); g = g->g_next) {
            if (++visited > maxObjects) {
                out.walkLimit = true;
                return;
            }
            if (wanted.count(&g->g_pd) != 0)
                out.found.insert(&g->g_pd);
            t_class* cl = pd_class(&g->g_pd);
            if (cl == canvas_class) {
                self(self, reinterpret_cast<t_canvas*>(g), depth + 1);
            } else if (cl == clone_class) {
                int const n = clone_get_n(g);
                for (int i = 0; i < n; ++i)
                    if (auto* inst = mcpCloneInstance(g, i))
                        self(self, reinterpret_cast<t_canvas*>(inst), depth + 1);
            }
            if (out.walkLimit)
                return;
        }
    };
    for (t_canvas* r = pd_this->pd_canvaslist; r && out.found.size() < wanted.size() && !out.walkLimit; r = r->gl_next)
        walk(walk, r, 0);
    return out;
}

/**
 * The names object `o` itself listens on: each symbol of its creation text that Pd binds to this very object (an
 * [r name], [r~ name], [catch~ name], a GUI's receive name, ...). Checked against the binding, never guessed from the
 * class. A GUI's receive name that was changed after creation (properties dialog) is read from its iemgui field too.
 * A $0 name is realised in `cnv` (the object's canvas; delta review 4); without it a $0 name is skipped.
 */
inline std::vector<juce::String> namesBoundTo(t_object* o, t_canvas* cnv = nullptr)
{
    std::vector<juce::String> out;
    if (!o) return out;
    auto* self = &o->ob_pd;
    auto consider = [&](t_symbol* s) {
        if (!s || !s->s_name || !s->s_name[0]) return;
        juce::String const name = juce::String::fromUTF8(s->s_name);
        if (std::find(out.begin(), out.end(), name) != out.end()) return;
        for (auto* b : bindersOf(s))
            if (b == self) {
                out.push_back(name);
                return;
            }
    };
    if (o->te_binbuf) {
        int const n = binbuf_getnatom(o->te_binbuf);
        t_atom const* a = binbuf_getvec(o->te_binbuf);
        for (int i = 1; i < n; ++i)
            if (a[i].a_type == A_SYMBOL) consider(a[i].a_w.w_symbol);
            else if (a[i].a_type == A_DOLLSYM && cnv) consider(canvas_realizedollar(cnv, a[i].a_w.w_symbol)); // $0-name
    }
    char const* cls = class_getname(pd_class(&o->ob_pd));
    static char const* const iem[] = { "hsl", "vsl", "tgl", "hradio", "vradio", "nbx", "bng", "vu", "cnv" };
    for (auto const* c : iem)
        if (cls && std::strcmp(cls, c) == 0) {
            consider(reinterpret_cast<t_iemgui*>(o)->x_rcv);
            break;
        }
    return out;
}

} // namespace mcp::bound
