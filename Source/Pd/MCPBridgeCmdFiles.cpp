/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: patch files and tabs (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::pdLoad(const juce::String& action, const juce::OSCMessage& msg)
{
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
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
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
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
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
        SmallArray<pd::Atom> atoms;
        atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
        atoms.add(pd::Atom(processor->generateSymbol("0")));
        processor->receiveSysMessage("mcp_clear_ids", atoms);

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
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
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
        // Sidecar next to the .pd — engine-private, invisible to vanilla Pd
        juce::File sidecarFile(destFilePath + ".mcpids.json");
        sidecarFile.replaceWithText(juce::JSON::toString(sidecar, true));

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
            if (oldKey != newKey && p->getMcp()->session.mcpStableObjectMap.count(oldKey)) {
                p->getMcp()->session.mcpStableObjectMap[newKey] = std::move(p->getMcp()->session.mcpStableObjectMap[oldKey]);
                p->getMcp()->session.mcpStableObjectMap.erase(oldKey);
            }
            p->getMcp()->gui().setPatchFile(cnv, f);
        });

        sendReply(replyAddr, destFilePath);
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

        // Sidecar read OUTSIDE sys_lock (small file, best-effort)
        juce::var sidecar;
        juce::File sidecarFile(srcFilePath + ".mcpids.json");
        if (sidecarFile.existsAsFile()) {
            auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
            if (auto* parsedObj = parsed.getDynamicObject()) {
                if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY")) {
                    sidecar = parsed;
                }
            }
        }

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
        juce::MessageManager::callAsync(
            [proc = processor, canvasName, bodyText, sidecar, replyAddr, bridge = this]() {
                int objectCount = 0;
                sys_lock();
                t_canvas* cnv = proc->getMcp()->getCanvasBySymbol(canvasName);
                if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
                if (cnv) {
                    auto canonKey = MCPBridge::canonicalCanvasKey(cnv);
                    // 0. Reset identity for this canvas tree BEFORE re-registering
                    proc->getMcp()->session.mcpStableObjectMap[canonKey.toStdString()].clear();

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

                    // 5. Re-register sidecar identities
                    if (auto* sidecarObj = sidecar.getDynamicObject())
                        MCPBridge::mcpApplyIdentitySidecar(proc, sidecarObj, canonKey, cnv);

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

        // Phase 3.1 bugfix: the open lambda was observed to deliver twice
        // (two tabs for one request). Once-guard per file — any repeat
        // within 2s is ignored (idempotent open, root cause independent).
        static std::unordered_map<juce::String, juce::int64> lastOpenAtMs;
        auto nowMs = juce::Time::getMillisecondCounter();
        auto& lastRef = lastOpenAtMs[srcFile.getFullPathName()];
        if (nowMs - lastRef < 2000) {
            sendReply(replyAddr, juce::String("ignored: open already in progress for " + srcFilePath));
            return;
        }
        lastRef = nowMs;

        // PRD Phase 3 §2.5: duplicate-tab guard — PlugData focuses the
        // existing tab instead of duplicating a patch (by design). Reply
        // honestly instead of falling into the async no-reply path.
        bool alreadyOpen = false;
        {
            sys_lock();
            for (auto const& tab : processor->getMcp()->gui().listTabs()) {
                if (tab.file == srcFile) {
                    alreadyOpen = true;
                    break;
                }
            }
            sys_unlock();
        }
        if (alreadyOpen) {
            sendReply(replyAddr, juce::String("already open: " + srcFile.getFullPathName() + " (focused existing tab)"));
            return;
        }

        // Sidecar read outside the message-thread hop
        juce::var sidecar;
        juce::File sidecarFile(srcFile.getFullPathName() + ".mcpids.json");
        if (sidecarFile.existsAsFile()) {
            auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
            if (auto* parsedObj = parsed.getDynamicObject()) {
                if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY"))
                    sidecar = parsed;
            }
        }

        // Acknowledge immediately — the tab-open runs on the message
        // thread (fire-and-forget); TS verifies via `tabs`/census.
        sendReply(replyAddr, juce::String("opening: " + srcFile.getFullPathName()));

        juce::MessageManager::callAsync(
            [p = processor, fileStr = srcFile.getFullPathName(), sidecar]() {
            bool opened = p->getMcp()->gui().openPatch(juce::File(fileStr));
            if (!opened) {
                p->logMessage("MCP open_patch: no editor");
                return;
            }

            // Phase 3.1 bugfix: belt-and-suspenders dedup — if more than
            // one tab for this file exists, keep the first and close extras.
            juce::File f(fileStr);
            p->getMcp()->gui().closeDuplicateTabs(f);

            // PRD Phase 3 §2.5: sidecar registration via the proven async
            // mcp_register_id path — NO sys_lock in this lambda (lock
            // inversion with the audio thread crashes the message thread).
            int queued = 0;
            if (auto* sidecarObj = sidecar.getDynamicObject()) {
                auto queueRegisters = [&p, &queued](juce::var const& idArray, juce::String const& canvasKey, t_canvas* target) {
                    const auto* arr = idArray.getArray();
                    if (arr == nullptr) return;
                    int idx = 0;
                    for (t_gobj* g = target->gl_list; g; g = g->g_next, ++idx) {
                        // find the sidecar entry for this index
                        for (auto const& v : *arr) {
                            if (auto* o = v.getDynamicObject()) {
                                if (static_cast<int>(static_cast<double>(o->getProperty("i"))) != idx) continue;
                                juce::String id = o->getProperty("id").toString();
                                if (id.isEmpty()) break;
                                SmallArray<pd::Atom> atoms;
                                atoms.add(pd::Atom(p->generateSymbol(canvasKey)));
                                atoms.add(pd::Atom(p->generateSymbol("0")));
                                atoms.add(pd::Atom(static_cast<float>(idx)));
                                atoms.add(pd::Atom(p->generateSymbol(id)));
                                p->receiveSysMessage("mcp_register_id", atoms);
                                queued++;
                                break;
                            }
                        }
                    }
                };

                juce::File f(fileStr);
                if (auto* cnv = p->getMcp()->gui().findOpenPatch(f)) {
                    juce::String canonKey = MCPBridge::canonicalCanvasKey(cnv);
                    queueRegisters(sidecarObj->getProperty("root"), canonKey, cnv);
                    // named subcanvases
                    if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                            if (pd_class(&g->g_pd) != canvas_class) continue;
                            auto* child = reinterpret_cast<t_canvas*>(g);
                            if (!child->gl_name) continue;
                            juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
                            auto childIds = subObj->getProperty(juce::Identifier(childName));
                            if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0)
                                queueRegisters(childIds, MCPBridge::canonicalCanvasKey(child), child);
                        }
                    }
                }
            }

            p->logMessage(juce::String("MCP open_patch: ") + fileStr
                + (queued > 0 ? " (" + juce::String(queued) + " identities queued)" : ""));
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

    juce::MessageManager::callAsync([p = processor, bridge = this, replyAddr]() {
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

    juce::MessageManager::callAsync([p = processor, bridge = this, target, replyAddr]() {
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

    juce::MessageManager::callAsync([p = processor, bridge = this, target, force, replyAddr]() {
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
