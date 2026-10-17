/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: diagnostics, bounds and collision reads (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

    // ── PRD diagnostic layer §2.2: /pd/diagnose — THE GRAPH X-RAY ───────
    // /pd/diagnose <canvasName> [corrId]
    // Read-only structured facts, computed where the data lives (no console
    // scraping, no TS graph analysis):
    //   dsp_cycles   : signal-graph cycles, each named by objects (PRD §2.2)
    //   unscheduled  : members of detected cycles (Pd refuses to schedule them)
    //   dangling_main_sig : objects with a DSP method but nothing wired into
    //                      their main signal inlet (informational)
    //   zeroed_vcgs  : bare [*~] with no float arg and no inlet-1 wire —
    //                  outputs constant 0, silently killing the downstream
    //                  chain (the Audio-VCA law, checked natively)
    //   mismatched   : rate-mismatched wires (sig→ctl / ctl→sig) — mostly
    //                  blocked by Pd at connect-time, emitted when present
    // Reply: /pd/diagnose/reply/<corrId> <jsonString>
void MCPBridge::pdDiagnose(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String replyAddr = "/pd/diagnose/reply/" + correlationId;

    juce::String json;
    bool hasCanvas = false;

    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

    if (cnv) {
        hasCanvas = true;
        json = computeDiagnoseFacts(processor, cnv, canvasName);
    }
    sys_unlock();

    if (!hasCanvas) {
        sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
        return;
    }
    sendReply(replyAddr, json);
    return;
}

    // ── /pd/clusters — context grouping (PRD_CONTEXT_LAYOUT_GUARD) ──────
    // /pd/clusters <canvasName> [corrId]
    // Reply: /pd/clusters/reply/<corrId> {"clusters":[{"id","kind","objects":[...]}]}
void MCPBridge::pdClusters(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[msg.size() - 1]) : "0";
    juce::String replyAddr = "/pd/clusters/reply/" + correlationId;

    juce::String json;
    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    json = cnv ? computeClusters(processor, cnv, canvasName)
               : juce::String("{\"error\":\"canvas not found\"}");
    sys_unlock();

    sendReply(replyAddr, json);
    return;
}

    // ── /pd/perf — CPU/DSP cost introspection (PRD 1.2) ─────────────────
    // /pd/perf <canvasName> [corrId]
    // Reply: /pd/perf/reply/<corrId> {cpu,dsp,canvases,signalObjects,
    //        estimatedWeight,top:[{canvas,weight,signalObjects,heavies}]}
void MCPBridge::pdPerf(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String replyAddr = "/pd/perf/reply/" + correlationId;

    juce::String json = "{\"error\":\"canvas not found\"}";
    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (cnv) json = computePerfFacts(processor, cnv, canvasName);
    sys_unlock();

    sendReply(replyAddr, json);
    return;
}

void MCPBridge::pdBounds(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[msg.size() - 1]) : "0";
    juce::String replyAddr = "/pd/bounds/reply/" + correlationId;

    juce::String json;
    bool hasCanvas = false;

    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

    if (cnv) {
        hasCanvas = true;
        std::unordered_map<t_gobj*, juce::String> ptrToId;
        auto mapIt = processor->getMcp()->session.mcpStableObjectMap.find(canonicalCanvasKey(cnv).toStdString());
        if (mapIt != processor->getMcp()->session.mcpStableObjectMap.end())
            for (auto& [tid, ptr] : mapIt->second)
                if (ptr) ptrToId[ptr] = juce::String(tid);

        auto guiCanvas = mcpLiveBounds(processor, cnv);
        auto guiView = processor->getMcp()->gui().viewState(cnv);

        float zoom = 1.0f;
        int vx = 0, vy = 0, vw = 1000, vh = 800;
        if (guiView) {
            zoom = guiView->zoom;
            if (zoom <= 0.001f) zoom = 1.0f;
            if (guiView->hasViewport) {
                auto va = guiView->viewArea;
                vx = static_cast<int>(std::round(va.getX() / zoom));
                vy = static_cast<int>(std::round(va.getY() / zoom));
                vw = static_cast<int>(std::round(va.getWidth() / zoom));
                vh = static_cast<int>(std::round(va.getHeight() / zoom));
            }
        }

        json = "{\"canvas\":\"" + jsonEscape(canvasName) + "\""
             + ",\"zoom\":" + juce::String(zoom, 3)
             + ",\"viewport\":{\"x\":" + juce::String(vx)
             + ",\"y\":" + juce::String(vy)
             + ",\"w\":" + juce::String(vw)
             + ",\"h\":" + juce::String(vh) + "}"
             + ",\"objects\":[";
        bool first = true;
        int idx = 0;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, y, guiCanvas.get(), &x, &yy, &w, &h);
            juce::String tid = ptrToId.count(y)
                ? ptrToId[y]
                : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx));

            t_class* cl = pd_class(&y->g_pd);
            const char* clName = class_getname(cl);
            juce::String className = clName ? juce::String::fromUTF8(clName) : juce::String("unknown");

            juce::String nodeClass = className;
            juce::String abstractName;

            if (cl == canvas_class) {
                nodeClass = "pd";
                if (pd::getAbstractionFileName(y, abstractName) && abstractName.isNotEmpty()) {
                    nodeClass = abstractName;
                }
            } else if (cl == garray_class) {
                nodeClass = "table";
            } else if (pd::Interface::isTextObject(y)) {
                t_text* textObj = reinterpret_cast<t_text*>(y);
                if (textObj->te_type == T_MESSAGE) {
                    nodeClass = "msg";
                } else if (textObj->te_type == T_ATOM) {
                    nodeClass = className.containsIgnoreCase("symbol") ? "symbolatom" : "floatatom";
                } else if (textObj->te_type == T_TEXT) {
                    nodeClass = "text";
                } else {
                    char* textBuf = nullptr;
                    int textSize = 0;
                    binbuf_gettext(textObj->te_binbuf, &textBuf, &textSize);
                    if (textBuf && textSize > 0) {
                        juce::String fullText = juce::String::fromUTF8(textBuf, textSize).trim();
                        juce::String firstTok = fullText.upToFirstOccurrenceOf(" ", false, false);
                        if (firstTok.isNotEmpty()) nodeClass = firstTok;
                        freebytes(textBuf, textSize);
                    }
                }
            }

            t_object* ob = pd::Interface::checkObject(y);
            bool sigIn0 = (ob != nullptr && obj_issignalinlet(ob, 0) != 0);

            if (!first) json += ",";
            json += "{\"tempId\":\"" + jsonEscape(tid) + "\""
                  + ",\"x\":" + juce::String(x)
                  + ",\"y\":" + juce::String(yy)
                  + ",\"w\":" + juce::String(w)
                  + ",\"h\":" + juce::String(h)
                  + ",\"sigIn0\":" + (sigIn0 ? "true" : "false")
                  + ",\"nodeClass\":\"" + juce::JSON::escapeString(nodeClass) + "\"}";
            first = false;
        }
        json += "]}";
    }
    sys_unlock();

    if (!hasCanvas) {
        sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
        return;
    }
    sendReply(replyAddr, json);
    return;
}

void MCPBridge::pdCollisions(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String replyAddr = "/pd/collisions/reply/" + correlationId;

    juce::String json;
    bool hasCanvas = false;

    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

    if (cnv) {
        hasCanvas = true;
        struct R { juce::String tid; int x, y, w, h; };
        std::vector<R> rects;
        std::unordered_map<t_gobj*, juce::String> ptrToId;
        auto mapIt = processor->getMcp()->session.mcpStableObjectMap.find(canonicalCanvasKey(cnv).toStdString());
        if (mapIt != processor->getMcp()->session.mcpStableObjectMap.end())
            for (auto& [tid, ptr] : mapIt->second)
                if (ptr) ptrToId[ptr] = juce::String(tid);

        auto guiCanvas = mcpLiveBounds(processor, cnv);
        int idx = 0;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, y, guiCanvas.get(), &x, &yy, &w, &h);
            juce::String tid = ptrToId.count(y)
                ? ptrToId[y]
                : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx));
            rects.push_back({ tid, x, yy, w, h });
        }

        const int PAD = 5;
        json = "{\"canvas\":\"" + jsonEscape(canvasName) + "\",\"collisions\":[";
        bool first = true;
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                const auto& a = rects[i];
                const auto& b = rects[j];
                bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                        && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                if (hit) {
                    if (!first) json += ",";
                    json += "{\"a\":\"" + jsonEscape(a.tid) + "\",\"b\":\"" + jsonEscape(b.tid) + "\"}";
                    first = false;
                }
            }
        }
        for (auto const& rg : processor->getMcp()->session.mcpRegions) {
            if (rg.title.isEmpty()) continue;
            int rx = static_cast<int>(rg.x), ry = static_cast<int>(rg.y);
            int rw = static_cast<int>(rg.w), rh = 26;
            for (size_t i = 0; i < rects.size(); ++i) {
                const auto& a = rects[i];
                bool hit = a.x < rx + rw + PAD && a.x + a.w + PAD > rx
                        && a.y < ry + rh + PAD && a.y + a.h + PAD > ry;
                if (hit) {
                    if (!first) json += ",";
                    json += "{\"a\":\"" + jsonEscape(a.tid) + "\",\"b\":\"region_title:" + jsonEscape(rg.title) + "\"}";
                    first = false;
                }
            }
        }
        json += "]}";
    }
    sys_unlock();

    if (!hasCanvas) {
        sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
        return;
    }
    sendReply(replyAddr, json);
    return;
}

void MCPBridge::pdWireOcclusions(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String replyAddr = "/pd/wire_occlusions/reply/" + correlationId;

    juce::String json;
    bool hasCanvas = false;

    sys_lock();
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

    if (cnv) {
        hasCanvas = true;
        std::vector<t_gobj*> objs;
        std::vector<juce::String> names;
std::vector<juce::String> classNames;
        std::unordered_map<t_gobj*, int> ptrToIdx;
        std::unordered_map<t_gobj*, juce::String> ptrToId;
        auto mapIt = processor->getMcp()->session.mcpStableObjectMap.find(canonicalCanvasKey(cnv).toStdString());
        if (mapIt != processor->getMcp()->session.mcpStableObjectMap.end())
            for (auto& [tid, ptr] : mapIt->second)
                if (ptr) ptrToId[ptr] = juce::String(tid);

        int idx = 0;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
            objs.push_back(y);
            ptrToIdx[y] = idx;
            names.push_back(ptrToId.count(y)
                ? ptrToId[y]
                : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx)));
        }

        auto guiCanvas = mcpLiveBounds(processor, cnv);
        struct R { int x, y, w, h; };
        std::vector<R> rects(objs.size());
        for (size_t i = 0; i < objs.size(); ++i) {
            int x = 0, yy = 0, w = 0, h = 0;
            getTrueObjectBounds(cnv, objs[i], guiCanvas.get(), &x, &yy, &w, &h);
            rects[i] = { x, yy, w, h };
        }

        // Edges via one linetraverser walk (same as diagnose §2).
        struct Edge { int si, di, outno, nout, inno, nin; };
        std::vector<Edge> edges;
        t_linetraverser lt;
        t_outconnect* oc = nullptr;
        linetraverser_start(&lt, cnv);
        while ((oc = linetraverser_next_nosize(&lt))) {
            auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
            auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
            if (si == ptrToIdx.end() || di == ptrToIdx.end()) continue;
            edges.push_back({ si->second, di->second, lt.tr_outno, lt.tr_nout, lt.tr_inno, lt.tr_nin });
        }

        // Anchor-line vs box test (Liang-Barsky, same as TS rule so
        // results agree — only the rects are now true).
        auto lineHitsBox = [](double x1, double y1, double x2, double y2,
                              double bx, double by, double bw, double bh) {
            double minX = std::min(x1, x2), maxX = std::max(x1, x2);
            double minY = std::min(y1, y2), maxY = std::max(y1, y2);
            if (maxX < bx || minX > bx + bw || maxY < by || minY > by + bh) return false;
            double dx = x2 - x1, dy = y2 - y1, t0 = 0.0, t1 = 1.0;
            double p[4] = { -dx, dx, -dy, dy };
            double q[4] = { x1 - bx, bx + bw - x1, y1 - by, by + bh - y1 };
            for (int i = 0; i < 4; ++i) {
                if (p[i] == 0) { if (q[i] < 0) return false; }
                else {
                    double t = q[i] / p[i];
                    if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
                    else { if (t < t0) return false; if (t < t1) t1 = t; }
                }
            }
            return t0 < t1 && t0 > 0.05 && t1 < 0.95;
        };

        json = "{\"canvas\":\"" + jsonEscape(canvasName) + "\",\"model\":\"anchor-line\",\"occlusions\":[";
        bool first = true;
        for (auto& e : edges) {
            int si = e.si, di = e.di;
            double x1 = mcpIoletX(rects[si].x, rects[si].w, e.outno, e.nout);
            double y1 = rects[si].y + rects[si].h;
            double x2 = mcpIoletX(rects[di].x, rects[di].w, e.inno, e.nin);
            double y2 = rects[di].y;
            for (size_t k = 0; k < objs.size(); ++k) {
                if ((int)k == si || (int)k == di) continue;
                if (lineHitsBox(x1, y1, x2, y2, rects[k].x, rects[k].y, rects[k].w, rects[k].h)) {
                    if (!first) json += ",";
                    json += "{\"srcId\":\"" + jsonEscape(names[si]) + "\",\"destId\":\"" + jsonEscape(names[di])
                          + "\",\"via\":\"" + jsonEscape(names[k]) + "\"}";
                    first = false;
                }
            }
        }
        json += "]}";
    }
    sys_unlock();

    if (!hasCanvas) {
        sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
        return;
    }
    sendReply(replyAddr, json);
    return;
}

void MCPBridge::pdMeasureText(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/measure_text <jsonStringsArray> <fontSize> <correlationId>
    // Fast font metrics batch using JUCE Font / Inter font.
    // Returns: /pd/measure_text/reply/<corrId> {"text": width, ...}
    if (msg.size() >= 2) {
        auto jsonArrayStr  = getArgString(msg[0]);
        float fontSize     = getArgFloat(msg[1]);
        if (fontSize <= 0.0f) fontSize = 12.0f;
        auto correlationId = (msg.size() > 2) ? getArgString(msg[2]) : "0";
        auto replyAddr     = "/pd/measure_text/reply/" + correlationId;

        juce::var parsed = juce::JSON::parse(jsonArrayStr);
        juce::Font font = processor->getMcp()->gui().defaultFont().withHeight(fontSize);
        auto* resultObj = new juce::DynamicObject();

        if (auto* arr = parsed.getArray()) {
            for (const auto& item : *arr) {
                auto str = item.toString();
                resultObj->setProperty(str, juce::roundToInt(juce::GlyphArrangement().getStringWidth(font, str)));
            }
        } else if (parsed.isString()) {
            auto str = parsed.toString();
            resultObj->setProperty(str, juce::roundToInt(juce::GlyphArrangement().getStringWidth(font, str)));
        }

        juce::String jsonResult = juce::JSON::toString(juce::var(resultObj));
        sendReply(replyAddr, jsonResult);
    }
    return;
}
