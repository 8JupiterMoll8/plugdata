/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: patch files and tabs (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

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

        // Write content to destination path provided by TS
        juce::File destFile(destFilePath);
        if (!destFile.replaceWithText(content)) {
            sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
            return;
        }

        sendReply(replyAddr, destFilePath);
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

        juce::File srcFile(srcFilePath);
        if (!srcFile.existsAsFile()) {
            sendReply(replyAddr, -1.0f);
            return;
        }

        // Split into dir + filename for binbuf_read
        auto fileDir  = srcFile.getParentDirectory().getFullPathName();
        auto fileName = srcFile.getFileName();

        int objectCount = 0;

        // Stop all active probes before clearing — they hold t_outconnect*
        // pointers that become dangling after canvas clear.
        probeManager.stopAllProbes("load_content_reset");

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            // 1. Clear existing objects
            mcp::PdOps::clearCanvasAudioThread(cnv);

            // 2. Load .pd file — strip the #N canvas header line then
            //    use pasteDirect() which correctly handles both #X obj
            //    and #X connect lines via binbuf_eval with sym_X bound
            //    to the canvas. This is the same path batch_atomic uses.
            t_binbuf* b = binbuf_new();
            if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                // Find the end of the first statement (#N canvas ... ;)
                // and build a new binbuf from the rest (#X lines only).
                int natom = binbuf_getnatom(b);
                t_atom* vec = binbuf_getvec(b);
                int startIdx = 0;
                for (int i = 0; i < natom; i++) {
                    if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                }
                if (startIdx < natom) {
                    // Serialise remaining atoms back to text for pasteDirect
                    t_binbuf* body = binbuf_new();
                    binbuf_add(body, natom - startIdx, vec + startIdx);
                    char* textBuf = nullptr;
                    int   textLen = 0;
                    binbuf_gettext(body, &textBuf, &textLen);
                    if (textBuf && textLen > 0) {
                        // pasteDirect binds sym_X to canvas — handles both
                        // #X obj and #X connect correctly
                        pasteDirect(cnv, juce::String::fromUTF8(textBuf, textLen).toRawUTF8());
                        freebytes(textBuf, textLen);
                    }
                    binbuf_free(body);
                }
            }
            binbuf_free(b);

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
                // Strip stray "#A saved" lines leaked by ELSE savestate
                // objects inside GOP abstractions (the same defect
                // pasteDirect guards against). They are not real array data
                // — legitimate arrays carry their own name (e.g. "#A gtab")
                // — and they break hvcc and external Pd. Filtering on save
                // keeps the written .pd valid for Heavy / vanilla Pd.
                {
                    juce::StringArray contentLines;
                    contentLines.addLines(content);
                    juce::String clean;
                    for (auto& ln : contentLines) {
                        auto trimmed = ln.trimStart();
                        if (trimmed.startsWith("#A saved")) continue;
                        clean += ln;
                        clean += "\n";
                    }
                    content = clean;
                }

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

        if (sidecarError.isEmpty()) {
            sendReply(replyAddr, destFilePath);
        } else {
            juce::Array<juce::var> args;
            args.add(destFilePath);
            args.add("ids not saved: " + sidecarError);
            sendReply(replyAddr, args);
        }
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

        juce::File srcFile(srcFilePath);
        if (!srcFile.existsAsFile()) {
            sendReply(replyAddr, -1.0f);
            return;
        }

        // Sidecar read OUTSIDE sys_lock (small file); P3.5 S7: a truncated or unreadable one is ignored with a note.
        juce::String sidecarNote;
        juce::var sidecar = MCPBridge::readIdentitySidecar(srcFile, sidecarNote);

        auto fileDir  = srcFile.getParentDirectory().getFullPathName();
        auto fileName = srcFile.getFileName();

        // Prepare the paste body OFF the message thread (pure file I/O).
        juce::String bodyText;
        {
            t_binbuf* b = binbuf_new();
            if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                int natom = binbuf_getnatom(b);
                t_atom* vec = binbuf_getvec(b);
                int startIdx = 0;
                for (int i = 0; i < natom; i++) {
                    if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                }
                if (startIdx < natom) {
                    t_binbuf* body = binbuf_new();
                    binbuf_add(body, natom - startIdx, vec + startIdx);
                    char* textBuf = nullptr;
                    int   textLen = 0;
                    binbuf_gettext(body, &textBuf, &textLen);
                    if (textBuf && textLen > 0) {
                        bodyText = juce::String::fromUTF8(textBuf, textLen);
                        freebytes(textBuf, textLen);
                    }
                    binbuf_free(body);
                }
            }
            binbuf_free(b);
        }

        probeManager.stopAllProbes("load_patch_reset");

        // The canvas mutation MUST run on the JUCE message thread:
        // pasteDirect creates Pd objects — GUI objects, abstractions and
        // GOP [cnv] — which instantiate JUCE Components. Creating those off
        // the message thread is illegal and was the intermittent load
        // crash. Same callAsync pattern as the native encapsulate handlers.
        postMessage(
            [proc = processor, canvasName, bodyText, sidecar, sidecarNote, srcFile, replyAddr, bridge = this]() {
                int objectCount = 0;
                sys_lock();
                t_canvas* cnv = proc->getMcp()->getCanvasBySymbol(canvasName);
                if (cnv) {
                    // 0. Reset identity for this canvas tree BEFORE re-registering
                    proc->getMcp()->identity.clearCanvas(cnv);

                    // 1. Clear existing objects (message-thread-safe)
                    mcp::PdOps::clearCanvas(cnv);

                    // 2. Paste the prepared body
                    if (bodyText.isNotEmpty())
                        pasteDirect(cnv, bodyText.toRawUTF8());

                    // 3. Fire loadbang on the ROOT canvas (recurses into
                    // subpatches) — see #81: banging only child subpatches
                    // left top-level [loadbang]/sliders un-fired, so reloads
                    // came up silent.
                    canvas_loadbang(cnv);

                    // 4. DSP graph recompile
                    canvas_update_dsp();

                    // 5. Re-register sidecar identities (P3.5 S7: the one validating applier)
                    MCPBridge::SidecarStats stats;
                    if (auto* sidecarObj = sidecar.getDynamicObject())
                        stats = MCPBridge::mcpApplyIdentitySidecar(proc, sidecarObj, cnv, srcFile);
                    auto const report = MCPBridge::sidecarReport(srcFile, stats, sidecarNote);
                    if (report.isNotEmpty())
                        proc->logMessage(report);

                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                        objectCount++;
                }
                sys_unlock();

                proc->getMcp()->synchroniseCanvases();
                if (objectCount > 0) proc->startDSP();
                // Clear/free replaced every prior object — its index-based
                // undo entries now dangle. Reset the queue to stay crash-safe.
                resetCanvasUndo(proc, canvasName);
                // Also drop MCP transactions: their inverse deltas reference
                // the objects we just replaced and would fire on dead tempIds.
                if (cnv) proc->getMcp()->clearMcpTransactions(cnv);

                // arg0 = objectCount, arg1 = undoReset.
                juce::Array<juce::var> loadReplyArgs;
                loadReplyArgs.add(static_cast<double>(objectCount));
                loadReplyArgs.add(1.0);
                bridge->sendReply(replyAddr, loadReplyArgs);
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
        for (auto const& tab : p->getMcp()->gui().listTabs()) {
            auto* o = new juce::DynamicObject();
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
