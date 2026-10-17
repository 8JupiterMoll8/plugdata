/*
 // Pd operations the MCP bridge needs on top of pd::Interface (Plan 3 P3.2 S3).
 // They were fork additions inside Source/Pd/Interface.h, which now matches
 // upstream (docs/mcp-bridge/SEAM.md). Callers: Source/Pd/MCPBridge.cpp, the
 // mcp_* cases in PluginProcessor.cpp, Patch::canUndo/canRedo.
 */
#pragma once

#include "Pd/Interface.h"

extern "C" {
extern int garray_getname(t_garray* x, t_symbol** namep);
}

namespace mcp {

struct PdOps {

    static t_canvas* findOwnerCanvas(t_glist* gl, t_gobj* obj)
    {
        if (!gl || !obj) return nullptr;
        for (t_gobj* g = gl->gl_list; g; g = g->g_next) {
            if (g == obj) return reinterpret_cast<t_canvas*>(gl);
            if (pd_class(&g->g_pd) == canvas_class) {
                if (auto* found = findOwnerCanvas(reinterpret_cast<t_glist*>(g), obj))
                    return found;
            }
        }
        return nullptr;
    }

    static t_canvas* unlinkGobj(t_glist* gl, t_gobj* obj)
    {
        if (!gl || !obj) return nullptr;
        if (gl->gl_list == obj) {
            gl->gl_list = obj->g_next;
            return reinterpret_cast<t_canvas*>(gl);
        }
        for (t_gobj* g = gl->gl_list; g; g = g->g_next) {
            if (g->g_next == obj) {
                g->g_next = obj->g_next;
                return reinterpret_cast<t_canvas*>(gl);
            }
            if (pd_class(&g->g_pd) == canvas_class) {
                if (auto* found = unlinkGobj(reinterpret_cast<t_glist*>(g), obj))
                    return found;
            }
        }
        return nullptr;
    }

    // Audio-thread-safe object removal for /pd/batch_atomic (runs inside the
    // enqueueFunctionAsync lambda, i.e. on the audio thread before performDSP).
    // Unlike removeObjects() (message thread only), this performs:
    //   - NO canvas_undo_add (undo machinery is scheduler-thread-only; see the
    //     canvas_undo_free crash precedent — batch deletes are engine-undoable
    //     only via harness snapshots, never the Pd undo stack),
    //   - NO GUI selection / editor teardown: glist_select/deselect and gobj_vis
    //     must NOT run here because they race with the JUCE message thread.
    //   - Clean wire disconnect via canvas_deletelinesfor (in plugdata,
    //     _canvas_delete_line is a no-op, so this safely unhooks all t_outconnects
    //     under canvas_suspend_dsp without touching GUI).
    //   - Unlinks from gl_editor->e_selection / e_grab so MessageManager never
    //     dereferences the freed pointer.
    //   - Frees rtext via rtext_free.
    // What it does: disconnect wires + unlink editor refs + unlink gl_list + pd_free.
    // pd_free clears weakrefs atomically (Instance::clearWeakReferences hook),
    // and the trailing synchroniseCanvases (message thread) removes editor components
    // while canvas_update_dsp rebuilds the DSP tree safely.
    static void removeObjectsAudioThread(t_canvas* cnv, SmallArray<t_gobj*> const& objects)
    {
        int const dspstate = canvas_suspend_dsp();

        for (auto* obj : objects) {
            if (!obj) continue;
            if (libpd_this_instance()->pd_newest == &obj->g_pd)
                libpd_this_instance()->pd_newest = nullptr;

            // 1. Find owning canvas across canvas and subcanvases first
            t_canvas* owner = findOwnerCanvas(cnv, obj);
            if (!owner) {
                for (t_canvas* c = pd_this->pd_canvaslist; c && !owner; c = c->gl_next) {
                    owner = findOwnerCanvas(c, obj);
                }
            }

            t_canvas* targetCnv = owner ? owner : cnv;

            // 2. Disconnect all lines and free rtext cleanly WHILE STILL IN gl_list!
            // (canvas_deletelinesfor uses linetraverser which walks gl_list to disconnect outlets)
            if (auto* ob = pd_checkobject(&obj->g_pd)) {
                canvas_deletelinesfor(targetCnv, ob);
                if (targetCnv->gl_editor) {
                    if (auto* rt = glist_getrtext(targetCnv, ob, 0))
                        rtext_free(rt);
                }
            }

            // 3. Unlink from editor selection and grab to protect MessageManager
            if (targetCnv->gl_editor) {
                if (targetCnv->gl_editor->e_grab == obj)
                    targetCnv->gl_editor->e_grab = nullptr;

                t_selection** prevSel = &targetCnv->gl_editor->e_selection;
                while (*prevSel) {
                    if ((*prevSel)->sel_what == obj) {
                        t_selection* toFree = *prevSel;
                        *prevSel = (*prevSel)->sel_next;
                        freebytes(toFree, sizeof(*toFree));
                    } else {
                        prevSel = &(*prevSel)->sel_next;
                    }
                }
            }

            // 4. Unlink from gl_list (ONLY AFTER lines are disconnected)
            unlinkGobj(targetCnv, obj);

            // 5. Free object only if safely unlinked from canvas
            if (owner) {
                int const wasdeleting = canvas_setdeleting(owner, 1);
                pd_free(&obj->g_pd);
                canvas_setdeleting(owner, wasdeleting);
            }
        }

        canvas_resume_dsp(dspstate);
        canvas_dirty(cnv, 1);
    }

    // Audio-thread-safe WHOLESALE clear. Collects the top-level objects and
    // removes them via removeObjectsAudioThread (unlinks editor selection/rtext,
    // disconnects wires, frees via pd_free). Unlike the raw "clear" message
    // (glist_clear -> glist_delete), which leaves the JUCE editor holding stale
    // pointers to freed objects and segfaults on the next GUI refresh — the
    // exact crash class as the batch-delete bug, but on the load/clear path.
    static void clearCanvasAudioThread(t_canvas* cnv)
    {
        if (!cnv) return;
        SmallArray<t_gobj*> objs;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) objs.add(g);
        if (objs.size() > 0)
            removeObjectsAudioThread(cnv, objs);
    }

    // The bridge reaches canvases without an editor (closed subpatches, canvases
    // found by symbol). pd::Interface::removeObjects needs one (glist_select,
    // the undo sequence), so route those to the audio-thread delete.
    static void removeObjects(t_canvas* cnv, SmallArray<t_gobj*> const& objects)
    {
        if (!cnv) return;

        if (!cnv->gl_editor) {
            removeObjectsAudioThread(cnv, objects);
            return;
        }

        pd::Interface::removeObjects(cnv, objects);
    }

    // Message-thread-safe WHOLESALE clear. Collects top-level objects and
    // removes them via removeObjects() (deselects editor, registers undo,
    // glist_delete). Use this from the JUCE message thread — creating/deleting
    // objects off the message thread is what crashed the load path.
    static void clearCanvas(t_canvas* cnv)
    {
        if (!cnv) return;
        SmallArray<t_gobj*> objs;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) objs.add(g);
        if (objs.size() > 0)
            removeObjects(cnv, objs);
    }

    // pd::Interface::renameObject needs an editor and the object's rtext; an
    // object that is not visible has no rtext (glist_getrtext(.., 0) returns 0)
    // and would be dereferenced. Skip the rename then, as the fork's guards in
    // Interface.h did before P3.2 S3.
    static void renameObject(t_canvas* cnv, t_gobj* obj, char const* buf, size_t const bufsize)
    {
        if (!cnv || !cnv->gl_editor) return;
        if (!glist_getrtext(cnv, reinterpret_cast<t_text*>(obj), 0)) return;

        pd::Interface::renameObject(cnv, obj, buf, bufsize);
    }

    // Read Pd's undo queue directly. Patch's own flags (canPatchUndo/Redo) are
    // updated asynchronously by the canvas_undo_redo callback, so they lag right
    // after a mutation. Unlocked read from the message thread: P3.4.
    static int canUndo(t_canvas* cnv)
    {
        if (!cnv) return 0;
        t_undo* udo = canvas_undo_get(cnv);

        if (udo && udo->u_queue && udo->u_last && udo->u_last != udo->u_queue) {
            return udo->u_last->name && strcmp(udo->u_last->name, "no") != 0;
        }

        return 0;
    }

    static int canRedo(t_canvas* cnv)
    {
        if (!cnv) return 0;
        t_undo* udo = canvas_undo_get(cnv);

        if (udo && udo->u_queue && udo->u_last && udo->u_last->next) {
            return udo->u_last->next->name && strcmp(udo->u_last->next->name, "no") != 0;
        }

        return 0;
    }

};

} // namespace mcp
