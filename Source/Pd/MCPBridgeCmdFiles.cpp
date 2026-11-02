/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: patch files and tabs (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

#include <cstring>

namespace {

// P11.2 V10: a .pd file read and checked BEFORE any canvas is cleared. `error` is set (and the caller clears
// nothing) when the file is missing, binbuf_read fails, or the file is not a whole Pd patch (review H1): no
// "#N canvas" header (leading "#N struct" template lines are allowed and kept), a subpatch or graph left open at the
// end (depth != 0), a "#X restore" with no open subpatch, atoms after the last ';', or foreign statements (not
// #X/#N/#A) with no box at all. A valid empty patch ("#N canvas ...;" only) is a valid body of 0 objects.
// `fileObjects` counts the boxes the paste creates on the target canvas: statements at depth 0 (obj, msg,
// floatatom, symbolatom, listbox, text, scalar) and each "#X restore" that closes a depth-1 subpatch or graph;
// `fileWires` the "#X connect" lines at depth 0.
struct PatchBody {
    std::string text; // review L6: the bytes as the file has them (a symbol that is not UTF-8 is not rewritten)
    int fileObjects = 0;
    int fileWires = 0;
    int arrayLines = 0; // review L6: "#A" array-content lines (not "#A saved"); the paste does not restore them
    juce::String error;
};

bool atomIs(t_atom const& a, char const* s)
{
    return a.a_type == A_SYMBOL && a.a_w.w_symbol && std::strcmp(a.a_w.w_symbol->s_name, s) == 0;
}

PatchBody readPatchBody(PluginProcessor& processor, juce::File const& file)
{
    PatchBody out;
    if (!file.existsAsFile()) {
        out.error = "error: file not found: " + file.getFullPathName();
        return out;
    }
    juce::String const untouched = " (the canvas was not touched)";
    // Review L7: binbuf_read makes Pd symbols (gensym): under the Pd lock.
    McpHost::PdLockScope pdLock(processor);
    t_binbuf* b = binbuf_new();
    if (binbuf_read(b, file.getFileName().toRawUTF8(), file.getParentDirectory().getFullPathName().toRawUTF8(), 0) != 0) {
        binbuf_free(b);
        out.error = "error: could not read " + file.getFullPathName() + untouched;
        return out;
    }
    int const natom = binbuf_getnatom(b);
    t_atom* vec = binbuf_getvec(b);
    auto fail = [&](juce::String const& why) {
        binbuf_free(b);
        out.error = "error: " + why + ": " + file.getFullPathName() + untouched;
        return out;
    };
    // Leading "#N struct" template statements (kept for the paste), then the "#N canvas" header (dropped).
    int headerAt = 0;
    std::vector<std::pair<int, int>> keep; // [from, to) atom ranges for the paste body
    while (headerAt + 1 < natom && atomIs(vec[headerAt], "#N") && atomIs(vec[headerAt + 1], "struct")) {
        int end = headerAt;
        while (end < natom && vec[end].a_type != A_SEMI) ++end;
        if (end >= natom) break;
        keep.push_back({ headerAt, end + 1 });
        headerAt = end + 1;
    }
    if (natom - headerAt < 2 || !atomIs(vec[headerAt], "#N") || !atomIs(vec[headerAt + 1], "canvas"))
        return fail("not a Pd patch (no '#N canvas' header)");
    if (vec[natom - 1].a_type != A_SEMI)
        return fail("not a whole Pd patch (atoms after the last ';')");
    int startIdx = natom;
    for (int i = headerAt; i < natom; i++) {
        if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
    }
    int depth = 0;
    bool foreign = false;
    for (int i = startIdx; i < natom;) {
        int end = i;
        while (end < natom && vec[end].a_type != A_SEMI) ++end;
        if (end - i >= 2 && atomIs(vec[i], "#N") && atomIs(vec[i + 1], "canvas")) {
            ++depth;
        } else if (end - i >= 2 && atomIs(vec[i], "#X")) {
            if (atomIs(vec[i + 1], "restore")) {
                if (depth == 0)
                    return fail("not a whole Pd patch (a '#X restore' closes no subpatch)");
                if (--depth == 0) ++out.fileObjects;
            } else if (depth == 0) {
                if (atomIs(vec[i + 1], "connect")) {
                    ++out.fileWires;
                } else {
                    for (char const* k : { "obj", "msg", "floatatom", "symbolatom", "listbox", "text", "scalar" })
                        if (atomIs(vec[i + 1], k)) { ++out.fileObjects; break; }
                }
            }
        } else if (end > i && atomIs(vec[i], "#A")) {
            if (!(end - i >= 2 && atomIs(vec[i + 1], "saved")))
                ++out.arrayLines;
        } else if (end > i && !atomIs(vec[i], "#N") && !atomIs(vec[i], "#X")) {
            foreign = true;
        }
        i = end + 1;
    }
    if (depth != 0)
        return fail("not a whole Pd patch (cut off inside a subpatch)");
    if (foreign && out.fileObjects == 0)
        return fail("not a Pd patch (no box, only foreign lines)");
    keep.push_back({ startIdx, natom });
    t_binbuf* body = binbuf_new();
    for (auto const& [from, to] : keep)
        if (to > from) binbuf_add(body, to - from, vec + from);
    char* textBuf = nullptr;
    int textLen = 0;
    binbuf_gettext(body, &textBuf, &textLen);
    if (textBuf && textLen > 0) {
        out.text.assign(textBuf, static_cast<size_t>(textLen));
        freebytes(textBuf, textLen);
    }
    binbuf_free(body);
    binbuf_free(b);
    return out;
}

// Wires on `c` itself (review H1: compared with the file's "#X connect" lines after the paste).
int countWires(t_canvas* c)
{
    int n = 0;
    t_linetraverser t;
    linetraverser_start(&t, c);
    while (linetraverser_next_nosize(&t)) ++n;
    return n;
}

// Pd's red boxes after a paste: text_class objects of type T_OBJECT (a class that could not be created). Top-level
// entries carry their index; ones inside subpatches/abstractions/clone instances the top-level container's index and
// a path. Review L8: an empty box ("[ ]", no atoms) is not a failed create; clone instances are walked.
void collectFailedCreates(t_canvas* c, int topIndex, juce::String const& path, juce::Array<juce::var>& out, int depth)
{
    if (!c || depth > 32) return;
    int index = 0;
    for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
        int const top = depth == 0 ? index : topIndex;
        if (pd_class(&g->g_pd) == text_class && reinterpret_cast<t_text*>(g)->te_type == T_OBJECT
            && reinterpret_cast<t_text*>(g)->te_binbuf && binbuf_getnatom(reinterpret_cast<t_text*>(g)->te_binbuf) > 0) {
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
                if (auto* inst = clone_get_instance(g, i))
                    collectFailedCreates(reinterpret_cast<t_canvas*>(inst), top,
                        (path.isEmpty() ? juce::String() : path + "/") + juce::String(index) + ":clone#" + juce::String(i), out, depth + 1);
        }
    }
}

// The "#A saved" lines ELSE savestate objects inside GOP abstractions leak into a canvas's content. They are not
// real array data (legitimate arrays carry their own name, e.g. "#A gtab") and they break hvcc and external Pd.
// save_patch and save_content both strip them, so the two write the same bytes (P11.2 V11).
juce::String stripSavedArrayLines(juce::String const& content)
{
    juce::StringArray contentLines;
    contentLines.addLines(content);
    juce::String clean;
    for (auto& ln : contentLines) {
        if (ln.trimStart().startsWith("#A saved")) continue;
        clean += ln;
        clean += "\n";
    }
    return clean;
}

} // namespace

void MCPBridge::pdLoad(const juce::String& action, const juce::OSCMessage& msg)
{
    if (runInPdContext(&MCPBridge::pdLoad, action, msg)) return; // P3.4 S3: structural edit
    if (msg.size() >= 2 && processor) {
        auto file = getArgString(msg[0]);
        auto folder = getArgString(msg[1]);
        sys_lock();
        t_atom args[2];
        SETSYMBOL(&args[0], gensym(file.toRawUTF8()));
        SETSYMBOL(&args[1], gensym(folder.toRawUTF8()));
        pd_typedmess(gensym("pd")->s_thing, gensym("open"), 2, args);
        sys_unlock();
        sendRawReply("/pd/loaded");
    }
    return;
}

    // /pd/save_content <canvasName> <destFilePath> [correlationId]
    // Serialises the live canvas to a .pd file using getCanvasContent() — pure
    // in-memory path, no binbuf_write overhead. Faster than /pd/dump and does
    // not require a separate dumpDir argument.
    // Reply: /pd/save_content/reply/<corrId>  arg0=destFilePath  (or error string)
void MCPBridge::pdSaveContent(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto destFilePath  = getArgString(msg[1]);
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

        juce::String content;
        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            char* buf = nullptr;
            int   bufsize = 0;
            pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
            if (buf) {
                content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
            }
        }
        sys_unlock();

        juce::String replyAddr = correlationId.isNotEmpty()
            ? "/pd/save_content/reply/" + correlationId
            : "/pd/save_content/reply";

        if (content.isEmpty()) {
            sendReply(replyAddr, juce::String("error: canvas not found or empty"));
            return;
        }

        // Write content to destination path provided by TS (P11.2 V11: the same bytes save_patch writes)
        content = stripSavedArrayLines(content);
        juce::File destFile(destFilePath);
        if (!destFile.replaceWithText(content)) {
            sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
            return;
        }

        // arg1 (P11.2 V11): SHA-256 of the written file, the hash the identity sidecar stores as pdHash
        juce::Array<juce::var> args;
        args.add(destFilePath);
        args.add(MCPBridge::sidecarPdHash(destFile));
        sendReply(replyAddr, args);
    }
    return;
}

    // /pd/load_content <canvasName> <srcFilePath> [correlationId]
    // Clears the canvas and reconstructs it atomically from a .pd file using
    // binbuf_read + binbuf_eval — Pd's native file loader. Handles the full
    // .pd format (including #N canvas header) correctly.
    // DSP recompiles once at the end. Disk read is outside sys_lock.
    // Reply: /pd/load_content/reply/<corrId>  arg0=objectCount (float)
void MCPBridge::pdLoadContent(const juce::String& action, const juce::OSCMessage& msg)
{
    if (runInPdContext(&MCPBridge::pdLoadContent, action, msg)) return; // P3.4 S3: structural edit
    if (msg.size() >= 2 && processor) {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto srcFilePath   = getArgString(msg[1]);
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

        juce::String replyAddr = correlationId.isNotEmpty()
            ? "/pd/load_content/reply/" + correlationId
            : "/pd/load_content/reply";

        // P11.2: read and check the file BEFORE the clear; a missing, unreadable or non-patch file is -1, untouched.
        auto const patchBody = readPatchBody(*processor, juce::File(srcFilePath));
        if (patchBody.error.isNotEmpty()) {
            sendReply(replyAddr, -1.0f);
            return;
        }

        int objectCount = 0;

        // Stop all active probes before clearing — they hold t_outconnect*
        // pointers that become dangling after canvas clear.
        probeManager.stopAllProbes("load_content_reset");

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            // 1. Clear existing objects
            mcp::PdOps::clearCanvasAudioThread(cnv);

            // 2. Paste the checked body (#X obj and #X connect via pasteDirect, sym_X bound to the canvas: the
            //    same path batch_atomic uses).
            if (!patchBody.text.empty())
                pasteDirect(cnv, patchBody.text.c_str());

            // 3. Fire loadbang on the ROOT canvas (canvas_loadbang recurses
            // into child subpatches). Previously only direct child subpatches
            // were banged, so the root's own [loadbang] objects and iemgui
            // widgets never received LB_LOAD — a reloaded patch's sliders
            // never re-sent their values downstream (line~) and it came up
            // SILENT even though the saved values were correct. #81.
            canvas_loadbang(cnv);

            // 4. DSP graph recompile
            canvas_update_dsp();

            // Count objects
            for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                objectCount++;
        }
        sys_unlock();

        // All prior objects were freed and replaced — their index-based undo
        // entries are now dangling. Drop the queue before anything can undo.
        resetCanvasUndo(processor, canvasName);

        // Synchronise canvas UI and restart DSP.
        // startDSP() is called directly here — same pattern as /pd/dsp
        // handler which also calls it from the OSC receiver thread.
        processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });

        if (objectCount > 0 && processor)
            processor->startDSP();

        // 5. Clear identity map — PHASE 0 reconcile fires on next batch_atomic
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(juce::String(canvasName));
        atoms.add(juce::String("0"));
        processor->getMcp()->callSys("mcp_clear_ids", atoms);

        processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });

        // arg0 = objectCount, arg1 = undoReset (clear+rebuild flushed the
        // stale undo queue). Old TS clients read arg0 and ignore arg1.
        juce::Array<juce::var> loadReplyArgs;
        loadReplyArgs.add(static_cast<double>(objectCount));
        loadReplyArgs.add(1.0);
        sendReply(replyAddr, loadReplyArgs);
    }
    return;
}

    // ── PRD Phase 3 §2.1: NATIVE SAVE WITH IDENTITY SIDECAR ─────────────
    // /pd/save_patch <canvas> <filePath> [corrId]
    // C++ owns the save: getCanvasContent writes the .pd, and a sidecar
    // (<file>.mcpids.json) carries the stable tempId map AT SAVE TIME —
    // index→tempId for the root canvas plus every named subcanvas. Identity
    // never crosses the TS boundary, so a later load can restore it exactly
    // (no positional guessing). Vanilla Pd sees a normal .pd file.
    // Reply: /pd/save_patch/reply/<corrId> <destFilePath> | "error: ..."
void MCPBridge::pdSavePatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto destFilePath  = getArgString(msg[1]);
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

        juce::String replyAddr = correlationId.isNotEmpty()
            ? "/pd/save_patch/reply/" + correlationId
            : "/pd/save_patch/reply";

        juce::String content;
        juce::var sidecar;

        // P11.2 V12: which file the tab showing this canvas saves to now (GUI state, read before the Pd lock), so the
        // reply can say when this save re-points it.
        auto const tabs = processor->getMcp()->onMessageThread([p = processor] { return p->getMcp()->gui().listTabs(); }, 2000, "save_patch");

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            char* buf = nullptr;
            int   bufsize = 0;
            pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
            if (buf) {
                content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
            }

            if (content.isNotEmpty()) {
                // Strip stray "#A saved" lines leaked by ELSE savestate objects inside GOP abstractions (shared with
                // save_content): they keep the written .pd valid for Heavy / vanilla Pd.
                content = stripSavedArrayLines(content);

                // Identity sidecar: reverse map (gobj → tempId) per canvas.
                // Built under THIS sys_lock (cnv is only guaranteed valid
                // here); the shared writer is used by the GUI-save hook.
                sidecar = buildIdentitySidecar(processor, cnv);
            }
        }
        sys_unlock();

        if (content.isEmpty()) {
            sendReply(replyAddr, juce::String("error: canvas not found or empty"));
            return;
        }

        juce::File destFile(destFilePath);
        if (!destFile.replaceWithText(content)) {
            sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
            return;
        }
        // Sidecar next to the .pd — engine-private, invisible to vanilla Pd. P3.5 S7: v2 with the .pd's hash; a
        // failed write is reported in the reply (arg1) instead of ignored.
        auto const sidecarError = MCPBridge::writeIdentitySidecarFile(sidecar, destFile);
        if (sidecarError.isNotEmpty())
            processor->logMessage("MCP save_patch: " + sidecarError);

        // PRD Phase 3 §2.5 (save-binding): bind the file to the canvas the
        // way native Save-As does — tab title + current path update.
        // NOTE: deliberately setCurrentFile ONLY — Patch::savePatch(URL)
        // also runs reloadAbstractions() (recreates abstraction instances)
        // and crashed when a queued close_tab tore the canvas down mid-
        // reload. Title derives from currentFile; pd-side state untouched.
        // Compute the canonical key NOW, on this (valid) thread — the async
        // lambda must NOT dereference the raw `cnv` pointer (it may be torn
        // down before the lambda runs; the original code only compared it).
        const std::string oldKey = MCPBridge::canonicalCanvasKey(cnv).toStdString();
        const std::string newKey = juce::File(destFilePath).getFileName().toStdString();
        processor->getMcp()->enqueuePd([p = processor, cnv, destFilePath, oldKey, newKey]() {
            juce::File f(destFilePath);
            p->getMcp()->identity.rekey(cnv); // P3.5 S3: the record follows the canvas; only its key changes
            // P3.4 S3: the tab title is GUI state: set it on the message thread, if the canvas still exists.
            p->getMcp()->postMessage([p, cnv, f] {
                sys_lock();
                bool const live = mcpCanvasIsLive(cnv);
                sys_unlock();
                if (live) p->getMcp()->gui().setPatchFile(cnv, f);
            });
        });

        // Reply (P11.2 V12): arg0 the path, arg1 "" or "ids not saved: ...", arg2 {rebound:{fromPath,toPath}|null,
        // idsSaved, tab}: `tab` = an open tab shows this canvas; `rebound` = that tab saved elsewhere until now (its
        // Ctrl+S goes to toPath from here on). null = nothing re-pointed (same file, or no tab shows the canvas).
        juce::var rebound;
        bool shownInTab = false;
        if (tabs) {
            for (auto const& tab : *tabs) {
                if (tab.patch != cnv) continue;
                shownInTab = true;
                if (tab.file != destFile) {
                    auto* r = new juce::DynamicObject();
                    r->setProperty("fromPath", tab.file.getFullPathName());
                    r->setProperty("toPath", destFile.getFullPathName());
                    rebound = juce::var(r);
                }
                break;
            }
        }
        auto* report = new juce::DynamicObject();
        report->setProperty("rebound", rebound);
        report->setProperty("idsSaved", sidecarError.isEmpty());
        report->setProperty("tab", shownInTab);
        juce::Array<juce::var> args;
        args.add(destFilePath);
        args.add(sidecarError.isEmpty() ? juce::String() : "ids not saved: " + sidecarError);
        args.add(juce::JSON::toString(juce::var(report), true));
        sendReply(replyAddr, args);
    }
    return;
}

    // ── PRD Phase 3 §2.2: NATIVE LOAD WITH IDENTITY RESTORE ─────────────
    // /pd/load_patch <canvas> <filePath> [corrId]
    // Same canvas mechanics as load_content (clear + pasteDirect + loadbang +
    // canvas_update_dsp), then re-registers the sidecar tempIds DIRECTLY into
    // the stable map (root + named subcanvases) and bumps the version once.
    // Objects without a sidecar entry are auto-adopted by the Phase 2 census
    // naming on next census. No TS involvement, no positional guessing.
    // Reply: /pd/load_patch/reply/<corrId> <objectCount> (float, -1 = file missing)
void MCPBridge::pdLoadPatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto srcFilePath   = getArgString(msg[1]);
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

        juce::String replyAddr = correlationId.isNotEmpty()
            ? "/pd/load_patch/reply/" + correlationId
            : "/pd/load_patch/reply";

        // P11.2 V10: the reply. arg0 the object count (-1 = nothing loaded; old clients read only this), arg1 1 (undo
        // reset) or "error: ...", arg2 the JSON report {canvasFound, fileObjects, created, failedCreates:[{index,
        // text, path?}], sidecar:{present, applied, skipped, renamed}, error?}.
        auto reply = [this, replyAddr](double count, juce::var status, juce::DynamicObject* report) {
            juce::Array<juce::var> args;
            args.add(count);
            args.add(status);
            args.add(juce::JSON::toString(juce::var(report), true));
            sendReply(replyAddr, args);
        };
        auto failReport = [](juce::String const& error, bool canvasFound, int fileObjects) {
            auto* r = new juce::DynamicObject();
            r->setProperty("canvasFound", canvasFound);
            r->setProperty("fileObjects", fileObjects);
            r->setProperty("created", 0);
            r->setProperty("failedCreates", juce::Array<juce::var>());
            auto* sc = new juce::DynamicObject();
            sc->setProperty("present", false);
            sc->setProperty("applied", 0);
            sc->setProperty("skipped", 0);
            sc->setProperty("renamed", 0);
            r->setProperty("sidecar", juce::var(sc));
            r->setProperty("error", error);
            return r;
        };

        // Read and check the file BEFORE anything is cleared: a missing, unreadable or non-patch file leaves the
        // canvas as it is (X#1: a load used to wipe the canvas and answer success).
        juce::File srcFile(srcFilePath);
        auto const patchBody = readPatchBody(*processor, srcFile);
        if (patchBody.error.isNotEmpty()) {
            reply(-1.0, patchBody.error, failReport(patchBody.error, true, 0));
            return;
        }
        std::string const bodyText = patchBody.text;
        int const arrayLines = patchBody.arrayLines;
        int const fileObjects = patchBody.fileObjects;
        int const fileWires = patchBody.fileWires;

        // Sidecar read OUTSIDE sys_lock (small file); P3.5 S7: a truncated or unreadable one is ignored with a note.
        juce::String sidecarNote;
        juce::var sidecar = MCPBridge::readIdentitySidecar(srcFile, sidecarNote);

        probeManager.stopAllProbes("load_patch_reset");

        // The canvas mutation MUST run on the JUCE message thread:
        // pasteDirect creates Pd objects — GUI objects, abstractions and
        // GOP [cnv] — which instantiate JUCE Components. Creating those off
        // the message thread is illegal and was the intermittent load
        // crash. Same callAsync pattern as the native encapsulate handlers.
        postMessage(
            [proc = processor, canvasName, bodyText, fileObjects, fileWires, arrayLines, sidecar, sidecarNote, srcFile, reply, failReport]() {
                int objectCount = 0;
                int wires = 0;
                MCPBridge::SidecarStats stats;
                juce::Array<juce::var> failedCreates;
                sys_lock();
                t_canvas* cnv = proc->getMcp()->getCanvasBySymbol(canvasName);
                if (!cnv) {
                    sys_unlock();
                    auto const error = "error: canvas not found: " + canvasName + " (nothing was loaded)";
                    reply(-1.0, error, failReport(error, false, fileObjects));
                    return;
                }
                {
                    // 0. Reset identity for this canvas tree BEFORE re-registering
                    proc->getMcp()->identity.clearCanvas(cnv);

                    // 1. Clear existing objects (message-thread-safe)
                    mcp::PdOps::clearCanvas(cnv);

                    // 2. Paste the prepared body
                    if (!bodyText.empty())
                        pasteDirect(cnv, bodyText.c_str());

                    // 3. Fire loadbang on the ROOT canvas (recurses into
                    // subpatches) — see #81: banging only child subpatches
                    // left top-level [loadbang]/sliders un-fired, so reloads
                    // came up silent.
                    canvas_loadbang(cnv);

                    // 4. DSP graph recompile
                    canvas_update_dsp();

                    // 5. Re-register sidecar identities (P3.5 S7: the one validating applier)
                    if (auto* sidecarObj = sidecar.getDynamicObject())
                        stats = MCPBridge::mcpApplyIdentitySidecar(proc, sidecarObj, cnv, srcFile);
                    auto const report = MCPBridge::sidecarReport(srcFile, stats, sidecarNote);
                    if (report.isNotEmpty())
                        proc->logMessage(report);

                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                        objectCount++;
                    collectFailedCreates(cnv, 0, {}, failedCreates, 0); // P11.2 V10: Pd's red boxes
                    wires = countWires(cnv); // review H1: the wires that were really made
                }
                sys_unlock();

                proc->getMcp()->synchroniseCanvases();
                if (objectCount > 0) proc->startDSP();
                // Clear/free replaced every prior object — its index-based
                // undo entries now dangle. Reset the queue to stay crash-safe.
                resetCanvasUndo(proc, canvasName);

                auto* report = new juce::DynamicObject();
                report->setProperty("canvasFound", true);
                report->setProperty("fileObjects", fileObjects);
                report->setProperty("created", objectCount);
                report->setProperty("failedCreates", failedCreates);
                auto* sc = new juce::DynamicObject();
                sc->setProperty("present", sidecar.isObject());
                sc->setProperty("applied", stats.restored);
                sc->setProperty("skipped", stats.skipped);
                sc->setProperty("renamed", stats.renamed);
                if (sidecarNote.isNotEmpty()) sc->setProperty("note", sidecarNote);
                report->setProperty("sidecar", juce::var(sc));
                // Review H1: wires the file lists but the canvas did not get; `ok` = the file is on the canvas whole
                // (every box made, none a red box, every wire made). Missing wires also turn arg1 into an error.
                int const missingWires = std::max(0, fileWires - wires);
                report->setProperty("fileWires", fileWires);
                report->setProperty("wires", wires);
                report->setProperty("missingWires", missingWires);
                // Review L6: array contents ("#A" lines) are dropped by the paste: said, and not ok.
                report->setProperty("arrayLines", arrayLines);
                if (arrayLines > 0)
                    report->setProperty("arraysNote", "array contents not restored (" + juce::String(arrayLines) + " #A lines in the file)");
                report->setProperty("ok", objectCount == fileObjects && failedCreates.isEmpty() && missingWires == 0 && arrayLines == 0);
                juce::var status = 1.0;
                if (missingWires > 0)
                    status = "error: " + juce::String(missingWires) + " of " + juce::String(fileWires) + " wires in the file were not made";
                else if (arrayLines > 0)
                    status = "error: array contents not restored (" + juce::String(arrayLines) + " #A lines)";
                reply(static_cast<double>(objectCount), status, report);
            });
    }
    return;
}

    // ── PRD Phase 3 §2.5: OPEN PATCH IN NEW TAB ─────────────────────────
    // /pd/open_patch <filePath> [corrId]
    // Opens a .pd file in a NEW editor tab (native File→Open path), then
    // pre-registers the sidecar identities if present — the tab comes up
    // fully named. Reply: /pd/open_patch/reply/<corrId> "<status text>"
void MCPBridge::pdOpenPatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto srcFilePath   = getArgString(msg[1]);
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        juce::String replyAddr = "/pd/open_patch/reply/" + correlationId;

        juce::File srcFile(srcFilePath);
        if (!srcFile.existsAsFile()) {
            sendReply(replyAddr, juce::String("error: file not found: " + srcFilePath));
            return;
        }

        // Phase 3.1 bugfix: the open lambda was observed to deliver twice (two tabs for one request). P3.5 S2b
        // (L27): the guard holds while the open is in flight (cleared when the posted open finishes), not for a
        // fixed 2 s: a time window swallowed a real reopen right after a close, and the patch came back neither
        // open nor with its ids. A repeat after the open finished meets the already-open check below.
        static std::mutex openInFlightLock;
        static std::unordered_set<juce::String> openInFlight;
        {
            std::lock_guard<std::mutex> g(openInFlightLock);
            if (!openInFlight.insert(srcFile.getFullPathName()).second) {
                sendReply(replyAddr, juce::String("ignored: open already in progress for " + srcFilePath));
                return;
            }
        }
        struct InFlightRelease {
            explicit InFlightRelease(juce::String p) : path(std::move(p)) { }
            InFlightRelease(InFlightRelease const&) = delete;
            InFlightRelease& operator=(InFlightRelease const&) = delete;
            juce::String path;
            ~InFlightRelease()
            {
                std::lock_guard<std::mutex> g(openInFlightLock);
                openInFlight.erase(path);
            }
        };
        // Released when the last owner goes: this handler on an early return, else the posted open lambda.
        auto inFlight = std::make_shared<InFlightRelease>(srcFile.getFullPathName());

        // PRD Phase 3 §2.5: duplicate-tab guard — PlugData focuses the
        // existing tab instead of duplicating a patch (by design). Reply
        // honestly instead of falling into the async no-reply path.
        // P3.4 S3: the tabs are GUI state: read them on the message thread (no Pd lock needed).
        bool alreadyOpen = false;
        if (auto tabs = processor->getMcp()->onMessageThread([p = processor] { return p->getMcp()->gui().listTabs(); }, 2000, "open_patch")) {
            for (auto const& tab : *tabs) {
                if (tab.file == srcFile) {
                    alreadyOpen = true;
                    break;
                }
            }
        }
        if (alreadyOpen) {
            sendReply(replyAddr, juce::String("already open: " + srcFile.getFullPathName() + " (focused existing tab)"));
            return;
        }

        // Sidecar read outside the message-thread hop (P3.5 S7: checked; a truncated one is ignored with a note)
        juce::String sidecarNote;
        juce::var sidecar = MCPBridge::readIdentitySidecar(srcFile, sidecarNote);

        // Acknowledge immediately — the tab-open runs on the message
        // thread (fire-and-forget); TS verifies via `tabs`/census.
        sendReply(replyAddr, juce::String("opening: " + srcFile.getFullPathName()));

        postMessage(
            [p = processor, fileStr = srcFile.getFullPathName(), sidecar, sidecarNote, inFlight]() {
            bool opened = p->getMcp()->gui().openPatch(juce::File(fileStr));
            if (!opened) {
                p->logMessage("MCP open_patch: no editor");
                return;
            }

            // Phase 3.1 bugfix: belt-and-suspenders dedup — if more than
            // one tab for this file exists, keep the first and close extras.
            juce::File f(fileStr);
            p->getMcp()->gui().closeDuplicateTabs(f);

            // P3.5 S7: the one validating applier (same as load_patch), in place under the Pd lock. It replaced a
            // queue of mcp_register_id calls per entry that checked nothing (an edited .pd got the old ids at the
            // old indices).
            MCPBridge::SidecarStats stats;
            if (auto* sidecarObj = sidecar.getDynamicObject()) {
                McpHost::PdLockScope lock(*p);
                if (auto* cnv = p->getMcp()->gui().findOpenPatch(f))
                    stats = MCPBridge::mcpApplyIdentitySidecar(p, sidecarObj, cnv, f);
            }
            auto const report = MCPBridge::sidecarReport(f, stats, sidecarNote);
            p->logMessage(juce::String("MCP open_patch: ") + fileStr
                + (stats.restored > 0 ? " (" + juce::String(stats.restored) + " names restored)" : ""));
            if (report.isNotEmpty())
                p->logMessage(report);
            (void)opened;
        });
    }
    return;
}

    // ── PRD Phase 3 §2.5: LIST OPEN TABS ────────────────────────────────
    // /pd/list_tabs [corrId]
    // Enumerates every open root tab: title, file, focused flag, object
    // count. Reply: /pd/list_tabs/reply/<corrId> <json>
void MCPBridge::pdListTabs(const juce::String& action, const juce::OSCMessage& msg)
{
    auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
    juce::String replyAddr = "/pd/list_tabs/reply/" + correlationId;

    postMessage([p = processor, bridge = this, replyAddr]() {
        juce::Array<juce::var> list;
        auto const tabs = p->getMcp()->gui().listTabs();
        auto const tokens = p->getMcp()->assignTabTokens(tabs); // P11.5 V24
        for (size_t ti = 0; ti < tabs.size(); ++ti) {
            auto const& tab = tabs[ti];
            auto* o = new juce::DynamicObject();
            // P11.5 V24: the tab's receiver namespace token ("pNN"), computed here. nsCollided: another open tab
            // held the default, so this one got the next free token (kept in its identity sidecar).
            if (ti < tokens.size()) {
                o->setProperty("ns", tokens[ti].token);
                o->setProperty("nsCollided", tokens[ti].collided);
                o->setProperty("nsFromSidecar", tokens[ti].fromSidecar);
                if (tokens[ti].overridden) { // review M4: the sidecar's token was taken by an older open tab
                    o->setProperty("nsOverridden", true);
                    o->setProperty("nsPersisted", tokens[ti].persisted);
                }
            }
            o->setProperty("title", tab.title);
            o->setProperty("file", tab.file.getFullPathName());
            o->setProperty("focused", tab.focused);
            // C++ TRUTH: the canonical canvas key (the same key the
            // identity map uses). TS derives receiver-namespace suffixes
            // from THIS so creation and triggering agree by construction.
            if (auto* cnvp = tab.patch)
                o->setProperty("key", canonicalCanvasKey(cnvp));
            int count = 0;
            sys_lock();
            if (auto* cnv = tab.patch) {
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next) count++;
            }
            sys_unlock();
            o->setProperty("objects", count);
            list.add(juce::var(o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("tabs", list);
        bridge->sendReply(replyAddr, juce::JSON::toString(juce::var(root)));
    });
    return;
}

    // ── PRD Phase 3 §2.5: FOCUS TAB ─────────────────────────────────────
    // /pd/focus_tab <file-or-name> [corrId]
    // Makes the named tab the focused one — "main" then points at it.
    // Matches by full path or file name. Reply: .../reply/<corrId> "<status>"
void MCPBridge::pdFocusTab(const juce::String& action, const juce::OSCMessage& msg)
{
    auto target       = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String replyAddr = "/pd/focus_tab/reply/" + correlationId;

    postMessage([p = processor, bridge = this, target, replyAddr]() {
        if (auto title = p->getMcp()->gui().focusTab(target)) {
            bridge->sendReply(replyAddr, juce::String("focused: ") + *title);
            return;
        }
        bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
    });
    return;
}

    // ── PRD Phase 3 §2.5: CLOSE TAB ─────────────────────────────────────
    // /pd/close_tab <file-or-name> [force] [corrId]
    // Closes the named tab. Refuses unsaved changes unless force=1.
    // Reply: .../reply/<corrId> "<status>"
void MCPBridge::pdCloseTab(const juce::String& action, const juce::OSCMessage& msg)
{
    auto target        = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    float force        = msg.size() > 1 ? getArgFloat(msg[1]) : 0.0f;
    auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
    juce::String replyAddr = "/pd/close_tab/reply/" + correlationId;

    postMessage([p = processor, bridge = this, target, force, replyAddr]() {
        auto const closed = p->getMcp()->gui().closeTab(target, force >= 0.5f);
        if (closed.status == McpGuiHost::CloseResult::Dirty) {
            bridge->sendReply(replyAddr, juce::String(
                "error: '" + closed.title + "' has unsaved changes — save first or pass force=1"));
            return;
        }
        if (closed.status == McpGuiHost::CloseResult::Closed) {
            bridge->sendReply(replyAddr, juce::String("closed: " + closed.title));
            return;
        }
        bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
    });
    return;
}
