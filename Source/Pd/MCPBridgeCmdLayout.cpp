/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: layout (tidy, align, zoom, deoverlap, guard, compose, pillars, flow) (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::pdTidy(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int count = static_cast<int>(getArgFloat(msg[1]));
        std::vector<juce::String> targetIds;
        int cursor = 2;
        for (int i = 0; i < count && cursor < msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            SmallArray<t_gobj*> targetObjs;
            if (!targetIds.empty()) {
                for (const auto& id : targetIds) {
                    t_gobj* g = processor->getMcp()->resolveStableId(canvasName, id);
                    if (g) targetObjs.add(g);
                }
            } else {
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                    targetObjs.add(y);
                }
            }

            if (!targetObjs.empty()) {
                pd::Interface::tidy(cnv, targetObjs);
                processor->getMcp()->synchroniseCanvases();
            }
            sendReply("/pd/tidy/reply/" + correlationId, 1.0f);
        } else {
            sendReply("/pd/tidy/reply/" + correlationId, 0.0f);
        }
    }
    return;
}

void MCPBridge::pdAlign(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto alignStr = getArgString(msg[1]).toLowerCase();
        int count = static_cast<int>(getArgFloat(msg[2]));
        std::vector<juce::String> targetIds;
        int cursor = 3;
        for (int i = 0; i < count && cursor < msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            postMessage([proc = processor, cnv, canvasName, alignStr, targetIds, correlationId, bridge = this]() {
                if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/align/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
                std::vector<t_gobj*> targets;
                for (const auto& id : targetIds) {
                    t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id);
                    if (g) targets.push_back(g);
                }

                if (!proc->getMcp()->gui().alignObjects(cnv, targetIds.empty() ? nullptr : &targets, alignStr)) {
                    bridge->sendReply("/pd/align/reply/" + correlationId, 0.0f);
                    return;
                }

                resetCanvasUndo(proc, canvasName);
                bridge->sendReply("/pd/align/reply/" + correlationId, 1.0f);
            });
        } else {
            sendReply("/pd/align/reply/" + correlationId, 0.0f);
        }
    }
    return;
}

void MCPBridge::pdZoomToFit(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = (msg.size() >= 2) ? getArgString(msg[1]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        postMessage([proc = processor, cnv, correlationId, bridge = this]() {
            if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/zoom_to_fit/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
            if (proc->getMcp()->gui().zoomToFit(cnv)) {
                bridge->sendReply("/pd/zoom_to_fit/reply/" + correlationId, 1.0f);
            } else {
                bridge->sendReply("/pd/zoom_to_fit/reply/" + correlationId, 0.0f);
            }
        });
    }
    return;
}

void MCPBridge::pdDeoverlap(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/deoverlap <canvas> <count> <id…> <corrId>
    // PRD layout-v2 Phase B1: minimal-displacement push on TRUE rects
    // (gobj_getrect via getObjectBounds, AABB + 5px pad — same as collisions).
    // Visual-only: pd::Interface::moveObject (w_displacefn), same path as
    // mcp_move_batch_id — no DSP touch, zero dropout. Single undo sequence.
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int count = static_cast<int>(getArgFloat(msg[1]));
        std::vector<juce::String> targetIds;
        int cursor = 2;
        for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (!cnv) {
            sendReply("/pd/deoverlap/reply/" + correlationId, 0.0f);
            return;
        }

        auto guiFacts = mcpFetchGuiFacts(processor, canvasName); // P3.4 S3: GUI facts before the lock
        sys_lock();
        std::vector<t_gobj*> objs;
        if (!targetIds.empty()) {
            for (auto& id : targetIds) {
                t_gobj* g = processor->getMcp()->resolveStableId(canvasName, id);
                if (g) objs.push_back(g);
            }
        } else {
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { if (mcpIsDecorativeBackground(y)) continue; objs.push_back(y); }
        }
        if (objs.empty()) {
            sys_unlock();
            sendReply("/pd/deoverlap/reply/" + correlationId, 1.0f);
            return;
        }

        auto guiCanvas = guiFacts.boundsFor(cnv);
        struct DR { t_gobj* g; int x, y, w, h; };
        std::vector<DR> rects;
        rects.reserve(objs.size());
        for (auto* g : objs) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, g, guiCanvas.get(), &x, &yy, &w, &h);
            rects.push_back({ g, x, yy, w, h });
        }

        const int PAD = 5;
        const int MAXPASS = 24;
        auto snap10 = [](int v) { return (v / 10) * 10; };
        for (int pass = 0; pass < MAXPASS; pass++) {
            bool anyHit = false;
            for (size_t i = 0; i < rects.size(); i++) {
                for (size_t j = i + 1; j < rects.size(); j++) {
                    auto& a = rects[i];
                    auto& b = rects[j];
                    bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                            && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                    if (!hit) continue;
                    anyHit = true;
                    int overlapX = std::min(a.x + a.w + PAD - b.x, b.x + b.w + PAD - a.x);
                    int overlapY = std::min(a.y + a.h + PAD - b.y, b.y + b.h + PAD - a.y);
                    // Keep relative order: push the later object (j) along the
                    // minimal axis, snapped to the 10px grid (min 10px step).
                    if (overlapX <= overlapY) {
                        int acx = a.x + a.w / 2, bcx = b.x + b.w / 2;
                        int dx = snap10(overlapX + 9);
                        if (dx < 10) dx = 10;
                        b.x += (bcx >= acx ? dx : -dx);
                    } else {
                        int acy = a.y + a.h / 2, bcy = b.y + b.h / 2;
                        int dy = snap10(overlapY + 9);
                        if (dy < 10) dy = 10;
                        b.y += (bcy >= acy ? dy : -dy);
                    }
                }
            }
            for (auto const& rg : processor->getMcp()->session.mcpRegions) {
                if (rg.title.isEmpty()) continue;
                int rx = static_cast<int>(rg.x), ry = static_cast<int>(rg.y);
                int rw = static_cast<int>(rg.w), rh = 26;
                for (auto& r : rects) {
                    if (r.x < rx + rw + PAD && r.x + r.w + PAD > rx
                     && r.y < ry + rh + PAD && r.y + r.h + PAD > ry) {
                        anyHit = true;
                        int targetY = ry + rh + PAD;
                        int snappedY = snap10(targetY + 9);
                        if (snappedY <= targetY) snappedY += 10;
                        r.y = snappedY;
                    }
                }
            }
            if (!anyHit) break;
        }

        int moved = 0;
        for (auto& r : rects) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
            if (r.x != ox || r.y != oy) {
                pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                moved++;
            }
        }
        if (moved > 0) {
            canvas_dirty(cnv, 1);
            resetCanvasUndo(processor, canvasName);
        }
        sys_unlock();

        processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
        sendReply("/pd/deoverlap/reply/" + correlationId, 1.0f);
    }
    return;
}

    // ── /pd/layout_guard — full inline C++ layout guard ─────────────────
    // /pd/layout_guard <canvas> <pad> <snap> <fixOccl> <corrId>
    // PRD_CONTEXT_LAYOUT_GUARD: the SAME guard batch_atomic runs inline —
    // sanitizeLayout (minimal-axis de-overlap, region-title aware) then
    // fixOcclusions (nudge boxes off wire paths). Context-preserving: keeps
    // the semantic placement and only pushes what clashes, so it reads as
    // "tidy up my arrangement" rather than a full re-layout.
    // Reply: <moved> <occlusionsFixed>.
void MCPBridge::pdLayoutGuard(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    int guardPad  = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : 20;
    int guardSnap = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 10;
    bool fixOccl  = msg.size() > 3 ? (getArgFloat(msg[3]) != 0.0f) : true;
    auto correlationId = msg.size() > 4 ? getArgString(msg[4]) : "0";
    juce::String replyAddr = "/pd/layout_guard/reply/" + correlationId;

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (!cnv) {
        juce::Array<juce::var> none;
        none.add(0.0f);
        none.add(0.0f);
        sendReply(replyAddr, none);
        return;
    }

    sys_lock();
    int moved = sanitizeLayout(processor, cnv, guardPad, guardSnap);
    int occlusions = fixOccl ? fixOcclusions(processor, cnv, guardPad, guardSnap) : 0;
    if (moved > 0 || occlusions > 0) resetCanvasUndo(processor, canvasName);
    sys_unlock();

    processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
    juce::Array<juce::var> guardReply;
    guardReply.add(static_cast<float>(moved));
    guardReply.add(static_cast<float>(occlusions));
    sendReply(replyAddr, guardReply);
    return;
}

    // ── /pd/compose — P4 role-based composition ─────────────────────────
    // /pd/compose <canvas> <pad> <snap> <corrId>
void MCPBridge::pdCompose(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    int pad  = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : 5;
    int snap = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 10;
    auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";
    juce::String replyAddr = "/pd/compose/reply/" + correlationId;

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (!cnv) { sendReply(replyAddr, 0.0f); return; }

    sys_lock();
    int moved = composeLayout(processor, cnv, canvasName, pad, snap);
    if (moved > 0) resetCanvasUndo(processor, canvasName);
    sys_unlock();

    processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
    sendReply(replyAddr, static_cast<float>(moved));
    return;
}

void MCPBridge::pdPillars(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/pillars <canvas> [count id1 id2...] <corrId>
    // PRD layout-v2 Phase B: Eurorack modular column arrangement.
    // Assigns objects to columns at x = 50, 350, 650, 950... by floor(x/300),
    // with strictly monotonic Y ordering per pillar, snapped to 10px grid.
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int count = (msg.size() > 2) ? static_cast<int>(getArgFloat(msg[1])) : 0;
        std::vector<juce::String> targetIds;
        int cursor = 2;
        for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor])
                           : (msg.size() > 1 ? getArgString(msg[1]) : "0");

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (!cnv) {
            sendReply("/pd/pillars/reply/" + correlationId, 0.0f);
            return;
        }

        auto guiFacts = mcpFetchGuiFacts(processor, canvasName); // P3.4 S3: GUI facts before the lock
        sys_lock();
        std::vector<t_gobj*> objs;
        if (!targetIds.empty()) {
            for (auto& id : targetIds) {
                t_gobj* g = processor->getMcp()->resolveStableId(canvasName, id);
                if (g) objs.push_back(g);
            }
        } else {
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { if (mcpIsDecorativeBackground(y)) continue; objs.push_back(y); }
        }
        if (objs.empty()) {
            sys_unlock();
            sendReply("/pd/pillars/reply/" + correlationId, 1.0f);
            return;
        }

        auto guiCanvas = guiFacts.boundsFor(cnv);
        struct PR { t_gobj* g; int x, y, w, h; int pillar; };
        std::vector<PR> rects;
        rects.reserve(objs.size());
        for (auto* g : objs) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, g, guiCanvas.get(), &x, &yy, &w, &h);
            int p = std::max(0, (x + 100) / 300);
            rects.push_back({ g, x, yy, w, h, p });
        }

        auto snap10 = [](int v) { return (v / 10) * 10; };

        // Group into pillars and sort each pillar by original Y
        std::map<int, std::vector<size_t>> pillarGroups;
        for (size_t i = 0; i < rects.size(); ++i) {
            pillarGroups[rects[i].pillar].push_back(i);
        }

        for (auto& [pIdx, indices] : pillarGroups) {
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                return rects[a].y < rects[b].y;
            });
            int colX = 50 + pIdx * 300;
            int curY = 50;
            for (size_t idx : indices) {
                rects[idx].x = colX;
                rects[idx].y = curY;
                curY = snap10(curY + rects[idx].h + 20);
            }
        }

        int moved = 0;
        for (auto& r : rects) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
            if (r.x != ox || r.y != oy) {
                pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                moved++;
            }
        }
        if (moved > 0) {
            canvas_dirty(cnv, 1);
            resetCanvasUndo(processor, canvasName);
        }
        sys_unlock();

        processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
        sendReply("/pd/pillars/reply/" + correlationId, 1.0f);
    }
    return;
}

void MCPBridge::pdFlow(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/flow <canvas> [count id1 id2...] <corrId>
    // PRD layout-v2 Phase B: Audio-semantic ranking.
    // Sources (no signal in) top rank -> DSP processing mid -> sinks (dac~/catch~) bottom.
    // Control objects placed in left gutter. 10px grid snapped, single undo sequence.
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int count = (msg.size() > 2) ? static_cast<int>(getArgFloat(msg[1])) : 0;
        std::vector<juce::String> targetIds;
        int cursor = 2;
        for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
            targetIds.push_back(getArgString(msg[cursor++]));
        }
        auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor])
                           : (msg.size() > 1 ? getArgString(msg[1]) : "0");

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (!cnv) {
            sendReply("/pd/flow/reply/" + correlationId, 0.0f);
            return;
        }

        auto guiFacts = mcpFetchGuiFacts(processor, canvasName); // P3.4 S3: GUI facts before the lock
        sys_lock();
        std::vector<t_gobj*> objs;
        if (!targetIds.empty()) {
            for (auto& id : targetIds) {
                t_gobj* g = processor->getMcp()->resolveStableId(canvasName, id);
                if (g) objs.push_back(g);
            }
        } else {
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { if (mcpIsDecorativeBackground(y)) continue; objs.push_back(y); }
        }
        if (objs.empty()) {
            sys_unlock();
            sendReply("/pd/flow/reply/" + correlationId, 1.0f);
            return;
        }

        auto snap10 = [](int v) { return (v / 10) * 10; };

        // 1. Identify connections & rate
        std::unordered_map<t_gobj*, std::vector<t_gobj*>> sigDownstream;
        std::unordered_map<t_gobj*, int> sigInCount;
        std::unordered_set<t_gobj*> hasSignalOut;
        std::unordered_set<t_gobj*> hasSignalIn;

        t_linetraverser lt;
        linetraverser_start(&lt, cnv);
        t_outconnect* oc = nullptr;
        while ((oc = linetraverser_next_nosize(&lt))) {
            t_gobj* sg = &lt.tr_ob->ob_g;
            t_gobj* dg = &lt.tr_ob2->ob_g;
            bool srcIsSig = (lt.tr_outlet && lt.tr_outlet->o_sym == gensym("signal"));
            bool destIsSig = (lt.tr_ob2 && obj_issignalinlet(lt.tr_ob2, lt.tr_inno) != 0);
            if (srcIsSig || destIsSig) {
                sigDownstream[sg].push_back(dg);
                sigInCount[dg]++;
                hasSignalOut.insert(sg);
                hasSignalIn.insert(dg);
            }
        }

        // 2. Classify objects
        struct FR {
            t_gobj* g;
            int x, y, w, h;
            int category; // 0 = control gutter, 1 = source, 2 = DSP, 3 = sink
            int rank;
            juce::String nodeClass;
        };
        std::vector<FR> rects;
        rects.reserve(objs.size());

        auto guiCanvas = guiFacts.boundsFor(cnv);
        for (auto* g : objs) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, g, guiCanvas.get(), &x, &yy, &w, &h);

            t_class* cl = pd_class(&g->g_pd);
            const char* clName = class_getname(cl);
            juce::String className = clName ? juce::String::fromUTF8(clName) : juce::String("unknown");
            juce::String nodeClass = className;
            juce::String abstractName;
            if (cl == canvas_class) {
                nodeClass = "pd";
                if (pd::getAbstractionFileName(g, abstractName) && abstractName.isNotEmpty())
                    nodeClass = abstractName;
            } else if (cl == garray_class) {
                nodeClass = "table";
            } else if (pd::Interface::isTextObject(g)) {
                t_text* textObj = reinterpret_cast<t_text*>(g);
                if (textObj->te_type == T_MESSAGE) nodeClass = "msg";
                else if (textObj->te_type == T_ATOM) nodeClass = className.containsIgnoreCase("symbol") ? "symbolatom" : "floatatom";
                else if (textObj->te_type == T_TEXT) nodeClass = "text";
                else {
                    char* tb = nullptr; int tsz = 0;
                    binbuf_gettext(textObj->te_binbuf, &tb, &tsz);
                    if (tb && tsz > 0) {
                        juce::String full = juce::String::fromUTF8(tb, tsz).trim();
                        nodeClass = full.upToFirstOccurrenceOf(" ", false, false);
                        freebytes(tb, tsz);
                    }
                }
            }

            t_object* ob = pd::Interface::checkObject(g);
            bool isSigIn0 = (ob != nullptr && obj_issignalinlet(ob, 0) != 0);
            bool isSink = (nodeClass == "dac~" || nodeClass == "catch~" || nodeClass == "throw~"
                           || (hasSignalIn.count(g) > 0 && hasSignalOut.count(g) == 0));
            bool isSource = ((isSigIn0 == false && sigInCount[g] == 0 && (hasSignalOut.count(g) > 0 || nodeClass.endsWith("~")))
                             || nodeClass == "noise~" || nodeClass == "r~" || nodeClass == "readsf~");

            int cat = 2; // default DSP
            if (nodeClass == "r" || nodeClass == "s" || nodeClass == "f" || nodeClass == "float"
                || nodeClass == "pack" || nodeClass == "unpack" || nodeClass == "t" || nodeClass == "trigger"
                || nodeClass == "bng" || nodeClass == "tgl" || nodeClass == "msg"
                || nodeClass == "floatatom" || nodeClass == "symbolatom"
                || (!nodeClass.endsWith("~") && hasSignalIn.count(g) == 0 && hasSignalOut.count(g) == 0)) {
                cat = 0; // Control Left Gutter
            } else if (isSink) {
                cat = 3; // Sink Bottom
            } else if (isSource) {
                cat = 1; // Source Top
            }

            rects.push_back({ g, x, yy, w, h, cat, (cat == 1 ? 0 : (cat == 3 ? 3 : 1)), nodeClass });
        }

        // 3. Compute topological ranks for DSP objects (cat == 2)
        for (auto& r : rects) {
            if (r.category == 1) { // Source
                std::vector<t_gobj*> queue = sigDownstream[r.g];
                int rk = 1;
                std::unordered_set<t_gobj*> visited;
                while (!queue.empty() && rk < 3) {
                    std::vector<t_gobj*> nextQ;
                    for (auto* down : queue) {
                        if (visited.count(down)) continue;
                        visited.insert(down);
                        for (auto& target : rects) {
                            if (target.g == down && target.category == 2) {
                                target.rank = std::max(target.rank, rk);
                            }
                        }
                        for (auto* nxt : sigDownstream[down]) nextQ.push_back(nxt);
                    }
                    queue = nextQ;
                    rk++;
                }
            }
        }

        // 4. Place Control gutter (cat == 0) at x = 50
        int ctlY = 50;
        for (auto& r : rects) {
            if (r.category == 0) {
                r.x = 50;
                r.y = ctlY;
                ctlY = snap10(ctlY + r.h + 20);
            }
        }

        // 5. Place Signal ranks at x >= 250
        std::map<int, std::vector<size_t>> rankGroups;
        for (size_t i = 0; i < rects.size(); ++i) {
            if (rects[i].category != 0) {
                rankGroups[rects[i].rank].push_back(i);
            }
        }

        for (auto& [rank, indices] : rankGroups) {
            int rankY = 50 + rank * 150;
            int curX = 250;
            for (size_t idx : indices) {
                rects[idx].x = curX;
                rects[idx].y = rankY;
                curX = snap10(curX + rects[idx].w + 30);
            }
        }

        // 6. Minimal deoverlap pass to ensure no collision
        const int PAD = 5;
        for (int pass = 0; pass < 12; pass++) {
            bool hit = false;
            for (size_t i = 0; i < rects.size(); i++) {
                for (size_t j = i + 1; j < rects.size(); j++) {
                    auto& a = rects[i];
                    auto& b = rects[j];
                    if (a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                        && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y) {
                        hit = true;
                        b.x = snap10(a.x + a.w + 20);
                    }
                }
            }
            if (!hit) break;
        }

        int moved = 0;
        for (auto& r : rects) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
            if (r.x != ox || r.y != oy) {
                pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                moved++;
            }
        }
        if (moved > 0) {
            canvas_dirty(cnv, 1);
            resetCanvasUndo(processor, canvasName);
        }
        sys_unlock();

        processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
        sendReply("/pd/flow/reply/" + correlationId, 1.0f);
    }
    return;
}
