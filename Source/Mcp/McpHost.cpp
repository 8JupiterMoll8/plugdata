/*
 // The MCP host (Plan 3 P3.2 S4). The functions below were PluginProcessor members;
 // moved verbatim, with `session.` / `processor.` prefixes. See Source/Mcp/McpHost.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cstring>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Mcp/McpGuiFields.h" // review M2: GUI send/receive/range into the content stamp
#include "Utility/SettingsFile.h"
#include "Canvas.h"
#include "PluginEditor.h"
#include "Utility/Config.h"
#include "Mcp/McpAudit.h"
#include "Mcp/McpRtGuard.h"
#include "Mcp/McpNativeUndo.h"

extern "C" int ugen_getsortno(void); // d_ugen.c (P3.4 S5)

McpHost::McpHost(PluginProcessor& p)
    : processor(p)
    , guiHost(createMcpGuiHost(p))
{
    sysTable(); // a bad selector table fails here, at startup
    if (mcp::audit::enabled())
        identity.setAccessCheck(&mcp::audit::checkIdentityAccess); // P3.5 S2: report access without the Pd lock
    // P3.5 S3: identity records are per live canvas. The liveness check is a pd::WeakReference: plugdata's Pd
    // clears it when the canvas is freed, so a record never outlives its canvas (nor passes to a new canvas at
    // the same address).
    mcp::IdentityService::Hooks hooks;
    hooks.baseKey = [](t_canvas* c) { return identityBaseKey(c).toStdString(); };
    hooks.liveness = [this](t_canvas* c) {
        pd::WeakReference ref(c, &processor);
        return std::function<bool()>([ref] { return ref.isValid(); });
    };
    // S4: the theft check. An id held by an object that is no longer in its canvas may be taken over.
    hooks.objectLive = [](t_canvas* c, t_gobj* g) {
        for (t_gobj* y = c ? c->gl_list : nullptr; y; y = y->g_next)
            if (y == g) return true;
        return false;
    };
    // S5: a weak reference per bound object, so an id never resolves to another object at a reused address.
    hooks.objectGuard = [this](t_gobj* g) {
        pd::WeakReference ref(g, &processor);
        return std::function<bool()>([ref] { return ref.isValid(); });
    };
    identity.setHooks(std::move(hooks));
}

// P3.5 S4: the prefix of a minted id (owner decision O1: type + number): an abstraction's file name, "pd" for
// a subpatch, "table" for an array, else the class; sanitized ("osc~" -> "osc_sig", "plate.rev.m~" -> "plate_rev").
juce::String McpHost::mintPrefix(t_gobj* g)
{
    if (!g)
        return "obj";
    t_class* cl = pd_class(&g->g_pd);
    char const* clName = class_getname(cl);
    juce::String type = clName ? juce::String::fromUTF8(clName) : juce::String("obj");
    if (cl == canvas_class) {
        juce::String abstractName;
        pd::getAbstractionFileName(g, abstractName);
        type = abstractName.isNotEmpty() ? abstractName : juce::String("pd");
    } else if (cl == garray_class) {
        type = "table";
    }
    return pd::sanitizeClassNameForTempId(type);
}

// P3.5 S3: a canvas's wire key before any "#n": a root is its file name, a subcanvas "pd-<name>", an unnamed
// canvas "pd-anon" (the old canonicalCanvasKey used the pointer there, which survived address reuse).
juce::String McpHost::identityBaseKey(t_canvas* cnv)
{
    if (!cnv)
        return {};
    juce::String const name = cnv->gl_name && cnv->gl_name->s_name ? juce::String::fromUTF8(cnv->gl_name->s_name) : juce::String();
    if (cnv->gl_owner != nullptr) {
        if (name.startsWith("pd-")) return name;
        if (name.isNotEmpty()) return "pd-" + name;
        return "pd-anon";
    }
    if (name.isNotEmpty() && name != "main" && name != "pd-main")
        return name;
    return "pd-anon";
}

void McpHost::registerOpenCanvases()
{
    PdLockScope lock(processor);
    if (!pd_this)
        return;
    identity.prune();
    // S6: one walk also takes each canvas's structure stamp (object count + FNV-1a of the object pointers): a
    // GUI create/delete/retype changes it, and IdentityCore::observeStamp bumps that canvas's version, so
    // analyze_patch(sync) sees a manual edit on its next identity_version (S0: 0.04-0.13 us per 37 objects).
    // P11.5 V23: the same walk takes each canvas's CONTENT stamp: the structure plus every object's x/y and text atoms
    // and the canvas's wires (linetraverser), with the content stamps of the canvases inside it folded in, so a hand
    // edit anywhere below a tab changes that tab's contentVersion (not its identity version).
    bool edited = false;
    auto mix = [](uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; };
    // Review M2: a GUI's send/receive names and range live outside its text atoms (a hand change in the properties
    // dialog edits them only); they go into the stamp through the McpGuiFields readers. The value does not (a moved
    // slider is play, not an edit).
    static std::unordered_map<std::string, std::string> const guiClasses {
        { "hsl", "hsl" }, { "vsl", "vsl" }, { "tgl", "tgl" }, { "hradio", "hradio" }, { "vradio", "vradio" },
        { "nbx", "nbx" }, { "bng", "bng" }, { "vu", "vu" }, { "cnv", "cnv" }, { "knob", "knob" }, { "gatom", "floatatom" }
    };
    auto guiStamp = [&](t_object* ob, uint64_t h) {
        auto const it = guiClasses.find(class_getname(pd_class(&ob->te_g.g_pd)));
        if (it == guiClasses.end())
            return h;
        std::unique_ptr<juce::DynamicObject> props(mcpBuildGuiProps(juce::String(it->second), ob));
        for (char const* key : { "min", "max", "send", "receive", "log" })
            if (props->hasProperty(key))
                h = mix(h, static_cast<uint64_t>(std::hash<std::string> {}(key + std::string("=") + props->getProperty(key).toString().toStdString())));
        return h;
    };
    std::function<uint64_t(t_canvas*)> walk = [&](t_canvas* c) -> uint64_t {
        identity.canvasMap(c);
        identity.refreshKey(c); // review M3: renamed behind our back (GUI Save-As, subpatch rename)
        uint64_t stamp = 1469598103934665603ull;
        uint64_t content = 1469598103934665603ull;
        uint64_t count = 0;
        // Review M2: the graph-on-parent flags and box of this canvas
        content = mix(content, static_cast<uint64_t>(c->gl_isgraph) | static_cast<uint64_t>(c->gl_hidetext) << 1);
        content = mix(content, static_cast<uint64_t>(static_cast<uint32_t>(c->gl_pixwidth)) << 32 | static_cast<uint32_t>(c->gl_pixheight));
        content = mix(content, static_cast<uint64_t>(static_cast<uint32_t>(c->gl_xmargin)) << 32 | static_cast<uint32_t>(c->gl_ymargin));
        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++count) {
            stamp = (stamp ^ reinterpret_cast<uintptr_t>(g)) * 1099511628211ull;
            content = mix(content, reinterpret_cast<uintptr_t>(g));
            if (t_object* gui = pd_checkobject(&g->g_pd))
                content = guiStamp(gui, content);
            if (t_object* ob = pd_checkobject(&g->g_pd)) {
                content = mix(content, static_cast<uint32_t>(ob->te_xpix) | (static_cast<uint64_t>(static_cast<uint32_t>(ob->te_ypix)) << 32));
                if (ob->te_binbuf) {
                    int const n = binbuf_getnatom(ob->te_binbuf);
                    t_atom const* a = binbuf_getvec(ob->te_binbuf);
                    content = mix(content, static_cast<uint64_t>(n));
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
                        content = mix(content, v);
                    }
                }
            }
            if (pd_class(&g->g_pd) == canvas_class)
                content = mix(content, walk(reinterpret_cast<t_canvas*>(g)));
        }
        t_linetraverser t;
        linetraverser_start(&t, c);
        while (linetraverser_next_nosize(&t)) {
            content = mix(content, reinterpret_cast<uintptr_t>(t.tr_ob));
            content = mix(content, reinterpret_cast<uintptr_t>(t.tr_ob2));
            content = mix(content, static_cast<uint64_t>(t.tr_outno) << 32 | static_cast<uint32_t>(t.tr_inno));
        }
        edited = identity.observeStamp(c, stamp ^ (count << 40)) || edited;
        identity.observeContent(c, content);
        return content;
    };
    std::vector<t_canvas*> roots; // pd_canvaslist is newest first: walk it oldest first
    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next)
        roots.push_back(c);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        walk(*it);
    if (edited)
        identity.prune(); // drop the entries of objects deleted by hand
    identity.expireTombstones(); // review M2
    rebindTombstones();
}

// P3.5 S5 (L9): before an abstraction reload (a save of that abstraction, or encapsulate_to_file) every id bound to
// one of its instances leaves a tombstone {id, index, file}; the reload recreates the instance at the same index
// (glist_doreload restores it, P3.5 S0 read 2).
void McpHost::tombstoneAbstractionInstances(juce::String const& fileName)
{
    PdLockScope lock(processor);
    if (!pd_this || fileName.isEmpty())
        return;
    std::function<void(t_canvas*)> walk = [&](t_canvas* c) {
        int index = 0;
        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
            if (pd_class(&g->g_pd) != canvas_class)
                continue;
            juce::String absName;
            pd::getAbstractionFileName(g, absName);
            if (absName == fileName) {
                auto const id = identity.idOf(g);
                if (!id.empty())
                    identity.addTombstone(c, id, index, absName.toStdString());
            }
            walk(reinterpret_cast<t_canvas*>(g));
        }
    };
    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next)
        walk(c);
}

// ... and afterwards each such id is bound to the object at that index if it is an instance of the same file.
void McpHost::rebindTombstones()
{
    PdLockScope lock(processor);
    if (!identity.anyTombstones())
        return;
    std::function<void(t_canvas*)> walk = [&](t_canvas* c) {
        for (auto const& stone : identity.tombstones(c)) {
            if (identity.valid(c, stone.id)) {
                // Review M1: plugdata's Ctrl+S reloads AFTER a callAsync: a box still alive right after the save may
                // die a moment later. Only a stone older than 5 s whose box still lives is done with.
                if (mcp::IdentityService::nowMs() > stone.bornMs + 5000)
                    identity.dropTombstone(c, stone.id);
                continue;
            }
            int index = 0;
            for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
                if (index != stone.index)
                    continue;
                juce::String absName;
                if (pd_class(&g->g_pd) == canvas_class)
                    pd::getAbstractionFileName(g, absName);
                if (absName.toStdString() == stone.tag && identity.idOf(g).empty()) {
                    identity.bind(c, stone.id, g);
                    identity.dropTombstone(c, stone.id);
                }
                break;
            }
        }
        for (t_gobj* g = c->gl_list; g; g = g->g_next)
            if (pd_class(&g->g_pd) == canvas_class)
                walk(reinterpret_cast<t_canvas*>(g));
    };
    for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
        walk(c);
}

std::vector<McpHost::TabTokenRow> McpHost::assignTabTokens(std::vector<McpGuiHost::TabInfo> const& tabs)
{
    // Oldest tab first: a root's age is its place in pd_canvaslist (newest first); other tabs go last.
    std::vector<size_t> order(tabs.size());
    std::vector<juce::String> keys(tabs.size());
    {
        PdLockScope lock(processor);
        std::map<t_canvas*, int> age;
        int n = 0;
        for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
            age[c] = n++;
        std::vector<int> ageOf(tabs.size());
        for (size_t i = 0; i < tabs.size(); ++i) {
            order[i] = i;
            auto const it = age.find(tabs[i].patch);
            ageOf[i] = it == age.end() ? -1 : n - it->second; // larger = older
            keys[i] = tabs[i].patch ? MCPBridge::canonicalCanvasKey(tabs[i].patch) : tabs[i].title;
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ageOf[a] > ageOf[b]; });
    }
    auto sidecarOf = [](juce::File const& pd) { return juce::File(pd.getFullPathName() + ".mcpids.json"); };
    std::vector<mcp::TabTokens::Tab> in;
    for (auto const i : order) {
        std::string persisted;
        if (tabs[i].file.existsAsFile()) {
            auto const json = juce::JSON::parse(sidecarOf(tabs[i].file).loadFileAsString());
            if (json.isObject() && json["ns"].isString())
                persisted = json["ns"].toString().toStdString();
        }
        in.push_back({ tabs[i].patch, keys[i].toStdString(), persisted });
    }
    std::vector<TabTokenRow> rows(tabs.size());
    std::lock_guard<std::mutex> guard(tabTokenLock);
    auto const assigned = tabTokens.assign(in);
    tabTokenNonDefault.clear();
    for (size_t k = 0; k < order.size() && k < assigned.size(); ++k) {
        auto const i = order[k];
        auto const& a = assigned[k];
        rows[i] = { juce::String(a.token), a.collided, a.fromSidecar, a.overridden, juce::String(a.persisted) };
        // Kept for the tab's next save (buildIdentitySidecar); nothing is written here (review M4).
        if (a.token != mcp::defaultTabToken(in[k].key))
            tabTokenNonDefault[tabs[i].patch] = juce::String(a.token);
    }
    return rows;
}

juce::String McpHost::tabTokenToPersist(t_canvas* root)
{
    std::lock_guard<std::mutex> guard(tabTokenLock);
    auto const it = tabTokenNonDefault.find(root);
    return it == tabTokenNonDefault.end() ? juce::String() : it->second;
}

McpHost::PdLockScope::PdLockScope(PluginProcessor& p)
{
    p.setThis();
    sys_lock();
}

McpHost::PdLockScope::~PdLockScope()
{
    sys_unlock();
}

McpHost::~McpHost()
{
    shutdown();
}

void McpHost::postMessage(std::function<void()> fn)
{
    juce::MessageManager::callAsync([weak = juce::WeakReference<McpHost>(this), fn = std::move(fn), work = mcp::v2::captureWork()] {
        mcp::v2::WorkScope const scope(work); // P4.1 S3: replies stay with the request that posted this; it is done after
        if (weak.get() != nullptr)
            fn();
    });
}

void McpHost::shutdown()
{
    if (session.mcpShuttingDown.exchange(true))
        return;
    focusTimer.stopTimer();
    ceilingTimer.stopTimer();
    if (mcpBridge)
        mcpBridge->shutdown();
    std::function<void()> job;
    while (session.mcpPdQueue.try_dequeue(job))
        job = nullptr; // never run: the patches they would touch are about to go
}

// Audio-device xruns since the device opened; -1 when unknown (no device, a
// plugin build, or a backend that does not count them). P3.4 S0: lets live
// gates prove "zero dropouts" by measurement. Reads the device manager only.
int McpHost::xrunCount() const
{
    if (auto* dm = ProjectInfo::getDeviceManager())
        if (auto* dev = dm->getCurrentAudioDevice())
            return dev->getXRunCount();
    return -1;
}

// P9 round 3: the console rate limit runs on a steady clock (not the wall clock, which can jump).
static int64_t steadyMs()
{
    return static_cast<int64_t>(juce::Time::getMillisecondCounterHiRes());
}

void McpHost::createBridge()
{
    auto const mcpEnabled = processor.settingsFile->getProperty<bool>("mcp_bridge_enabled");
    auto const mcpListen = processor.settingsFile->getProperty<int>("mcp_bridge_listen_port");
    auto const mcpSend = processor.settingsFile->getProperty<int>("mcp_bridge_send_port");
    // P9: the output ceiling's switch, read once here (startup) and mirrored into the session; missing key = on,
    // only a literal false = off (round 1). Its breaker's trips and releases go to the console from a timer.
    session.outputCeiling.setStandalone(ProjectInfo::isStandalone);
    applyOutputCeilingSettingAtStartup();
    auto const ceilingNow = session.outputCeiling.read();
    ceilingConsole.start(ceilingNow.breakerTrips, ceilingNow.breakerReleases, steadyMs());
    ceilingTimer.startTimer(250);
    // P3.4 S0: with PLUGDATA_MCP_AUDIT=1, time every Pd-lock hold (no-op otherwise).
    processor.setThis();
    mcp::audit::installLock(&processor.audioLock);
    mcpBridge = std::make_unique<MCPBridge>(&processor, mcpListen, mcpSend);
    focusTimer.startTimer(50); // P3.4 S3: publishes the focused root for getCanvasBySymbol("main")
    if (!mcpEnabled) {
        mcpBridge->stop();
    }
}

// P9: the settings panel's toggle. The only way the ceiling goes off while PlugData runs (the AI has no verb for it,
// a patch message is not a setting, and a settings-file edit is ignored by mcpSettingsChanged); on also resets the
// breaker (the owner's reset).
void McpHost::setOutputCeilingFromGui(bool const on)
{
    session.outputCeiling.setEnabled(on);
    if (on)
        session.outputCeiling.requestBreakerReset();
    processor.logMessage(on ? "Output ceiling on: the output is held at 0 dBFS"
                            : "Output ceiling switched OFF in the settings: overs and NaN reach the audio device");
}

// P9: {"active":bool,"setting":"on"|"off","reason"?:..,"clampedSamples":n,"nonFiniteSamples":n,"clampedBlocks":n,
// "breakerTrips":n,"breakerReleases":n,"startedAtMs":n,"breakerEngaged":bool,"lastClampMs":n|null}; startedAtMs = the
// wall-clock ms this process's ceiling started (the server detects a restart by it); lastClampMs = ms since the last block the ceiling
// touched. Atomics only: any thread.
// P9 round 1: only a literal JSON `false` is off. "off", 0, null, a string: on (a malformed value never disarms it).
bool McpHost::ceilingSettingIsOn(juce::var const& value)
{
    return !(value.isBool() && !static_cast<bool>(value));
}

void McpHost::applyOutputCeilingSettingAtStartup()
{
    auto const value = processor.settingsFile->getPropertyAsValue("mcp_bridge_ai_ceiling").getValue();
    bool const on = ceilingSettingIsOn(value);
    session.outputCeiling.setEnabled(on);
    if (!value.isBool())
        processor.settingsFile->setProperty("mcp_bridge_ai_ceiling", true); // the file and the panel agree with it
    if (!on && session.outputCeiling.isStandalone()) // set from ProjectInfo::isStandalone in createBridge
        processor.logError("OUTPUT CEILING IS OFF (Settings > Advanced > MCP bridge): overs, NaN and sustained "
                           "full-scale sound reach your speakers unless PlugData's limiter catches them");
}

void McpHost::pollOutputCeiling()
{
    auto const t = session.outputCeiling.read();
    for (auto const& line : ceilingConsole.poll(t.breakerTrips, t.breakerReleases, steadyMs())) {
        if (line.find("released") != std::string::npos)
            processor.logMessage(juce::String(line));
        else
            processor.logWarning(juce::String(line));
    }
}

juce::String McpHost::outputCeilingJson() const
{
    auto const& ceiling = session.outputCeiling;
    auto const t = ceiling.read();
    bool const standalone = ceiling.isStandalone();
    bool const on = ceiling.isEnabled();
    juce::String json = juce::String("{\"active\":") + (standalone && on ? "true" : "false")
        + ",\"setting\":\"" + (on ? "on" : "off") + "\"";
    if (!standalone)
        json << ",\"reason\":\"plugin\"";
    else if (!on)
        json << ",\"reason\":\"setting_off\"";
    json << ",\"clampedSamples\":" << juce::String(static_cast<juce::uint64>(t.clampedSamples))
         << ",\"nonFiniteSamples\":" << juce::String(static_cast<juce::uint64>(t.nonFiniteSamples))
         << ",\"clampedBlocks\":" << juce::String(static_cast<juce::uint64>(t.clampedBlocks))
         << ",\"breakerTrips\":" << juce::String(static_cast<juce::uint64>(t.breakerTrips))
         << ",\"breakerReleases\":" << juce::String(static_cast<juce::uint64>(t.breakerReleases))
         << ",\"startedAtMs\":" << juce::String(static_cast<juce::int64>(t.startedAtMs))
         << ",\"breakerEngaged\":" << (t.breakerEngaged ? "true" : "false") << ",\"lastClampMs\":";
    if (t.lastClampMs < 0)
        json << "null";
    else
        json << juce::String(static_cast<juce::uint32>(juce::Time::getMillisecondCounter() - static_cast<juce::uint32>(t.lastClampMs)));
    return json + "}";
}

juce::String McpHost::getMcpBridgeStatus() const
{
    return mcpBridge ? mcpBridge->getStatus() : juce::String("unavailable");
}

// PluginProcessor::settingsChanged, first: the mcp_bridge_* settings are the host's.
bool McpHost::settingsChanged(String const& name, var const& value)
{
    if (name.startsWith("mcp_bridge_")) {
        mcpSettingsChanged(name, value);
        return true;
    }
    return false;
}

void McpHost::mcpSettingsChanged(String const& name, var const& value)
{
    if (name == "mcp_bridge_ai_ceiling") {
        // P9 (M1, round 1): ON from anywhere; OFF only through setOutputCeilingFromGui (the panel switches the host
        // first; the setting's change arrives after, asynchronously) or at startup. A settings-file edit to false while
        // PlugData runs is refused AND written back as true, so the file, the panel and the ceiling agree. A malformed
        // value is on and is written back as true too.
        if (ceilingSettingIsOn(value)) {
            session.outputCeiling.setEnabled(true);
            if (!value.isBool())
                postMessage([this] { processor.settingsFile->setProperty("mcp_bridge_ai_ceiling", true); });
        } else if (session.outputCeiling.isEnabled()) {
            processor.logMessage("Output ceiling stays on: switch it off in Settings > Advanced > MCP bridge");
            postMessage([this] {
                if (session.outputCeiling.isEnabled())
                    processor.settingsFile->setProperty("mcp_bridge_ai_ceiling", true);
            });
        }
        return;
    }
    if (!mcpBridge) return;

    if (name == "mcp_bridge_enabled") {
        if (static_cast<bool>(value)) {
            mcpBridge->start();
        } else {
            mcpBridge->stop();
        }
    } else if (name == "mcp_bridge_listen_port" || name == "mcp_bridge_send_port") {
        // Recreate the bridge socket with the new ports. Applies immediately;
        // the MCP server must be configured to match the listen port.
        auto const enabled = processor.settingsFile->getProperty<bool>("mcp_bridge_enabled");
        auto const listen = processor.settingsFile->getProperty<int>("mcp_bridge_listen_port");
        auto const send = processor.settingsFile->getProperty<int>("mcp_bridge_send_port");
        // P3.4 S8: the same object on new ports. Rebuilding it here destroyed the bridge while the audio
        // thread could be inside its audioTick or midiInputTick.
        mcpBridge->rebind(listen, send);
        if (enabled && !mcpBridge->isConnected()) mcpBridge->start();
        if (!enabled) mcpBridge->stop();
        processor.logMessage("MCP bridge ports updated (listen " + String(listen) + ", send " + String(send) + ")");
    }
}

// ── Monitor tap: solo / mute (NON-DESTRUCTIVE) ──────────────────────────────
// Runs per Pd SUB-BLOCK right after performDSP, so it works for any JUCE block
// size (Pd runs pdBlockSize sub-blocks inside a larger buffer) and the soloed
// signal goes through the master gain/limiter exactly like it does in the mix.
// The patch is never touched; the tap EXPIRES and a dead signal turns it off.
void McpHost::applyMonitorTap(float* out, int numSamples, int numChannels)
{
    if (session.mcpTapMode.load(std::memory_order_relaxed) == McpSession::mcpTapOff) return;
    auto const nowMs = juce::Time::getMillisecondCounter();
    if (nowMs > session.mcpTapUntilMs.load(std::memory_order_relaxed)) {
        session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        session.mcpTapSig.store(nullptr, std::memory_order_relaxed);
        return;
    }
    // P3.4 S5: touch the signal only under the Pd lock and only while the DSP chain it was resolved in is
    // still the current one (a rebuild frees signals; the timer then resolves the tap again). A busy lock or
    // an old chain leaves this block untouched rather than wait or read freed memory.
    juce::ScopedTryLock const pdLock(processor.audioLock);
    if (!pdLock.isLocked() || pd_getdspstate() == 0
        || session.mcpTapEpoch.load(std::memory_order_relaxed) != ugen_getsortno())
        return;
    // Re-read s_vec EVERY sub-block: Pd swaps outlet buffers per DSP tick, so a
    // captured vector goes stale after one block.
    auto* sig = static_cast<t_signal*>(session.mcpTapSig.load(std::memory_order_acquire));
    float* vec = (sig != nullptr && sig->s_vec != nullptr) ? sig->s_vec : nullptr;
    int const n = (vec != nullptr) ? sig->s_n : 0;
    if (vec == nullptr || n <= 0) {
        session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        return;
    }
    int const mode = session.mcpTapMode.load(std::memory_order_relaxed);
    float const g = session.mcpTapGain.load(std::memory_order_relaxed);
    int const copy = juce::jmin(n, numSamples);
    for (int ch = 0; ch < numChannels; ++ch) {
        float* d = out + static_cast<size_t>(ch) * static_cast<size_t>(numSamples);
        if (mode == McpSession::mcpTapSolo) {
            for (int i = 0; i < copy; ++i) d[i] = vec[i] * g;
            for (int i = copy; i < numSamples; ++i) d[i] = 0.0f;
        } else {
            // mute: subtract the node's contribution at unity — exact on the
            // pre-limiter path (downstream of the target, linear mixing).
            for (int i = 0; i < copy; ++i) d[i] -= vec[i];
        }
    }
}

bool McpHost::mcpOpenRecorder(juce::String const& path)
{
    juce::File const destFile(path);
    auto outStream = std::unique_ptr<juce::FileOutputStream>(destFile.createOutputStream());
    if (!outStream || outStream->failedToOpen()) {
        post("MCP record: could not create output stream for %s", path.toRawUTF8());
        return false;
    }
    // FileOutputStream appends to an existing file; a take must start empty.
    outStream->setPosition(0);
    outStream->truncate();

    double sampleRate = processor.getSampleRate();
    if (sampleRate <= 0)
        sampleRate = 44100.0;
    // Cap at stereo so the file plays everywhere; the audible mix is on 0-1.
    int const numCh = jlimit(1, 2, processor.getTotalNumOutputChannels());

    // 16-bit PCM = maximum player compatibility.
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(outStream.get(), sampleRate, static_cast<unsigned int>(numCh), 16, {}, 0));
    if (!writer) {
        post("MCP record: createWriterFor failed (sr=%.0f ch=%d)", sampleRate, numCh);
        return false;
    }
    outStream.release(); // the writer owns the stream now

    if (!session.mcpRecorderThread.isThreadRunning())
        session.mcpRecorderThread.startThread();
    // ~3 s of FIFO at 44.1 kHz between the audio thread and the disk.
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), session.mcpRecorderThread, 1 << 17);

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> previous;
    {
        const ScopedLock sl(session.mcpRecorderLock);
        previous = std::move(session.mcpThreadedWriter);
        session.mcpThreadedWriter = std::move(threaded);
        session.mcpRecorderPath = path;
    }
    previous.reset(); // flush any stale take outside the lock
    post("MCP record: writer created sr=%.0f ch=%d path=%s", sampleRate, numCh, path.toRawUTF8());
    return true;
}

void McpHost::mcpCloseRecorder()
{
    session.mcpRecording.store(false, std::memory_order_release);
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> finished;
    {
        const ScopedLock sl(session.mcpRecorderLock);
        finished = std::move(session.mcpThreadedWriter);
    }
    finished.reset(); // flushes the FIFO and closes the file, off the audio thread
}

// MCP zero-dropout recorder tap, called at the end of processConstant (the full
// constant-path output block) and processVariable (after output is ready). Taps the
// final output buffer directly, no canvas objects needed.
void McpHost::writeRecorderTap(dsp::AudioBlock<float> const& buffer)
{
    mcp::rt::Scope const rtScope; // P3.4 S6
    if (!session.mcpRecording.load(std::memory_order_acquire))
        return;

    const ScopedTryLock sl(session.mcpRecorderLock);
    if (!sl.isLocked() || !session.mcpThreadedWriter)
        return;

    // Audio thread: no allocation, no file I/O. ThreadedWriter::write only
    // copies into its FIFO; a full FIFO drops this block instead of blocking.
    int const available = static_cast<int>(buffer.getNumChannels());
    if (available <= 0)
        return;
    float const* channels[2] = { buffer.getChannelPointer(0), buffer.getChannelPointer(available > 1 ? 1 : 0) };
    session.mcpThreadedWriter->write(channels, static_cast<int>(buffer.getNumSamples()));
}

// "main" must ALWAYS resolve to the true top-level patch. The editor's current
// canvas can be a subpatch OR an abstraction tab the artist has navigated into —
// automating against that would silently target the voice's internals instead of
// the patch (phantom "receive->canvas connection failed", census listing internals
// as top-level, silent recalls). Walk gl_owner all the way up (past abstractions
// too, unlike canvas_getrootfor which stops at them) so MCP operations always land
// on the actual root patch. Message thread only (it reads the editors).
void McpHost::refreshFocusedRoot()
{
    t_canvas* root = nullptr;
    for (auto* editor : processor.getEditors()) {
        if (editor) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                auto* glist = reinterpret_cast<t_glist*>(cnv->patch.getRawPointer());
                while (glist && glist->gl_owner) glist = glist->gl_owner;
                if (glist) {
                    root = reinterpret_cast<t_canvas*>(glist);
                    break;
                }
            }
        }
    }
    // P3.5 S3: no editor (the plugin's window is closed, or a headless host): "main" is the most recently opened
    // patch the processor holds, never the first canvas (which with no patch open is Pd's float-array template).
    if (!root) {
        for (int i = static_cast<int>(processor.patches.size()) - 1; i >= 0 && !root; --i) {
            if (auto* glist = reinterpret_cast<t_glist*>(processor.patches[i]->getRawPointer())) {
                while (glist->gl_owner) glist = glist->gl_owner;
                root = reinterpret_cast<t_canvas*>(glist);
            }
        }
    }
    session.mcpFocusedRoot.store(root, std::memory_order_release);
}

t_canvas* McpHost::getCanvasBySymbol(const String& canvas_symbol)
{
    // "main" is the true root of the focused editor's current canvas, published by the message thread
    // (refreshFocusedRoot), used only while it is still an open root patch. P3.5 S3: with no patch open, "main"
    // is nothing (it used to be the first canvas: Pd's float-array template).
    if (canvas_symbol == "pd-main" || canvas_symbol == "main" || canvas_symbol.isEmpty()) {
        PdLockScope lock(processor);
        if (auto* root = session.mcpFocusedRoot.load(std::memory_order_acquire)) {
            for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
                if (c == root) return root;
        }
        return nullptr;
    }

    // P3.5 S3 (#62): every other name is a wire key of the identity records ("song.pd", "pd-voice", "pd-voice#2",
    // or the "pd-" spelling of either). Unknown names answer nullptr: no fallback to the first canvas, no
    // per-request Pd symbol.
    // Review H4: the records already known answer first (no walk on the hot path: batch connects resolve per
    // wire); only an unknown name walks every open canvas (new canvases, renamed ones) and asks again.
    PdLockScope lock(processor);
    auto const key = canvas_symbol.toStdString();
    if (auto* known = identity.canvasByKey(key))
        return known;
    registerOpenCanvases();
    return identity.canvasByKey(key);
}

t_canvas* McpHost::getCanvasBySymbolStrict(const String& canvas_symbol)
{
    return getCanvasBySymbol(canvas_symbol); // P3.5 S3: every lookup is strict now
}

void McpHost::synchroniseCanvases()
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) {
        if (!mm->isThisTheMessageThread()) {
            postMessage([this] { synchroniseCanvases(); });
            return;
        }
    } else {
        return;
    }

    for (auto* editor : processor.getEditors()) {
        if (!editor) continue;
        SmallArray<Component::SafePointer<Canvas>> canvases;
        for (auto* canvas : editor->getCanvases()) {
            if (canvas) canvases.add(canvas);
        }
        for (auto& cnv : canvases) {
            if (cnv.getComponent()) {
                cnv->synchronise();
            }
        }
    }
}

juce::String McpHost::getStableId(t_gobj* g) const
{
    PdLockScope lock(processor);
    return juce::String(identity.idOf(g));
}

juce::String McpHost::getOrAdoptStableId(t_canvas* canvas, t_gobj* g)
{
    if (!g) return {};
    PdLockScope lock(processor);
    auto const existing = getStableId(g);
    if (existing.isNotEmpty()) return existing;
    if (!canvas) return {};

    // P3.5 S4: one minter (IdentityCore::mint), only for an object of this canvas.
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next)
        if (y == g)
            return juce::String(identity.mint(canvas, g, mintPrefix(g).toStdString()));
    return {};
}

t_gobj* McpHost::resolveStableId(const String& canvasName, const String& objectId)
{
    PdLockScope lock(processor);
    // Identity is keyed by the canvas's CANONICAL key. "main" is a
    // focus-dependent ALIAS, never a stored key — resolve the canvas first and
    // derive its canonical key so both sides of the map agree.
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    if (!canvas) return nullptr; // P3.5 S3: no canvas, no id
    auto idStr = objectId.toStdString();
    
    // P3.5 S5: IdentityCore::resolve checks the binding itself (the object it was bound to still lives: its weak
    // reference, not just an address; this closes Report F3, address reuse) and drops a stale entry. The
    // gl_list walk keeps the old guarantee that the object is still in THIS canvas.
    if (t_gobj* bound = identity.resolve(canvas, idStr)) {
        for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next)
            if (y_obj == bound)
                return bound;
        identity.eraseId(canvas, idStr); // moved out of this canvas: forget it here
    }

    // P5.1 sink floor: no index fallback. A bare "12" or "obj_12" (or an empty ref, which read as index 0) used to
    // resolve to the 12th object of gl_list when no tempId matched: the AI could address an object it was never
    // told the name of (a speaker guard included). An MCP ref is a tempId or nothing; the caller answers the error.

    return nullptr;
}

void McpHost::reconcileIdentity(const String& canvasName)
{
    PdLockScope lock(processor);
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    if (!canvas)
        return;

    auto& canvasMap = identity.canvasMap(canvas);

    // Step A: live pointer set from gl_list (the ground truth).
    std::unordered_set<t_gobj*> livePointers;
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next)
        livePointers.insert(y);

    // Step B: evict entries whose gobj no longer exists on the canvas (eraseId keeps the bound serials in step;
    // the object's serial stays: the address may be reused by a bound object).
    std::vector<std::string> gone;
    for (auto const& [id, ptr] : canvasMap)
        if (livePointers.find(ptr) == livePointers.end())
            gone.push_back(id);
    for (auto const& id : gone)
        identity.eraseId(canvas, id);
    if (!gone.empty())
        identity.bumpVersion(canvas);

    // Step C: adopt objects the identity map doesn't track yet (GUI-created).
    std::unordered_set<t_gobj*> trackedPointers;
    for (auto& [_, ptr] : canvasMap)
        trackedPointers.insert(ptr);

    for (t_gobj* y = canvas->gl_list; y; y = y->g_next) {
        if (trackedPointers.find(y) == trackedPointers.end())
            identity.mint(canvas, y, mintPrefix(y).toStdString()); // P3.5 S4: one minter
    }
}

void McpHost::sendMCPReply(const String& replyAddr, mcp::SysList const& values)
{
    if (!mcpBridge)
        return;
    juce::Array<juce::var> args;
    for (int i = 0; i < values.size(); ++i) {
        if (values[i].isNumber)
            args.add(static_cast<double>(values[i].number));
        else
            args.add(values[i].text);
    }
    mcpBridge->sendReply(replyAddr, args);
}

namespace {
std::string objectText(t_gobj* g)
{
    t_object* ob = pd_checkobject(&g->g_pd);
    if (!ob || !ob->te_binbuf) return {};
    char* buf = nullptr;
    int len = 0;
    binbuf_gettext(ob->te_binbuf, &buf, &len);
    std::string text = (buf && len > 0) ? std::string(buf, static_cast<size_t>(len)) : std::string();
    if (buf) freebytes(buf, static_cast<size_t>(len));
    return text;
}
constexpr size_t kUndoJournalMax = 512;

// The fork's line~/vline~ ignore their creation argument (the batch seeds it, MCPBridgeCmdBatchParse.cpp): an
// undo or redo that rebuilds one from its saved text must seed it again, or [line~ 440] restarts at 0.
void reseedLine(t_gobj* g)
{
    t_object* ob = pd_checkobject(&g->g_pd);
    if (!ob || !ob->te_binbuf || binbuf_getnatom(ob->te_binbuf) < 2) return;
    t_atom const* v = binbuf_getvec(ob->te_binbuf);
    if (v[0].a_type != A_SYMBOL || v[1].a_type != A_FLOAT) return;
    auto const* name = v[0].a_w.w_symbol->s_name;
    if (std::strcmp(name, "line~") != 0 && std::strcmp(name, "vline~") != 0) return;
    t_atom f;
    SETFLOAT(&f, v[1].a_w.w_float);
    pd_typedmess(&ob->ob_pd, gensym("float"), 1, &f);
}
} // namespace

// P4.4: the canvas's journal of ids its undo/redo steps (and AI deletes) took away. Keyed by the canvas pointer but
// checked through the identity service's liveness hook, so a new canvas at a reused address never inherits it.
std::vector<McpUndoGhost>& McpHost::undoJournalFor(t_canvas* cnv)
{
    auto& j = session.undoJournal[cnv];
    if (!j.alive || !j.alive()) {
        j.ghosts.clear();
        pd::WeakReference ref(cnv, &processor);
        j.alive = [ref] { return ref.isValid(); };
    }
    return j.ghosts;
}

void McpHost::clearUndoJournal(t_canvas* cnv)
{
    session.undoJournal.erase(cnv);
}

// P4.4: one step of the canvas's own undo queue (AI and manual edits alike). Pd recreates the objects an undo or
// redo brings back, so their ids would be lost: the ids of objects the step takes away go to the canvas's journal,
// and an object the step rebuilds without an id gets a journal id back (same text and place; else, for a retype,
// the only ghost at the same place).
bool McpHost::undoStep(t_canvas* cnv, bool redo)
{
    PdLockScope lock(processor);
    if (!cnv || !(redo ? mcp::undo::canRedo(cnv) : mcp::undo::canUndo(cnv)))
        return false;

    struct Before { std::string id; t_gobj* obj; std::string text; int x, y, index; };
    std::vector<Before> before;
    std::unordered_map<t_gobj*, std::string> textBefore; // every box, to see which ones the step rebuilt
    {
        std::unordered_map<t_gobj*, int> indexOf; // only objects in the list are read: a map entry may be stale
        int i = 0;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
            indexOf[g] = i++;
            textBefore[g] = objectText(g);
        }
        if (auto* ids = identity.findCanvas(cnv)) {
            for (auto const& [id, obj] : *ids) {
                auto const at = indexOf.find(obj);
                if (at == indexOf.end()) continue;
                t_object* ob = pd_checkobject(&obj->g_pd);
                before.push_back({ id, obj, textBefore[obj], ob ? ob->te_xpix : 0, ob ? ob->te_ypix : 0, at->second });
            }
        }
    }

    {
        mcp::undo::EditorScope editor(cnv);
        if (redo)
            pd::Interface::redo(cnv);
        else
            pd::Interface::undo(cnv);
    }

    std::unordered_set<t_gobj*> live;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) live.insert(g);
    auto& journal = undoJournalFor(cnv);
    bool changed = false;
    for (auto const& b : before) {
        // Still bound to a live object (the same one, or the one the retype hook moved it to): kept. Gone, or
        // another object now sits at the old address: the id leaves with its object.
        t_gobj* now = identity.resolve(cnv, b.id);
        bool const reused = now == b.obj && objectText(now) != b.text;
        if (now && live.count(now) && !reused) continue;
        identity.eraseId(cnv, b.id);
        journal.push_back({ b.id, b.text, b.x, b.y, b.index });
        changed = true;
    }
    if (journal.size() > kUndoJournalMax)
        journal.erase(journal.begin(), journal.begin() + static_cast<long>(journal.size() - kUndoJournalMax));

    // The boxes the step rebuilt (new, or a new text at an old address).
    std::vector<t_gobj*> rebuilt;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
        auto const it = textBefore.find(g);
        if (it == textBefore.end() || it->second != objectText(g)) rebuilt.push_back(g);
    }
    for (auto* g : rebuilt) reseedLine(g);

    if (!journal.empty() && !rebuilt.empty()) {
        std::vector<std::string> ids; // resolve() may drop stale entries, so not while walking the map
        if (auto* map = identity.findCanvas(cnv))
            for (auto const& entry : *map) ids.push_back(entry.first);
        std::unordered_set<t_gobj*> tracked;
        for (auto const& id : ids)
            if (t_gobj* obj = identity.resolve(cnv, id)) tracked.insert(obj);
        auto bind = [&](t_gobj* g, std::vector<McpUndoGhost>::iterator ghost) {
            if (identity.resolve(cnv, ghost->id)) return; // the id is in use again: leave it there
            if (identity.bind(cnv, ghost->id, g) == mcp::IdentityService::BindResult::Refused) return;
            journal.erase(ghost);
            tracked.insert(g);
            changed = true;
        };
        // Pass 1: the same text at the same place (an object brought back), newest ghost first.
        for (auto* g : rebuilt) {
            if (tracked.count(g)) continue;
            t_object* ob = pd_checkobject(&g->g_pd);
            if (!ob) continue;
            auto const text = objectText(g);
            for (auto it = journal.end(); it != journal.begin();) {
                --it;
                if (it->text == text && it->x == ob->te_xpix && it->y == ob->te_ypix) {
                    bind(g, it);
                    break;
                }
            }
        }
        // Pass 2: another text at the same place (a retype undone or redone). Pd puts the box back at its old
        // index, not the one it had (a retype moves it to the end), so the place decides, and only when exactly one
        // ghost and one rebuilt box share it.
        for (auto* g : rebuilt) {
            if (tracked.count(g)) continue;
            t_object* ob = pd_checkobject(&g->g_pd);
            if (!ob) continue;
            int boxesHere = 0;
            for (auto* h : rebuilt) {
                t_object* hb = pd_checkobject(&h->g_pd);
                if (hb && !tracked.count(h) && hb->te_xpix == ob->te_xpix && hb->te_ypix == ob->te_ypix) ++boxesHere;
            }
            auto match = journal.end();
            int ghostsHere = 0;
            for (auto it = journal.begin(); it != journal.end(); ++it)
                if (it->x == ob->te_xpix && it->y == ob->te_ypix) {
                    ++ghostsHere;
                    match = it;
                }
            if (boxesHere == 1 && ghostsHere == 1) bind(g, match);
        }
    }
    if (changed) identity.bumpVersion(cnv);

    postMessage([this] {
        for (auto* editor : processor.getEditors())
            editor->triggerAsyncUpdate();
    });
    return true;
}

void McpHost::journalId(t_canvas* cnv, std::string const& id, t_gobj* obj)
{
    // Only a real binding (resolveStableId has no index fallback since P5.1; kept as a guard).
    if (!cnv || !obj || identity.resolve(cnv, id) != obj) return;
    int index = 0;
    for (t_gobj* g = cnv->gl_list; g && g != obj; g = g->g_next) ++index;
    t_object* ob = pd_checkobject(&obj->g_pd);
    auto& journal = undoJournalFor(cnv);
    journal.push_back({ id, objectText(obj), ob ? ob->te_xpix : 0, ob ? ob->te_ypix : 0, index });
    if (journal.size() > kUndoJournalMax)
        journal.erase(journal.begin());
}

