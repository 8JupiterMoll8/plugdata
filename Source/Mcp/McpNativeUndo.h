/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// One undo history for AI and manual edits (Plan 4 P4.4). MCP mutations register Pd's OWN undo actions on the
// canvas they change, so Pd's per-canvas queue holds every edit in time order: one AI call is one undo sequence
// (decision 9), Ctrl+Z and /pd/undo walk the same queue, and nothing has to flush it. Pd's entries address objects by
// index, which stays valid because every MCP change is now registered in order.
// Every function here runs in the Pd context (Pd lock held).
#pragma once

#include "Pd/Interface.h"

#include <utility>
#include <vector>

extern "C" {
// g_editor.c: not in Pd's headers.
void* canvas_undo_set_pastebinbuf(t_canvas* x, t_binbuf* b, int numpasted, int duplicate, int d_offset);
}

namespace mcp::undo {

// The sequence name the GUI shows ("Undo AI edit"); Pd keeps the pointer, so it is a literal.
inline constexpr char const* kAiStep = "AI edit";
inline constexpr int kCutClear = 2; // g_editor.c UCUT_CLEAR

// Pd's selection and paste undo need an editor; a closed subpatch has none. Made for the scope, then removed
// again (as pd::ScopedCurrentCanvas does), so the canvas looks as it did.
struct EditorScope {
    t_canvas* cnv;
    bool made = false;
    explicit EditorScope(t_canvas* c)
        : cnv(c)
    {
        if (cnv && !cnv->gl_editor) {
            canvas_create_editor(cnv);
            made = true;
        }
    }
    ~EditorScope()
    {
        if (made && cnv->gl_owner && cnv->gl_editor)
            canvas_destroy_editor(cnv);
    }
    EditorScope(EditorScope const&) = delete;
    EditorScope& operator=(EditorScope const&) = delete;
};

inline bool canUndo(t_canvas* cnv)
{
    t_undo* u = cnv ? canvas_undo_get(cnv) : nullptr;
    return u && u->u_queue && u->u_last && u->u_last != u->u_queue;
}

inline bool canRedo(t_canvas* cnv)
{
    t_undo* u = cnv ? canvas_undo_get(cnv) : nullptr;
    return u && u->u_queue && u->u_last && u->u_last->next;
}

// Bracket one AI call. An empty sequence (nothing registered) is removed by Pd itself (canvas_undo_add).
inline void beginStep(t_canvas* cnv) { canvas_undo_add(cnv, UNDO_SEQUENCE_START, kAiStep, nullptr); }
inline void endStep(t_canvas* cnv) { canvas_undo_add(cnv, UNDO_SEQUENCE_END, kAiStep, nullptr); }

inline t_symbol* straightPath() { return gensym("empty"); } // a wire without a custom path

// Before obj_disconnect.
inline void disconnect(t_canvas* cnv, t_object* src, int outno, t_object* dst, int inno)
{
    canvas_undo_add(cnv, UNDO_DISCONNECT, "disconnect",
        canvas_undo_set_disconnect(cnv, canvas_getindex(cnv, &src->ob_g), outno, canvas_getindex(cnv, &dst->ob_g), inno, straightPath()));
}

// After a successful obj_connect.
inline void connect(t_canvas* cnv, t_object* src, int outno, t_object* dst, int inno)
{
    canvas_undo_add(cnv, UNDO_CONNECT, "connect",
        canvas_undo_set_connect(cnv, canvas_getindex(cnv, &src->ob_g), outno, canvas_getindex(cnv, &dst->ob_g), inno, straightPath()));
}

// Before the objects are removed: Pd's "clear" entry (its undo pastes them back at their old indices, with their
// wires). Leaves nothing selected.
template <typename Objects>
inline void cut(t_canvas* cnv, Objects const& objects)
{
    EditorScope editor(cnv);
    glist_noselect(cnv);
    for (auto* obj : objects)
        if (obj) glist_select(cnv, obj);
    canvas_undo_add(cnv, UNDO_CUT, "clear", canvas_undo_set_cut(cnv, kCutClear));
    glist_noselect(cnv);
}

// After objects were appended at the end of the canvas: one paste entry over the last `count` objects. Its undo
// removes them; its redo pastes their saved text back at the same place.
inline void pasteAppended(t_canvas* cnv, int count)
{
    if (count <= 0) return;
    int total = 0;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) ++total;
    t_binbuf* b = binbuf_new();
    int i = 0;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next, ++i)
        if (i >= total - count) gobj_save(g, b);
    EditorScope editor(cnv);
    glist_noselect(cnv);
    canvas_undo_add(cnv, UNDO_PASTE, "paste", canvas_undo_set_pastebinbuf(cnv, b, count, 0, 0));
    binbuf_free(b);
}

// Before objects move: the position of every object (undo puts them back, redo moves them again). The
// "move already registered" flag a GUI drag uses is left as it was, so the artist's next drag still registers.
inline void motion(t_canvas* cnv)
{
    auto* ed = pd::Interface::getInstanceEditor();
    int const was = ed->canvas_undo_already_set_move;
    canvas_undo_add(cnv, UNDO_MOTION, "motion", canvas_undo_set_move(cnv, 0));
    ed->canvas_undo_already_set_move = was;
}

// Layout passes that move objects themselves (sanitizeLayout, compose, the rect loops): take the positions before,
// then movedSince() registers ONE motion step whose undo puts every object back, only when something moved.
// The motion buffer mirrors g_editor.c's t_undo_move (as Interface.h mirrors Pd's instance editor).
inline constexpr t_float kNoPosition = -1.0e9f;

struct Positions {
    std::vector<std::pair<t_float, t_float>> at; // by gl_list index; kNoPosition for a non-patchable gobj
};

inline Positions positions(t_canvas* cnv)
{
    Positions p;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
        t_object* ob = pd_checkobject(&g->g_pd);
        p.at.emplace_back(ob ? ob->te_xpix : kNoPosition, ob ? ob->te_ypix : kNoPosition);
    }
    return p;
}

inline bool movedSince(t_canvas* cnv, Positions const& before)
{
    struct MoveElem { int e_index; t_float e_xpix; t_float e_ypix; };
    struct Move { MoveElem* u_vec; int u_n; };
    auto const now = positions(cnv);
    if (now.at.size() != before.at.size() || now.at == before.at)
        return false; // nothing moved (or objects came or went: not a pure move, nothing safe to register)
    auto* ed = pd::Interface::getInstanceEditor();
    int const was = ed->canvas_undo_already_set_move;
    auto* buf = static_cast<Move*>(canvas_undo_set_move(cnv, 0)); // the current (new) positions...
    ed->canvas_undo_already_set_move = was;
    for (int i = 0; i < buf->u_n; ++i) { // ...replaced by the old ones: undo moves there, redo back
        auto const idx = static_cast<size_t>(buf->u_vec[i].e_index);
        if (idx < before.at.size()) {
            buf->u_vec[i].e_xpix = before.at[idx].first;
            buf->u_vec[i].e_ypix = before.at[idx].second;
        }
    }
    beginStep(cnv);
    canvas_undo_add(cnv, UNDO_MOTION, "motion", buf);
    endStep(cnv);
    return true;
}

} // namespace mcp::undo
