/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// One undo history for AI and manual edits (Plan 4 P4.4). MCP mutations register Pd's OWN undo actions on the
// canvas they change, so Pd's per-canvas queue holds every edit in time order: one AI call is one undo sequence
// (decision 9), Ctrl+Z and /pd/undo walk the same queue, and nothing has to flush it. Pd's entries address objects by
// index, which stays valid because every MCP change on a registrable() canvas is registered in order; anything
// else (closed GOP modules, load, clear, encapsulate) flushes that canvas's queue.
// Every function here runs in the Pd context (Pd lock held).
#pragma once

#include "Pd/Interface.h"

#include <algorithm>
#include <array>
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

// P6.2 UF: a read-only look at the queue, for "undo the whole AI turn". `depth` is the number of steps Ctrl+Z (and
// /pd/undo) can take from the undo position back to the queue head, `redo` the number /pd/redo can take forward; a
// SEQUENCE_END ... SEQUENCE_START run (nested runs included) is ONE step, exactly as canvas_undo_undo and
// canvas_undo_redo take it. `top` holds the names of the newest min(depth, maxNames) undo steps, newest first: a
// sequence is named by its START entry ("AI edit" for every MCP call, plugdata's GUI names "Move", "Paste object/s"
// ..., vanilla's "motion", "apply" ...). A sequence still open (a GUI drag or an AI call in progress) is not a step
// yet: its newest inner entry is, under that entry's own name, which is what Ctrl+Z would take now.
// Pd's names are string literals or symbol names (Patch::startUndoSequence passes a symbol): both live for the
// whole session, so the pointers stay valid after the lock is released. Walks pointers only: no allocation.
inline constexpr int kPeekMaxNames = 32;

struct Peek {
    int depth = 0;
    int redo = 0;
    int named = 0;                                // entries of `top` in use
    std::array<char const*, kPeekMaxNames> top {}; // newest first
};

inline Peek peek(t_canvas* cnv, int maxNames = kPeekMaxNames)
{
    Peek p;
    int const cap = std::clamp(maxNames, 0, kPeekMaxNames);
    t_undo* u = cnv ? canvas_undo_get(cnv) : nullptr;
    if (!u || !u->u_queue || !u->u_last)
        return p;
    // Backwards from the undo position to the head (UNDO_INIT), where canvas_undo_undo stops.
    for (t_undo_action* a = u->u_last; a && a != u->u_queue && a->type != UNDO_INIT; a = a->prev) {
        char const* name = a->name;
        if (a->type == UNDO_SEQUENCE_END) {
            int nesting = 1;
            t_undo_action* s = a->prev;
            for (; s && s->type != UNDO_INIT; s = s->prev) {
                if (s->type == UNDO_SEQUENCE_END)
                    ++nesting;
                else if (s->type == UNDO_SEQUENCE_START && --nesting < 1)
                    break;
            }
            if (!s || s->type == UNDO_INIT) { // no START (Pd: "undo sequence missing start"): Pd's walk ends at the head
                if (p.named < cap) p.top[p.named++] = name ? name : "";
                ++p.depth;
                break;
            }
            name = s->name;
            a = s; // the loop's step goes on before the START
        }
        if (p.named < cap) p.top[p.named++] = name ? name : "";
        ++p.depth;
    }
    // Forwards from the undo position: the steps canvas_undo_redo can take.
    for (t_undo_action* a = u->u_last->next; a; a = a->next) {
        if (a->type == UNDO_SEQUENCE_START) {
            int nesting = 1;
            t_undo_action* s = a->next;
            for (; s; s = s->next) {
                if (s->type == UNDO_SEQUENCE_START)
                    ++nesting;
                else if (s->type == UNDO_SEQUENCE_END && --nesting < 1)
                    break;
            }
            if (!s) { // no END: Pd's redo takes the rest as one step
                ++p.redo;
                break;
            }
            a = s;
        }
        ++p.redo;
    }
    return p;
}

// A closed GOP subpatch draws in its parent (glist_getcanvas walks up): selection there would make rtexts in the
// parent's editor and motion would mix parent and own coordinates (review 2026-10-20). Such a canvas registers
// nothing; its edits flush its queue instead, as before P4.4.
inline bool registrable(t_canvas* cnv)
{
    return cnv && !(cnv->gl_owner && cnv->gl_isgraph && !cnv->gl_havewindow);
}

// Bracket one AI call. Opened lazily, on the first action registered: an AI call that changes nothing leaves no
// step, and does not drop the artist's redo history (Pd rebranches when an action is added). Pd removes an empty
// sequence itself.
inline void beginStep(t_canvas* cnv) { canvas_undo_add(cnv, UNDO_SEQUENCE_START, kAiStep, nullptr); }
inline void endStep(t_canvas* cnv) { canvas_undo_add(cnv, UNDO_SEQUENCE_END, kAiStep, nullptr); }

struct Step {
    t_canvas* cnv = nullptr;
    bool open = false;
    void ensure()
    {
        if (!open && cnv) {
            beginStep(cnv);
            open = true;
        }
    }
    void close()
    {
        if (open) endStep(cnv);
        open = false;
    }
};

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
// wires). Leaves nothing selected. Only on a registrable() canvas.
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

// Motion entries. canvas_undo_move moves each object by (stored - gobj_getrect x1/y1), so the stored positions are
// rect positions (not te_xpix: a comment's rect is 1 px lower). The buffer mirrors g_editor.c's t_undo_move (as
// Interface.h mirrors Pd's instance editor).
inline constexpr t_float kNoPosition = -1.0e9f;

struct Positions {
    std::vector<std::pair<t_float, t_float>> at; // by gl_list index; kNoPosition for a non-patchable gobj
};

inline Positions positions(t_canvas* cnv)
{
    Positions p;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (pd_checkobject(&g->g_pd)) {
            gobj_getrect(g, cnv, &x1, &y1, &x2, &y2);
            p.at.emplace_back(static_cast<t_float>(x1), static_cast<t_float>(y1));
        } else {
            p.at.emplace_back(kNoPosition, kNoPosition);
        }
    }
    return p;
}

// One motion entry whose undo puts every object at `at` (rect positions by index). The "move already registered"
// flag a GUI drag uses is left as it was, so the artist's next drag still registers.
inline void motionTo(t_canvas* cnv, Positions const& at)
{
    struct MoveElem { int e_index; t_float e_xpix; t_float e_ypix; };
    struct Move { MoveElem* u_vec; int u_n; };
    auto* ed = pd::Interface::getInstanceEditor();
    int const was = ed->canvas_undo_already_set_move;
    auto* buf = static_cast<Move*>(canvas_undo_set_move(cnv, 0));
    ed->canvas_undo_already_set_move = was;
    for (int i = 0; i < buf->u_n; ++i) {
        auto const idx = static_cast<size_t>(buf->u_vec[i].e_index);
        if (idx < at.at.size()) {
            buf->u_vec[i].e_xpix = at.at[idx].first;
            buf->u_vec[i].e_ypix = at.at[idx].second;
        }
    }
    canvas_undo_add(cnv, UNDO_MOTION, "motion", buf);
}

// Before objects move later in the same step (the batch's layout guard): undo puts them where they are now.
inline void motion(t_canvas* cnv) { motionTo(cnv, positions(cnv)); }

// Layout passes that move objects themselves (move_batch_id, sanitizeLayout, compose, the rect loops): take the
// positions before, then movedSince() registers ONE step whose undo puts every object back, only when something
// moved and the canvas is registrable().
inline bool movedSince(t_canvas* cnv, Positions const& before)
{
    if (!registrable(cnv)) return false;
    auto const now = positions(cnv);
    if (now.at.size() != before.at.size() || now.at == before.at)
        return false; // nothing moved (or objects came or went: not a pure move, nothing safe to register)
    beginStep(cnv);
    motionTo(cnv, before);
    endStep(cnv);
    return true;
}

// Pd's undo entries address objects by index: after an unregistered structural change the queue must go.
inline void flush(t_canvas* cnv)
{
    canvas_undo_free(cnv);
    canvas_undo_init(cnv);
}

} // namespace mcp::undo
