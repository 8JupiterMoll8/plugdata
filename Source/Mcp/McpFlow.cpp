/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12a flow mode, the bridge side (A2 ownership read, A3 the switch). See McpFlowMode.h and McpOwnership.h.
//
//   /pd/flow_mode_get   <corrId>               -> /pd/flow_mode_get/reply/<corrId>   {on, sinceMs, bootToken}
//   /pd/flow_mode_off   <corrId> [by]          -> /pd/flow_mode_off/reply/<corrId>   {on:false, wasOn}
//   /pd/flow_mode_offer <corrId>               -> /pd/flow_mode_offer/reply/<corrId> {shown, on}
//   /pd/ownership       <corrId> <canvas> <id...> -> /pd/ownership/reply/<corrId> {canvasKey, bootToken, ambiguousTab,
//                       objects:[{id, found, origin, owned, why, class}]} | /pd/ownership/error/<corrId> "<reason>"
//   event /pd/flow_mode/changed <on|off> <by>
//
// No verb turns flow mode ON. The offer only shows a chip with fixed words; the artist's ✓ on it
// (McpHost::answerApprovalChip, called by the canvas input only) is what switches it on.

#include "../Pd/MCPBridgeCmdCommon.h"
#include "Mcp/McpOwnership.h"

namespace {

std::int64_t flowNowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }

constexpr std::int64_t kFlowOfferTtlMs = 60000;
constexpr int kOwnershipMaxIds = 256;

bool onCanvas(t_canvas* c, t_gobj* g)
{
    for (t_gobj* y = c ? c->gl_list : nullptr; y; y = y->g_next)
        if (y == g)
            return true;
    return false;
}

std::string textOf(t_gobj* g)
{
    auto* ob = pd_checkobject(&g->g_pd);
    if (!ob || !ob->te_binbuf)
        return {};
    char* text = nullptr;
    int len = 0;
    mcpGetObjectText(ob, &text, &len);
    std::string out;
    if (text && len > 0) {
        out.assign(text, static_cast<size_t>(len));
        freebytes(text, len);
    }
    return out;
}

// Repaint every canvas on the message thread (the badge and the chips), whichever thread changed the state.
void repaintFlowLater(McpHost& host)
{
    juce::MessageManager::callAsync([weak = juce::WeakReference<McpHost>(&host)] {
        if (auto* h = weak.get())
            h->gui().repaintAll();
    });
}

} // namespace

// ── McpHost ──────────────────────────────────────────────────────────────────────────────────────────────

bool McpHost::answerApprovalChip(std::string const& nonce, bool yes, std::int64_t nowMs)
{
    if (nonce.empty())
        return false;
    auto& state = session.approvals;
    if (mcp::isFlowNonce(nonce)) {
        // The flow offer: ✓ only once its chip has been fully on screen for the click delay (as every chip's click);
        // Esc / ✗ just takes it away (flow mode stays as it is).
        if (yes && !state.clickAllowed(nonce, nowMs))
            return false;
        auto chip = state.take(nonce, nowMs);
        if (!chip)
            return false;
        if (yes && chip->flowOffer) {
            bool const was = session.flowMode.isOn();
            session.flowMode.artistTurnOn(nowMs);
            if (!was) {
                processor.logMessage("Flow mode ON: the AI changes and deletes its own objects without asking (Ctrl+Z undoes; click FLOW to switch off)");
                if (mcpBridge)
                    mcpBridge->sendFlowModeChanged(true, "artist");
            }
        }
        repaintFlowLater(*this);
        return true;
    }
    auto chip = state.take(nonce, nowMs);
    if (!chip)
        return false;
    if (mcpBridge)
        mcpBridge->sendApprovalAnswer(juce::String(nonce), yes);
    return true;
}

bool McpHost::offerFlowMode(std::int64_t nowMs)
{
    if (session.flowMode.isOn())
        return false;
    auto& state = session.approvals;
    for (auto const& c : state.visible(nowMs))
        if (mcp::isFlowNonce(c.nonce))
            state.clear(c.nonce); // one offer at a time
    auto& rng = juce::Random::getSystemRandom();
    mcp::ApprovalChip chip;
    chip.nonce = mcp::makeFlowNonce(static_cast<std::uint64_t>(rng.nextInt64()), static_cast<std::uint64_t>(rng.nextInt64()));
    chip.flowOffer = true;
    chip.canvas = "session";
    chip.reason = "flow mode";
    chip.title = "FLOW MODE ON? The AI changes its own objects without asking";
    chip.detail = "your objects, tables and subpatches still ask \xc2\xb7 Ctrl+Z undoes \xc2\xb7 click FLOW to stop";
    bool const shown = state.show(std::move(chip), kFlowOfferTtlMs, nowMs);
    if (shown)
        repaintFlowLater(*this);
    return shown;
}

bool McpHost::flowModeOff(juce::String const& by)
{
    bool const was = session.flowMode.off();
    if (was) {
        processor.logMessage("Flow mode OFF (" + by + "): the AI asks again before deleting");
        if (mcpBridge)
            mcpBridge->sendFlowModeChanged(false, by);
        repaintFlowLater(*this);
    }
    return was;
}

juce::String McpHost::flowModeJson()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("on", session.flowMode.isOn());
    o->setProperty("sinceMs", static_cast<juce::int64>(session.flowMode.sinceMs()));
    o->setProperty("bootToken", mcpBridge ? mcpBridge->getBootToken() : juce::String());
    return juce::JSON::toString(juce::var(o), true);
}

std::optional<juce::String> McpHost::ownershipJson(juce::String const& canvasName, juce::StringArray const& ids, juce::String& error)
{
    using namespace mcp::ownership;
    registerOpenCanvases(); // records and keys for every open canvas (takes the Pd lock itself)
    PdLockScope lock(processor);
    t_canvas* cnv = getCanvasBySymbol(canvasName);
    if (!cnv) {
        error = "canvas not found: " + canvasName;
        return std::nullopt;
    }
    // Ambiguous tab: the name as given (no "#n") matches more than one open root tab.
    bool ambiguous = false;
    if (!canvasName.containsChar('#')) {
        t_canvas* root = canvas_getrootfor(cnv);
        auto const base = identityBaseKey(root);
        int same = 0;
        for (t_canvas* r = pd_getcanvaslist(); r; r = r->gl_next)
            if (!r->gl_owner && identityBaseKey(r) == base)
                ++same;
        ambiguous = same > 1;
    }
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("canvasKey", juce::String(identity.keyOf(cnv)));
    rootObj->setProperty("bootToken", mcpBridge ? mcpBridge->getBootToken() : juce::String());
    rootObj->setProperty("ambiguousTab", ambiguous);
    juce::Array<juce::var> rows;
    for (int i = 0; i < ids.size() && i < kOwnershipMaxIds; ++i) {
        auto const id = ids[i];
        auto* row = new juce::DynamicObject();
        row->setProperty("id", id);
        Facts f;
        f.ambiguousTab = ambiguous;
        t_gobj* g = identity.resolve(cnv, id.toStdString());
        if (g && !onCanvas(cnv, g))
            g = nullptr;
        if (g) {
            f.found = true;
            f.originAi = identity.originOf(g) == mcp::IdentityService::Origin::Ai;
            t_class* cl = pd_class(&g->g_pd);
            juce::String const cls = juce::String::fromUTF8(class_getname(cl));
            row->setProperty("class", cls);
            f.container = cl == canvas_class || cl == clone_class;
            f.dataHolder = cl == garray_class || isDataHolder(cls.toStdString(), textOf(g));
            if (auto* target = pd_checkobject(&g->g_pd)) {
                t_linetraverser t;
                linetraverser_start(&t, cnv);
                while (linetraverser_next_nosize(&t)) {
                    if (t.tr_ob2 != target)
                        continue;
                    auto* src = &t.tr_ob->te_g;
                    if (identity.originOf(src) != mcp::IdentityService::Origin::Ai) {
                        f.nonAiInWire = true;
                        break;
                    }
                }
            }
        }
        auto const why = decide(f);
        row->setProperty("found", f.found);
        row->setProperty("origin", f.originAi ? "ai" : "unknown");
        row->setProperty("isContainer", f.container);
        row->setProperty("isDataHolder", f.dataHolder);
        row->setProperty("owned", why == Why::AiOwned);
        row->setProperty("why", juce::String(whyName(why)));
        rows.add(juce::var(row));
    }
    rootObj->setProperty("truncated", ids.size() > kOwnershipMaxIds);
    rootObj->setProperty("objects", rows);
    return juce::JSON::toString(juce::var(rootObj), true);
}

// ── MCPBridge handlers ───────────────────────────────────────────────────────────────────────────────────

void MCPBridge::pdFlowModeGet(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!processor) return;
    // Every argument after the corrId is ignored: a read never changes the switch.
    sendReply("/pd/flow_mode_get/reply/" + correlationId, processor->getMcp()->flowModeJson());
}

void MCPBridge::pdFlowModeOff(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!processor) return;
    // Off is the only direction this verb knows, whatever else it is sent (an "on" argument is ignored).
    auto const by = msg.size() > 1 ? getArgString(msg[1]).substring(0, 32).retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789_-") : juce::String();
    bool const was = processor->getMcp()->flowModeOff(by.isEmpty() ? juce::String("server") : by);
    auto* o = new juce::DynamicObject();
    o->setProperty("on", false);
    o->setProperty("wasOn", was);
    sendReply("/pd/flow_mode_off/reply/" + correlationId, juce::JSON::toString(juce::var(o), true));
}

void MCPBridge::pdFlowModeOffer(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!processor) return;
    // The chip's words are fixed; no argument reaches it. The reply never carries its nonce.
    bool const shown = processor->getMcp()->offerFlowMode(flowNowMs());
    auto* o = new juce::DynamicObject();
    o->setProperty("shown", shown);
    o->setProperty("on", processor->getMcp()->session.flowMode.isOn());
    sendReply("/pd/flow_mode_offer/reply/" + correlationId, juce::JSON::toString(juce::var(o), true));
}

void MCPBridge::pdOwnership(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 1) return;
    auto const correlationId = getArgString(msg[0]);
    if (msg.size() < 2) {
        sendReply("/pd/ownership/error/" + correlationId, juce::String("usage: /pd/ownership <corrId> <canvas> <id...>"));
        return;
    }
    auto const canvas = normalizeCanvas(getArgString(msg[1]));
    juce::StringArray ids;
    for (int i = 2; i < msg.size(); ++i)
        ids.add(getArgString(msg[i]));
    juce::String error;
    if (auto json = processor->getMcp()->ownershipJson(canvas, ids, error))
        sendReply("/pd/ownership/reply/" + correlationId, *json);
    else
        sendReply("/pd/ownership/error/" + correlationId, error);
}

void MCPBridge::sendFlowModeChanged(bool on, const juce::String& by)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/flow_mode/changed") };
    msg.addArgument(juce::String(on ? "on" : "off"));
    msg.addArgument(by);
    sender.send(msg);
}
