/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: encapsulate into subpatches, files and GOP modules (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::pdEncapsulate(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto subpatchName = getArgString(msg[1]);
        int count = static_cast<int>(getArgFloat(msg[2]));
        std::vector<juce::String> targetIds;
        int cursor = 3;
        for (int i = 0; i < count && cursor < msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);

        if (cnv) {
            postMessage([proc = processor, cnv, canvasName, subpatchName, targetIds, correlationId, bridge = this]() {
                if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
                if (!proc->getMcp()->gui().ensureView(cnv)) {
                    bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
                    return;
                }

                // P3.5 S2: identity and gl_list work under the Pd lock (message thread; it used to run unlocked
                // while the worker renamed ids: a data race on the identity map).
                McpHost::PdLockScope lock(*proc);

                // Phase 3: reconcile identity BEFORE resolving tempIds so a stale id
                // can't resolve to the wrong object. Shared routine.
                proc->getMcp()->reconcileIdentity(canvasName);

                // SELECTION-FREE: resolve ids straight to Object* and hand the
                // explicit list to the native encapsulate. The GUI selection is
                // never touched — so a leftover selection cannot ride along, and
                // no read-back guard is needed. Fail closed on the first miss.
                std::vector<t_gobj*> toEncapsulate;
                for (auto const& id : targetIds)
                    toEncapsulate.push_back(proc->getMcp()->resolveStableId(canvasName, id));
                int const encapsulateCount = proc->getMcp()->gui().countViewObjects(cnv, toEncapsulate);
                if (encapsulateCount < 0) {
                    bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
                    return;
                }

                // Capture (tempId → class+text) BEFORE encapsulation so we can
                // re-attach the ORIGINAL tempIds to the recreated inner objects.
                // encapsulateSelection copy+RECREATES them, so pointers (and the
                // identity entries) would otherwise be lost and the objects
                // auto-adopted under new gui_/osc_sig_ names.
                struct EncRec { juce::String id, cls, text; bool used = false; };
                std::vector<EncRec> encRecs;
                for (auto const& id : targetIds) {
                    t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id);
                    if (!g) continue;
                    EncRec r;
                    r.id = id;
                    r.cls = juce::String::fromUTF8(class_getname(pd_class(&g->g_pd)));
                    if (auto* ob = pd_checkobject(&g->g_pd)) {
                        char* tb = nullptr; int ts = 0;
                        mcpGetObjectText(ob, &tb, &ts);
                        if (tb && ts > 0) { r.text = juce::String::fromUTF8(tb, ts).trim(); freebytes(tb, ts); }
                    }
                    encRecs.push_back(r);
                }

                // Snapshot the parent object count so we can verify the net effect
                // (N objects out, 1 [pd] in). Encapsulation copy+RECREATES the objects
                // inside the subpatch — pointers change — so we verify by COUNT.
                int parentCountBefore = 0;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                    parentCountBefore++;

                proc->getMcp()->gui().encapsulate(cnv, subpatchName, toEncapsulate);

                // ── PHASE 2: POST-OP VERIFICATION + ROLLBACK ─────────────
                // Confirm the op applied: a canvas-class [pd] now exists, the parent
                // lost exactly N-1 objects, and the subpatch holds at least N. On
                // mismatch, roll back with ONE native undo (encapsulateSelection
                // wraps the whole op in a single "Encapsulate" undo sequence,
                // Canvas.cpp:2571) — done BEFORE resetCanvasUndo() flushes the stack.
                t_gobj* newestObj = pd::Interface::getNewest(cnv);
                bool verified = false;
                if (newestObj && pd_class(&newestObj->g_pd) == canvas_class) {
                    int parentCountAfter = 0;
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                        parentCountAfter++;
                    int subCount = 0;
                    auto* subCnv = reinterpret_cast<t_canvas*>(newestObj);
                    for (t_gobj* y = subCnv->gl_list; y; y = y->g_next)
                        subCount++;
                        int const n = encapsulateCount;
                    verified = (parentCountAfter == parentCountBefore - n + 1) && (subCount >= n);
                }

                if (!verified) {
                    pd::Interface::undo(cnv);            // single-step rollback
                    proc->getMcp()->reconcileIdentity(canvasName); // re-align map post-rollback
                    bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
                    return;
                }

                if (newestObj) {
                    proc->getMcp()->identity.canvasMap(cnv)[subpatchName.toStdString()] = newestObj;
                    proc->getMcp()->identity.assignSerial(newestObj);
                    proc->getMcp()->identity.bumpVersion();

                    // Re-attach the original tempIds to the recreated inner
                    // objects (match by class + object text; greedy, best-effort).
                    auto* innerCnv = reinterpret_cast<t_canvas*>(newestObj);
                    auto& parentMap = proc->getMcp()->identity.canvasMap(cnv);
                    auto& childMap = proc->getMcp()->identity.canvasMap(innerCnv);
                    for (t_gobj* y = innerCnv->gl_list; y; y = y->g_next) {
                        juce::String cls = juce::String::fromUTF8(class_getname(pd_class(&y->g_pd)));
                        juce::String txt;
                        if (auto* ob = pd_checkobject(&y->g_pd)) {
                            char* tb = nullptr; int ts = 0;
                            mcpGetObjectText(ob, &tb, &ts);
                            if (tb && ts > 0) { txt = juce::String::fromUTF8(tb, ts).trim(); freebytes(tb, ts); }
                        }
                        for (auto& r : encRecs) {
                            if (r.used || r.cls != cls) continue;
                            if (r.text.isNotEmpty() && txt.isNotEmpty() && r.text != txt) continue;
                            r.used = true;
                            childMap[r.id.toStdString()] = y;
                            parentMap.erase(r.id.toStdString());
                            proc->getMcp()->identity.assignSerial(y);
                            proc->getMcp()->identity.bumpVersion();
                            break;
                        }
                    }
                }

                // Encapsulation removed objects from the parent canvas and
                // re-parented them into a fresh [pd] — index-based undo
                // entries on the parent now dangle. Flush to keep undo safe.
                resetCanvasUndo(proc, canvasName);

                bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 1.0f);
            });
        } else {
            sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
        }
    }
    return;
}

void MCPBridge::pdEncapsulateToFile(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 4 && processor) {
        auto canvasName   = normalizeCanvas(getArgString(msg[0]));
        auto abstrName    = getArgString(msg[1]);
        auto filePath     = getArgString(msg[2]);
        int  count        = static_cast<int>(getArgFloat(msg[3]));
        std::vector<juce::String> targetIds;
        int cursor = 4;
        for (int i = 0; i < count && cursor < msg.size(); i++)
            targetIds.push_back(getArgString(msg[cursor++]));
        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);

        if (cnv) {
            postMessage([proc = processor, cnv, canvasName, abstrName, filePath, targetIds, correlationId, bridge = this]() {
                if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
                if (!proc->getMcp()->gui().ensureView(cnv)) {
                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                    return;
                }

                // ── Step 1: Encapsulate selected objects ─────────────────
                // Selection-free (same as /pd/encapsulate): reconcile, resolve the
                // ids straight to Object*, fail closed on the first miss, and hand
                // the explicit list to the native encapsulate. No GUI selection.
                // P3.5 S2: identity and gl_list work under the Pd lock, released before the file write (Step 3).
                auto identityLock = std::make_unique<McpHost::PdLockScope>(*proc);
                proc->getMcp()->reconcileIdentity(canvasName);

                std::vector<t_gobj*> toEncapsulate;
                for (const auto& id : targetIds)
                    toEncapsulate.push_back(proc->getMcp()->resolveStableId(canvasName, id));
                int const encapsulateCount = proc->getMcp()->gui().countViewObjects(cnv, toEncapsulate);
                if (encapsulateCount < 0) {
                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                    return;
                }

                int parentCountBefore = 0;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                    parentCountBefore++;

                proc->getMcp()->gui().encapsulate(cnv, abstrName, toEncapsulate);

                // ── PHASE 2: POST-OP VERIFICATION + ROLLBACK ─────────────
                // Same guarantee as /pd/encapsulate: verify by COUNT (encapsulation
                // recreates objects, so pointers change), fail closed + single-step
                // undo rollback on mismatch, BEFORE we export/rewrite anything.
                {
                    t_gobj* npd = pd::Interface::getNewest(cnv);
                    bool ok = false;
                    if (npd && pd_class(&npd->g_pd) == canvas_class) {
                        int parentCountAfter = 0;
                        for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                            parentCountAfter++;
                        int subCount = 0;
                        auto* sub = reinterpret_cast<t_canvas*>(npd);
                        for (t_gobj* y = sub->gl_list; y; y = y->g_next)
                            subCount++;
                    int const n = encapsulateCount;
                        ok = (parentCountAfter == parentCountBefore - n + 1) && (subCount >= n);
                    }
                    if (!ok) {
                        pd::Interface::undo(cnv);
                        proc->getMcp()->reconcileIdentity(canvasName);
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }
                }

                // ── Step 2: Find the new [pd abstrName] subpatch ─────────
                t_gobj* newestObj = pd::Interface::getNewest(cnv);
                if (!newestObj) {
                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                    return;
                }
                t_canvas* subCnv = reinterpret_cast<t_canvas*>(newestObj);

                // Record position before we replace it
                int posX = subCnv->gl_obj.te_xpix;
                int posY = subCnv->gl_obj.te_ypix;

                // ── Step 3: Save subpatch content to file ────────────────
                // getCanvasContent runs under sys_lock; disk write is outside.
                juce::String content;
                identityLock.reset();
                sys_lock();
                {
                    char* buf = nullptr; int bufsize = 0;
                    pd::Interface::getCanvasContent(subCnv, &buf, &bufsize);
                    if (buf) {
                        content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                        freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                    }
                    // getCanvasContent emits a SUBPATCH header
                    // ("#N canvas x y w h name mapped;") because subCnv still
                    // has an owner. An abstraction FILE must use the ROOT
                    // header ("#N canvas x y w h font;") — saving the wrong
                    // header makes Pd crash the instant [name] is
                    // instantiated (abstraction refs in load_patch / GUI).
                    if (content.startsWith("#N canvas"))
                    {
                        int semi = content.indexOfChar(';');
                        if (semi > 0)
                        {
                            juce::String header = "#N canvas "
                                + juce::String(subCnv->gl_screenx1) + " "
                                + juce::String(subCnv->gl_screeny1) + " "
                                + juce::String(subCnv->gl_screenx2 - subCnv->gl_screenx1) + " "
                                + juce::String(subCnv->gl_screeny2 - subCnv->gl_screeny1) + " "
                                + juce::String(subCnv->gl_font) + ";";
                            content = header + content.substring(semi + 1);
                        }
                    }
                }
                sys_unlock();

                if (content.isEmpty()) {
                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                    return;
                }

                juce::File targetFile(filePath);
                targetFile.getParentDirectory().createDirectory();
                bool writeOk = targetFile.replaceWithText(content);
                if (!writeOk) {
                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                    return;
                }

                // ── Step 4: Replace [pd abstrName] with [abstrName] ref ──
                // Delete the inline subpatch, create abstraction reference.
                //
                // CRITICAL: encapsulateSelection rewired the parent's external
                // wires onto the inline [pd abstrName] box, but glist_delete
                // frees that box AND its connections. Snapshot the boundary
                // wires first, then re-attach them to the [abstrName] reference
                // — otherwise the extraction silently disconnects the chain.
                struct BoundaryWire { t_gobj* far; int farPort; int boxPort; };
                std::vector<BoundaryWire> inWires, outWires;
                {
                    for (auto const& c : proc->getMcp()->gui().viewWiresOf(cnv, newestObj)) {
                        if (c.dst == newestObj) {
                            // far -> box (inbound): far's outlet -> box inlet
                            if (t_gobj* fg = c.src)
                                inWires.push_back({ fg, c.outlet, c.inlet });
                        } else if (c.src == newestObj) {
                            // box -> far (outbound): box outlet -> far's inlet
                            if (t_gobj* fg = c.dst)
                                outWires.push_back({ fg, c.inlet, c.outlet });
                        }
                    }
                }

                sys_lock();
                {
                    // Delete the [pd abstrName] subpatch object
                    glist_delete(cnv, newestObj);
                    // Create [abstrName] abstraction reference at same position
                    // using pasteDirect — same path as batch_atomic PHASE 4
                    juce::String objLine = "#X obj " + juce::String(posX) + " " + juce::String(posY) + " " + abstrName + ";";
                    pasteDirect(cnv, objLine.toRawUTF8());
                    // Re-attach the parent boundary wires to the new reference.
                    t_gobj* rewired = pd::Interface::getNewest(cnv);
                    t_object* rewiredOb = rewired ? pd::Interface::checkObject(rewired) : nullptr;
                    if (rewiredOb) {
                        for (auto& w : inWires) {
                            if (t_object* fo = pd::Interface::checkObject(w.far))
                                pd::Interface::createConnection(cnv, fo, w.farPort, rewiredOb, w.boxPort);
                        }
                        for (auto& w : outWires) {
                            if (t_object* fo = pd::Interface::checkObject(w.far))
                                pd::Interface::createConnection(cnv, rewiredOb, w.boxPort, fo, w.farPort);
                        }
                    }
                    // Single DSP recompile
                    canvas_update_dsp();
                }
                sys_unlock();

                // ── Step 5: Register new abstraction object in identity map
                {
                    McpHost::PdLockScope lock(*proc); // P3.5 S2
                    t_gobj* newObj = pd::Interface::getNewest(cnv);
                    if (newObj) {
                        proc->getMcp()->identity.canvasMap(cnv)[abstrName.toStdString()] = newObj;
                        proc->getMcp()->identity.assignSerial(newObj);
                        proc->getMcp()->identity.bumpVersion();
                    }
                }

                // ── Step 6: Reload abstractions so Pd registers the file ─
                proc->reloadAbstractions(juce::File(filePath), cnv);

                proc->getMcp()->enqueuePd([p = proc] { p->getMcp()->synchroniseCanvases(); });

                // glist_delete + pasteDirect above freed/reordered objects
                // on the parent canvas — flush the stale undo queue.
                resetCanvasUndo(proc, canvasName);

                bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 1.0f);
            });
        } else {
            sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
        }
    }
    return;
}

void MCPBridge::pdEncapsulateGop(const juce::String& action, const juce::OSCMessage& msg)
{
    // PRD GOP Module v2 Phase A: atomic parent-canvas swap for a TS-generated
    // MERDA abstraction. TS already generated + wrote the .pd file (file bytes
    // never cross OSC); C++ does delete-originals + create-[module] + rewire
    // parent boundary under ONE sys_lock + ONE canvas_update_dsp.
    // Args: [canvasName, moduleName, filePath, jsonSpec, correlationId]
    // jsonSpec: {"targetIds":["tid",...],
    //            "inbound":[{"src":"tid","out":0,"in":0}],   // parent→box
    //            "outbound":[{"out":0,"dest":"tid","in":0}], // box→parent
    //            "posX":123,"posY":45}
    // Reply: JSON string on /pd/encapsulate_gop/reply/<corrId>.
    // NOTE: native UndoSequence is Phase B; undo is via TS rollback snapshot.
    if (msg.size() >= 4 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto moduleName  = getArgString(msg[1]).trim();
        auto filePath    = getArgString(msg[2]);
        auto jsonSpec    = getArgString(msg[3]);
        auto correlationId = (msg.size() > 4) ? getArgString(msg[4]) : juce::String("0");
        auto replyAddr = "/pd/encapsulate_gop/reply/" + correlationId;

        juce::String safeName;
        for (auto c : moduleName) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
                safeName += c;
        }
        if (safeName.endsWith(".pd")) safeName = safeName.dropLastCharacters(3);

        // Pre-parse JSON outside the message-thread lambda (zero contention).
        juce::StringArray targetIds;
        struct GopIn { juce::String src; int srcOut; int boxIn; };
        struct GopOut { int boxOut; juce::String dest; int destIn; };
        std::vector<GopIn> inbound;
        std::vector<GopOut> outbound;
        int posX = 0, posY = 0;
        juce::String specError;
        {
            juce::var parsed = juce::JSON::parse(jsonSpec);
            if (auto* o = parsed.getDynamicObject()) {
                if (auto* arr = o->getProperty("targetIds").getArray())
                    for (auto& v : *arr) targetIds.add(v.toString());
                if (auto* arr = o->getProperty("inbound").getArray())
                    for (auto& e : *arr)
                        if (auto* w = e.getDynamicObject())
                            inbound.push_back({ w->getProperty("src").toString(),
                                static_cast<int>(w->getProperty("out")),
                                static_cast<int>(w->getProperty("in")) });
                if (auto* arr = o->getProperty("outbound").getArray())
                    for (auto& e : *arr)
                        if (auto* w = e.getDynamicObject())
                            outbound.push_back({ static_cast<int>(w->getProperty("out")),
                                w->getProperty("dest").toString(),
                                static_cast<int>(w->getProperty("in")) });
                posX = static_cast<int>(o->getProperty("posX"));
                posY = static_cast<int>(o->getProperty("posY"));
            } else {
                specError = "spec JSON parse failed";
            }
        }

        if (safeName.isEmpty()) {
            sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_NAME\",\"detail\":\"empty module name\"}"));
        } else if (specError.isNotEmpty()) {
            sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_SPEC\",\"detail\":\"" + jsonEscape(specError) + "\"}"));
        } else if (targetIds.isEmpty()) {
            sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_SPEC\",\"detail\":\"no targetIds\"}"));
        } else {
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv) {
                sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_NO_CANVAS\",\"detail\":\"" + jsonEscape(canvasName) + "\"}"));
            } else {
                postMessage([proc = processor, cnv, canvasName, safeName, filePath, targetIds, inbound, outbound, posX, posY, correlationId, replyAddr, bridge = this]() {
                    if (mcpCanvasGone(cnv)) { bridge->sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_NO_CANVAS\",\"detail\":\"canvas closed\"}")); return; } // P3.4 S4: closed since the handler ran
                    // Phase 3: reconcile identity BEFORE resolving so a stale id
                    // can't resolve to a live-but-wrong object (same guarantee as
                    // /pd/encapsulate). GOP already fails closed on missing ids.
                    proc->getMcp()->reconcileIdentity(canvasName);

                    // Resolve everything BEFORE touching the canvas.
                    SmallArray<t_gobj*> toDelete;
                    juce::StringArray missing;
                    for (auto& tid : targetIds) {
                        if (t_gobj* g = proc->getMcp()->resolveStableId(canvasName, tid))
                            toDelete.add(g);
                        else
                            missing.add(tid);
                    }
                    struct ResWire { t_gobj* far; int farPort; int boxPort; };
                    std::vector<ResWire> inWires;
                    std::vector<ResWire> outWires;
                    for (auto& w : inbound) {
                        if (t_gobj* g = proc->getMcp()->resolveStableId(canvasName, w.src))
                            inWires.push_back({ g, w.srcOut, w.boxIn });
                        else
                            missing.add(w.src);
                    }
                    for (auto& w : outbound) {
                        if (t_gobj* g = proc->getMcp()->resolveStableId(canvasName, w.dest))
                            outWires.push_back({ g, w.destIn, w.boxOut });
                        else
                            missing.add(w.dest);
                    }
                    if (missing.size() > 0) {
                        bridge->sendReply(replyAddr, "{\"ok\":false,\"error\":\"GOP_TARGET_NOT_FOUND\",\"detail\":\"" + jsonEscape(missing.joinIntoString(",")) + "\"}");
                        return;
                    }

                    // Drop boundary wires whose far endpoint sits INSIDE the
                    // delete set (stale spec): after deletion those pointers
                    // are freed and obj_connect would UAF on them.
                    juce::StringArray staleErrs;
                    {
                        std::unordered_set<t_gobj*> doomed;
                        for (auto* g : toDelete) doomed.insert(g);
                        auto stale = [&](t_gobj* far) { return doomed.find(far) != doomed.end(); };
                        inWires.erase(std::remove_if(inWires.begin(), inWires.end(),
                            [&](auto& w) {
                                if (stale(w.far)) { staleErrs.add("stale-in:" + juce::String(w.boxPort)); return true; }
                                return false;
                            }), inWires.end());
                        outWires.erase(std::remove_if(outWires.begin(), outWires.end(),
                            [&](auto& w) {
                                if (stale(w.far)) { staleErrs.add("stale-out:" + juce::String(w.boxPort)); return true; }
                                return false;
                            }), outWires.end());
                    }

                    int connected = 0;
                    juce::StringArray connErrs;
                    juce::String newTempId;
                    sys_lock();
                    {
                        // 1. Create the abstraction box FIRST so a red-box
                        // failure leaves the originals untouched.
                        juce::String objLine = "#X obj " + juce::String(posX) + " " + juce::String(posY) + " " + safeName + ";";
                        pasteDirect(cnv, objLine.toRawUTF8());
                        t_gobj* newObj = pd::Interface::getNewest(cnv);
                        t_object* newOb = newObj ? pd::Interface::checkObject(newObj) : nullptr;
                        bool redBox = !newOb || (newOb->te_type == T_OBJECT && pd_class(&newObj->g_pd) == text_class);
                        if (redBox) {
                            // Roll the failed box back out; originals intact.
                            if (newObj) glist_delete(cnv, newObj);
                            canvas_update_dsp();
                            bridge->sendReply(replyAddr, "{\"ok\":false,\"error\":\"GOP_CREATE_FAILED\",\"detail\":\"[" + jsonEscape(safeName) + "] couldn't create (red box)\"}");
                        } else {
                            auto gopKey = canonicalCanvasKey(cnv).toStdString();
                            // 2. Delete originals via the message-thread-safe
                            // remover. removeObjects() clears selection first (noselect),
                            // opens UNDO_SEQUENCE_START, records UNDO_CUT, and suspends DSP around free.
                            for (auto* g : toDelete) {
                                proc->getMcp()->identity.canvasMap(cnv).erase(
                                    [&]() -> std::string {
                                        for (auto& [tid, ptr] : proc->getMcp()->identity.canvasMap(cnv))
                                            if (ptr == g) return tid;
                                        return "";
                                    }());
                                proc->getMcp()->identity.eraseSerial(g);
                            }
                            mcp::PdOps::removeObjectsAudioThread(cnv, toDelete);
                            proc->getMcp()->identity.bumpVersion();

                            // 3. Rewire parent boundary through the new box with createConnection.
                            for (auto& w : inWires) {
                                t_object* so = pd::Interface::checkObject(w.far);
                                if (so && newOb && pd::Interface::createConnection(cnv, so, w.farPort, newOb, w.boxPort))
                                    connected++;
                                else
                                    connErrs.add("in:" + juce::String(w.boxPort));
                            }
                            for (auto& w : outWires) {
                                t_object* d_o = pd::Interface::checkObject(w.far);
                                if (d_o && newOb && pd::Interface::createConnection(cnv, newOb, w.boxPort, d_o, w.farPort))
                                    connected++;
                                else
                                    connErrs.add("out:" + juce::String(w.boxPort));
                            }
                            canvas_dirty(cnv, 1);
                            // Single DSP recompile for the whole swap.
                            canvas_update_dsp();

                            // Clear undo queue safely on Pd scheduler thread to prevent stale pointer crashes on Ctrl+Z.
                            mcp::SysList undoAtoms; // P3.4 S7: plain values, no Pd symbols
                            undoAtoms.add(juce::String(canvasName));
                            proc->getMcp()->callSys("mcp_clear_undo", undoAtoms);
                            for (auto& e : staleErrs) connErrs.add(e);
                            proc->getMcp()->identity.canvasMap(cnv)[safeName.toStdString()] = newObj;
                            proc->getMcp()->identity.assignSerial(newObj);
                            proc->getMcp()->identity.bumpVersion();
                            newTempId = safeName;
                        }
                    }
                    sys_unlock();

                    if (newTempId.isEmpty()) return; // error reply already sent

                    // Sync GUI so plugdata displays the newly instantiated abstraction.
                    proc->getMcp()->enqueuePd([p = proc] { p->getMcp()->synchroniseCanvases(); });

                    juce::String receipt = "{\"ok\":true,\"tempId\":\"" + jsonEscape(newTempId)
                        + "\",\"inlets\":" + juce::String(static_cast<int>(inWires.size()))
                        + ",\"outlets\":" + juce::String(static_cast<int>(outWires.size()))
                        + ",\"connected\":" + juce::String(connected)
                        + ",\"warnings\":[" + [&]() -> juce::String {
                            juce::StringArray q;
                            for (auto& e : connErrs) q.add("\"" + jsonEscape(e) + "\"");
                            return q.joinIntoString(",");
                        }() + "]}";
                    bridge->sendReply(replyAddr, receipt);
                });
            }
        }
    }
    return;
}
