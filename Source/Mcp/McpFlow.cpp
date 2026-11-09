/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12a flow mode, the bridge side (A2 ownership read, A3 the switch). See McpFlowMode.h and McpOwnership.h.
//
//   /pd/flow_mode_get   <corrId>               -> /pd/flow_mode_get/reply/<corrId>   {on, sinceMs, bootToken}
//   /pd/flow_mode_off   <corrId> [by]          -> /pd/flow_mode_off/reply/<corrId>   {on:false, wasOn}
//   /pd/ownership       <corrId> <canvas> <id...> -> /pd/ownership/reply/<corrId> {canvasKey, bootToken, ambiguousTab,
//                       visible (P11.9: on screen now), canvasObjects (P11.9: objects on this canvas, comments not counted),
//                       objects:[{id, found, origin, owned, why, class}]} | /pd/ownership/error/<corrId> "<reason>"
//   event /pd/flow_mode/changed <on|off> <by>
//
// No verb turns flow mode ON. P12a.1: the artist's click on Alphi, the robot face in a corner of the canvas
// (McpHost::alphiClicked, called by the canvas click only, McpAlphi.h) switches it on, the next click off.

#include "../Pd/MCPBridgeCmdCommon.h"
#include "Mcp/McpOwnership.h"
#include "../Pd/MCPBridgePdAccessors.h" // slice 3: mcp_last_seq for seqBy

namespace {

std::int64_t flowNowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }

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

bool isGuiClass(char const* cls)
{
    static std::unordered_set<std::string> const guis { "hsl", "vsl", "tgl", "hradio", "vradio", "nbx", "bng", "vu", "cnv", "knob", "gatom" };
    return cls && guis.count(cls) > 0;
}

// Review M1: the edit stamp's GUI part (the properties a hand edit in the properties dialog changes; not the value).
uint64_t guiPropsStamp(t_object* ob, uint64_t h)
{
    static std::unordered_map<std::string, std::string> const guiClasses {
        { "hsl", "hsl" }, { "vsl", "vsl" }, { "tgl", "tgl" }, { "hradio", "hradio" }, { "vradio", "vradio" },
        { "nbx", "nbx" }, { "bng", "bng" }, { "vu", "vu" }, { "cnv", "cnv" }, { "knob", "knob" }, { "gatom", "floatatom" }
    };
    auto const it = guiClasses.find(class_getname(pd_class(&ob->te_g.g_pd)));
    if (it == guiClasses.end())
        return h;
    std::unique_ptr<juce::DynamicObject> props(mcpBuildGuiProps(juce::String(it->second), ob));
    for (char const* key : { "min", "max", "send", "receive", "log", "label", "size", "width", "height" })
        if (props && props->hasProperty(key))
            h = (h ^ static_cast<uint64_t>(std::hash<std::string> {}(key + std::string("=") + props->getProperty(key).toString().toStdString()))) * 1099511628211ull;
    return h;
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
    auto chip = session.approvals.take(nonce, nowMs);
    if (!chip)
        return false;
    if (mcpBridge)
        mcpBridge->sendApprovalAnswer(juce::String(nonce), yes);
    return true;
}

bool McpHost::alphiClicked(std::int64_t nowMs)
{
    // Review L3: no switch while the bridge does not serve (nothing could use it, and nothing could switch it off).
    if (!bridgeRunning())
        return session.flowMode.isOn();
    // P12a.1: one flip per click (a double click flips once).
    if (!alphiClicks.accept(nowMs))
        return session.flowMode.isOn();
    if (!mcp::alphiNextOn(session.flowMode.isOn())) {
        flowModeOff("alphi");
        return false;
    }
    session.flowMode.artistTurnOn(nowMs);
    processor.logMessage("Flow mode ON: the AI changes and deletes its own objects without asking (Ctrl+Z undoes; click Alphi again to stop)");
    if (mcpBridge)
        mcpBridge->sendFlowModeChanged(true, "artist");
    repaintFlowLater(*this);
    return true;
}

bool McpHost::alphiBrake(std::int64_t nowMs)
{
    if (!bridgeRunning())
        return session.flowMode.isOn();
    // The 350 ms double-click guard, as for alphiClicked; a slower second click meets the 600 ms after this off in
    // the canvas's way ON (mcp::alphiClickAllowed).
    if (!alphiClicks.accept(nowMs))
        return session.flowMode.isOn();
    flowModeOff("alphi"); // off already (another door won the race): nothing happens
    return false;
}

bool McpHost::bridgeRunning() const
{
    return mcpBridge && mcpBridge->isRunning();
}

void McpHost::bridgeStopping()
{
    if (session.mcpShuttingDown.load()) {
        session.flowMode.off(); // teardown: no console line, no event, no repaint
        return;
    }
    flowModeOff("bridge_stop");
}

uint64_t McpHost::editStamp(t_gobj* g)
{
    uint64_t h = 1469598103934665603ull;
    auto* ob = g ? pd_checkobject(&g->g_pd) : nullptr;
    if (!ob)
        return h;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uintptr_t>(pd_class(&g->g_pd)));
    // A played value is not authorship: an atom box keeps its value in te_binbuf, so its atoms stay out; an IEM GUI's
    // creation atoms carry its init value, so they stay out too (its properties are read below).
    bool const valueHolder = ob->te_type == T_ATOM || isGuiClass(class_getname(pd_class(&g->g_pd)));
    if (ob->te_binbuf && !valueHolder) {
        int const n = binbuf_getnatom(ob->te_binbuf);
        t_atom const* a = binbuf_getvec(ob->te_binbuf);
        mix(static_cast<uint64_t>(n));
        for (int i = 0; i < n; ++i) {
            uint64_t v = static_cast<uint64_t>(a[i].a_type) << 56;
            if (a[i].a_type == A_FLOAT) {
                uint32_t bits = 0;
                std::memcpy(&bits, &a[i].a_w.w_float, sizeof(bits));
                v ^= bits;
            } else if (a[i].a_type == A_SYMBOL || a[i].a_type == A_DOLLSYM) {
                v ^= reinterpret_cast<uintptr_t>(a[i].a_w.w_symbol);
            } else if (a[i].a_type == A_DOLLAR) {
                v ^= static_cast<uint64_t>(a[i].a_w.w_index);
            }
            mix(v);
        }
    }
    return guiPropsStamp(ob, h);
}

void McpHost::restampAfterAiChange(t_gobj* g, bool wasIntact)
{
    if (g && wasIntact && identity.originOf(g) == mcp::IdentityService::Origin::Ai)
        identity.setOriginAi(g, editStamp(g));
}

void McpHost::noteAiGuiValue(t_gobj* g)
{
    t_object* ob = g ? pd_checkobject(&g->g_pd) : nullptr;
    double value = 0.0;
    if (!ob || !mcpGuiValue(ob, value)) return;
    auto& map = session.aiGuiValues;
    if (map.size() > 4096) // dead entries only; a live patch never holds this many AI-set GUIs
        for (auto it = map.begin(); it != map.end();)
            it = (it->second.alive && it->second.alive()) ? std::next(it) : map.erase(it);
    pd::WeakReference ref(g, &processor);
    map[g] = { [ref] { return ref.isValid(); }, value };
}

std::optional<double> McpHost::aiGuiValue(t_gobj* g)
{
    auto const it = session.aiGuiValues.find(g);
    if (it == session.aiGuiValues.end()) return std::nullopt;
    if (!it->second.alive || !it->second.alive()) {
        session.aiGuiValues.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

void McpHost::noteAiSeqRange(unsigned int before, unsigned int after)
{
    if (before == after) return; // the dispatch delivered nothing that was recorded
    auto& ring = session.aiSeqRanges;
    if (session.aiSeqCount > 0) {
        auto& newest = ring[(session.aiSeqHead + McpSession::kAiSeqRanges - 1) % McpSession::kAiSeqRanges];
        if (newest.after == before) { // nothing was recorded between the two dispatches: one range
            newest.after = after;
            return;
        }
    }
    if (session.aiSeqCount == McpSession::kAiSeqRanges) session.aiSeqFull = true; // the oldest is overwritten now
    ring[session.aiSeqHead] = { before, after };
    session.aiSeqHead = (session.aiSeqHead + 1) % McpSession::kAiSeqRanges;
    session.aiSeqCount = std::min(session.aiSeqCount + 1, McpSession::kAiSeqRanges);
}

std::optional<juce::String> McpHost::seqBy(unsigned int seq) const
{
    if (seq == 0) return std::nullopt;
    for (size_t i = 0; i < session.aiSeqCount; ++i) {
        auto const& r = session.aiSeqRanges[i];
        // (before, after] with wrap-around: seq - before in [1, after - before].
        unsigned int const span = r.after - r.before;
        unsigned int const at = seq - r.before;
        if (at >= 1 && at <= span) return juce::String("ai");
    }
    if (session.aiSeqFull) {
        // The oldest kept range is the one the head overwrites next. A seq at or before its start may have been
        // covered by a dropped range: not known. Ages are taken from the counter now (wrap-safe up to 2^31).
        auto const& oldest = session.aiSeqRanges[session.aiSeqHead];
        unsigned int const now = mcp_last_seq();
        if (now - seq >= now - oldest.before) return std::nullopt;
    }
    return juce::String("other");
}

uint64_t McpHost::consoleSeqNow()
{
    std::lock_guard<std::mutex> const lock(session.consoleLock);
    return session.consoleSeq;
}

std::vector<McpHost::ConsoleHit> McpHost::consoleSince(uint64_t seq)
{
    std::vector<ConsoleHit> out;
    std::lock_guard<std::mutex> const lock(session.consoleLock);
    for (auto it = session.consoleLines.rbegin(); it != session.consoleLines.rend() && it->seq > seq; ++it)
        out.insert(out.begin(), { it->object, it->tempId, it->text, it->error });
    return out;
}

std::vector<juce::String> McpHost::consoleLinesSince(uint64_t seq, void const* object)
{
    std::vector<juce::String> out;
    if (!object) return out;
    for (auto const& hit : consoleSince(seq))
        if (hit.object == object) out.push_back(hit.text);
    return out;
}

bool McpHost::aiIntact(t_gobj* g)
{
    if (!g || identity.originOf(g) != mcp::IdentityService::Origin::Ai)
        return false;
    if (identity.originStampOf(g) == editStamp(g))
        return true;
    // Seen edited once: the artist's for good (sticky), so a later AI set that happens to restore the old stamp never
    // makes it the AI's again.
    identity.clearOrigin(g);
    return false;
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
    // P11.9 item 1: what the artist's screen shows, read on the message thread BEFORE the Pd lock (never wait for the
    // message thread under it). No answer in time (a stalled GUI), no editor: nothing is on screen, so nothing is free.
    auto const views = onMessageThread([this] { return gui().visibleViews(); }, 1000, "ownership");
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
    // P11.9 item 1: this canvas is a split view of a showing, not minimised editor window right now (flow frees
    // nothing elsewhere). Coordinator decision (P11.9): the canvas's own object count, for the proportional brake.
    rootObj->setProperty("visible", views.has_value() && mcp::visible::onScreen(cnv, *views));
    int canvasObjects = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        // Review M2: comments are not counted (padding a canvas with comments must not loosen the brake).
        auto* ob = pd_checkobject(&y->g_pd);
        if (!(ob && ob->te_type == T_TEXT))
            ++canvasObjects;
    }
    rootObj->setProperty("canvasObjects", canvasObjects);
    // Review L3: ONE walk of the wires per call. Review M1: an end that is not the AI's intact box makes the other
    // end ask (in-wire and out-wire alike). intact() is cached per object (the stamp reads GUI properties).
    std::unordered_map<t_gobj*, bool> intactCache;
    auto intact = [&](t_gobj* g) {
        auto it = intactCache.find(g);
        if (it != intactCache.end()) return it->second;
        bool const v = aiIntact(g);
        intactCache.emplace(g, v);
        return v;
    };
    std::unordered_set<t_gobj*> foreignIn, foreignOut;
    {
        t_linetraverser t;
        linetraverser_start(&t, cnv);
        while (linetraverser_next_nosize(&t)) {
            auto* src = &t.tr_ob->te_g;
            auto* dst = &t.tr_ob2->te_g;
            if (!intact(src)) foreignIn.insert(dst);
            if (!intact(dst)) foreignOut.insert(src);
        }
    }
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
            f.editedInPlace = f.originAi && !intact(g);
            t_class* cl = pd_class(&g->g_pd);
            juce::String const cls = juce::String::fromUTF8(class_getname(cl));
            row->setProperty("class", cls);
            f.container = cl == canvas_class || cl == clone_class;
            f.dataHolder = cl == garray_class || isDataHolder(cls.toStdString(), textOf(g));
            f.nonAiInWire = foreignIn.count(g) > 0;
            f.nonAiOutWire = foreignOut.count(g) > 0;
        }
        auto const why = decide(f);
        row->setProperty("found", f.found);
        row->setProperty("origin", f.originAi && !f.editedInPlace ? "ai" : "unknown"); // edited in place: the artist's
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
