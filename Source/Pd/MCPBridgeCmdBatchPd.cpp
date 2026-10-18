/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stage 2 (P3.4 S9): the mutation, in the Pd context (message thread, sys_lock).
// Phases 0-4 here; connect and the wire scan in MCPBridgeCmdBatchConnect.cpp.

#include "MCPBridgeCmdBatch.h"

extern int dsp_update_deferred;  // defined in m_obj.c

void mcp::BatchJob::runPdJob()
{
    auto tLambdaStart = std::chrono::high_resolution_clock::now();
    cnv = processor->getMcp()->getCanvasBySymbolStrict(canvasName);
    if (!cnv) {
        canvasNotFound = true;
        post("batch_atomic: canvas '%s' not found — mutation refused", canvasName.toRawUTF8());
    }

    if (cnv) {
        dsp_update_deferred = 1;
        auto canonKey = MCPBridge::canonicalCanvasKey(cnv);
        auto canonStr = canonKey.toStdString();

        reconcile(canonStr);
        disconnectPhase();
        deletePhase(canonStr);
        editPhase(canonStr);
        createPhase(canonStr);
        connectPhase();
        scanConnections(canonStr);

        canvas_dirty(cnv, 1);
        dsp_update_deferred = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        canvas_update_dsp();
        auto t1 = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        post("batch_atomic: canvas_update_dsp took %lld us", (long long)us);
    }

    // Fire loadbangs
    for (auto* g : createdPtrs) {
        if (g) {
            if (pd_class(&g->g_pd) == canvas_class)
                canvas_loadbang(reinterpret_cast<t_canvas*>(g));
            else if (zgetfn(&g->g_pd, gensym("loadbang")))
                vmess(&g->g_pd, gensym("loadbang"), "f", LB_LOAD);
        }
    }

    // (layout guard + fallback run on the message thread below.)

    auto tLambdaEnd = std::chrono::high_resolution_clock::now();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(tLambdaEnd - tLambdaStart).count();
    post("batch_atomic: TOTAL lambda took %lld us", (long long)totalUs);
}

void mcp::BatchJob::reconcile(const std::string& canonStr)
{
    // PHASE 0: PRE-FLIGHT AUTO-RECONCILE
    // Single walk of gl_list to sync identity map with live canvas.
    // Detects manual GUI edits (user created/deleted objects) and
    // fixes the map BEFORE any mutation — zero extra OSC round-trips,
    // zero audio dropout (runs inside the same audio-thread lambda).
    {
        auto canvasStr = canonStr;
        auto& canvasMap = processor->getMcp()->identity.canvasMap(cnv);

        // Step A: Build set of all live pointers from gl_list (O(n))
        std::unordered_set<t_gobj*> livePointers;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
            livePointers.insert(y);

        // Step B: Evict stale entries — user deleted objects via GUI
        std::vector<std::string> toEvict;
        for (auto& [tempId, ptr] : canvasMap) {
            if (livePointers.find(ptr) == livePointers.end()) {
                toEvict.push_back(tempId); // P3.5 S5: serial kept (the address may be reused by a bound object)
            }
        }
        for (auto& id : toEvict) {
            canvasMap.erase(id);
            processor->getMcp()->identity.bumpVersion();
            reconcileEvicted++;
        }

        // Step C: Adopt untracked objects — user created via GUI
        // Build reverse set of tracked pointers for O(1) lookup
        std::unordered_set<t_gobj*> trackedPointers;
        for (auto& [_, ptr] : canvasMap)
            trackedPointers.insert(ptr);

        // Build set of tempIds being edited in this transaction
        // so we can re-assign them instead of creating gui_* names.
        std::unordered_set<std::string> editedIds;
        for (auto& pe : preEdits)
            editedIds.insert(pe.objectId.toStdString());

        // Also build ordered list of evicted tempIds for re-use
        // (Step B evicted them because their pointer changed after edit)
        std::vector<std::string> evictedEditIds;
        for (auto& id : toEvict) {
            if (editedIds.count(id))
                evictedEditIds.push_back(id);
        }
        size_t evictedEditIdx = 0;

        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
            if (trackedPointers.find(y) == trackedPointers.end()) {
                // Untracked object — auto-assign a tempId
                // Read class name for a meaningful prefix
                // If this untracked object corresponds to an edited
                // tempId (pointer changed after renameObject), reuse
                // the original tempId instead of assigning gui_*.
                // This keeps the identity map stable across edits.
                std::string autoId;
                if (evictedEditIdx < evictedEditIds.size()) {
                    autoId = evictedEditIds[evictedEditIdx++];
                    canvasMap[autoId] = y;
                    processor->getMcp()->identity.assignSerial(y);
                    processor->getMcp()->identity.bumpVersion();
                } else {
                    // P3.5 S4: the one minter (it binds and bumps).
                    processor->getMcp()->identity.mint(cnv, y, McpHost::mintPrefix(y).toStdString());
                }
                reconcileAdopted++;
            }
        }

         // Step D: Connection scan — MOVED to post-PHASE-5 (bug #5,
         // fault gauntlet 2026-09-09). Scanning inside PHASE 0 made
         // detectedConnections lag one transaction: the receipt
         // showed PRE-mutation wires; wires created by this very
         // batch only appeared one call later. Scan now runs AFTER
         // connect/edit/delete so the receipt is always current.
         // (linetraverser walks every wire on the canvas in O(wires);
         // both endpoints have tempIds — new creates are registered
         // in the identity map by PHASE 4 before PHASE 5 wiring.)
         // Build reverse ptr→tempId map (O(m)) — kept for the
         // post-mutation Step D scan below (bug #5 move).
    }
}

void mcp::BatchJob::disconnectPhase()
{
    // PHASE 1: DISCONNECT — remove wires BEFORE objects are deleted
    // so endpoints and stable IDs remain valid for clean unlinking.
    for (auto& pdc : preDisconnects) {
        t_gobj* sg = processor->getMcp()->resolveStableId(canvasName, pdc.srcId);
        t_gobj* dg = processor->getMcp()->resolveStableId(canvasName, pdc.destId);
        if (sg && dg) {
            t_object* so = pd::Interface::checkObject(sg);
            t_object* d_o = pd::Interface::checkObject(dg);
            // Only count wires that ACTUALLY existed. obj_disconnect
            // is a silent no-op for a missing wire, but we counted
            // the attempt ("1 disconnect(s)" while removing nothing).
            if (so && d_o && pd::Interface::hasConnection(cnv, so, pdc.srcOut, d_o, pdc.destIn)) {
                obj_disconnect(so, pdc.srcOut, d_o, pdc.destIn);
                disconnected++;
            }
        }
    }
}

void mcp::BatchJob::deletePhase(const std::string& canonStr)
{
    // PHASE 2: DELETE — audio-thread-safe object removal (zero undo/GUI
    // overhead; editor reconciles via trailing synchroniseCanvases).
    // Capture each doomed object's creation text + bounds + wires
    // FIRST so the inverse can recreate it (message-delta, no pointers).
    {
        auto& delMap = processor->getMcp()->identity.canvasMap(cnv);
        std::unordered_map<t_gobj*, std::string> ptrToTempId;
        for (auto& [tid, ptr] : delMap)
            if (ptr) ptrToTempId[ptr] = tid;

        std::unordered_set<t_gobj*> deleteSet;
        for (auto& pd : preDeletes) {
            t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pd.objectId);
            if (!obj) continue;
            deleteSet.insert(obj);

            DeletedSnapshot ds;
            ds.tempId = pd.objectId;
            juce::String cls = juce::String::fromUTF8(class_getname(pd_class(&obj->g_pd)));
            ds.kind = "obj";
            if (cls == "text" || cls == "comment") ds.kind = "text";
            else if (cls == "msg" || cls == "message") ds.kind = "msg";
            if (auto* tobj = pd::Interface::checkObject(obj)) {
                char* tb = nullptr; int tsz = 0;
                mcpGetObjectText(tobj, &tb, &tsz);
                juce::String full = (tb && tsz) ? juce::String::fromUTF8(tb, tsz) : juce::String();
                if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));
                juce::StringArray toks; toks.addTokens(full, " ", "");
                if (ds.kind == "obj") {
                    ds.type = toks.size() > 0 ? toks[0] : full;
                    for (int i = 1; i < toks.size(); ++i) ds.args.add(toks[i]);
                } else {
                    ds.type = cls;   // "text"/"msg" — formatAsPdLine strips it
                    ds.args = toks;
                }
            }
            int bx = 0, by = 0, bw = 0, bh = 0;
            pd::Interface::getObjectBounds(cnv, obj, &bx, &by, &bw, &bh);
            ds.x = bx; ds.y = by;
            deletedSnapshots.push_back(ds);
        }

        // Wires touching any doomed object (captured while tempIds live).
        if (!deleteSet.empty()) {
            t_linetraverser lt;
            linetraverser_start(&lt, cnv);
            t_outconnect* ltoc = nullptr;
            while ((ltoc = linetraverser_next_nosize(&lt))) {
                t_gobj* sg = &lt.tr_ob->ob_g;
                t_gobj* dg = &lt.tr_ob2->ob_g;
                if (!deleteSet.count(sg) && !deleteSet.count(dg)) continue;
                auto sIt = ptrToTempId.find(sg);
                auto dIt = ptrToTempId.find(dg);
                if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
                deletedWires.push_back({ juce::String(sIt->second),
                    static_cast<int>(lt.tr_outno),
                    juce::String(dIt->second),
                    static_cast<int>(lt.tr_inno) });
            }
        }
    }

    SmallArray<t_gobj*> toDelete;
    for (auto& pd : preDeletes) {
        t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pd.objectId);
        if (obj) {
            processor->getMcp()->identity.canvasMap(cnv).erase(pd.objectId.toStdString());
            processor->getMcp()->identity.eraseSerial(obj);
            processor->getMcp()->identity.bumpVersion();
            toDelete.add(obj);
            deleted++;
        }
    }
    if (toDelete.size() > 0) {
        mcp::PdOps::removeObjectsAudioThread(cnv, toDelete);
    }
}

void mcp::BatchJob::editPhase(const std::string& canonStr)
{
    // PHASE 3: EDIT
    for (auto& pe : preEdits) {
        t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pe.objectId);
        if (obj) {
            t_object* o = pd::Interface::checkObject(obj);
            if (o) {
                // Capture old creation text so undo can edit it back.
                {
                    char* tb = nullptr; int tsz = 0;
                    mcpGetObjectText(o, &tb, &tsz);
                    juce::String oldT = (tb && tsz) ? juce::String::fromUTF8(tb, tsz) : juce::String();
                    if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));
                    juce::StringArray toks; toks.addTokens(oldT, " ", "");
                    EditUndo eu;
                    eu.objectId = pe.objectId;
                    eu.type = toks.size() > 0 ? toks[0] : oldT;
                    for (int i = 1; i < toks.size(); ++i) eu.args.add(toks[i]);
                    editUndoEntries.push_back(eu);
                }
                mcp::PdOps::renameObject(cnv, obj, pe.newText.toRawUTF8(), pe.newText.length());
                t_gobj* newObj = nullptr; bool still = false;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                    if (y == obj) { still = true; break; }
                newObj = still ? obj : pd::Interface::getNewest(cnv);
                if (newObj) {
                    // The retyped object takes over its id (the old one is freed, so this is no theft).
                    if (processor->getMcp()->identity.bind(cnv, pe.objectId.toStdString(), newObj) == mcp::IdentityService::BindResult::Same)
                        processor->getMcp()->identity.bumpVersion(); // the edit itself is an identity change
                    // C++ truth: when a class can't be instantiated Pd
                    // substitutes text_class (g_text.c canvas_objtext,
                    // "... couldn't create") and leaves an ERROR BOX — the
                    // object still exists, so counting it as `edited` was a
                    // FALSE SUCCESS (a bogus retype reported ok:true). Detect
                    // the substitution and report an edit failure instead.
                    juce::String wantCls = pe.newText.upToFirstOccurrenceOf(" ", false, false).trim().toLowerCase();
                    t_object* no = pd::Interface::checkObject(newObj);
                    juce::String gotCls = no
                        ? juce::String::fromUTF8(class_getname(pd_class(&no->ob_pd))).toLowerCase()
                        : juce::String();
                    if (!no || (gotCls == "text" && wantCls != "text" && wantCls != "comment")) {
                        editFailures.push_back({ pe.objectId.toStdString(),
                            pe.newText.toStdString(),
                            "class could not be created — object left as an error box (check the external/library)" });
                    } else {
                        edited++;
                    }
                }
            }
        }
    }
}

void mcp::BatchJob::createPhase(const std::string& canonStr)
{
    // PHASE 4: CREATE via pasteDirect
    int preCreateCount = 0;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) preCreateCount++;

    if (createCount > 0 && pastaBuffer.isNotEmpty()) {
        pasteDirect(cnv, pastaBuffer.toRawUTF8());

        std::vector<t_gobj*> allObjects;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
            allObjects.push_back(g);
        int newCount = static_cast<int>(allObjects.size()) - preCreateCount;

        struct NewObjInfo {
            t_gobj* ptr = nullptr;
            int glIndex = -1;
            int x = 0;
            int y = 0;
            int type = -1;
            juce::String firstWord;
            juce::String className;
            bool isRedBox = false;
        };

        std::vector<NewObjInfo> newInfos;
        for (int i = 0; i < newCount; ++i) {
            t_gobj* g = allObjects[preCreateCount + i];
            NewObjInfo info;
            info.ptr = g;
            info.glIndex = preCreateCount + i;
            t_object* ob = pd::Interface::checkObject(g);
            if (ob) {
                info.x = ob->te_xpix;
                info.y = ob->te_ypix;
                info.type = ob->te_type;
                t_class* cl = pd_class(&g->g_pd);
                if (cl) {
                    const char* cName = class_getname(cl);
                    if (cName) info.className = juce::String::fromUTF8(cName);
                }
                info.isRedBox = (ob->te_type == T_OBJECT && cl == text_class);
                if (ob->te_binbuf) {
                    char* tb = nullptr; int tsz = 0;
                    binbuf_gettext(ob->te_binbuf, &tb, &tsz);
                    if (tb && tsz > 0) {
                        juce::String full = juce::String::fromUTF8(tb, tsz).trim();
                        info.firstWord = full.upToFirstOccurrenceOf(" ", false, false);
                        freebytes(tb, tsz);
                    }
                }
            }
            newInfos.push_back(info);
        }

        auto computeMatchScore = [](const PendingCreate& pc, const NewObjInfo& no) -> int {
            int score = 0;
            // Exact coordinate match is a strong structural anchor
            if (pc.x == no.x && pc.y == no.y) score += 20;

            // Red-box match: Pd creates text_class dummy, but te_binbuf preserves original type
            if (no.isRedBox && no.firstWord == pc.objType) {
                score += 25;
                return score;
            }

            // Class name / binbuf first word match
            if (no.firstWord.isNotEmpty() && no.firstWord == pc.objType) {
                score += 20;
            } else if (no.className.isNotEmpty() && no.className == pc.objType) {
                score += 20;
            }

            // Kind match
            if (pc.kind == "msg" && no.type == T_MESSAGE) score += 10;
            else if (pc.kind == "text" && no.type == T_TEXT) score += 10;
            else if ((pc.kind == "floatatom" || pc.kind == "symbolatom") &&
                     (no.type == T_ATOM || no.className.containsIgnoreCase("atom"))) score += 10;
            else if ((pc.kind == "obj" || pc.kind.isEmpty()) && no.type == T_OBJECT) score += 5;

            return score;
        };

        // Monotonic alignment of created gobjs to pendingCreates.
        // Since pasteDirect appends strictly in buffer order, newInfos
        // is guaranteed to be a monotonic subsequence of pendingCreates.
        int pCur = 0;
        const int totalPending = static_cast<int>(pendingCreates.size());

        // Red-box ghosts (failed creates) collected for rollback
        // AFTER alignment, so the mapping scan reflects the final
        // gl_list. A failed create must be a pure no-op (parity with
        // the GOP-encapsulate path).
        SmallArray<t_gobj*> redBoxes;

        for (int o = 0; o < newCount; ++o) {
            const auto& no = newInfos[o];
            int bestP = -1;
            int bestScore = 0;

            int remainingObjs = newCount - 1 - o;
            int maxSearchP = totalPending - remainingObjs;

            for (int p = pCur; p < maxSearchP; ++p) {
                int score = computeMatchScore(pendingCreates[p], no);
                if (score > bestScore) {
                    bestScore = score;
                    bestP = p;
                }
            }

            if (bestP < 0) {
                bestP = pCur;
            }

            // All pendingCreates between pCur and bestP produced NO gobj on canvas
            for (int p = pCur; p < bestP; ++p) {
                createFailures.push_back({
                    pendingCreates[p].tempId.toStdString(),
                    pendingCreates[p].objType.toStdString(),
                    "couldn't create"
                });
            }

            auto& pc = pendingCreates[bestP];

            // Check if this object is a dummy red box
            if (no.isRedBox) {
                // Roll the red box back so a failed create leaves
                // NOTHING behind (parity with the GOP-encapsulate
                // path). The ghost is still named in createFailures.
                createFailures.push_back({
                    pc.tempId.toStdString(),
                    pc.objType.toStdString(),
                    "couldn't create — red box rolled back (nothing left on canvas)"
                });
                redBoxes.add(no.ptr);
            } else {
                // Map the stable tempId to the live gobj pointer
                processor->getMcp()->identity.bind(cnv, pc.tempId.toStdString(), no.ptr); // parse refused theft already

                // Auto-seed line~/vline~ init value (fork ignores creation args)
                if (pc.seedInit) {
                    t_object* so = pd::Interface::checkObject(no.ptr);
                    if (so) {
                        t_atom sa;
                        SETFLOAT(&sa, pc.initValue);
                        pd_typedmess(reinterpret_cast<t_pd*>(so), gensym("float"), 1, &sa);
                    }
                }

                createdIds.push_back(pc.tempId.toStdString());
                createdPtrs.push_back(no.ptr);
                mappingIndices.push_back(static_cast<int32>(no.glIndex));
                created++;
            }

            pCur = bestP + 1;
        }

        // Any remaining pendingCreates after all newInfos were matched
        // had no corresponding live object
        for (int p = pCur; p < totalPending; ++p) {
            createFailures.push_back({
                pendingCreates[p].tempId.toStdString(),
                pendingCreates[p].objType.toStdString(),
                "couldn't create"
            });
        }

        // Roll back red-box ghosts now that alignment is done.
        // Audio-thread-safe removal (they were created moments ago in
        // this same lambda; GUI has not synced them yet). Rebuild the
        // index mappings against the post-delete gl_list so the reply's
        // tempId→index pairs stay correct.
        if (redBoxes.size() > 0) {
            mcp::PdOps::removeObjectsAudioThread(cnv, redBoxes);
            for (size_t mi = 0; mi < mappingIndices.size() && mi < createdPtrs.size(); ++mi) {
                if (!createdPtrs[mi]) continue;
                int idx = 0, found = -1;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                    if (y == createdPtrs[mi]) { found = idx; break; }
                }
                if (found >= 0) mappingIndices[mi] = found;
            }
        }
    }
}
