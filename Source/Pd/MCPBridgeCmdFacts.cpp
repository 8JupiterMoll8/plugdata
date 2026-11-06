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
#include "MCPBridgePdAccessors.h"

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
        juce::String json;
        bool const ran = host->pdWait([&] {
            t_canvas* cnv = host->getCanvasBySymbol(canvasName);
            if (!cnv) {
                json = errorJson("canvas not found: " + canvasName);
                return;
            }
            auto& session = host->session;
            session.wireTable.disarm();
            session.wireWires.clear();
            session.wireOpen = false;
            session.wireCanvas = canvasName;
            std::vector<bool> matched(wants.size(), false);
            std::unordered_map<t_object*, juce::String> ids; // review L7: one registry lookup per object
            auto idOf = [&](t_object* ob) -> juce::String const& {
                auto it = ids.find(ob);
                if (it == ids.end()) it = ids.emplace(ob, host->getStableId(&ob->ob_g)).first;
                return it->second;
            };
            std::unordered_map<std::string, std::vector<size_t>> wanted; // "src:outlet>dst:inlet" -> request rows
            for (size_t k = 0; k < wants.size(); ++k)
                wanted[(wants[k].src + ":" + juce::String(wants[k].outlet) + ">" + wants[k].dst + ":" + juce::String(wants[k].inlet)).toStdString()].push_back(k);
            int skippedSignal = 0;
            t_linetraverser lt;
            t_outconnect* oc = nullptr;
            linetraverser_start(&lt, cnv);
            while ((oc = linetraverser_next_nosize(&lt))) {
                juce::String const& src = idOf(lt.tr_ob);
                juce::String const& dst = idOf(lt.tr_ob2);
                bool take = all;
                if (!all) {
                    auto const hit = wanted.find((src + ":" + juce::String(lt.tr_outno) + ">" + dst + ":" + juce::String(lt.tr_inno)).toStdString());
                    if (hit != wanted.end()) {
                        take = true;
                        for (size_t k : hit->second) matched[k] = true;
                    }
                }
                if (!take) continue;
                if (obj_issignaloutlet(lt.tr_ob, lt.tr_outno)) {
                    ++skippedSignal; // the AC taps cover signal wires
                    continue;
                }
                if (static_cast<int>(session.wireWires.size()) < mcp::wire::kMaxWires)
                    session.wireWires.push_back({ oc, src, dst, lt.tr_outno, lt.tr_inno });
            }
            std::vector<void const*> keys;
            for (auto const& w : session.wireWires) keys.push_back(w.oc);
            session.wireArmedAtMs = wireClock();
            // Opening the window turns Pd's forwarding on through the bridge's own flag (McpHost's hook): plugdata's
            // debugging flag, the DSP graph and the artist's display stay as they are.
            int const watched = session.wireTable.arm(keys.data(), static_cast<int>(keys.size()), session.wireArmedAtMs, static_cast<uint32_t>(std::max(0, windowMs)), &wireClock);
            session.wireOpen = watched > 0;
            session.wireForwardingAtArm = session.wireOpen && plugdata_debugging_or_activity_enabled() != 0;
            auto* root = new juce::DynamicObject();
            root->setProperty("armed", session.wireOpen);
            root->setProperty("forwarding", session.wireForwardingAtArm);
            root->setProperty("windowMs", static_cast<int>(session.wireTable.windowMs()));
            root->setProperty("wires", watched);
            root->setProperty("skippedSignal", skippedSignal);
            juce::Array<juce::var> unresolved;
            for (size_t k = 0; k < wants.size(); ++k)
                if (!matched[k])
                    unresolved.add(wants[k].src + ":" + juce::String(wants[k].outlet) + ">" + wants[k].dst + ":" + juce::String(wants[k].inlet));
            root->setProperty("unresolved", unresolved);
            if (!session.wireOpen) root->setProperty("error", "no control wire to watch");
            json = toJson(root);
        }, kPdWaitMs);
        sendReply(address, ran ? json : errorJson("the Pd context did not answer in time"));
        return;
    }

    if (mode == "read") {
        juce::String json;
        bool const ran = host->pdWait([&] {
            auto& session = host->session;
            if (!session.wireOpen) {
                json = errorJson("no wire window armed (arm one first)");
                return;
            }
            auto& table = session.wireTable;
            // Review: forwarding sampled at the arm and now (before the close). Off at either: the counts are unknown
            // (null), never 0. A window that already closed itself at its end had forwarding until then.
            bool const stillOpen = table.armed();
            bool const forwarding = session.wireForwardingAtArm && (!stillOpen || plugdata_debugging_or_activity_enabled() != 0);
            table.disarm(); // under the Pd lock: no record() is running now
            session.wireOpen = false;
            auto* root = new juce::DynamicObject();
            root->setProperty("canvas", session.wireCanvas);
            root->setProperty("windowMs", static_cast<int>(table.windowMs()));
            root->setProperty("ms", static_cast<int>(wireClock() - session.wireArmedAtMs));
            root->setProperty("forwarding", forwarding);
            if (!forwarding) root->setProperty("reason", "Pd did not forward wire messages for the whole window: the counts are unknown");
            juce::Array<juce::var> list;
            for (auto const& w : session.wireWires) {
                mcp::wire::Reading r;
                table.read(w.oc, r);
                auto* o = new juce::DynamicObject();
                o->setProperty("src", w.src);
                o->setProperty("outlet", w.outlet);
                o->setProperty("dst", w.dst);
                o->setProperty("inlet", w.inlet);
                if (!forwarding) {
                    for (char const* k : { "messageCount", "lastSelector", "lastValue", "lastAtMs" })
                        o->setProperty(k, juce::var());
                    list.add(juce::var(o));
                    continue;
                }
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
        proc->setThis(); // review L6: sys_getsr/sys_get_outchannels read the current Pd instance
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
// `since` = the last `seq` the caller saw (-1 or absent: from the start). Lines come from McpHost::consoleMessage with
// a monotonic seq (review: the old store index broke at 800 lines and hid repeats); a repeat bumps its line's seq.
void MCPBridge::pdConsoleHistory(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor) return;
    auto const corr = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    // An int32 arg keeps every seq exact (a float32 loses steps past 2^24); text works too.
    double sinceArg = -1.0;
    if (msg.size() > 1) {
        if (msg[1].isInt32()) sinceArg = msg[1].getInt32();
        else if (msg[1].isString()) sinceArg = getArgString(msg[1]).getDoubleValue();
        else sinceArg = getArgFloat(msg[1]);
    }
    uint64_t const since = sinceArg < 0 ? 0 : static_cast<uint64_t>(sinceArg);
    int const maxLines = juce::jlimit(1, 800, msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 100);
    auto& session = processor->getMcp()->session;
    std::vector<McpSession::ConsoleLine> copy;
    uint64_t lastSeq = 0;
    {
        std::lock_guard<std::mutex> const lock(session.consoleLock);
        lastSeq = session.consoleSeq;
        for (auto const& l : session.consoleLines)
            if (l.seq > since) copy.push_back(l);
    }
    bool const truncated = static_cast<int>(copy.size()) > maxLines;
    if (truncated) copy.erase(copy.begin(), copy.end() - maxLines);
    juce::Array<juce::var> list;
    for (auto const& l : copy) {
        auto* o = new juce::DynamicObject();
        o->setProperty("seq", static_cast<juce::int64>(l.seq));
        o->setProperty("firstSeq", static_cast<juce::int64>(l.firstSeq));
        o->setProperty("type", l.error ? "error" : "print");
        o->setProperty("text", l.text);
        o->setProperty("repeats", l.repeats);
        o->setProperty("tempId", l.tempId.isNotEmpty() ? juce::var(l.tempId) : juce::var());
        list.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("lastSeq", static_cast<juce::int64>(lastSeq));
    root->setProperty("truncated", truncated);
    root->setProperty("lines", list);
    root->setProperty("note", "seq is monotonic (a repeated line keeps one entry, its seq and repeats move); the last 800 lines are kept; warnings are stored as errors");
    sendReply("/pd/console_history/reply/" + corr, toJson(root));
}
