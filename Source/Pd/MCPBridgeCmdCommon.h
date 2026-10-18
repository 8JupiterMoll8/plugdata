/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Everything a bridge command file needs (Plan 3 P3.3): the includes the handlers
// were written against, the Pd C declarations they use, and the few helpers that
// more than one command file shares. MCPBridge.cpp and every MCPBridgeCmd<Domain>.cpp
// include this and nothing else of the bridge's own.

#pragma once

#include "MCPBridge.h"
#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Mcp/McpGuiHost.h" // all GUI work (Plan 3 P3.2 S6): no plugdata GUI header here
#include "Mcp/McpGuiFields.h" // t_fake_knob & co. reads (generated AllGuis.h)
#include "Constants.h" // overlay bits (Origin ... AIHud)
#include <g_all_guis.h>       // t_slider live fields (x_min/x_max/x_fval) for obj_get
#include "Pd/Interface.h"
#include "Mcp/PdOps.h" // fork Pd ops (removeObjects routing, audio-thread delete, clear)
#include "McpIemArgs.h" // IEM GUI short-form creation-arg guard (shared)
#include "MCPBridgeSafety.h" // loopback + output-path guards (Report F6)
#include "MCPBridgeHandoff.h" // safe timed waits on queued lambdas (Report F3)
#include "MCPIdentity.h" // renameStableId (finding C6)
#include "MCPJson.h" // jsonEscape for hand-built JSON (finding C6)
#include "MCPBridgeInternal.h" // getArgString/getArgFloat/jsonEscape/... shared with the command files
#include "Utility/SettingsFile.h" // compiledMode (hvcc_mode) in /pd/perf
#include "Utility/Config.h"
#include "../../Libraries/fftw3/api/fftw3.h"
#include "Pd/Library.h"

#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <map>

#include <algorithm>

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
#include <s_inter.h>
#include <g_all_guis.h> // t_iemgui x_snd/x_rcv for screenshot GUI-widget labels
#include <m_imp.h>      // obj_starttraverseoutlet/obj_nexttraverseoutlet (duplicate-wire check)

extern "C" {
extern t_class* clone_class; // defined in g_clone.c (pd core)
extern int clone_get_n(t_gobj*);
extern t_glist* clone_get_instance(t_gobj*, int);
}

extern t_class *text_class;

struct _outlet
{
    t_object *o_owner;
    struct _outlet *o_next;
    t_outconnect *o_connections;
    t_symbol *o_sym;
};
}

// ── Undo-stack safety ──────────────────────────────────────────────
// Free a canvas's Pd undo queue after an MCP mutation that removes or
// replaces objects (delete / clear / load).
//
// Why: MCP mutation paths (pasteDirect for create, removeObjectsAudioThread
// for delete) and the wholesale clear/load paths intentionally do NOT
// register Pd undo actions. But Pd's undo entries reference their target
// objects by *index* (canvas_undo_apply stores u_index, resolved via
// glist_nth; create/recreate/cut do the same). Deleting or clearing objects
// shifts or frees those targets, leaving the queue holding stale
// t_undo_action nodes whose data/indices no longer match the live list. The
// next undo/redo walks them and dereferences freed memory — the observed
// "unsupported undo command <garbage int>" heap-corruption crash.
//
// Since MCP never contributes correct undo actions, the only safe state for
// the queue after such a mutation is EMPTY. Routes through
// receiveSysMessage("mcp_clear_undo"), the path /pd/clear_undo uses; that
// runs synchronously on the calling thread and takes the Pd lock itself
// (canvas_undo_free -> canvas_suspend_dsp needs the lock, not a thread).
// Plan 3 P3.4 S2: synchronous on purpose. Queued, the clear ran up to one
// audio block later, after the caller had pushed its own MCP transaction
// (finding L12: batch_atomic's undo never worked) or after the NEXT command
// had pushed one (a move right after a batch lost its undo).
inline void resetCanvasUndo(PluginProcessor* proc, juce::String const& canvasName)
{
    if (!proc) return;
    SmallArray<pd::Atom> atoms;
    atoms.add(pd::Atom(proc->generateSymbol(canvasName)));
    proc->receiveSysMessage("mcp_clear_undo", atoms);
}

// ── PRD_OBJ_GET_SET P4: shadow of values posted to named buses (/param) ──────
// Written by the /param domain, read by /pd/param_get.
inline std::mutex s_paramShadowMutex;
inline std::unordered_map<std::string, std::vector<t_atom>> s_paramShadow;

// =========================================================================
// Zero-Dropout Paste Helpers
// =========================================================================

// Direct binbuf_eval paste — bypasses canvas_dopaste entirely.
// Skips: undo serialization, object selection, editmode switch,
// and the inner canvas_suspend_dsp/canvas_resume_dsp cycle.
inline void pasteDirect(t_canvas* cnv, char const* buf)
{
    // Sanitise the paste buffer: drop top-level "#A" (array) lines.
    //
    // pasteDirect nulls sym_A->s_thing (below) because pasted object lines
    // never carry array data — but binbuf_eval still DISPATCHES any "#A" line
    // to that symbol. With s_thing == nullptr that is a null dereference →
    // segfault. GOP-module saves leak stray "#A saved" lines (ELSE savestate
    // objects inside the abstraction), so loading ANY patch that references a
    // GOP module crashed. Arrays are recreated by their "#X obj table/array"
    // lines, so dropping the stray "#A" rows is safe.
    juce::String filtered;
    {
        juce::StringArray lines;
        lines.addLines(juce::String(buf));
        for (auto& ln : lines) {
            if (ln.trimStart().startsWith("#A ")) continue;
            filtered += ln;
            filtered += "\n";
        }
    }

    size_t const len = filtered.getNumBytesAsUTF8();
    t_binbuf* b = binbuf_new();
    binbuf_text(b, filtered.toRawUTF8(), len);

    t_symbol* sym_X = gensym("#X");
    t_symbol* sym_N = gensym("#N");
    t_symbol* sym_A = gensym("#A");

    t_pd* saved_X = sym_X->s_thing;
    t_pd* saved_N = sym_N->s_thing;
    t_pd* saved_A = sym_A->s_thing;

    sym_A->s_thing = nullptr;
    sym_X->s_thing = &cnv->gl_pd;
    sym_N->s_thing = &pd_canvasmaker;

    binbuf_eval(b, 0, 0, 0);

    sym_A->s_thing = saved_A;
    sym_X->s_thing = saved_X;
    sym_N->s_thing = saved_N;

    binbuf_free(b);
}

// Shared: the live object bounds of the JUCE Canvas showing a Pd canvas
// (message thread; nullptr when no view shows it). Was mcpFindGuiCanvasFor.
inline std::unique_ptr<McpGuiHost::LiveBounds> mcpLiveBounds(PluginProcessor* proc, t_canvas* c)
{
    if (!proc || !c) return nullptr;
    return proc->getMcp()->gui().liveObjectBounds(c);
}

// Shared true-rect bounds (GUI-aware). MUST run on the JUCE message thread
// (reads Canvas object components). Used by the layout X-ray AND the inline
// layout guard so both agree on geometry.
inline void mcpGetTrueObjectBounds(t_canvas* c, t_gobj* y, McpGuiHost::LiveBounds const* guiCanvas, int* x, int* yy, int* w, int* h)
{
    *x = 0; *yy = 0; *w = 0; *h = 0;
    pd::Interface::getObjectBounds(c, y, x, yy, w, h);

    if (guiCanvas) {
        auto it = guiCanvas->find(y);
        if (it != guiCanvas->end()) {
            auto b = it->second;
            if (b.getWidth() > 0 && b.getHeight() > 0) {
                *w = b.getWidth();
                *h = b.getHeight();
                return;
            }
        }
    }

    if (pd::Interface::isTextObject(y) && (*w <= 10 || *h <= 10)) {
        t_text* textObj = reinterpret_cast<t_text*>(y);
        if (textObj && textObj->te_binbuf) {
            char* textBuf = nullptr;
            int textSize = 0;
            binbuf_gettext(textObj->te_binbuf, &textBuf, &textSize);
            if (textBuf && textSize > 0) {
                int fontWidth = glist_fontwidth(c);
                int fontHeight = glist_fontheight(c);
                if (fontWidth <= 0) fontWidth = 7;
                if (fontHeight <= 0) fontHeight = 14;
                juce::String fullText = juce::String::fromUTF8(textBuf, textSize).trim();
                auto lines = juce::StringArray::fromLines(fullText);
                int maxLineLen = 0;
                for (const auto& line : lines) maxLineLen = std::max(maxLineLen, line.length());
                int textW = maxLineLen * fontWidth + 12;
                int textH = std::max(1, lines.size()) * fontHeight + 7;
                if (*w <= 10) *w = textW;
                if (*h <= 10) *h = textH;
                freebytes(textBuf, textSize);
            }
        }
    }

    if (*w <= 0) *w = 60;
    if (*h <= 0) *h = 20;
}

// Decorative/background objects are NEVER collision sources: a faceplate [pic]
// intentionally overlaps every widget placed on it, and [cnv] boxes/lines are
// the same category. Including them shoved a console's whole GUI off its
// background image on every layout pass.
inline bool mcpIsDecorativeBackground(t_gobj* g)
{
    if (!g) return false;
    t_class* cl = pd_class(&g->g_pd);
    if (!cl) return false;
    juce::String name = juce::String::fromUTF8(class_getname(cl));
    return name == "pic" || name.endsWithIgnoreCase("/pic")
        || name == "cnv" || name.endsWithIgnoreCase("/cnv");
}

// ── Spectral analysis (MCPBridgeSpectral.cpp), shared by the render, the /meter/spectral probe and the armed capture ──
juce::var spectralVarFromSamples(float const* samples, int N, double sampleRate);
juce::String spectralJsonFromSamples(float const* samples, int N, double sampleRate);
juce::String renderSpectralJson(float const* data, int n, double sampleRate);
juce::String armedSpectralJson(float const* data, int n, double sampleRate);
float estimateFrequency(const float* buf, int n, float sampleRate); // defined near ProbeManager (MCPBridge.cpp)

// ── mcpIoletX — true Pd iolet anchor X ──────────────────────────────────
// Ports sit at  objX + (objW - IOWIDTH) * index / (count-1) + IOMIDDLE  (see
// pure-data g_canvas.c drawline / Interface.h), i.e. a single-port object's
// port is at its LEFT edge — NOT its centre. Modelling a cord as centre→centre
// therefore MISSES occlusions whenever the real port is off-centre (most
// objects) and could also report false ones — the "C++ says 0 occlusions but
// the wire clearly runs through the box" false negative.
inline double mcpIoletX(int objX, int objW, int index, int count)
{
    int plus = (count <= 1 ? 1 : count - 1);
    return static_cast<double>(objX)
         + (static_cast<double>(objW - IOWIDTH) * index) / plus
         + IOMIDDLE;
}

// The layout verbs measure through this name; one definition for all of them.
inline void getTrueObjectBounds(t_canvas* c, t_gobj* y, McpGuiHost::LiveBounds const* guiCanvas, int* x, int* yy, int* w, int* h)
{
    mcpGetTrueObjectBounds(c, y, guiCanvas, x, yy, w, h);
}

#include "MCPBridgeControlWindow.h"
