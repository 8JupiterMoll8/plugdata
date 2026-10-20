/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: selection, AI overlays, moodboard, canvas and GEM capture (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

    // /pd/select <canvas> <count> <tempId…> <corrId>
    // PRD 1.5: programmatically SET the GUI selection (highlight) by tempId so
    // the artist SEES exactly what the AI is about to act on. count==0 clears.
    // Runs on the message thread (Canvas components); DSP untouched, zero dropout.
    // Reply: /pd/select/reply/<corrId> <selectedCount>
void MCPBridge::pdSelect(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    int  count      = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : 0;
    std::vector<juce::String> ids;
    int cursor = 2;
    for (int i = 0; i < count && cursor < msg.size(); i++) ids.push_back(getArgString(msg[cursor++]));
    auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) { sendReply("/pd/select/reply/" + correlationId, 0.0f); return; }

    postMessage([proc = processor, cnv, canvasName, ids, correlationId, bridge = this]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/select/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        std::vector<t_gobj*> targets;
        for (const auto& id : ids) {
            t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id);
            if (!g) continue;
            targets.push_back(g);
        }
        auto selection = proc->getMcp()->gui().select(cnv, targets);
        if (!selection) { bridge->sendReply("/pd/select/reply/" + correlationId, 0.0f); return; }
        auto const& chosen = selection->objects;

        // Two-way sync: report the programmatic selection to the MCP so
        // get_selection() reflects it (mirrors Sidebar::showParameters).
        if (!chosen.empty()) {
            auto subpatchSym = proc->generateSymbol(selection->title);
            if (chosen.size() == 1) {
                SmallArray<pd::Atom> atoms;
                atoms.add(pd::Atom(subpatchSym));
                atoms.add(pd::Atom(proc->generateSymbol(chosen[0].type)));
                atoms.add(pd::Atom(static_cast<float>(chosen[0].index)));
                auto b = chosen[0].bounds;
                atoms.add(pd::Atom(static_cast<float>(b.getX())));
                atoms.add(pd::Atom(static_cast<float>(b.getY())));
                bridge->sendSelectionTelemetry("selected_object", atoms);
            } else {
                SmallArray<pd::Atom> countAtoms;
                countAtoms.add(pd::Atom(subpatchSym));
                countAtoms.add(pd::Atom(static_cast<float>(chosen.size())));
                bridge->sendSelectionTelemetry("selection_count", countAtoms);

                SmallArray<pd::Atom> indexAtoms;
                indexAtoms.add(pd::Atom(subpatchSym));
                for (auto const& o : chosen)
                    indexAtoms.add(pd::Atom(static_cast<float>(o.index)));
                bridge->sendSelectionTelemetry("selection_indices", indexAtoms);
            }
        }

        bridge->sendReply("/pd/select/reply/" + correlationId, static_cast<float>(chosen.size()));
    });
    return;
}

    // /pd/ai_overlay <canvas> <corrId> <state> <count> <tempId…>
    // PRD overlay: mark objects with an AI state drawn by the canvas AI overlay
    // (1=changed, 2=proposed, 3=error; state 0 with no ids clears ALL).
    // Message-thread only; pure draw state — never touches the DSP graph.
    // Reply: /pd/ai_overlay/reply/<corrId> <markedCount>
void MCPBridge::pdAiOverlay(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    int  state         = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 0;
    bool transient     = msg.size() > 3 && getArgFloat(msg[3]) > 0.5f;
    int  count         = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : 0;
    std::vector<juce::String> ids;
    int cursor = 5;
    for (int i = 0; i < count && cursor < msg.size(); i++) ids.push_back(getArgString(msg[cursor++]));

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) { sendReply("/pd/ai_overlay/reply/" + correlationId, 0.0f); return; }

    postMessage([proc = processor, cnv, canvasName, state, transient, ids, correlationId, bridge = this]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/ai_overlay/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        juce::ScopedLock overlaySl(proc->getMcp()->session.mcpOverlayLock);
        if (state == 0 && ids.empty()) {
            // Clear ALL — but only transient ones when the caller is transient.
            if (transient) {
                for (auto it = proc->getMcp()->session.mcpAiOverlay.begin(); it != proc->getMcp()->session.mcpAiOverlay.end(); ) {
                    if (it->second.transient) it = proc->getMcp()->session.mcpAiOverlay.erase(it);
                    else ++it;
                }
            } else {
                proc->getMcp()->session.mcpAiOverlay.clear();
            }
        } else if (state == 0) {
            // Clear specific ids — transient clears only touch transient marks,
            // so an ambient flash can never erase a persistent answer/diff mark.
            for (auto const& id : ids) {
                t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id);
                if (!g) continue;
                auto it = proc->getMcp()->session.mcpAiOverlay.find(g);
                if (it == proc->getMcp()->session.mcpAiOverlay.end()) continue;
                if (!transient || it->second.transient) proc->getMcp()->session.mcpAiOverlay.erase(it);
            }
        } else {
            for (auto const& id : ids) {
                if (t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id))
                    proc->getMcp()->session.mcpAiOverlay[g] = McpSession::AiMark { state, transient };
            }
        }
        proc->getMcp()->gui().repaintView(cnv);
        bridge->sendReply("/pd/ai_overlay/reply/" + correlationId, static_cast<float>(proc->getMcp()->session.mcpAiOverlay.size()));
    });
    return;
}

    // /pd/ai_ghost <canvas> <corrId> <jsonArray>
    // PRD overlay: ghost/preview of PROPOSED (uncommitted) changes drawn above the
    // patch. Each item: {"kind":"box",x,y,w,h,label} or {"kind":"wire",src,dest}
    // (wire endpoints are computed from object bounds). Empty array clears.
    // Message-thread only; pure draw state — never touches the DSP graph.
    // Reply: /pd/ai_ghost/reply/<corrId> <ghostCount>
void MCPBridge::pdAiGhost(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto jsonStr       = msg.size() > 2 ? getArgString(msg[2]) : juce::String("[]");

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) { sendReply("/pd/ai_ghost/reply/" + correlationId, 0.0f); return; }

    auto parsed = juce::JSON::parse(jsonStr);

    postMessage([proc = processor, cnv, canvasName, correlationId, bridge = this, parsed]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/ai_ghost/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        // P3.4 S11 (TSan): the Pd lock before the overlay lock, the order the GUI's sync and paint use.
        sys_lock();
        juce::ScopedLock overlaySl(proc->getMcp()->session.mcpOverlayLock);
        proc->getMcp()->session.mcpGhosts.clear();
        if (auto* arr = parsed.getArray()) {
            for (auto const& v : *arr) {
                auto* o = v.getDynamicObject();
                if (!o) continue;
                McpSession::McpGhost g;
                if (o->getProperty("kind").toString() == "wire") {
                    g.kind = 1;
                    t_gobj* gs = proc->getMcp()->resolveStableId(canvasName, o->getProperty("src").toString());
                    t_gobj* gd = proc->getMcp()->resolveStableId(canvasName, o->getProperty("dest").toString());
                    if (!gs || !gd) continue;
                    int sx, sy, sw, sh, dx, dy, dw, dh;
                    pd::Interface::getObjectBounds(cnv, gs, &sx, &sy, &sw, &sh);
                    pd::Interface::getObjectBounds(cnv, gd, &dx, &dy, &dw, &dh);
                    g.x1 = sx + sw * 0.5f; g.y1 = sy + sh;
                    g.x2 = dx + dw * 0.5f; g.y2 = dy;
                } else {
                    g.kind = 0;
                    g.x = static_cast<float>(o->getProperty("x"));
                    g.y = static_cast<float>(o->getProperty("y"));
                    g.w = static_cast<float>(o->getProperty("w"));
                    g.h = static_cast<float>(o->getProperty("h"));
                    g.label = o->getProperty("label").toString();
                }
                proc->getMcp()->session.mcpGhosts.push_back(g);
            }
        }
        sys_unlock();
        proc->getMcp()->gui().repaintView(cnv);
        bridge->sendReply("/pd/ai_ghost/reply/" + correlationId, static_cast<float>(proc->getMcp()->session.mcpGhosts.size()));
    });
    return;
}

    // /pd/ai_annotate <canvas> <corrId> <jsonArray>
    // PRD overlay: short in-place AI annotations ("why / what"), drawn as a
    // translucent tag near an object. Each item {target:tempId, text} (anchored
    // below the object) or {x,y,text}. Empty array clears. Message-thread only;
    // pure draw state — never touches the DSP graph.
    // Reply: /pd/ai_annotate/reply/<corrId> <count>
void MCPBridge::pdAiAnnotate(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto jsonStr       = msg.size() > 2 ? getArgString(msg[2]) : juce::String("[]");

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) { sendReply("/pd/ai_annotate/reply/" + correlationId, 0.0f); return; }

    auto parsed = juce::JSON::parse(jsonStr);

    postMessage([proc = processor, cnv, canvasName, correlationId, bridge = this, parsed]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/ai_annotate/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        // P3.4 S11 (TSan): the Pd lock before the overlay lock, the order the GUI's sync and paint use.
        sys_lock();
        juce::ScopedLock overlaySl(proc->getMcp()->session.mcpOverlayLock);
        proc->getMcp()->session.mcpAnnotations.clear();
        if (auto* arr = parsed.getArray()) {
            for (auto const& v : *arr) {
                auto* o = v.getDynamicObject();
                if (!o) continue;
                McpSession::McpAnnotation a;
                a.text = o->getProperty("text").toString();
                a.kind = o->getProperty("kind").toString();
                a.t = juce::Time::getMillisecondCounterHiRes() * 0.001;
                if (a.text.isEmpty()) continue;
                auto target = o->getProperty("target").toString();
                if (target.isNotEmpty()) {
                    t_gobj* g = proc->getMcp()->resolveStableId(canvasName, target);
                    if (!g) continue;
                    int x, y, w, h;
                    pd::Interface::getObjectBounds(cnv, g, &x, &y, &w, &h);
                    float const ox = static_cast<float>(x), oy = static_cast<float>(y);
                    float const ow = static_cast<float>(w), oh = static_cast<float>(h);
                    float const nw = a.text.length() * 7.0f + 28.0f; // approximate note size
                    constexpr float nh = 18.0f;
                    // Avoid overlapping ANY object or existing note.
                    std::vector<juce::Rectangle<float>> avoid;
                    for (t_gobj* y2 = cnv->gl_list; y2; y2 = y2->g_next) {
                        int bx, by, bw, bh;
                        pd::Interface::getObjectBounds(cnv, y2, &bx, &by, &bw, &bh);
                        avoid.emplace_back(static_cast<float>(bx), static_cast<float>(by),
                                           static_cast<float>(bw), static_cast<float>(bh));
                    }
                    for (auto const& an : proc->getMcp()->session.mcpAnnotations)
                        avoid.emplace_back(an.x - 6.0f, an.y - 9.0f, an.text.length() * 7.0f + 28.0f, 18.0f);
                    for (auto const& rg : proc->getMcp()->session.mcpRegions) {
                        if (rg.title.isNotEmpty())
                            avoid.emplace_back(rg.x, rg.y, rg.w, 24.0f);
                    }
                    auto fits = [&](float nx, float ny) {
                        juce::Rectangle<float> r(nx, ny, nw, nh);
                        for (auto& ob : avoid) if (r.expanded(6.0f).intersects(ob)) return false;
                        return true;
                    };
                    struct Cand { float nx, ny, lx, ly; }; // note top-left + leader point
                    std::vector<Cand> const cands = {
                        { ox + ow + 14.0f, oy + oh * 0.5f - nh * 0.5f, ox + ow, oy + oh * 0.5f },       // right
                        { ox - nw - 14.0f, oy + oh * 0.5f - nh * 0.5f, ox, oy + oh * 0.5f },            // left
                        { ox, oy - nh - 20.0f, ox + ow * 0.5f, oy },                                     // above
                        { ox, oy + oh + 20.0f, ox + ow * 0.5f, oy + oh },                                // below
                        { ox + ow + 14.0f, oy - 20.0f, ox + ow, oy },                                    // upper-right
                    };
                    Cand chosen = cands[0];
                    for (auto& c : cands) if (fits(c.nx, c.ny)) { chosen = c; break; }
                    a.x = chosen.nx + 6.0f; // note anchor (tag left = a.x - 6)
                    a.y = chosen.ny + 9.0f; // note anchor (tag centre = a.y)
                    a.hasLeader = true;
                    a.leaderX = chosen.lx;
                    a.leaderY = chosen.ly;
                    a.targetId = target;
                } else {
                    a.x = static_cast<float>(o->getProperty("x"));
                    a.y = static_cast<float>(o->getProperty("y"));
                }
                proc->getMcp()->session.mcpAnnotations.push_back(a);
            }
        }
        sys_unlock();
        proc->getMcp()->gui().repaintView(cnv);
        juce::var details = juce::Array<juce::var>();
        for (auto const& an : proc->getMcp()->session.mcpAnnotations) {
            auto* d = new juce::DynamicObject();
            d->setProperty("targetId", an.targetId);
            d->setProperty("text", an.text);
            d->setProperty("kind", an.kind);
            d->setProperty("x", an.x);
            d->setProperty("y", an.y);
            d->setProperty("hasLeader", an.hasLeader);
            d->setProperty("leaderX", an.leaderX);
            d->setProperty("leaderY", an.leaderY);
            details.append(d);
        }
        juce::Array<juce::var> replyArgs;
        replyArgs.add(static_cast<float>(proc->getMcp()->session.mcpAnnotations.size()));
        replyArgs.add(juce::JSON::toString(details, true));
        bridge->sendReply("/pd/ai_annotate/reply/" + correlationId, replyArgs);
    });
    return;
}

    // /pd/ai_region <canvas> <corrId> <jsonArray>
    // PRD overlay: titled REGIONS grouping objects. Items {targets:[tempIds], title, kind}
    // (bounds computed from the objects) or {x,y,w,h,title,kind}. Empty array clears.
    // Drawn BEHIND the objects so the patch structure shows through.
    // Reply: /pd/ai_region/reply/<corrId> <count>
void MCPBridge::pdAiRegion(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto jsonStr       = msg.size() > 2 ? getArgString(msg[2]) : juce::String("[]");

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) { sendReply("/pd/ai_region/reply/" + correlationId, 0.0f); return; }

    auto parsed = juce::JSON::parse(jsonStr);

    postMessage([proc = processor, cnv, canvasName, correlationId, bridge = this, parsed]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/ai_region/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        // DEADLOCK FIX: resolve bounds under the Pd lock WITHOUT holding
        // mcpOverlayLock. The GUI/render path takes sys_lock -> mcpOverlayLock;
        // holding BOTH here in the opposite order deadlocked and froze the UI.
        // Build into a local vector, then swap it in under mcpOverlayLock only.
        std::vector<McpSession::McpRegion> built;
        if (auto* arr = parsed.getArray()) {
            sys_lock();
            for (auto const& v : *arr) {
                auto* o = v.getDynamicObject();
                if (!o) continue;
                McpSession::McpRegion r;
                r.title = o->getProperty("title").toString();
                r.kind = o->getProperty("kind").toString();
                auto* targets = o->getProperty("targets").getArray();
                if (targets && !targets->isEmpty()) {
                    for (auto const& tv : *targets) {
                        r.targetIds.add(tv.toString());
                    }
                    bool first = true;
                    int minX = 0, minY = 0, maxX = 0, maxY = 0;
                    for (auto const& tv : *targets) {
                        t_gobj* g = proc->getMcp()->resolveStableId(canvasName, tv.toString());
                        if (!g) continue;
                        int x, y, w, h;
                        pd::Interface::getObjectBounds(cnv, g, &x, &y, &w, &h);
                        if (first) { minX = x; minY = y; maxX = x + w; maxY = y + h; first = false; }
                        else {
                            minX = std::min(minX, x); minY = std::min(minY, y);
                            maxX = std::max(maxX, x + w); maxY = std::max(maxY, y + h);
                        }
                    }
                    if (first) continue;
                    constexpr int pad = 22;
                    r.x = static_cast<float>(minX - pad);
                    r.y = static_cast<float>(minY - pad - 16);
                    r.w = static_cast<float>(maxX - minX + pad * 2);
                    r.h = static_cast<float>(maxY - minY + pad * 2 + 16);
                } else {
                    r.x = static_cast<float>(o->getProperty("x"));
                    r.y = static_cast<float>(o->getProperty("y"));
                    r.w = static_cast<float>(o->getProperty("w"));
                    r.h = static_cast<float>(o->getProperty("h"));
                }
                built.push_back(r);
            }
            sys_unlock();
        }
        {
            juce::ScopedLock overlaySl(proc->getMcp()->session.mcpOverlayLock);
            proc->getMcp()->session.mcpRegions = std::move(built);
        }
        proc->getMcp()->gui().repaintView(cnv);
        bridge->sendReply("/pd/ai_region/reply/" + correlationId, static_cast<float>(proc->getMcp()->session.mcpRegions.size()));
    });
    return;
}

    // /pd/ai_hud <canvas> <corrId> <jsonObject>
    //   active: bool, chapter: string, tool: string, latency: string, prompt: string, receipt: string, telemetry: string
    // Reply: /pd/ai_hud/reply/<corrId> 1.0f
void MCPBridge::pdAiHud(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() < 3 || !processor) {
        sendReply("/pd/ai_hud/reply/invalid", 0.0f);
        return;
    }
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = getArgString(msg[1]);
    auto jsonStr = getArgString(msg[2]);

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);

    auto parsed = juce::JSON::parse(jsonStr);

    postMessage([proc = processor, cnv, correlationId, bridge = this, parsed]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/ai_hud/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
        juce::ScopedLock overlaySl(proc->getMcp()->session.mcpOverlayLock);
        proc->getMcp()->session.mcpHud = McpSession::McpHud();
        if (auto* o = parsed.getDynamicObject()) {
            proc->getMcp()->session.mcpHud.active = o->hasProperty("active") ? static_cast<bool>(o->getProperty("active")) : true;
            proc->getMcp()->session.mcpHud.chapter = o->getProperty("chapter").toString();
            proc->getMcp()->session.mcpHud.tool = o->getProperty("tool").toString();
            proc->getMcp()->session.mcpHud.latency = o->getProperty("latency").toString();
            proc->getMcp()->session.mcpHud.prompt = o->getProperty("prompt").toString();
            proc->getMcp()->session.mcpHud.receipt = o->getProperty("receipt").toString();
            proc->getMcp()->session.mcpHud.telemetry = o->getProperty("telemetry").toString();
        }
        if (cnv) {
            proc->getMcp()->gui().repaintView(cnv);
        }
        bridge->sendReply("/pd/ai_hud/reply/" + correlationId, proc->getMcp()->session.mcpHud.active ? 1.0f : 0.0f);
    });
    return;
}

    // /pd/get_overlays <canvas> <corrId>
    // Reply: /pd/get_overlays/reply/<corrId> <jsonState>
void MCPBridge::pdGetOverlays(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

    t_canvas* cnv = processor ? processor->getMcp()->getCanvasBySymbol(canvasName) : nullptr;

    postMessage([proc = processor, cnv, correlationId, bridge = this]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/get_overlays/reply/" + correlationId, juce::String("{\"error\":\"canvas closed\"}")); return; } // P3.4 S4: closed since the handler ran
        int mask = proc ? proc->getMcp()->gui().overlayMask(cnv).value_or(0) : 0;
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("mask", mask);
        obj->setProperty("origin", static_cast<bool>(mask & Origin));
        obj->setProperty("border", static_cast<bool>(mask & Border));
        obj->setProperty("index", static_cast<bool>(mask & Index));
        obj->setProperty("coordinate", static_cast<bool>(mask & Coordinate));
        obj->setProperty("objectActivity", static_cast<bool>(mask & ActivationState));
        obj->setProperty("connectionActivity", static_cast<bool>(mask & ConnectionActivity));
        obj->setProperty("order", static_cast<bool>(mask & Order));
        obj->setProperty("direction", static_cast<bool>(mask & Direction));
        obj->setProperty("behind", static_cast<bool>(mask & Behind));
        obj->setProperty("aiState", static_cast<bool>(mask & AIState));
        obj->setProperty("aiRegions", static_cast<bool>(mask & AIRegions));
        obj->setProperty("aiAnnotations", static_cast<bool>(mask & AIAnnotations));
        obj->setProperty("aiGhosts", static_cast<bool>(mask & AIGhosts));
        obj->setProperty("aiHud", static_cast<bool>(mask & AIHud));
        obj->setProperty("hasTargets", proc ? proc->getMcp()->session.hasMcpOverlayTargets() : false);
        obj->setProperty("targetedCount", proc ? static_cast<int>(proc->getMcp()->session.getMcpOverlayTargetsCount()) : 0);

        juce::String jsonStr = juce::JSON::toString(juce::var(obj.get()));
        bridge->sendReply("/pd/get_overlays/reply/" + correlationId, jsonStr);
    });
    return;
}

    // /pd/set_overlays <canvas> <corrId> <jsonArgsOrBitmask>
    // Reply: /pd/set_overlays/reply/<corrId> <jsonState>
void MCPBridge::pdSetOverlays(const juce::String& action, const juce::OSCMessage& msg)
{
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto argStr        = msg.size() > 2 ? getArgString(msg[2]) : "{}";

    t_canvas* cnv = processor ? processor->getMcp()->getCanvasBySymbol(canvasName) : nullptr;

    postMessage([proc = processor, cnv, canvasName, argStr, correlationId, bridge = this]() {
        if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/set_overlays/reply/" + correlationId, juce::String("{\"error\":\"canvas closed\"}")); return; } // P3.4 S4: closed since the handler ran
        auto const viewMask = proc ? proc->getMcp()->gui().overlayMask(cnv) : std::nullopt;
        if (!viewMask) {
            bridge->sendReply("/pd/set_overlays/reply/" + correlationId, juce::String("{}"));
            return;
        }
        int currentMask = *viewMask;
        auto parsed = juce::JSON::parse(argStr);
        if (parsed.isObject()) {
            auto* o = parsed.getDynamicObject();
            auto updateBit = [&](const char* prop, int flag) {
                if (o->hasProperty(prop)) {
                    bool enable = static_cast<bool>(o->getProperty(prop));
                    if (enable) currentMask |= flag;
                    else currentMask &= ~flag;
                }
            };
            updateBit("origin", Origin);
            updateBit("border", Border);
            updateBit("index", Index);
            updateBit("coordinate", Coordinate);
            updateBit("objectActivity", ActivationState);
            updateBit("connectionActivity", ConnectionActivity);
            updateBit("order", Order);
            updateBit("direction", Direction);
            updateBit("behind", Behind);
            updateBit("aiState", AIState);
            updateBit("aiRegions", AIRegions);
            updateBit("aiAnnotations", AIAnnotations);
            updateBit("aiGhosts", AIGhosts);
            updateBit("aiHud", AIHud);

            if (proc) {
                if (o->hasProperty("clearTargets") && static_cast<bool>(o->getProperty("clearTargets"))) {
                    proc->getMcp()->session.clearMcpOverlayTargets();
                } else if (o->hasProperty("targets")) {
                    auto targetsVar = o->getProperty("targets");
                    if (targetsVar.isArray()) {
                        auto* arr = targetsVar.getArray();
                        std::unordered_set<t_gobj*> targetGobjs;
                        for (int i = 0; i < arr->size(); ++i) {
                            juce::String id = (*arr)[i].toString();
                            t_gobj* g = proc->getMcp()->resolveStableId(canvasName, id);
                            if (g) targetGobjs.insert(g);
                        }
                        bool strict = o->hasProperty("strictCables") ? static_cast<bool>(o->getProperty("strictCables")) : false;
                        proc->getMcp()->session.setMcpOverlayTargets(std::move(targetGobjs), strict);
                    }
                }
            }
        } else if (parsed.isInt() || parsed.isInt64()) {
            currentMask = static_cast<int>(parsed);
        }

        proc->getMcp()->gui().setOverlayMask(cnv, currentMask);

        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("mask", currentMask);
        obj->setProperty("origin", static_cast<bool>(currentMask & Origin));
        obj->setProperty("border", static_cast<bool>(currentMask & Border));
        obj->setProperty("index", static_cast<bool>(currentMask & Index));
        obj->setProperty("coordinate", static_cast<bool>(currentMask & Coordinate));
        obj->setProperty("objectActivity", static_cast<bool>(currentMask & ActivationState));
        obj->setProperty("connectionActivity", static_cast<bool>(currentMask & ConnectionActivity));
        obj->setProperty("order", static_cast<bool>(currentMask & Order));
        obj->setProperty("direction", static_cast<bool>(currentMask & Direction));
        obj->setProperty("behind", static_cast<bool>(currentMask & Behind));
        obj->setProperty("aiState", static_cast<bool>(currentMask & AIState));
        obj->setProperty("aiRegions", static_cast<bool>(currentMask & AIRegions));
        obj->setProperty("aiAnnotations", static_cast<bool>(currentMask & AIAnnotations));
        obj->setProperty("aiGhosts", static_cast<bool>(currentMask & AIGhosts));
        obj->setProperty("aiHud", static_cast<bool>(currentMask & AIHud));
        obj->setProperty("hasTargets", proc ? proc->getMcp()->session.hasMcpOverlayTargets() : false);
        obj->setProperty("targetedCount", proc ? static_cast<int>(proc->getMcp()->session.getMcpOverlayTargetsCount()) : 0);

        juce::String jsonStr = juce::JSON::toString(juce::var(obj.get()));
        bridge->sendReply("/pd/set_overlays/reply/" + correlationId, jsonStr);
    });
    return;
}

void MCPBridge::pdMoodboard(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/moodboard <corrId> -> /pd/moodboard/reply/<corrId> <json>
    // The multimodal brief rail: every visible moodboard artifact with paths,
    // coordinates, bindings and the structured voice takes.
    auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    postMessage([proc = processor, correlationId, this] {
        juce::Array<juce::var> imagesArr, notesArr, takesArr, midiTakesArr, selArr;
        juce::String strokesJson = "[]";
        juce::String bboxJson = "{}";

        if (proc) {
            for (auto const& img : proc->getMcp()->session.getMcpReferenceImages()) {
                auto* o = new juce::DynamicObject();
                o->setProperty("id", img.id);
                o->setProperty("path", img.filePath);
                o->setProperty("title", img.title);
                o->setProperty("x", img.x);
                o->setProperty("y", img.y);
                o->setProperty("w", img.width);
                o->setProperty("h", img.height);
                o->setProperty("targetId", img.targetId);
                o->setProperty("locked", img.isLocked);
                o->setProperty("visible", img.visible);
                imagesArr.add(juce::var(o));
            }
            for (auto const& a : proc->getMcp()->session.getMcpAnnotations()) {
                if (a.text.isEmpty()) continue;
                auto* o = new juce::DynamicObject();
                o->setProperty("text", a.text);
                o->setProperty("kind", a.kind);
                o->setProperty("targetId", a.targetId);
                notesArr.add(juce::var(o));
            }
            for (auto const& t : proc->getMcp()->session.getMcpVoiceTakes()) {
                auto* o = new juce::DynamicObject();
                o->setProperty("id", t.id);
                o->setProperty("wav", t.wavPath);
                o->setProperty("mode", t.mode);
                o->setProperty("bpm", t.bpm);
                o->setProperty("hits", t.hitCount);
                o->setProperty("notes", t.noteCount);
                o->setProperty("duration", t.durationSec);
                o->setProperty("visible", t.visible);
                takesArr.add(juce::var(o));
            }
            for (auto const& t : proc->getMcp()->session.getMcpMidiTakes()) {
                auto* o = new juce::DynamicObject();
                o->setProperty("id", t.id);
                o->setProperty("voice", t.voice);
                o->setProperty("bpm", t.bpm);
                o->setProperty("notes", t.noteCount);
                o->setProperty("duration", t.durationSec);
                o->setProperty("visible", t.visible);
                o->setProperty("json", t.json);
                midiTakesArr.add(juce::var(o));
            }

            if (auto sketch = proc->getMcp()->gui().focusedSketch()) {
                strokesJson = sketch->strokesJson;
                auto const sb = sketch->bounds;
                if (!sb.isEmpty()) {
                    auto* b = new juce::DynamicObject();
                    b->setProperty("x", sb.getX());
                    b->setProperty("y", sb.getY());
                    b->setProperty("w", sb.getWidth());
                    b->setProperty("h", sb.getHeight());
                    bboxJson = juce::JSON::toString(juce::var(b), true);
                }
                for (auto const& tid : sketch->selection) {
                    selArr.add(tid);
                }
            }
        }

        juce::String json = "{"
            "\"images\":" + juce::JSON::toString(juce::var(imagesArr), false)
            + ",\"notes\":" + juce::JSON::toString(juce::var(notesArr), false)
            + ",\"voiceTakes\":" + juce::JSON::toString(juce::var(takesArr), false)
            + ",\"midiTakes\":" + juce::JSON::toString(juce::var(midiTakesArr), false)
            + ",\"strokes\":" + strokesJson
            + ",\"bbox\":" + bboxJson
            + ",\"selection\":" + juce::JSON::toString(juce::var(selArr), false)
            + "}";
        sendReply("/pd/moodboard/reply/" + correlationId, json);
    });
    return;
}

    // /pd/screenshot_canvas <canvas> <scale> <corrId>
    // Renders the focused JUCE Canvas component to a PNG temp file and replies
    // with the absolute file path. The MCP server reads the file, base64-encodes
    // it, and returns an MCP ImageContent block so the LLM sees the patch visually.
    //
    // Threading: createComponentSnapshot MUST run on the JUCE Message Thread.
    //            We use callAsync (same pattern as zoom_to_fit). DSP is untouched —
    //            zero dropout guaranteed.
    //
    // Scale: 0.25–1.0 (default 0.5). Canvas can be large; 50% captures structure
    //        without burning LLM vision token budget.
void MCPBridge::pdScreenshotCanvas(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto canvasName     = normalizeCanvas(getArgString(msg[0]));
        float scale         = (msg.size() >= 2) ? static_cast<float>(getArgFloat(msg[1])) : 0.5f;
        auto  correlationId = (msg.size() >= 3) ? getArgString(msg[2]) : "0";
        bool  wantLabels    = (msg.size() >= 4) ? (getArgFloat(msg[3]) > 0.5f) : false;

        scale = juce::jlimit(0.1f, 2.0f, scale);

        // Resolve t_canvas — null is fine, we fall back to focused editor canvas
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);

        // Label collection runs inside the GUI host's capture (Message Thread),
        // after the view is resolved, so it always walks the exact canvas that
        // was rendered — never a stale/null pre-resolved cnv. sys_lock from the
        // Message Thread is safe (same pattern as /pd/diagnose).
        postMessage([proc = processor, cnv, scale, correlationId, bridge = this,
                                         wantLabels, canvasName]() {
            if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error: canvas closed")); return; } // P3.4 S4: closed since the handler ran
            McpGuiHost::LabelSource labelSource = [proc, canvasName](t_canvas* liveCnv) {
                std::vector<McpGuiHost::ScreenshotLabel> labels;
                static const std::unordered_set<juce::String> guiClasses = {
                    "knob", "vsl", "hsl", "vu", "tgl", "bng", "nbx",
                    "hradio", "vradio", "cnv"
                };

                // liveCnv: the t_canvas* of the captured view; walk to root for pd-main
                if (canvasName == "pd-main" || canvasName == "main") {
                    auto* g = reinterpret_cast<t_glist*>(liveCnv);
                    while (g && g->gl_owner) g = g->gl_owner;
                    if (g) liveCnv = reinterpret_cast<t_canvas*>(g);
                }

                // ptrToId from the identity map for tempId fallback naming (P3.5 S2: under the Pd lock)
                std::unordered_map<t_gobj*, juce::String> ptrToId;
                if (proc) {
                    McpHost::PdLockScope lock(*proc);
                    auto const* canvasIds = liveCnv ? proc->getMcp()->identity.findCanvas(liveCnv) : nullptr;
                    if (canvasIds)
                        for (auto& [tid, ptr] : *canvasIds)
                            if (ptr) ptrToId[ptr] = juce::String(tid);
                }

                sys_lock();
                if (liveCnv) {
                    for (t_gobj* y = liveCnv->gl_list; y; y = y->g_next) {
                        t_class* cl = pd_class(&y->g_pd);
                        const char* clName = class_getname(cl);
                        if (!clName) continue;
                        juce::String className = juce::String::fromUTF8(clName);
                        if (!guiClasses.count(className)) continue;

                        juce::String semantic;
                        if (className == "knob") {
                            semantic = mcpKnobLabelName(y);
                        } else {
                            auto* iem = reinterpret_cast<t_iemgui*>(y);
                            auto valid = [](t_symbol* s) -> bool {
                                return s && s != gensym("") && s->s_name && s->s_name[0]
                                    && juce::String::fromUTF8(s->s_name) != "empty";
                            };
                            // Guard with x_fsf flags — raw x_snd/x_rcv are garbage when not set (vu bug: "nosndno")
                            if (iem->x_fsf.x_rcv_able && valid(iem->x_rcv))      semantic = juce::String::fromUTF8(iem->x_rcv->s_name);
                            else if (iem->x_fsf.x_snd_able && valid(iem->x_snd)) semantic = juce::String::fromUTF8(iem->x_snd->s_name);
                        }
                        if (semantic.isEmpty()) {
                            auto it = ptrToId.find(y);
                            if (it != ptrToId.end()) semantic = it->second;
                        }
                        semantic = semantic.replace("\\ ", " ");

                        int x = 0, yy = 0, w = 0, h = 0;
                        pd::Interface::getObjectBounds(liveCnv, y, &x, &yy, &w, &h);
                        if (w <= 0) w = 60;
                        if (h <= 0) h = 20;

                        McpGuiHost::ScreenshotLabel lbl;
                        lbl.x = x; lbl.y = yy; lbl.w = w; lbl.h = h;
                        lbl.text = semantic.isNotEmpty()
                            ? semantic + " · " + className
                            : className;
                        labels.push_back(lbl);
                    }
                }
                sys_unlock();
                return labels;
            };
            auto const result = proc->getMcp()->gui().screenshot(cnv, scale, wantLabels ? &labelSource : nullptr, correlationId);
            bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, result);
        });
    }
    return;
}

    // /pd/gem_capture <corrId> [scale]
    // PRD 1.4: capture the live GEM render window (GemJUCEWindow — its own
    // OpenGL context, a separate native window) as a PNG temp file. NATIVE path:
    // no pix_snap/pix_write helper objects, no patch mutation. Mirrors
    // screenshot_canvas (XGetImage via createSnapshotOfNativeWindow, on the
    // JUCE message thread). DSP untouched — zero dropout.
    // Reply: /pd/gem_capture/reply/<corrId> <absPath | error:...>
void MCPBridge::pdGemCapture(const juce::String& action, const juce::OSCMessage& msg)
{
    auto  correlationId = msg.size() >= 1 ? getArgString(msg[0]) : "0";
    float scale         = msg.size() >= 2 ? static_cast<float>(getArgFloat(msg[1])) : 0.75f;
    scale = juce::jlimit(0.1f, 2.0f, scale);

    postMessage([proc = processor, scale, correlationId, bridge = this]() {
#if ENABLE_GEM
        auto img = proc->getMcp()->gui().captureGemWindow();
        if (!img.isValid()) {
            bridge->sendReply("/pd/gem_capture/reply/" + correlationId, juce::String("error:no_gem_window"));
            return;
        }

        // Downscale for vision token economy (default 0.75)
        if (std::abs(scale - 1.0f) > 0.01f && img.getWidth() > 0 && img.getHeight() > 0) {
            int targetW = juce::roundToInt(img.getWidth() * scale);
            int targetH = juce::roundToInt(img.getHeight() * scale);
            if (targetW > 0 && targetH > 0) {
                juce::Image resized(juce::Image::ARGB, targetW, targetH, true);
                juce::Graphics rg(resized);
                rg.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                rg.drawImage(img, juce::Rectangle<float>(0, 0, (float) targetW, (float) targetH),
                             juce::RectanglePlacement::stretchToFit);
                img = resized;
            }
        }

        auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("plugdata_gem_" + correlationId + ".png");
        juce::FileOutputStream fos(tmpFile);
        if (!fos.openedOk()) {
            bridge->sendReply("/pd/gem_capture/reply/" + correlationId, juce::String("error:file_open"));
            return;
        }
        juce::PNGImageFormat png;
        if (!png.writeImageToStream(img, fos)) {
            bridge->sendReply("/pd/gem_capture/reply/" + correlationId, juce::String("error:png_encode"));
            return;
        }
        fos.flush();
        bridge->sendReply("/pd/gem_capture/reply/" + correlationId, tmpFile.getFullPathName());
#else
        bridge->sendReply("/pd/gem_capture/reply/" + correlationId, juce::String("error:gem_disabled"));
#endif
    });
    return;
}
