/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// V1 read-only facts for the doctor (2026-11-06): /pd/inlet_values, /pd/wire_activity, /pd/audio_device, /pd/broken,
// /pd/console_history. None of them changes the patch or the audio. The Pd reads run in the Pd context (message
// thread under sys_lock, McpHost::pdWait); the device and console reads on the message thread. Nothing here runs on
// the audio thread; the one audio/Pd-thread part is the wire window's record() (MCPBridgeWireActivity.h).

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeInletValues.h"
#include "MCPBridgeWireActivity.h"
#include "MCPBridgeBroken.h"
#include "MCPBridgeObjectValues.h" // mcpGobjIsLive

namespace {

constexpr int kPdWaitMs = 3000;

juce::String toJson(juce::DynamicObject* root) { return juce::JSON::toString(juce::var(root), true); }

juce::String errorJson(juce::String const& text)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("error", text);
    return toJson(o);
}

uint32_t wireClock() { return juce::Time::getMillisecondCounter(); }

// The open wire window's wires, for the read (message thread, under the Pd lock: arm and read run there).
struct WatchedWire {
    void const* oc = nullptr;
    juce::String src, dst;
    int outlet = 0, inlet = 0;
};
struct WireWindowInfo {
    juce::String canvas;
    std::vector<WatchedWire> wires;
    uint32_t armedAtMs = 0;
    bool open = false;
};
WireWindowInfo& wireWindow()
{
    static WireWindowInfo w;
    return w;
}

} // namespace

// ── /pd/inlet_values <canvas> <corrId> <count> <tempId...> ─────────────────────────────────────────────────────────
void MCPBridge::pdInletValues(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 2) return;
    auto const canvasName = normalizeCanvas(getArgString(msg[0]));
    auto const corr = getArgString(msg[1]);
    auto const address = "/pd/inlet_values/reply/" + corr;
    int const count = msg.size() > 2 ? std::max(0, static_cast<int>(getArgFloat(msg[2]))) : 0;
    std::vector<juce::String> ids;
    for (int i = 0; i < count && 3 + i < static_cast<int>(msg.size()); ++i)
        ids.push_back(getArgString(msg[static_cast<size_t>(3 + i)]));
    if (ids.empty()) {
        sendReply(address, errorJson("no object given (/pd/inlet_values <canvas> <corrId> <count> <tempId...>)"));
        return;
    }
    juce::String json;
    auto* host = processor->getMcp();
    bool const ran = host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol(canvasName);
        if (!cnv) {
            json = errorJson("canvas not found: " + canvasName);
            return;
        }
        std::vector<t_object*> objects;
        std::unordered_set<t_object*> wanted;
        for (auto const& id : ids) {
            t_gobj* g = host->resolveStableId(canvasName, id);
            t_object* o = (g && mcpGobjIsLive(cnv, g)) ? pd::Interface::checkObject(g) : nullptr;
            objects.push_back(o);
            if (o) wanted.insert(o);
        }
        auto const wires = mcp::inlets::wiresInto(cnv, wanted);
        auto* root = new juce::DynamicObject();
        auto* out = new juce::DynamicObject();
        root->setProperty("canvas", canvasName);
        for (size_t i = 0; i < ids.size(); ++i) {
            auto* entry = new juce::DynamicObject();
            if (t_object* o = objects[i]) {
                entry->setProperty("class", juce::String::fromUTF8(class_getname(pd_class(&o->ob_pd))));
                auto const it = wires.find(o);
                entry->setProperty("inlets", mcp::inlets::objectInlets(o, it == wires.end() ? std::unordered_map<int, mcp::inlets::Wires> {} : it->second));
            } else {
                entry->setProperty("error", "unknown tempId");
            }
            out->setProperty(ids[i], juce::var(entry));
        }
        root->setProperty("objects", juce::var(out));
        json = toJson(root);
    }, kPdWaitMs);
    sendReply(address, ran ? json : errorJson("the Pd context did not answer in time"));
}

// ── /pd/wire_activity <canvas> <corrId> arm <windowMs> all | <count> (<srcId> <outlet> <dstId> <inlet>)... ────────
// ── /pd/wire_activity <canvas> <corrId> read ─────────────────────────────────────────────────────────────────────
void MCPBridge::pdWireActivity(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 3) return;
    auto const canvasName = normalizeCanvas(getArgString(msg[0]));
    auto const corr = getArgString(msg[1]);
    auto const mode = getArgString(msg[2]);
    auto const address = "/pd/wire_activity/reply/" + corr;
    auto* host = processor->getMcp();

    if (mode == "arm") {
        int const windowMs = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 500;
        bool const all = msg.size() > 4 && getArgString(msg[4]) == "all";
        struct Want {
            juce::String src, dst;
            int outlet, inlet;
        };
        std::vector<Want> wants;
        if (!all && msg.size() > 4) {
            int const n = std::max(0, static_cast<int>(getArgFloat(msg[4])));
            for (int i = 0; i < n && 5 + i * 4 + 3 < static_cast<int>(msg.size()); ++i) {
                size_t const at = static_cast<size_t>(5 + i * 4);
                wants.push_back({ getArgString(msg[at]), getArgString(msg[at + 2]), static_cast<int>(getArgFloat(msg[at + 1])), static_cast<int>(getArgFloat(msg[at + 3])) });
            }
        }
        if (!all && wants.empty()) {
            sendReply(address, errorJson("no wire given (arm <windowMs> all | <count> <srcId> <outlet> <dstId> <inlet> ...)"));
            return;
        }
        activateProbing(); // plugdata forwards wire messages only while its debugging/activity flag is on
        juce::String json;
        bool const ran = host->pdWait([&] {
            t_canvas* cnv = host->getCanvasBySymbol(canvasName);
            if (!cnv) {
                json = errorJson("canvas not found: " + canvasName);
                return;
            }
            auto& info = wireWindow();
            mcp::wire::table().disarm();
            info = {};
            info.canvas = canvasName;
            std::vector<bool> matched(wants.size(), false);
            int skippedSignal = 0;
            t_linetraverser lt;
            t_outconnect* oc = nullptr;
            linetraverser_start(&lt, cnv);
            while ((oc = linetraverser_next_nosize(&lt))) {
                juce::String const src = host->getStableId(&lt.tr_ob->ob_g);
                juce::String const dst = host->getStableId(&lt.tr_ob2->ob_g);
                bool take = all;
                for (size_t k = 0; !all && k < wants.size(); ++k)
                    if (wants[k].src == src && wants[k].dst == dst && wants[k].outlet == lt.tr_outno && wants[k].inlet == lt.tr_inno) {
                        take = true;
                        matched[k] = true;
                    }
                if (!take) continue;
                if (obj_issignaloutlet(lt.tr_ob, lt.tr_outno)) {
                    ++skippedSignal; // the AC taps cover signal wires
                    continue;
                }
                if (static_cast<int>(info.wires.size()) < mcp::wire::kMaxWires)
                    info.wires.push_back({ oc, src, dst, lt.tr_outno, lt.tr_inno });
            }
            std::vector<void const*> keys;
            for (auto const& w : info.wires) keys.push_back(w.oc);
            info.armedAtMs = wireClock();
            int const watched = mcp::wire::table().arm(keys.data(), static_cast<int>(keys.size()), info.armedAtMs, static_cast<uint32_t>(std::max(0, windowMs)), &wireClock);
            info.open = watched > 0;
            auto* root = new juce::DynamicObject();
            root->setProperty("armed", info.open);
            root->setProperty("windowMs", static_cast<int>(mcp::wire::table().windowMs()));
            root->setProperty("wires", watched);
            root->setProperty("skippedSignal", skippedSignal);
            juce::Array<juce::var> unresolved;
            for (size_t k = 0; k < wants.size(); ++k)
                if (!matched[k])
                    unresolved.add(wants[k].src + ":" + juce::String(wants[k].outlet) + ">" + wants[k].dst + ":" + juce::String(wants[k].inlet));
            root->setProperty("unresolved", unresolved);
            if (!info.open) root->setProperty("error", "no control wire to watch");
            json = toJson(root);
        }, kPdWaitMs);
        sendReply(address, ran ? json : errorJson("the Pd context did not answer in time"));
        return;
    }

    if (mode == "read") {
        juce::String json;
        bool const ran = host->pdWait([&] {
            auto& info = wireWindow();
            if (!info.open) {
                json = errorJson("no wire window armed (arm one first)");
                return;
            }
            auto& table = mcp::wire::table();
            table.disarm(); // under the Pd lock: no record() is running now
            info.open = false;
            auto* root = new juce::DynamicObject();
            root->setProperty("canvas", info.canvas);
            root->setProperty("windowMs", static_cast<int>(table.windowMs()));
            root->setProperty("ms", static_cast<int>(wireClock() - info.armedAtMs));
            juce::Array<juce::var> list;
            for (auto const& w : info.wires) {
                mcp::wire::Reading r;
                table.read(w.oc, r);
                auto* o = new juce::DynamicObject();
                o->setProperty("src", w.src);
                o->setProperty("outlet", w.outlet);
                o->setProperty("dst", w.dst);
                o->setProperty("inlet", w.inlet);
                o->setProperty("messageCount", static_cast<int>(r.count));
                auto const* sel = static_cast<t_symbol const*>(r.lastSelector);
                o->setProperty("lastSelector", sel && sel->s_name ? juce::var(juce::String::fromUTF8(sel->s_name)) : juce::var());
                if (r.lastKind == mcp::wire::ValueKind::Float)
                    o->setProperty("lastValue", static_cast<double>(r.lastFloat));
                else if (r.lastKind == mcp::wire::ValueKind::Symbol && r.lastSymbol)
                    o->setProperty("lastValue", juce::String::fromUTF8(static_cast<t_symbol const*>(r.lastSymbol)->s_name));
                else
                    o->setProperty("lastValue", juce::var());
                o->setProperty("lastAtMs", r.count > 0 ? juce::var(static_cast<int>(r.lastAtMs)) : juce::var());
                list.add(juce::var(o));
            }
            root->setProperty("wires", list);
            json = toJson(root);
        }, kPdWaitMs);
        sendReply(address, ran ? json : errorJson("the Pd context did not answer in time"));
        return;
    }
    sendReply(address, errorJson("unknown mode '" + mode + "' (arm | read)"));
}

// ── /pd/audio_device <corrId> ───────────────────────────────────────────────────────────────────────────────────
void MCPBridge::pdAudioDevice(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor) return;
    auto const corr = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    auto* proc = processor;
    auto* host = proc->getMcp();
    auto json = host->onMessageThread([proc] {
        auto* root = new juce::DynamicObject();
        auto* pd = new juce::DynamicObject();
        int const pdBlock = pd::Instance::getBlockSize();
        int const hostBlock = proc->juce::AudioProcessor::getBlockSize();
        pd->setProperty("sampleRate", static_cast<double>(sys_getsr()));
        pd->setProperty("outChannels", sys_get_outchannels());
        pd->setProperty("blockSize", pdBlock);
        pd->setProperty("oversampling", proc->oversampling.load());
        pd->setProperty("hostSampleRate", proc->getSampleRate());
        pd->setProperty("hostBlockSize", hostBlock);
        pd->setProperty("totalOutputChannels", proc->getTotalNumOutputChannels());
        // The rule of PluginProcessor::prepareToPlay (the member is private there): derived, and labelled so.
        pd->setProperty("variableBlockSize", !ProjectInfo::isStandalone || pdBlock <= 0 || hostBlock < pdBlock || hostBlock % pdBlock != 0);
        pd->setProperty("variableBlockSizeDerived", true);
        root->setProperty("pd", juce::var(pd));
        auto* dm = ProjectInfo::getDeviceManager();
        auto* dev = dm ? dm->getCurrentAudioDevice() : nullptr;
        if (!dm) {
            root->setProperty("device", juce::var());
            root->setProperty("reason", ProjectInfo::isStandalone ? "no device manager" : "plugin: the host owns the device");
        } else if (!dev) {
            root->setProperty("device", juce::var());
            root->setProperty("type", dm->getCurrentAudioDeviceType());
            root->setProperty("reason", "no audio device open");
        } else {
            auto* d = new juce::DynamicObject();
            d->setProperty("type", dev->getTypeName());
            d->setProperty("name", dev->getName());
            d->setProperty("open", dev->isOpen());
            d->setProperty("playing", dev->isPlaying());
            auto const outs = dev->getActiveOutputChannels();
            auto const ins = dev->getActiveInputChannels();
            d->setProperty("outputChannels", outs.countNumberOfSetBits());
            d->setProperty("outputMask", outs.toString(2));
            d->setProperty("inputChannels", ins.countNumberOfSetBits());
            d->setProperty("sampleRate", dev->getCurrentSampleRate());
            d->setProperty("bufferSize", dev->getCurrentBufferSizeSamples());
            d->setProperty("lastError", dev->getLastError().isEmpty() ? juce::var() : juce::var(dev->getLastError()));
            root->setProperty("device", juce::var(d));
            root->setProperty("sampleRateMatches", std::abs(dev->getCurrentSampleRate() - static_cast<double>(sys_getsr())) < 0.5);
        }
        // plugdata prints a failed setAudioDeviceSetup() to stderr and keeps nothing: say so, never invent one.
        root->setProperty("lastSetupError", juce::var());
        root->setProperty("lastSetupErrorReason", "not kept by plugdata (AudioSettingsPanel writes it to stderr)");
        return toJson(root);
    }, kPdWaitMs, "audio_device");
    sendReply("/pd/audio_device/reply/" + corr, json ? *json : errorJson("the message thread did not answer in time"));
}

// ── /pd/broken <canvas> <corrId> ────────────────────────────────────────────────────────────────────────────────
void MCPBridge::pdBroken(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 1) return;
    auto const canvasName = normalizeCanvas(getArgString(msg[0]));
    auto const corr = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
    auto* host = processor->getMcp();
    juce::String json;
    bool const ran = host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol(canvasName);
        if (!cnv) {
            json = errorJson("canvas not found: " + canvasName);
            return;
        }
        juce::Array<juce::var> found;
        mcp::broken::collectFailedCreates(cnv, 0, {}, found, 0);
        // Name the top-level object each entry sits in (the box itself, or its container) by tempId.
        std::vector<t_gobj*> top;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) top.push_back(g);
        for (auto& f : found)
            if (auto* o = f.getDynamicObject()) {
                int const idx = static_cast<int>(o->getProperty("index"));
                juce::String const id = idx >= 0 && idx < static_cast<int>(top.size()) ? host->getStableId(top[static_cast<size_t>(idx)]) : juce::String();
                o->setProperty("tempId", id.isNotEmpty() ? juce::var(id) : juce::var());
            }
        auto* root = new juce::DynamicObject();
        root->setProperty("canvas", canvasName);
        root->setProperty("count", found.size());
        root->setProperty("broken", found);
        json = toJson(root);
    }, kPdWaitMs);
    sendReply("/pd/broken/reply/" + corr, ran ? json : errorJson("the Pd context did not answer in time"));
}

// ── /pd/console_history <corrId> [since] [max] ──────────────────────────────────────────────────────────────────
void MCPBridge::pdConsoleHistory(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor) return;
    auto const corr = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    int const since = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : -1;
    int const maxLines = juce::jlimit(1, 800, msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 100);
    auto* host = processor->getMcp();
    juce::String json;
    // The console store is a message-thread deque; the tempId lookup needs the Pd lock: pdWait gives both.
    bool const ran = host->pdWait([&] {
        auto& lines = processor->getConsoleMessages();
        int const total = static_cast<int>(lines.size());
        int const first = std::max({ 0, since + 1, total - maxLines });
        juce::Array<juce::var> list;
        for (int i = first; i < total; ++i) {
            auto const& [object, text, type, width, repeats] = lines[static_cast<size_t>(i)];
            juce::ignoreUnused(width);
            auto* o = new juce::DynamicObject();
            o->setProperty("index", i);
            o->setProperty("type", type ? "error" : "print");
            o->setProperty("text", text);
            o->setProperty("repeats", repeats);
            // The pointer is only compared with the registry's, never dereferenced (the object may be gone).
            juce::String const id = object ? host->getStableId(static_cast<t_gobj*>(object)) : juce::String();
            o->setProperty("tempId", id.isNotEmpty() ? juce::var(id) : juce::var());
            list.add(juce::var(o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("total", total);
        root->setProperty("last", total - 1);
        root->setProperty("lines", list);
        root->setProperty("note", "index = position in plugdata's console store (the last 800 lines, repeats merged); warnings are stored as errors");
        json = toJson(root);
    }, kPdWaitMs);
    sendReply("/pd/console_history/reply/" + corr, ran ? json : errorJson("the Pd context did not answer in time"));
}
