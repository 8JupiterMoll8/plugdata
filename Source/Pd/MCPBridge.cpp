/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include "MCPBridge.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Canvas.h"
#include "Objects/ObjectBase.h"
#include "Pd/Interface.h"
#include "../../Libraries/fftw3/api/fftw3.h"

#include <set>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
#include <s_inter.h>

struct _outlet
{
    t_object *o_owner;
    struct _outlet *o_next;
    t_outconnect *o_connections;
    t_symbol *o_sym;
};
}

MCPBridge::MCPBridge(PluginProcessor* proc, int inPort, int outPort)
    : processor(proc)
    , listenPort(inPort)
    , sendPort(outPort)
    , probeManager(this)
{
    bootToken = juce::String(juce::Time::getMillisecondCounter()) + "-"
        + juce::String::toHexString(juce::Random::getSystemRandom().nextInt());
    start();
}

MCPBridge::~MCPBridge()
{
    stop();
}

bool MCPBridge::start()
{
    if (active.load()) return true;

    bool recvOk = receiver.connect(listenPort);
    if (recvOk) {
        receiver.addListener(this);
        active.store(true);
        reportedConnected = false;
        reportedLost = false;
        statusMessage = {};
        if (processor) {
            processor->logMessage("MCP bridge listening on UDP " + juce::String(listenPort));
        }
    } else {
        statusMessage = "port " + juce::String(listenPort) + " in use";
        if (processor) {
            processor->logError("MCP bridge: could not bind UDP " + juce::String(listenPort) + " (port in use?)");
        }
    }

    sender.connect("127.0.0.1", sendPort);

    return active.load();
}

void MCPBridge::stop()
{
    stopTimer();
    if (!active.load()) return;

    receiver.removeListener(this);
    receiver.disconnect();
    sender.disconnect();
    active.store(false);
    reportedConnected = false;
    reportedLost = false;
    statusMessage = {};
    if (processor) {
        processor->logMessage("MCP bridge stopped");
    }
}

bool MCPBridge::isConnected() const
{
    return active.load();
}

juce::String MCPBridge::getStatus() const
{
    if (!active.load()) return statusMessage.isNotEmpty() ? ("error: " + statusMessage) : juce::String("disabled");

    auto const last = lastServerActivity.load();
    if (last == 0) return "waiting for server";

    auto const elapsed = juce::Time::getMillisecondCounter() - last;
    if (elapsed > 15000) {
        // Server went quiet. Emit once (lazily — the Advanced panel polls
        // getStatus(), which is where an artist would look).
        if (!reportedLost && processor) {
            processor->logError("MCP server connection lost");
            reportedLost = true;
        }
        return "disconnected";
    }

    return "connected";
}

void MCPBridge::noteServerActivity()
{
    bool const first = (lastServerActivity.load() == 0);
    lastServerActivity.store(juce::Time::getMillisecondCounter());
    if (first && !reportedConnected && processor) {
        processor->logMessage("MCP server connected");
        reportedConnected = true;
    }
    reportedLost = false;
}

juce::String MCPBridge::normalizeCanvas(const juce::String& name)
{
    juce::String s = name.trim();
    if (s.isEmpty() || s == "main") return "pd-main";
    // PRD Phase 3.1: root-file tab symbols are the raw file name (the bound
    // gl_name, e.g. "phase3_roundtrip_test.pd") — pass through unmangled so
    // canvas resolution and identity map keys agree.
    if (s.endsWith(".pd")) return s;
    while (s.startsWith("pd-")) s = s.substring(3);
    if (s.isEmpty()) return "pd-main";
    return "pd-" + s;
}

static juce::String getArgString(const juce::OSCArgument& arg);
static float getArgFloat(const juce::OSCArgument& arg);

// The plugdata fork does not expose glist_nth; walk the glist ourselves.
static t_gobj* glistObjectAt(t_canvas* cnv, int index)
{
    if (!cnv || index < 0) return nullptr;
    int i = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (i++ == index) return y;
    }
    return nullptr;
}

// Build the textual object representation PlugData's GUI expects, applying
// the same kind mapping used by mcp_create_batch_id in PluginProcessor.
static juce::String buildObjectText(const juce::String& kind, const juce::StringArray& tokens)
{
    juce::StringArray t = tokens;
    if (kind == "msg") {
        if (!t.isEmpty() && t[0] == "msg") t.remove(0);
        return "msg " + t.joinIntoString(" ");
    }
    if (kind == "text" || kind == "comment") {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        return "comment " + t.joinIntoString(" ");
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        return "floatbox " + t.joinIntoString(" ");
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        return "symbolbox " + t.joinIntoString(" ");
    }
    return t.joinIntoString(" ");
}

// =========================================================================
// Zero-Dropout Paste Helpers
// =========================================================================

// Direct binbuf_eval paste — bypasses canvas_dopaste entirely.
// Skips: undo serialization, object selection, editmode switch,
// and the inner canvas_suspend_dsp/canvas_resume_dsp cycle.
static void pasteDirect(t_canvas* cnv, char const* buf)
{
    size_t const len = strlen(buf);
    t_binbuf* b = binbuf_new();
    binbuf_text(b, buf, len);

    t_symbol* sym_X = gensym("#X");
    t_symbol* sym_N = gensym("#N");
    t_symbol* sym_A = gensym("#A");

    t_pd* saved_X = sym_X->s_thing;
    t_pd* saved_N = sym_N->s_thing;
    t_pd* saved_A = sym_A->s_thing;

    sym_A->s_thing = nullptr;
    sym_X->s_thing = &cnv->gl_pd;
    sym_N->s_thing = &pd_canvasmaker;

    binbuf_eval(b, 0, 0, 0);

    sym_A->s_thing = saved_A;
    sym_X->s_thing = saved_X;
    sym_N->s_thing = saved_N;

    binbuf_free(b);
}

// Escape semicolons for Pd paste buffer format
static juce::String escapePdText(const juce::String& text)
{
    return text.replace(";", " \\;");
}

// Convert MCP kind + tokens to Pd paste-format line
static juce::String formatAsPdLine(const juce::String& kind,
                                    const juce::StringArray& tokens,
                                    int x, int y)
{
    juce::StringArray t = tokens;

    if (kind == "msg" || kind == "message" || (!t.isEmpty() && t[0] == "msg")) {
        if (!t.isEmpty() && (t[0] == "msg" || t[0] == "message")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X msg " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "text" || kind == "comment" || (!t.isEmpty() && (t[0] == "text" || t[0] == "comment"))) {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X text " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        return "#X floatatom " + juce::String(x) + " " + juce::String(y)
               + " " + t.joinIntoString(" ").trim() + ";";
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        return "#X symbolatom " + juce::String(x) + " " + juce::String(y)
               + " " + t.joinIntoString(" ").trim() + ";";
    }

    if (!t.isEmpty() && t[0] == "+") t.set(0, "\\+");
    return "#X obj " + juce::String(x) + " " + juce::String(y)
           + " " + t.joinIntoString(" ").trim() + ";";
}

void MCPBridge::oscMessageReceived(const juce::OSCMessage& message)
{
    auto addr = message.getAddressPattern().toString();
    while (addr.startsWith("/")) addr = addr.substring(1);
    auto parts = juce::StringArray::fromTokens(addr, "/", "");
    parts.removeEmptyStrings();

    if (parts.isEmpty()) return;

    // Any inbound message means the MCP server is alive and talking to us.
    noteServerActivity();

    auto const domain = parts[0];

    if (domain == "pd") {
        auto action = parts.size() > 1 ? parts[1] : "";
        handlePdDomain(action, message);
    } else if (domain == "param") {
        auto paramName = parts.size() > 1 ? parts[1] : "";
        handleParamDomain(paramName, message);
    } else if (domain == "trigger") {
        auto triggerAction = parts.size() > 1 ? parts[1] : "";
        handleTriggerDomain(triggerAction, message);
    } else if (domain == "telemetry") {
        auto telAction = parts.size() > 1 ? parts[1] : "";
        handleTelemetryDomain(telAction, message);
    } else if (domain == "array") {
        // /array/write|/array/read are hierarchical; /array ["stats", ...] is flat.
        auto arrayAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleArrayDomain(arrayAction, message);
    } else if (domain == "bridge") {
        auto bridgeAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleBridgeDomain(bridgeAction, message);
    } else if (domain == "morph") {
        auto morphAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleMorphDomain(morphAction, message);
    } else if (domain == "meter") {
        auto meterAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleMeterDomain(meterAction, message);
    } else if (domain == "transport") {
        auto action = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleTransportDomain(action, message);
    } else if (domain == "seq") {
        auto action = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleSeqDomain(action, message);
    }
}

void MCPBridge::oscBundleReceived(const juce::OSCBundle& bundle)
{
    for (auto const& elem : bundle) {
        if (elem.isMessage()) {
            oscMessageReceived(elem.getMessage());
        }
    }
}

// Helper to extract string from OSC argument
static juce::String getArgString(const juce::OSCArgument& arg)
{
    if (arg.isString()) return arg.getString();
    if (arg.isFloat32()) return juce::String(arg.getFloat32());
    if (arg.isInt32()) return juce::String(arg.getInt32());
    return {};
}

// Helper to extract float from OSC argument
static float getArgFloat(const juce::OSCArgument& arg)
{
    if (arg.isFloat32()) return arg.getFloat32();
    if (arg.isInt32()) return static_cast<float>(arg.getInt32());
    if (arg.isString()) return arg.getString().getFloatValue();
    return 0.0f;
}

// ─── PRD Phase 3 §2.2: identity sidecar applier ──────────────────────────
// Re-registers sidecar identities (root + named subcanvases) for a freshly
// loaded/opened canvas. Caller must hold sys_lock. Returns tempIds restored.
int MCPBridge::mcpApplyIdentitySidecar(PluginProcessor* processor, juce::DynamicObject* sidecarObj,
                                   const juce::String& rootKey, t_canvas* cnv)
{
    if (!processor || !sidecarObj || !cnv) return 0;

    auto registerArray = [&](juce::var const& idArray, juce::String const& mapKey, t_canvas* target) -> int {
        const auto* arr = idArray.getArray();
        if (arr == nullptr) return 0;
        std::unordered_map<int, juce::String> idxToId;
        for (auto const& v : *arr) {
            if (auto* o = v.getDynamicObject()) {
                int i = static_cast<int>(static_cast<double>(o->getProperty("i")));
                juce::String id = o->getProperty("id").toString();
                if (id.isNotEmpty()) idxToId[i] = id;
            }
        }
        int restored = 0;
        int idx = 0;
        for (t_gobj* g = target->gl_list; g; g = g->g_next, ++idx) {
            auto it = idxToId.find(idx);
            if (it == idxToId.end()) continue;
            processor->mcpStableObjectMap[mapKey.toStdString()][it->second.toStdString()] = g;
            if (processor->mcpStableSerialMap.find(g) == processor->mcpStableSerialMap.end())
                processor->mcpStableSerialMap[g] = processor->mcpSerialCounter++;
            restored++;
        }
        return restored;
    };

    int restored = registerArray(sidecarObj->getProperty("root"), rootKey, cnv);

    // Named subcanvases: key "pd-<gl_name>" matches bridge map-key convention
    if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
            if (pd_class(&g->g_pd) != canvas_class) continue;
            auto* child = reinterpret_cast<t_canvas*>(g);
            if (!child->gl_name) continue;
            juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
            auto childIds = subObj->getProperty(juce::Identifier(childName));
            if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0) {
                processor->mcpStableObjectMap[("pd-" + childName).toStdString()].clear();
                restored += registerArray(childIds, "pd-" + childName, child);
            }
        }
    }

    processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
    return restored;
}

// ─── Native sequencer helpers ────────────────────────────────────────────

static bool isPureDrumName(const juce::String& name)
{
    auto const lower = name.toLowerCase();
    static const char* const names[] = {
        "kick", "bd", "snare", "sd", "hh", "hat", "hihat", "openhat", "closedhat",
        "clap", "cp", "tom", "rim", "shaker", "cowbell", "cymbal", "crash", "ride", "perc"
    };
    for (auto const* nm : names) if (lower == nm) return true;
    return false;
}

static bool isBangTrack(const juce::String& name)
{
    if (!isPureDrumName(name)) return false;
    if (name.contains("_") || name.containsIgnoreCase("gate") || name.containsIgnoreCase("freq")
        || name.containsIgnoreCase("pitch") || name.containsIgnoreCase("cut")) return false;
    return true;
}

static float varToFloat(const juce::var& v, float def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true") ? 1.0f : 0.0f;
    return (float)(double)v;
}

static int varToInt(const juce::var& v, int def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    return (int)(double)v;
}

static bool varToBool(const juce::var& v, bool def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true");
    return (double)v != 0.0;
}

void MCPBridge::handlePdDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    if (action == "ping") {
        sendRawReply("/pd/pong");
        return;
    }

    if (action == "mcp_reload_lua") {
        auto correlationId = msg.size() >= 2 ? getArgString(msg[1]) : (msg.size() > 0 ? getArgString(msg[0]) : "0");
        sendReply("/pd/mcp_reload_lua/reply/" + correlationId, 1.0f);
        return;
    }

    if (action == "dsp") {
        if (msg.size() > 0) {
            float val = getArgFloat(msg[0]);
            if (processor) {
                if (val > 0.5f) processor->startDSP();
                else processor->releaseDSP();
            }
        }
        return;
    }

    if (action == "update_dsp") {
        // Full DSP graph recompile (stop+start). Needed after bulk
        // reconstruct operations (load/undo/redo) which create/connect
        // objects while the graph was compiled against an empty canvas.
        sys_lock();
        canvas_update_dsp();
        sys_unlock();
        return;
    }

    if (action == "vis") {
        if (msg.size() >= 2) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            float vis = getArgFloat(msg[1]);
            sys_lock();
            t_canvas* cnv = reinterpret_cast<t_canvas*>(pd_findbyclass(gensym(canvasName.toRawUTF8()), canvas_class));
            if (cnv) {
                t_atom a;
                SETFLOAT(&a, vis);
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("vis"), 1, &a);
            }
            sys_unlock();
        }
        return;
    }

    if (action == "census") {
        if (msg.size() >= 1 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_census", atoms);
        }
        return;
    }

    if (action == "typeof") {
        // /pd/typeof <canvasName> <tempId_or_index> <correlationId>
        // Returns: /pd/typeof/reply/<corrId> <className> <objectText>
        // Reads the object's class name and binbuf text directly from structs.
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto targetId = getArgString(msg[1]);
            auto correlationId = getArgString(msg[2]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/typeof/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Resolve by stableId first, fallback to numeric index
            t_gobj* gobj = processor->resolveStableId(canvasName, targetId);
            if (!gobj) {
                bool isNumber = targetId.isNotEmpty();
                for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
                    if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                        isNumber = false;
                        break;
                    }
                }
                if (isNumber) {
                    gobj = glistObjectAt(cnv, targetId.getIntValue());
                }
            }

            if (!gobj) {
                sys_unlock();
                sendReply("/pd/typeof/error/" + correlationId, "Object not found: " + targetId);
                return;
            }

            // Get class name from the object's pd struct
            juce::String className = juce::String::fromUTF8(class_getname(pd_class(&gobj->g_pd)));

            // PRD §2.1: canvas-class objects (subpatch/abstraction wrappers).
            // NOTE: class_getname(canvas_class) is "canvas" in Pd, not "pd" —
            // normalize to the census vocabulary. Resolve the real loaded-file
            // name (MERDA .m~ / .pd abstraction) via the shared helper so
            // /pd/typeof is uniformly useful; inline [pd foo] stays "pd".
            if (pd_class(&gobj->g_pd) == canvas_class) {
                juce::String abstractName;
                if (pd::getAbstractionFileName(gobj, abstractName)) {
                    className = abstractName;
                } else {
                    className = "pd";
                }
            }

            // Get object text from binbuf (e.g. "osc~ 440", "vcf~ 1200 5")
            juce::String objectText;
            t_object* obj = pd::Interface::checkObject(gobj);
            if (obj) {
                char* text = nullptr;
                int len = 0;
                pd::Interface::getObjectText(obj, &text, &len);
                if (text && len > 0) {
                    objectText = juce::String::fromUTF8(text, len);
                    freebytes(text, len);
                }
            }

            // Get position
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(cnv, gobj, &x, &y, &w, &h);

            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/typeof/reply/" + correlationId) };
            reply.addArgument(className);
            reply.addArgument(objectText);
            reply.addArgument(static_cast<int32>(x));
            reply.addArgument(static_cast<int32>(y));
            reply.addArgument(static_cast<int32>(w));
            reply.addArgument(static_cast<int32>(h));
            sender.send(reply);
        }
        return;
    }

    if (action == "ports") {
        // /pd/ports <canvasName> <tempId_or_index> <correlationId>
        // Returns: /pd/ports/reply/<corrId> <numInlets> <numOutlets> <inletTypes...> <outletTypes...>
        // inletTypes/outletTypes: "s" for signal, "c" for control
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto targetId = getArgString(msg[1]);
            auto correlationId = getArgString(msg[2]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Resolve by stableId first, fallback to numeric index
            t_gobj* gobj = processor->resolveStableId(canvasName, targetId);
            if (!gobj) {
                bool isNumber = targetId.isNotEmpty();
                for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
                    if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                        isNumber = false;
                        break;
                    }
                }
                if (isNumber) {
                    gobj = glistObjectAt(cnv, targetId.getIntValue());
                }
            }

            if (!gobj) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Object not found: " + targetId);
                return;
            }

            t_object* obj = pd::Interface::checkObject(gobj);
            if (!obj) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Target is not a valid Pd object: " + targetId);
                return;
            }

            int numInlets = obj_ninlets(obj);
            int numOutlets = obj_noutlets(obj);

            // Build type strings: "s" = signal, "c" = control
            juce::String inletTypes;
            for (int i = 0; i < numInlets; i++) {
                inletTypes += obj_issignalinlet(obj, i) ? "s" : "c";
            }

            juce::String outletTypes;
            for (int i = 0; i < numOutlets; i++) {
                outletTypes += obj_issignaloutlet(obj, i) ? "s" : "c";
            }

            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/ports/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(numInlets));
            reply.addArgument(static_cast<int32>(numOutlets));
            reply.addArgument(inletTypes);
            reply.addArgument(outletTypes);
            sender.send(reply);
        }
        return;
    }

    if (action == "identity_snapshot") {
        // /pd/identity_snapshot <canvasName> <correlationId>
        // Returns a JSON string with the complete identity state:
        // { "version": N, "canvas": "pd-main", "entries": [
        //   { "id": "fm_carrier", "index": 3, "type": "osc~", "text": "osc~ 440" },
        //   ...
        // ]}
        // Single O(n) walk of gl_list builds a reverse pointer→index map, then
        // iterates the stable map. No per-entry O(n) scan. Total: O(n+m).
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/identity_snapshot/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Step 1: Build pointer→index map with ONE walk of gl_list (O(n))
            std::unordered_map<t_gobj*, int> ptrToIndex;
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                ptrToIndex[y] = idx++;
            }
            int totalObjects = idx;

            // Step 2: Walk the identity map and validate each entry (O(m))
            auto canvasStr = canvasName.toStdString();
            auto& canvasMap = processor->mcpStableObjectMap[canvasStr];
            uint64_t version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);

            auto* rootObj = new juce::DynamicObject();
            rootObj->setProperty("version", static_cast<juce::int64>(version));
            rootObj->setProperty("canvas", canvasName);
            rootObj->setProperty("totalObjects", totalObjects);

            juce::Array<juce::var> entries;

            std::vector<std::string> toEvict;
            for (auto& [tempId, ptr] : canvasMap) {
                auto ptrIt = ptrToIndex.find(ptr);
                if (ptrIt == ptrToIndex.end()) {
                    // Object no longer on canvas — mark for eviction
                    toEvict.push_back(tempId);
                    continue;
                }

                // Serial verification (UAF defense)
                auto serialIt = processor->mcpStableSerialMap.find(ptr);
                if (serialIt == processor->mcpStableSerialMap.end()) {
                    toEvict.push_back(tempId);
                    continue;
                }

                auto* entry = new juce::DynamicObject();
                entry->setProperty("id", juce::String(tempId));
                entry->setProperty("index", ptrIt->second);

                // Read class name
                juce::String className = juce::String::fromUTF8(class_getname(pd_class(&ptr->g_pd)));
                entry->setProperty("class", className);

                // Read object text (type + args)
                t_object* obj = pd::Interface::checkObject(ptr);
                if (obj) {
                    char* text = nullptr;
                    int len = 0;
                    pd::Interface::getObjectText(obj, &text, &len);
                    if (text && len > 0) {
                        entry->setProperty("text", juce::String::fromUTF8(text, len));
                        freebytes(text, len);
                    }
                    entry->setProperty("inlets", obj_ninlets(obj));
                    entry->setProperty("outlets", obj_noutlets(obj));
                }

                entries.add(juce::var(entry));
            }

            // Evict stale entries
            for (auto& staleId : toEvict) {
                auto staleIt = canvasMap.find(staleId);
                if (staleIt != canvasMap.end()) {
                    processor->mcpStableSerialMap.erase(staleIt->second);
                    canvasMap.erase(staleIt);
                }
            }
            if (!toEvict.empty()) {
                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);
                rootObj->setProperty("version", static_cast<juce::int64>(version));
                rootObj->setProperty("evicted", static_cast<int>(toEvict.size()));
            }

            rootObj->setProperty("entryCount", entries.size());
            rootObj->setProperty("entries", entries);

            juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);
            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_snapshot/reply/" + correlationId) };
            reply.addArgument(jsonString);
            sender.send(reply);
        }
        return;
    }

    if (action == "identity_version") {
        // /pd/identity_version <correlationId>
        // Returns the current identity version counter.
        // Ultra-cheap: one atomic read, no locks, no canvas walk.
        // Node.js uses this to decide if a full snapshot fetch is needed.
        if (msg.size() >= 1 && processor) {
            auto correlationId = getArgString(msg[0]);
            uint64_t version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_version/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(static_cast<int>(version & 0x7FFFFFFF)));
            sender.send(reply);
        }
        return;
    }

    if (action == "batch_atomic") {
        // /pd/batch_atomic <canvas> <corrId> <deleteCount> <disconnectCount> <editCount> <createCount> <connectCount> [data...]
        // Performs delete + disconnect + edit + create + connect in ONE sys_lock with exactly ONE DSP recompile.
        // Protocol:
        //   Deletes: [tempId] * deleteCount
        //   Disconnects: [srcId, srcOut, destId, destIn] * disconnectCount
        //   Edits: [tempId, newType, nargs, arg1..argN] * editCount
        //   Creates: [tempId, x, y, kind, type, nargs, arg1..argN] * createCount
        //   Connects: [srcId, srcOut, destId, destIn] * connectCount
        // Reply: /pd/batch_atomic/reply/<corrId> <deletedCount> <disconnectedCount> <editedCount> <createdCount> <connectedCount> [inline mappings...]
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            
            int deleteCount = 0, disconnectCount = 0, editCount = 0, createCount = 0, connectCount = 0;
            int cursor = 2;

            if (msg.size() >= 7) {
                // 5-phase extended atomic protocol
                deleteCount = static_cast<int>(getArgFloat(msg[cursor++]));
                disconnectCount = static_cast<int>(getArgFloat(msg[cursor++]));
                editCount = static_cast<int>(getArgFloat(msg[cursor++]));
                createCount = static_cast<int>(getArgFloat(msg[cursor++]));
                connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
            } else {
                // Legacy 3-phase protocol fallback (creates, connects, edits)
                createCount = static_cast<int>(getArgFloat(msg[cursor++]));
                connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
                editCount = static_cast<int>(getArgFloat(msg[cursor++]));
            }

            int deleted = 0, disconnected = 0, edited = 0, created = 0, connected = 0;
            int reconcileEvicted = 0, reconcileAdopted = 0;
            std::vector<std::string> createdIds;
            std::vector<t_gobj*> createdPtrs;

            // =========================================================================
            // PHASE 0: PRE-PARSE outside audio thread — zero contention
            // =========================================================================
            struct PreDelete { juce::String objectId; };
            std::vector<PreDelete> preDeletes;
            for (int d = 0; d < deleteCount && cursor < msg.size(); d++)
                preDeletes.push_back({ getArgString(msg[cursor++]) });

            struct PreDisconnect { juce::String srcId; int srcOut; juce::String destId; int destIn; };
            std::vector<PreDisconnect> preDisconnects;
            for (int dc = 0; dc < disconnectCount && cursor + 3 < msg.size(); dc++) {
                auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
                auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
                preDisconnects.push_back({ s, so, d2, di });
            }

            struct PreEdit { juce::String objectId; juce::String newText; };
            std::vector<PreEdit> preEdits;
            for (int e = 0; e < editCount && cursor < msg.size(); e++) {
                auto oid = getArgString(msg[cursor++]);
                auto nt = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
                juce::StringArray tk;
                if (nt != "msg" && nt.isNotEmpty()) tk.add(nt);
                for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));
                preEdits.push_back({ oid, tk.joinIntoString(" ") });
            }

            struct PendingCreate { juce::String tempId; juce::String objType; float initValue; bool seedInit; };
            std::vector<PendingCreate> pendingCreates;
            juce::String pastaBuffer;
            for (int o = 0; o < createCount && cursor < msg.size(); o++) {
                auto oid = getArgString(msg[cursor++]);
                float px = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
                float py = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
                auto kind = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                auto ot = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
                juce::StringArray tk;
                if (ot.isNotEmpty()) tk.add(ot);
                for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));

                // line~ in this fork takes NO creation arguments (line_tilde_new(void)),
                // so `[line~ 0.057]` is silently created at 0. Capture a numeric init
                // arg here and re-seed it onto the object right after paste, so
                // `[r x] -> [line~ 440] -> [osc~]` actually starts at 440 instead of 0.
                bool seedInit = false;
                float initValue = 0.0f;
                if ((ot == "line~" || ot == "vline~") && na >= 1 && tk.size() >= 2) {
                    juce::String firstArg = tk[1];
                    if (firstArg.isNotEmpty()) {
                        auto charptr = firstArg.getCharPointer();
                        auto ptr = charptr;
                        juce::CharacterFunctions::readDoubleValue(ptr);
                        if (ptr - charptr == firstArg.getNumBytesAsUTF8()) {
                            initValue = firstArg.getFloatValue();
                            seedInit = true;
                        }
                    }
                }

                pastaBuffer += formatAsPdLine(kind, tk, static_cast<int>(px), static_cast<int>(py)) + "\n";
                pendingCreates.push_back({ oid, ot, initValue, seedInit });
            }

            // ALL connections go through obj_connect after paste (no #X connect in buffer)
            struct CrossConn { juce::String srcId; int srcOut; juce::String destId; int destIn; };
            std::vector<CrossConn> allConns;
            for (int c = 0; c < connectCount && cursor + 3 < msg.size(); c++) {
                auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
                auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
                allConns.push_back({ s, so, d2, di });
            }

            // =========================================================================
            // EXECUTE ON AUDIO THREAD — zero lock contention, zero dropout
            // =========================================================================

            // Storage for PHASE 0 reconcile results (filled inside lambda, read outside)
            struct DetectedConn { std::string srcId; int srcOut; std::string destId; int destIn; };
            std::vector<DetectedConn> detectedConnections;
            // Manual GUI patching has zero dropout because it enqueues mutations into
            // functionQueue, which runs on the audio thread during sendMessagesFromQueue()
            // BEFORE performDSP(). We do exactly the same thing here.

            extern int dsp_update_deferred;  // defined in m_obj.c
            juce::WaitableEvent done;
            t_canvas* cnv = nullptr;

            processor->enqueueFunctionAsync([&]() {
                auto tLambdaStart = std::chrono::high_resolution_clock::now();
                cnv = processor->getCanvasBySymbol(canvasName);
                if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

                if (cnv) {
                    dsp_update_deferred = 1;

                    // PHASE 0: PRE-FLIGHT AUTO-RECONCILE
                    // Single walk of gl_list to sync identity map with live canvas.
                    // Detects manual GUI edits (user created/deleted objects) and
                    // fixes the map BEFORE any mutation — zero extra OSC round-trips,
                    // zero audio dropout (runs inside the same audio-thread lambda).
                    {
                        auto canvasStr = canvasName.toStdString();
                        auto& canvasMap = processor->mcpStableObjectMap[canvasStr];

                        // Step A: Build set of all live pointers from gl_list (O(n))
                        std::unordered_set<t_gobj*> livePointers;
                        for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                            livePointers.insert(y);

                        // Step B: Evict stale entries — user deleted objects via GUI
                        std::vector<std::string> toEvict;
                        for (auto& [tempId, ptr] : canvasMap) {
                            if (livePointers.find(ptr) == livePointers.end()) {
                                toEvict.push_back(tempId);
                                processor->mcpStableSerialMap.erase(ptr);
                            }
                        }
                        for (auto& id : toEvict) {
                            canvasMap.erase(id);
                            processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
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

                        int adoptIdx = 0;
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
                                } else {
                                    juce::String className = juce::String::fromUTF8(
                                        class_getname(pd_class(&y->g_pd)));
                                    className = className.replace("~", "_t");
                                    autoId = ("gui_" + className + "_"
                                        + juce::String(adoptIdx++)).toStdString();
                                    while (canvasMap.count(autoId))
                                        autoId = ("gui_" + className + "_"
                                            + juce::String(adoptIdx++)).toStdString();
                                }
                                canvasMap[autoId] = y;
                                processor->mcpStableSerialMap[y] = processor->mcpSerialCounter++;
                                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                reconcileAdopted++;
                            }
                        }

                        // Step D: Connection scan — detect wires drawn manually by user.
                        // linetraverser walks every wire on the canvas in O(wires).
                        // Both endpoints already have tempIds after Steps B+C above.
                        // We build a compact flat list: [srcId, srcOut, destId, destIn, ...]
                        // appended to the batch_atomic reply so TS gets the full picture
                        // in one shot — zero extra OSC round-trips.
                        // Build reverse ptr→tempId map (O(m))
                        std::unordered_map<t_gobj*, std::string> ptrToTempId;
                        for (auto& [tid, ptr] : canvasMap)
                            if (ptr) ptrToTempId[ptr] = tid;

                        t_linetraverser lt;
                        linetraverser_start(&lt, cnv);
                        t_outconnect* ltOc = nullptr;
                        while ((ltOc = linetraverser_next_nosize(&lt))) {
                            t_gobj* sg = &lt.tr_ob->ob_g;
                            t_gobj* dg = &lt.tr_ob2->ob_g;
                            auto sIt = ptrToTempId.find(sg);
                            auto dIt = ptrToTempId.find(dg);
                            if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
                            detectedConnections.push_back({
                                sIt->second,
                                static_cast<int>(lt.tr_outno),
                                dIt->second,
                                static_cast<int>(lt.tr_inno)
                            });
                        }
                    }

                    // PHASE 1: DELETE
                    for (auto& pd : preDeletes) {
                        t_gobj* obj = processor->resolveStableId(canvasName, pd.objectId);
                        if (obj) {
                            processor->mcpStableObjectMap[canvasName.toStdString()].erase(pd.objectId.toStdString());
                            processor->mcpStableSerialMap.erase(obj);
                            processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                            SmallArray<t_gobj*> toDelete; toDelete.add(obj);
                            pd::Interface::removeObjects(cnv, toDelete);
                            deleted++;
                        }
                    }

                    // PHASE 2: DISCONNECT
                    for (auto& pdc : preDisconnects) {
                        t_gobj* sg = processor->resolveStableId(canvasName, pdc.srcId);
                        t_gobj* dg = processor->resolveStableId(canvasName, pdc.destId);
                        if (sg && dg) {
                            t_object* so = pd::Interface::checkObject(sg);
                            t_object* d_o = pd::Interface::checkObject(dg);
                            if (so && d_o) {
                                int si = 0, di2 = 0, idx = 0;
                                for (t_gobj* y = cnv->gl_list; y; y = y->g_next, idx++) {
                                    if (y == sg) si = idx; if (y == dg) di2 = idx;
                                }
                                t_atom ca[4];
                                SETFLOAT(&ca[0], static_cast<float>(si));
                                SETFLOAT(&ca[1], static_cast<float>(pdc.srcOut));
                                SETFLOAT(&ca[2], static_cast<float>(di2));
                                SETFLOAT(&ca[3], static_cast<float>(pdc.destIn));
                                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("disconnect"), 4, ca);
                                disconnected++;
                            }
                        }
                    }

                    // PHASE 3: EDIT
                    for (auto& pe : preEdits) {
                        t_gobj* obj = processor->resolveStableId(canvasName, pe.objectId);
                        if (obj) {
                            t_object* o = pd::Interface::checkObject(obj);
                            if (o) {
                                pd::Interface::renameObject(cnv, obj, pe.newText.toRawUTF8(), pe.newText.length());
                                t_gobj* newObj = nullptr; bool still = false;
                                for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                                    if (y == obj) { still = true; break; }
                                newObj = still ? obj : pd::Interface::getNewest(cnv);
                                if (newObj) {
                                    processor->mcpStableObjectMap[canvasName.toStdString()][pe.objectId.toStdString()] = newObj;
                                    if (newObj != obj) processor->mcpStableSerialMap.erase(obj);
                                    processor->mcpStableSerialMap[newObj] = processor->mcpSerialCounter++;
                                    processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                    edited++;
                                }
                            }
                        }
                    }

                    // PHASE 4: CREATE via pasteDirect
                    int preCreateCount = 0;
                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) preCreateCount++;

                    if (createCount > 0 && pastaBuffer.isNotEmpty()) {
                        pasteDirect(cnv, pastaBuffer.toRawUTF8());

                        std::vector<t_gobj*> allObjects;
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                            allObjects.push_back(g);
                        int newCount = static_cast<int>(allObjects.size()) - preCreateCount;
                        for (int i = 0; i < newCount && i < static_cast<int>(pendingCreates.size()); i++) {
                            t_gobj* newObj = allObjects[preCreateCount + i];
                            auto& pc = pendingCreates[i];
                            processor->mcpStableObjectMap[canvasName.toStdString()][pc.tempId.toStdString()] = newObj;
                            processor->mcpStableSerialMap[newObj] = processor->mcpSerialCounter++;
                            processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

                            // Auto-seed line~/vline~ init value (fork ignores creation args).
                            if (pc.seedInit) {
                                t_object* so = pd::Interface::checkObject(newObj);
                                if (so) {
                                    t_atom sa;
                                    SETFLOAT(&sa, pc.initValue);
                                    pd_typedmess(reinterpret_cast<t_pd*>(so), gensym("float"), 1, &sa);
                                }
                            }

                            createdIds.push_back(pc.tempId.toStdString());
                            createdPtrs.push_back(newObj);
                            created++;
                        }
                    }

                    // PHASE 5: CONNECT via obj_connect
                    for (auto& cc : allConns) {
                        t_gobj* sg = processor->resolveStableId(canvasName, cc.srcId);
                        t_gobj* dg = processor->resolveStableId(canvasName, cc.destId);
                        if (sg && dg) {
                            t_object* so = pd::Interface::checkObject(sg);
                            t_object* d_o = pd::Interface::checkObject(dg);
                            if (so && d_o) {
                                if (obj_connect(so, cc.srcOut, d_o, cc.destIn))
                                    connected++;
                            }
                        }
                    }

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

                auto tLambdaEnd = std::chrono::high_resolution_clock::now();
                auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(tLambdaEnd - tLambdaStart).count();
                post("batch_atomic: TOTAL lambda took %lld us", (long long)totalUs);

                done.signal();
            }); // end enqueueFunctionAsync lambda

            // Wait for audio thread to complete (max 2000ms)
            auto tWaitStart = std::chrono::high_resolution_clock::now();
            done.wait(2000);
            auto tWaitEnd = std::chrono::high_resolution_clock::now();
            auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(tWaitEnd - tWaitStart).count();
            post("batch_atomic: done.wait() took %lld us (%.1f ms)", (long long)waitUs, waitUs / 1000.0f);

            // Decoupled UI viewport sync - non-blocking async idle dispatch
            juce::MessageManager::callAsync([p = processor] {
                if (p) p->synchroniseCanvases();
            });

            // Build reply with counts and inline identity mappings
            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/batch_atomic/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(created));
            reply.addArgument(static_cast<int32>(connected));
            reply.addArgument(static_cast<int32>(edited));
            reply.addArgument(static_cast<int32>(deleted));
            reply.addArgument(static_cast<int32>(disconnected));
            // PHASE 0 reconcile counts (appended — backward compatible)
            reply.addArgument(static_cast<int32>(reconcileEvicted));
            reply.addArgument(static_cast<int32>(reconcileAdopted));

            // Append inline mappings — built from data collected in lambda
            // The identity mapping was already done inside the lambda (createdIds/createdPtrs).
            // We just need the index. Enqueue a quick lookup on audio thread.
            std::vector<int32> mappingIndices(createdIds.size(), -1);
            if (cnv && !createdIds.empty()) {
                juce::WaitableEvent mapDone;
                processor->enqueueFunctionAsync([&]() {
                    std::unordered_map<t_gobj*, int> ptrToIdx;
                    int idx = 0;
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, idx++)
                        ptrToIdx[y] = idx;
                    for (size_t i = 0; i < createdPtrs.size(); i++) {
                        auto it = ptrToIdx.find(createdPtrs[i]);
                        if (it != ptrToIdx.end()) mappingIndices[i] = it->second;
                    }
                    mapDone.signal();
                });
                mapDone.wait(500);
            }
            for (size_t i = 0; i < createdIds.size(); i++) {
                if (mappingIndices[i] >= 0) {
                    reply.addArgument(juce::String(createdIds[i]));
                    reply.addArgument(static_cast<int32>(mappingIndices[i]));
                }
            }

            // Append PHASE 0 detected connections as a compact JSON string.
            // Format: [{"s":"srcId","so":0,"d":"destId","di":0}, ...]
            // Sent as a single OSC string arg — backward compatible (TS ignores if absent).
            if (!detectedConnections.empty()) {
                juce::String dcJson = "[";
                for (size_t i = 0; i < detectedConnections.size(); i++) {
                    auto& dc = detectedConnections[i];
                    if (i > 0) dcJson += ",";
                    dcJson += "{\"s\":\"" + juce::String(dc.srcId)
                           + "\",\"so\":" + juce::String(dc.srcOut)
                           + ",\"d\":\"" + juce::String(dc.destId)
                           + "\",\"di\":" + juce::String(dc.destIn) + "}";
                }
                dcJson += "]";
                reply.addArgument(dcJson);
            }

            sender.send(reply);
        }
        return;
    }

    if (action == "connections") {
        // /pd/connections <canvasName> <correlationId>
        // Returns JSON array of all connections on the canvas.
        // Uses linetraverser + single O(n) ptr→index map. No file dump.
        // Response: /pd/connections/reply/<corrId> <jsonString>
        // JSON: { "canvas":"pd-main", "count": N, "connections": [
        //   { "srcIndex":0, "srcOut":0, "destIndex":1, "destIn":0, "srcId":"osc", "destId":"dac" }, ...
        // ]}
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/connections/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Build ptr→index map with single O(n) walk
            std::unordered_map<t_gobj*, int> ptrToIndex;
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                ptrToIndex[y] = idx++;
            }

            // Build reverse identity map (ptr → tempId)
            auto canvasStr = canvasName.toStdString();
            std::unordered_map<t_gobj*, juce::String> ptrToId;
            if (processor->mcpStableObjectMap.count(canvasStr)) {
                for (auto const& [id, ptr] : processor->mcpStableObjectMap[canvasStr]) {
                    if (ptr) ptrToId[ptr] = juce::String(id);
                }
            }

            // Walk connections with linetraverser
            auto* rootObj = new juce::DynamicObject();
            rootObj->setProperty("canvas", canvasName);

            juce::Array<juce::var> connArray;
            int connCount = 0;

            t_linetraverser t;
            linetraverser_start(&t, cnv);
            t_outconnect* oc = nullptr;
            while ((oc = linetraverser_next_nosize(&t))) {
                t_gobj* srcGobj = &t.tr_ob->ob_g;
                t_gobj* destGobj = &t.tr_ob2->ob_g;

                auto srcIt = ptrToIndex.find(srcGobj);
                auto destIt = ptrToIndex.find(destGobj);
                if (srcIt == ptrToIndex.end() || destIt == ptrToIndex.end()) continue;

                auto* c = new juce::DynamicObject();
                c->setProperty("srcIndex", srcIt->second);
                c->setProperty("srcOut", t.tr_outno);
                c->setProperty("destIndex", destIt->second);
                c->setProperty("destIn", t.tr_inno);
                c->setProperty("srcId", ptrToId.count(srcGobj) ? ptrToId[srcGobj] : juce::String());
                c->setProperty("destId", ptrToId.count(destGobj) ? ptrToId[destGobj] : juce::String());

                connArray.add(juce::var(c));
                connCount++;
            }

            rootObj->setProperty("count", connCount);
            rootObj->setProperty("connections", connArray);

            juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);
            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/connections/reply/" + correlationId) };
            reply.addArgument(jsonString);
            sender.send(reply);
        }
        return;
    }

    if (action == "dump") {
        if (msg.size() >= 3) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto dumpFile = getArgString(msg[1]);
            auto dumpDir = getArgString(msg[2]);
            auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "";

            sys_lock();
            t_canvas* cnv = processor ? processor->getCanvasBySymbol(canvasName) : nullptr;
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                t_binbuf* b = binbuf_new();
                canvas_savetemplatesto(cnv, b, 1);
                canvas_saveto(cnv, b);
                binbuf_write(b, dumpFile.toRawUTF8(), dumpDir.toRawUTF8(), 0);
                binbuf_free(b);
            }
            sys_unlock();

            juce::String replyAddress = correlationId.isNotEmpty() ? "/pd/dumped/" + correlationId : "/pd/dumped";
            sendReply(replyAddress, dumpFile);
        }
        return;
    }

    if (action == "load") {
        if (msg.size() >= 2 && processor) {
            auto file = getArgString(msg[0]);
            auto folder = getArgString(msg[1]);
            sys_lock();
            t_atom args[2];
            SETSYMBOL(&args[0], gensym(file.toRawUTF8()));
            SETSYMBOL(&args[1], gensym(folder.toRawUTF8()));
            pd_typedmess(gensym("pd")->s_thing, gensym("open"), 2, args);
            sys_unlock();
            sendRawReply("/pd/loaded");
        }
        return;
    }

    // /pd/save_content <canvasName> <destFilePath> [correlationId]
    // Serialises the live canvas to a .pd file using getCanvasContent() — pure
    // in-memory path, no binbuf_write overhead. Faster than /pd/dump and does
    // not require a separate dumpDir argument.
    // Reply: /pd/save_content/reply/<corrId>  arg0=destFilePath  (or error string)
    if (action == "save_content") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto destFilePath  = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String content;
            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                char* buf = nullptr;
                int   bufsize = 0;
                pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
                if (buf) {
                    content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                    freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                }
            }
            sys_unlock();

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/save_content/reply/" + correlationId
                : "/pd/save_content/reply";

            if (content.isEmpty()) {
                sendReply(replyAddr, juce::String("error: canvas not found or empty"));
                return;
            }

            // Write content to destination path provided by TS
            juce::File destFile(destFilePath);
            if (!destFile.replaceWithText(content)) {
                sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
                return;
            }

            sendReply(replyAddr, destFilePath);
        }
        return;
    }

     // /pd/record_start <filePath> [corrId]
    // Zero-dropout WAV recorder — taps processBlock output buffer directly.
    // No canvas objects created, no DSP recompile, no audio dropout.
    // The WAV writer is created lazily on the audio thread (first tap) so its
    // channel count always matches the real output buffer — getTotalNumOutputChannels()
    // can report a bus layout (e.g. 32ch) that differs from the actual buffer (e.g. 2ch).
    // Reply: /pd/record_start/reply/<corrId>  1.0=ok, 0.0=failed
    if (action == "record_start") {
        if (msg.size() >= 1 && processor) {
            auto filePath      = getArgString(msg[0]);
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
            juce::String replyAddr = "/pd/record_start/reply/" + correlationId;

            // Fail fast if we can't create the parent directory.
            juce::File destFile(filePath);
            auto parent = destFile.getParentDirectory();
            if (!parent.createDirectory().wasOk()) {
                post("MCP record_start: could not create directory %s", parent.getFullPathName().toRawUTF8());
                sendReply(replyAddr, 0.0f);
                return;
            }

            {
                const juce::ScopedLock sl(processor->mcpRecorderLock);
                processor->mcpWavWriter.reset();      // drop any stale writer
                processor->mcpRecorderPath = filePath;
                processor->mcpRecording.store(true, std::memory_order_release);
            }
            post("MCP record_start: armed (lazy writer) path=%s", filePath.toRawUTF8());
            sendReply(replyAddr, 1.0f);
        }
        return;
    }

    // /pd/record_stop [corrId]
    // Stop the zero-dropout WAV recorder and flush/close the file.
    // Reply: /pd/record_stop/reply/<corrId>  1.0=ok
    if (action == "record_stop") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        if (processor) {
            processor->mcpRecording.store(false, std::memory_order_release);
            {
                const juce::ScopedLock sl(processor->mcpRecorderLock);
                processor->mcpWavWriter.reset(); // flushes and closes the file
            }
        }
        sendReply("/pd/record_stop/reply/" + correlationId, 1.0f);
        return;
    }

    // /pd/load_content <canvasName> <srcFilePath> [correlationId]
    // Clears the canvas and reconstructs it atomically from a .pd file using
    // binbuf_read + binbuf_eval — Pd's native file loader. Handles the full
    // .pd format (including #N canvas header) correctly.
    // DSP recompiles once at the end. Disk read is outside sys_lock.
    // Reply: /pd/load_content/reply/<corrId>  arg0=objectCount (float)
    if (action == "load_content") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/load_content/reply/" + correlationId
                : "/pd/load_content/reply";

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, -1.0f);
                return;
            }

            // Split into dir + filename for binbuf_read
            auto fileDir  = srcFile.getParentDirectory().getFullPathName();
            auto fileName = srcFile.getFileName();

            int objectCount = 0;

            // Stop all active probes before clearing — they hold t_outconnect*
            // pointers that become dangling after canvas clear.
            probeManager.stopAllProbes("load_content_reset");

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                // 1. Clear existing objects
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("clear"), 0, nullptr);

                // 2. Load .pd file — strip the #N canvas header line then
                //    use pasteDirect() which correctly handles both #X obj
                //    and #X connect lines via binbuf_eval with sym_X bound
                //    to the canvas. This is the same path batch_atomic uses.
                t_binbuf* b = binbuf_new();
                if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                    // Find the end of the first statement (#N canvas ... ;)
                    // and build a new binbuf from the rest (#X lines only).
                    int natom = binbuf_getnatom(b);
                    t_atom* vec = binbuf_getvec(b);
                    int startIdx = 0;
                    for (int i = 0; i < natom; i++) {
                        if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                    }
                    if (startIdx < natom) {
                        // Serialise remaining atoms back to text for pasteDirect
                        t_binbuf* body = binbuf_new();
                        binbuf_add(body, natom - startIdx, vec + startIdx);
                        char* textBuf = nullptr;
                        int   textLen = 0;
                        binbuf_gettext(body, &textBuf, &textLen);
                        if (textBuf && textLen > 0) {
                            // pasteDirect binds sym_X to canvas — handles both
                            // #X obj and #X connect correctly
                            pasteDirect(cnv, juce::String::fromUTF8(textBuf, textLen).toRawUTF8());
                            freebytes(textBuf, textLen);
                        }
                        binbuf_free(body);
                    }
                }
                binbuf_free(b);

                // 3. Fire loadbangs on any newly created subpatches
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                    if (pd_class(&g->g_pd) == canvas_class)
                         canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                }

                // 4. DSP graph recompile
                canvas_update_dsp();

                // Count objects
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                    objectCount++;
            }
            sys_unlock();

            // Synchronise canvas UI and restart DSP.
            // startDSP() is called directly here — same pattern as /pd/dsp
            // handler which also calls it from the OSC receiver thread.
            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            if (objectCount > 0 && processor)
                processor->startDSP();

            // 5. Clear identity map — PHASE 0 reconcile fires on next batch_atomic
            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol("0")));
            processor->receiveSysMessage("mcp_clear_ids", atoms);

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply(replyAddr, static_cast<float>(objectCount));
        }
        return;
    }

    // ── PRD Phase 3 §2.1: NATIVE SAVE WITH IDENTITY SIDECAR ─────────────
    // /pd/save_patch <canvas> <filePath> [corrId]
    // C++ owns the save: getCanvasContent writes the .pd, and a sidecar
    // (<file>.mcpids.json) carries the stable tempId map AT SAVE TIME —
    // index→tempId for the root canvas plus every named subcanvas. Identity
    // never crosses the TS boundary, so a later load can restore it exactly
    // (no positional guessing). Vanilla Pd sees a normal .pd file.
    // Reply: /pd/save_patch/reply/<corrId> <destFilePath> | "error: ..."
    if (action == "save_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto destFilePath  = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/save_patch/reply/" + correlationId
                : "/pd/save_patch/reply";

            juce::String content;
            juce::var sidecar;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                char* buf = nullptr;
                int   bufsize = 0;
                pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
                if (buf) {
                    content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                    freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                }

                if (content.isNotEmpty()) {
                    // Identity sidecar: reverse map (gobj → tempId) per canvas
                    auto buildIdArray = [&](const juce::String& mapKey, t_canvas* c) -> juce::var {
                        juce::Array<juce::var> arr;
                        std::unordered_map<t_gobj*, juce::String> ptrToId;
                        auto mapIt = processor->mcpStableObjectMap.find(mapKey.toStdString());
                        if (mapIt != processor->mcpStableObjectMap.end()) {
                            for (auto const& [id, ptr] : mapIt->second)
                                if (ptr) ptrToId[ptr] = juce::String(id);
                        }
                        int idx = 0;
                        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++idx) {
                            auto it = ptrToId.find(g);
                            if (it == ptrToId.end()) continue;
                            auto* o = new juce::DynamicObject();
                            o->setProperty("i", idx);
                            o->setProperty("id", it->second);
                            arr.add(juce::var(o));
                        }
                        return juce::var(arr);
                    };

                    auto* root = new juce::DynamicObject();
                    root->setProperty("format", juce::String("MCP-IDENTITY"));
                    root->setProperty("v", 1);
                    root->setProperty("identity_version",
                        (double)processor->mcpIdentityVersion.load(std::memory_order_relaxed));
                    root->setProperty("root", buildIdArray(canvasName, cnv));

                    // Named subcanvases: key "pd-<gl_name>" matches the map keys
                    // the bridge uses for subpatch census/mutations.
                    auto* subObj = new juce::DynamicObject();
                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                        if (pd_class(&g->g_pd) != canvas_class) continue;
                        t_canvas* child = reinterpret_cast<t_canvas*>(g);
                        if (!child->gl_name) continue;
                        juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
                        auto childIds = buildIdArray("pd-" + childName, child);
                        if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0)
                            subObj->setProperty(childName, childIds);
                    }
                    root->setProperty("sub", juce::var(subObj));
                    sidecar = juce::var(root);
                }
            }
            sys_unlock();

            if (content.isEmpty()) {
                sendReply(replyAddr, juce::String("error: canvas not found or empty"));
                return;
            }

            juce::File destFile(destFilePath);
            if (!destFile.replaceWithText(content)) {
                sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
                return;
            }
            // Sidecar next to the .pd — engine-private, invisible to vanilla Pd
            juce::File sidecarFile(destFilePath + ".mcpids.json");
            sidecarFile.replaceWithText(juce::JSON::toString(sidecar, true));

            // PRD Phase 3 §2.5 (save-binding): bind the file to the canvas the
            // way native Save-As does — tab title + current path update.
            // NOTE: deliberately setCurrentFile ONLY — Patch::savePatch(URL)
            // also runs reloadAbstractions() (recreates abstraction instances)
            // and crashed when a queued close_tab tore the canvas down mid-
            // reload. Title derives from currentFile; pd-side state untouched.
            processor->enqueueFunctionAsync([p = processor, cnv, destFilePath]() {
                juce::File f(destFilePath);
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (canvas && canvas->patch.getPointer().get() == cnv) {
                            canvas->patch.setCurrentFile(URL(f));
                            canvas->patch.setTitle(f.getFileName());
                            return;
                        }
                    }
                }
            });

            sendReply(replyAddr, destFilePath);
        }
        return;
    }

    // ── PRD Phase 3 §2.2: NATIVE LOAD WITH IDENTITY RESTORE ─────────────
    // /pd/load_patch <canvas> <filePath> [corrId]
    // Same canvas mechanics as load_content (clear + pasteDirect + loadbang +
    // canvas_update_dsp), then re-registers the sidecar tempIds DIRECTLY into
    // the stable map (root + named subcanvases) and bumps the version once.
    // Objects without a sidecar entry are auto-adopted by the Phase 2 census
    // naming on next census. No TS involvement, no positional guessing.
    // Reply: /pd/load_patch/reply/<corrId> <objectCount> (float, -1 = file missing)
    if (action == "load_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/load_patch/reply/" + correlationId
                : "/pd/load_patch/reply";

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, -1.0f);
                return;
            }

            // Sidecar read OUTSIDE sys_lock (small file, best-effort)
            juce::var sidecar;
            juce::File sidecarFile(srcFilePath + ".mcpids.json");
            if (sidecarFile.existsAsFile()) {
                auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
                if (auto* parsedObj = parsed.getDynamicObject()) {
                    if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY")) {
                        sidecar = parsed;
                    }
                }
            }

            auto fileDir  = srcFile.getParentDirectory().getFullPathName();
            auto fileName = srcFile.getFileName();
            int objectCount = 0;

            probeManager.stopAllProbes("load_patch_reset");

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                // 0. Reset identity for this canvas tree BEFORE re-registering
                processor->mcpStableObjectMap[canvasName.toStdString()].clear();

                // 1. Clear existing objects
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("clear"), 0, nullptr);

                // 2. Load .pd file — strip the #N canvas header, pasteDirect
                t_binbuf* b = binbuf_new();
                if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                    int natom = binbuf_getnatom(b);
                    t_atom* vec = binbuf_getvec(b);
                    int startIdx = 0;
                    for (int i = 0; i < natom; i++) {
                        if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                    }
                    if (startIdx < natom) {
                        t_binbuf* body = binbuf_new();
                        binbuf_add(body, natom - startIdx, vec + startIdx);
                        char* textBuf = nullptr;
                        int   textLen = 0;
                        binbuf_gettext(body, &textBuf, &textLen);
                        if (textBuf && textLen > 0) {
                            pasteDirect(cnv, juce::String::fromUTF8(textBuf, textLen).toRawUTF8());
                            freebytes(textBuf, textLen);
                        }
                        binbuf_free(body);
                    }
                }
                binbuf_free(b);

                // 3. Loadbangs on newly created subpatches
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                    if (pd_class(&g->g_pd) == canvas_class)
                         canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                }

                // 4. DSP graph recompile
                canvas_update_dsp();

                // 5. Re-register sidecar identities directly into the stable map
                juce::DynamicObject* sidecarObj = sidecar.getDynamicObject();
                int restored = mcpApplyIdentitySidecar(processor, sidecarObj, canvasName, cnv);
                (void)restored;

                for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                    objectCount++;
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            if (objectCount > 0 && processor)
                processor->startDSP();
            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply(replyAddr, static_cast<float>(objectCount));
        }
        return;
    }

    // ── PRD Phase 3 §2.5: OPEN PATCH IN NEW TAB ─────────────────────────
    // /pd/open_patch <filePath> [corrId]
    // Opens a .pd file in a NEW editor tab (native File→Open path), then
    // pre-registers the sidecar identities if present — the tab comes up
    // fully named. Reply: /pd/open_patch/reply/<corrId> "<status text>"
    if (action == "open_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
            juce::String replyAddr = "/pd/open_patch/reply/" + correlationId;

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, juce::String("error: file not found: " + srcFilePath));
                return;
            }

            // Phase 3.1 bugfix: the open lambda was observed to deliver twice
            // (two tabs for one request). Once-guard per file — any repeat
            // within 2s is ignored (idempotent open, root cause independent).
            static std::unordered_map<juce::String, juce::int64> lastOpenAtMs;
            auto nowMs = juce::Time::getMillisecondCounter();
            auto& lastRef = lastOpenAtMs[srcFile.getFullPathName()];
            if (nowMs - lastRef < 2000) {
                sendReply(replyAddr, juce::String("ignored: open already in progress for " + srcFilePath));
                return;
            }
            lastRef = nowMs;

            // PRD Phase 3 §2.5: duplicate-tab guard — PlugData focuses the
            // existing tab instead of duplicating a patch (by design). Reply
            // honestly instead of falling into the async no-reply path.
            bool alreadyOpen = false;
            {
                sys_lock();
                for (auto* editor : processor->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (canvas && canvas->patch.getCurrentFile() == srcFile) {
                            alreadyOpen = true;
                            break;
                        }
                    }
                    if (alreadyOpen) break;
                }
                sys_unlock();
            }
            if (alreadyOpen) {
                sendReply(replyAddr, juce::String("already open: " + srcFile.getFullPathName() + " (focused existing tab)"));
                return;
            }

            // Sidecar read outside the message-thread hop
            juce::var sidecar;
            juce::File sidecarFile(srcFile.getFullPathName() + ".mcpids.json");
            if (sidecarFile.existsAsFile()) {
                auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
                if (auto* parsedObj = parsed.getDynamicObject()) {
                    if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY"))
                        sidecar = parsed;
                }
            }

            // Acknowledge immediately — the tab-open runs on the message
            // thread (fire-and-forget); TS verifies via `tabs`/census.
            sendReply(replyAddr, juce::String("opening: " + srcFile.getFullPathName()));

            processor->enqueueFunctionAsync(
                [p = processor, fileStr = srcFile.getFullPathName(), sidecar]() {
                bool opened = false;
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    editor->getTabComponent().openPatch(juce::URL(juce::File(fileStr)));
                    opened = true;
                    break;
                }
                if (!opened) {
                    p->logMessage("MCP open_patch: no editor");
                    return;
                }

                // Phase 3.1 bugfix: belt-and-suspenders dedup — if more than
                // one tab for this file exists, keep the first and close extras.
                juce::File f(fileStr);
                bool keptFirst = false;
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                        if (!keptFirst) { keptFirst = true; continue; }
                        editor->getTabComponent().closeTab(canvas);
                        p->logMessage("MCP open_patch: closed duplicate tab for " + fileStr);
                    }
                }

                // PRD Phase 3 §2.5: sidecar registration via the proven async
                // mcp_register_id path — NO sys_lock in this lambda (lock
                // inversion with the audio thread crashes the message thread).
                int queued = 0;
                if (auto* sidecarObj = sidecar.getDynamicObject()) {
                    auto queueRegisters = [&p, &queued](juce::var const& idArray, juce::String const& canvasKey, t_canvas* target) {
                        const auto* arr = idArray.getArray();
                        if (arr == nullptr) return;
                        int idx = 0;
                        for (t_gobj* g = target->gl_list; g; g = g->g_next, ++idx) {
                            // find the sidecar entry for this index
                            for (auto const& v : *arr) {
                                if (auto* o = v.getDynamicObject()) {
                                    if (static_cast<int>(static_cast<double>(o->getProperty("i"))) != idx) continue;
                                    juce::String id = o->getProperty("id").toString();
                                    if (id.isEmpty()) break;
                                    SmallArray<pd::Atom> atoms;
                                    atoms.add(pd::Atom(p->generateSymbol(canvasKey)));
                                    atoms.add(pd::Atom(p->generateSymbol("0")));
                                    atoms.add(pd::Atom(static_cast<float>(idx)));
                                    atoms.add(pd::Atom(p->generateSymbol(id)));
                                    p->receiveSysMessage("mcp_register_id", atoms);
                                    queued++;
                                    break;
                                }
                            }
                        }
                    };

                    juce::File f(fileStr);
                    for (auto* editor : p->getEditors()) {
                        if (!editor) continue;
                        for (auto* canvas : editor->getCanvases()) {
                            if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                            if (auto* cnv = canvas->patch.getPointer().get()) {
                                // PRD Phase 3.1: register under the tab's bound
                                // name (gl_name = file name) so named-tab
                                // addressing finds these identities.
                                juce::String boundName = cnv->gl_name
                                    ? juce::String::fromUTF8(cnv->gl_name->s_name)
                                    : juce::String(f.getFileName());
                                queueRegisters(sidecarObj->getProperty("root"), boundName, cnv);
                                // named subcanvases
                                if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
                                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                                        if (pd_class(&g->g_pd) != canvas_class) continue;
                                        auto* child = reinterpret_cast<t_canvas*>(g);
                                        if (!child->gl_name) continue;
                                        juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
                                        auto childIds = subObj->getProperty(juce::Identifier(childName));
                                        if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0)
                                            queueRegisters(childIds, "pd-" + childName, child);
                                    }
                                }
                            }
                            break;
                        }
                        break;
                    }
                }

                p->logMessage(juce::String("MCP open_patch: ") + fileStr
                    + (queued > 0 ? " (" + juce::String(queued) + " identities queued)" : ""));
                (void)opened;
            });
        }
        return;
    }

    // ── PRD Phase 3 §2.5: LIST OPEN TABS ────────────────────────────────
    // /pd/list_tabs [corrId]
    // Enumerates every open root tab: title, file, focused flag, object
    // count. Reply: /pd/list_tabs/reply/<corrId> <json>
    if (action == "list_tabs") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        juce::String replyAddr = "/pd/list_tabs/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, replyAddr]() {
            juce::Array<juce::var> list;
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                auto* focused = editor->getCurrentCanvas();
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    auto* o = new juce::DynamicObject();
                    o->setProperty("title", canvas->patch.getTitle());
                    o->setProperty("file", canvas->patch.getCurrentFile().getFullPathName());
                    o->setProperty("focused", canvas == focused);
                    int count = 0;
                    sys_lock();
                    if (auto* cnv = canvas->patch.getPointer().get()) {
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) count++;
                    }
                    sys_unlock();
                    o->setProperty("objects", count);
                    list.add(juce::var(o));
                }
            }
            auto* root = new juce::DynamicObject();
            root->setProperty("tabs", list);
            bridge->sendReply(replyAddr, juce::JSON::toString(juce::var(root)));
        });
        return;
    }

    // ── PRD Phase 3 §2.5: FOCUS TAB ─────────────────────────────────────
    // /pd/focus_tab <file-or-name> [corrId]
    // Makes the named tab the focused one — "main" then points at it.
    // Matches by full path or file name. Reply: .../reply/<corrId> "<status>"
    if (action == "focus_tab") {
        auto target       = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/focus_tab/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, target, replyAddr]() {
            juce::File f(target);
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    bool match = canvas->patch.getCurrentFile() == f
                        || canvas->patch.getCurrentFile().getFileName() == target;
                    if (match) {
                        editor->getTabComponent().showTab(canvas, canvas->patch.splitViewIndex);
                        editor->getTabComponent().setActiveSplit(canvas);
                        bridge->sendReply(replyAddr, juce::String("focused: ") + canvas->patch.getTitle());
                        return;
                    }
                }
            }
            bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
        });
        return;
    }

    // ── PRD Phase 3 §2.5: CLOSE TAB ─────────────────────────────────────
    // /pd/close_tab <file-or-name> [force] [corrId]
    // Closes the named tab. Refuses unsaved changes unless force=1.
    // Reply: .../reply/<corrId> "<status>"
    if (action == "close_tab") {
        auto target        = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        float force        = msg.size() > 1 ? getArgFloat(msg[1]) : 0.0f;
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        juce::String replyAddr = "/pd/close_tab/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, target, force, replyAddr]() {
            juce::File f(target);
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    bool match = canvas->patch.getCurrentFile() == f
                        || canvas->patch.getCurrentFile().getFileName() == target;
                    if (match) {
                        if (canvas->patch.isDirty() && force < 0.5f) {
                            bridge->sendReply(replyAddr, juce::String(
                                "error: '" + canvas->patch.getTitle() + "' has unsaved changes — save first or pass force=1"));
                            return;
                        }
                        juce::String title = canvas->patch.getTitle();
                        editor->getTabComponent().closeTab(canvas);
                        bridge->sendReply(replyAddr, juce::String("closed: " + title));
                        return;
                    }
                }
            }
            bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
        });
        return;
    }

    if (action == "clear") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("clear"), 0, nullptr);
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol("0")));
            processor->receiveSysMessage("mcp_clear_ids", atoms);

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendRawReply("/pd/cleared");
        }
        return;
    }

    if (action == "clear_undo") {
        // /pd/clear_undo <canvasName>
        // Routes through receiveSysMessage → mcp_clear_undo case which runs
        // on the Pd scheduler thread — the only thread where canvas_undo_free
        // (calls canvas_suspend_dsp internally) is safe to call.
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            processor->receiveSysMessage("mcp_clear_undo", atoms);
            sendRawReply("/pd/clear_undo/reply");
        }
        return;
    }

    if (action == "clear_selection") {
        if (processor) {
            SmallArray<pd::Atom> atoms;
            processor->receiveSysMessage("mcp_clear_selection", atoms);
        }
        return;
    }

    if (action == "clear_ids") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_clear_ids", atoms);
        }
        return;
    }

    if (action == "create_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);
            float x = getArgFloat(msg[3]);
            float y = getArgFloat(msg[4]);
            auto kind = getArgString(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));
            atoms.add(pd::Atom(x));
            atoms.add(pd::Atom(y));
            atoms.add(pd::Atom(processor->generateSymbol(kind)));

            for (int i = 6; i < msg.size(); ++i) {
                atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
            }
            processor->receiveSysMessage("mcp_create_id", atoms);
        }
        return;
    }

    if (action == "create_batch_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            float count = getArgFloat(msg[2]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(count));

            for (int i = 3; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_create_batch_id", atoms);
        }
        return;
    }

    if (action == "create") {
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto kind = getArgString(msg[1]);
            float x = getArgFloat(msg[2]);
            float y = getArgFloat(msg[3]);

            juce::StringArray tokens;
            for (int i = 4; i < msg.size(); ++i) {
                tokens.add(getArgString(msg[i]));
            }

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
                patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens));
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "create_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));
            int cursor = 3;
            int created = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
                for (int o = 0; o < count && cursor < msg.size(); o++) {
                    auto kind = getArgString(msg[cursor++]);
                    float x = getArgFloat(msg[cursor++]);
                    float y = getArgFloat(msg[cursor++]);
                    int nargs = static_cast<int>(getArgFloat(msg[cursor++]));

                    juce::StringArray tokens;
                    for (int a = 0; a < nargs && cursor < msg.size(); a++) {
                        tokens.add(getArgString(msg[cursor++]));
                    }

                    if (patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens))) {
                        created++;
                    }
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/create_batch/reply/" + correlationId, static_cast<float>(created));
        }
        return;
    }

    if (action == "connect_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto srcId = getArgString(msg[2]);
            float srcOut = getArgFloat(msg[3]);
            auto destId = getArgString(msg[4]);
            float destIn = getArgFloat(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(srcId)));
            atoms.add(pd::Atom(srcOut));
            atoms.add(pd::Atom(processor->generateSymbol(destId)));
            atoms.add(pd::Atom(destIn));

            processor->receiveSysMessage("mcp_connect_id", atoms);
        }
        return;
    }

    if (action == "connect_batch_id") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_connect_batch_id", atoms);
        }
        return;
    }

    if (action == "connect_batch" || action == "connect") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = action == "connect_batch" ? getArgString(msg[1]) : "0";
            int startIdx = action == "connect_batch" ? 3 : 1;
            int count = action == "connect_batch" ? static_cast<int>(getArgFloat(msg[2])) : 1;
            int done = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                for (int c = 0; c < count && startIdx + 3 < msg.size(); c++) {
                    int srcIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int srcOut = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int destIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int destIn = static_cast<int>(getArgFloat(msg[startIdx++]));

                    t_atom cArgs[4];
                    SETFLOAT(&cArgs[0], static_cast<float>(srcIdx));
                    SETFLOAT(&cArgs[1], static_cast<float>(srcOut));
                    SETFLOAT(&cArgs[2], static_cast<float>(destIdx));
                    SETFLOAT(&cArgs[3], static_cast<float>(destIn));
                    pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("connect"), 4, cArgs);
                    done++;
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            if (action == "connect_batch") {
                sendReply("/pd/connect_batch/reply/" + correlationId, static_cast<float>(done));
            }
        }
        return;
    }

    if (action == "disconnect_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto srcId = getArgString(msg[2]);
            float srcOut = getArgFloat(msg[3]);
            auto destId = getArgString(msg[4]);
            float destIn = getArgFloat(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(srcId)));
            atoms.add(pd::Atom(srcOut));
            atoms.add(pd::Atom(processor->generateSymbol(destId)));
            atoms.add(pd::Atom(destIn));

            processor->receiveSysMessage("mcp_disconnect_id", atoms);
        }
        return;
    }

    if (action == "disconnect_batch_id") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_disconnect_batch_id", atoms);
        }
        return;
    }

    if (action == "disconnect") {
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int srcIdx = static_cast<int>(getArgFloat(msg[1]));
            int srcOut = static_cast<int>(getArgFloat(msg[2]));
            int destIdx = static_cast<int>(getArgFloat(msg[3]));
            int destIn = static_cast<int>(getArgFloat(msg[4]));

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
                t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
                if (src && dest) {
                    pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "disconnect_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));
            int cursor = 3;
            int done = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                for (int c = 0; c < count && cursor + 3 < msg.size(); c++) {
                    int srcIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                    int srcOut = static_cast<int>(getArgFloat(msg[cursor++]));
                    int destIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                    int destIn = static_cast<int>(getArgFloat(msg[cursor++]));

                    t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
                    t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
                    if (src && dest) {
                        pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                        done++;
                    }
                }
                if (done > 0) canvas_dirty(cnv, 1);
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/disconnect_batch/reply/" + correlationId, static_cast<float>(done));
        }
        return;
    }

    if (action == "delete_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));

            processor->receiveSysMessage("mcp_delete_id", atoms);
        }
        return;
    }

    if (action == "delete_batch_id") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));

            SmallArray<pd::Atom> atoms;
            processor->receiveSysMessage("mcp_suspend_dsp", atoms);

            int deleted = 0;
            for (int i = 0; i < count && 3 + i < msg.size(); i++) {
                auto id = getArgString(msg[3 + i]);
                if (id.isNotEmpty() && id != "nil") {
                    SmallArray<pd::Atom> delAtoms;
                    delAtoms.add(pd::Atom(processor->generateSymbol(canvasName)));
                    delAtoms.add(pd::Atom(processor->generateSymbol(correlationId)));
                    delAtoms.add(pd::Atom(processor->generateSymbol(id)));
                    processor->receiveSysMessage("mcp_delete_id", delAtoms);
                    deleted++;
                }
            }

            processor->receiveSysMessage("mcp_resume_dsp", atoms);
            sendReply("/pd/delete_batch_id/reply/" + correlationId, static_cast<float>(deleted));
        }
        return;
    }

    if (action == "delete_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            std::vector<int> indices;
            for (int i = 2; i < msg.size(); ++i) {
                indices.push_back(static_cast<int>(getArgFloat(msg[i])));
            }
            std::sort(indices.rbegin(), indices.rend());

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                SmallArray<t_gobj*> toDelete;
                for (int idx : indices) {
                    t_gobj* obj = glistObjectAt(cnv, idx);
                    if (obj) toDelete.add(obj);
                }
                if (toDelete.size() > 0) {
                    pd::Interface::removeObjects(cnv, toDelete);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/delete_batch/reply/" + correlationId, static_cast<float>(indices.size()));
        }
        return;
    }

    if (action == "delete") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int index = static_cast<int>(getArgFloat(msg[1]));

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                t_gobj* obj = glistObjectAt(cnv, index);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    pd::Interface::removeObjects(cnv, toDelete);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "move_id" || action == "move_batch" || action == "move_batch_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }

            if (action == "move_id") {
                processor->receiveSysMessage("mcp_move_id", atoms);
            } else if (action == "move_batch_id") {
                processor->receiveSysMessage("mcp_move_batch_id", atoms);
            } else {
                processor->receiveSysMessage("mcp_move_batch", atoms);
            }
        }
        return;
    }

    if (action == "rename_id") {
        if (msg.size() >= 4 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto oldId = getArgString(msg[2]);
            auto newId = getArgString(msg[3]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(oldId)));
            atoms.add(pd::Atom(processor->generateSymbol(newId)));
            processor->receiveSysMessage("mcp_rename_id", atoms);
        }
        return;
    }

    // /pd/rename_id_batch <canvas> <count> <old0> <new0> <old1> <new1> ... <corrId>
    // Batch rename with a reply — renames directly in mcpStableObjectMap
    // (inline, synchronous — no receiveSysMessage async queue).
    if (action == "rename_id_batch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = static_cast<int>(getArgFloat(msg[1]));
            int cursor = 2;
            int renamed = 0;

            // Inline rename — directly mutates mcpStableObjectMap, no queue.
            auto& map = processor->mcpStableObjectMap[canvasName.toStdString()];
            for (int i = 0; i < count && (cursor + 1) < msg.size(); i++) {
                auto oldId = getArgString(msg[cursor++]).toStdString();
                auto newId = getArgString(msg[cursor++]).toStdString();
                auto it = map.find(oldId);
                if (it != map.end()) {
                    map[newId] = it->second;
                    map.erase(it);
                    renamed++;
                }
            }
            if (renamed > 0)
                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";
            sendReply("/pd/rename_id_batch/reply/" + correlationId, static_cast<float>(renamed));
        }
        return;
    }

    if (action == "edit_id") {
        if (msg.size() >= 4 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);
            auto kind = getArgString(msg[3]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));
            atoms.add(pd::Atom(processor->generateSymbol(kind)));

            for (int i = 4; i < msg.size(); ++i) {
                atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
            }
            processor->receiveSysMessage("mcp_edit_id", atoms);
        }
        return;
    }

    if (action == "get_mappings") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_get_mappings", atoms);
        }
        return;
    }

    if (action == "register_id") {
        if (msg.size() >= 4 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            float index = getArgFloat(msg[2]);
            auto objectId = getArgString(msg[3]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(index));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));

            processor->receiveSysMessage("mcp_register_id", atoms);
        }
        return;
    }

    if (action == "encapsulate") {
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

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, subpatchName, targetIds, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (!canvasComp) {
                        bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
                        return;
                    }

                    canvasComp->patch.deselectAll();
                    for (const auto& id : targetIds) {
                        t_gobj* g = proc->resolveStableId(canvasName, id);
                        if (g) {
                            for (auto* objComp : canvasComp->objects) {
                                if (objComp && objComp->getPointer() == g) {
                                    canvasComp->setSelected(objComp, true);
                                    break;
                                }
                            }
                        }
                    }

                    canvasComp->encapsulateSelection(subpatchName);

                    t_gobj* newestObj = pd::Interface::getNewest(cnv);
                    if (newestObj) {
                        proc->mcpStableObjectMap[canvasName.toStdString()][subpatchName.toStdString()] = newestObj;
                        proc->mcpStableSerialMap[newestObj] = proc->mcpSerialCounter++;
                        proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    }

                    bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    // /pd/encapsulate_to_file <canvas> <name> <filePath> <count> <id0> ... [corrId]
    // Full to_abstraction in one atomic C++ call:
    //   1. encapsulateSelection() → [pd name] subpatch (same as /pd/encapsulate)
    //   2. getCanvasContent() on the new subpatch → write to filePath on disk
    //   3. Delete [pd name], create [name] abstraction reference at same position
    //   4. reloadAbstractions() so Pd registers the file
    // Reply: /pd/encapsulate_to_file/reply/<corrId>  1.0=ok, 0.0=failed
    if (action == "encapsulate_to_file") {
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

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, abstrName, filePath, targetIds, correlationId, bridge = this]() {
                    // ── Find canvas UI component ────────────────────────────
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c; break;
                            }
                        }
                        if (canvasComp) break;
                    }
                    if (!canvasComp) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    // ── Step 1: Encapsulate selected objects ─────────────────
                    canvasComp->patch.deselectAll();
                    for (const auto& id : targetIds) {
                        t_gobj* g = proc->resolveStableId(canvasName, id);
                        if (g) {
                            for (auto* objComp : canvasComp->objects) {
                                if (objComp && objComp->getPointer() == g) {
                                    canvasComp->setSelected(objComp, true);
                                    break;
                                }
                            }
                        }
                    }
                    canvasComp->encapsulateSelection(abstrName);

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
                    sys_lock();
                    {
                        char* buf = nullptr; int bufsize = 0;
                        pd::Interface::getCanvasContent(subCnv, &buf, &bufsize);
                        if (buf) {
                            content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                            freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                        }
                    }
                    sys_unlock();

                    if (content.isEmpty()) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    juce::File destFile(filePath);
                    destFile.getParentDirectory().createDirectory();
                    if (!destFile.replaceWithText(content)) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    // ── Step 4: Replace [pd abstrName] with [abstrName] ref ──
                    // Delete the inline subpatch, create abstraction reference.
                    sys_lock();
                    {
                        // Delete the [pd abstrName] subpatch object
                        glist_delete(cnv, newestObj);
                        // Create [abstrName] abstraction reference at same position
                        // using pasteDirect — same path as batch_atomic PHASE 4
                        juce::String objLine = "#X obj " + juce::String(posX) + " " + juce::String(posY) + " " + abstrName + ";";
                        pasteDirect(cnv, objLine.toRawUTF8());
                        // Single DSP recompile
                        canvas_update_dsp();
                    }
                    sys_unlock();

                    // ── Step 5: Register new abstraction object in identity map
                    t_gobj* newObj = pd::Interface::getNewest(cnv);
                    if (newObj) {
                        proc->mcpStableObjectMap[canvasName.toStdString()][abstrName.toStdString()] = newObj;
                        proc->mcpStableSerialMap[newObj] = proc->mcpSerialCounter++;
                        proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    }

                    // ── Step 6: Reload abstractions so Pd registers the file ─
                    proc->reloadAbstractions(juce::File(filePath), cnv);

                    proc->enqueueFunctionAsync([p = proc] { p->synchroniseCanvases(); });

                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "tidy") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = static_cast<int>(getArgFloat(msg[1]));
            std::vector<juce::String> targetIds;
            int cursor = 2;
            for (int i = 0; i < count && cursor < msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                SmallArray<t_gobj*> targetObjs;
                if (!targetIds.empty()) {
                    for (const auto& id : targetIds) {
                        t_gobj* g = processor->resolveStableId(canvasName, id);
                        if (g) targetObjs.add(g);
                    }
                } else {
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                        targetObjs.add(y);
                    }
                }

                if (!targetObjs.empty()) {
                    pd::Interface::tidy(cnv, targetObjs);
                    processor->synchroniseCanvases();
                }
                sendReply("/pd/tidy/reply/" + correlationId, 1.0f);
            } else {
                sendReply("/pd/tidy/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "align") {
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

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, alignStr, targetIds, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (!canvasComp) {
                        bridge->sendReply("/pd/align/reply/" + correlationId, 0.0f);
                        return;
                    }

                    if (!targetIds.empty()) {
                        canvasComp->patch.deselectAll();
                        for (const auto& id : targetIds) {
                            t_gobj* g = proc->resolveStableId(canvasName, id);
                            if (g) {
                                for (auto* objComp : canvasComp->objects) {
                                    if (objComp && objComp->getPointer() == g) {
                                        canvasComp->setSelected(objComp, true);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    Align alignMode = Align::Left;
                    if (alignStr == "right") alignMode = Align::Right;
                    else if (alignStr == "hcentre" || alignStr == "hcenter") alignMode = Align::HCentre;
                    else if (alignStr == "hdistribute") alignMode = Align::HDistribute;
                    else if (alignStr == "top") alignMode = Align::Top;
                    else if (alignStr == "bottom") alignMode = Align::Bottom;
                    else if (alignStr == "vcentre" || alignStr == "vcenter") alignMode = Align::VCentre;
                    else if (alignStr == "vdistribute") alignMode = Align::VDistribute;

                    canvasComp->alignObjects(alignMode);
                    bridge->sendReply("/pd/align/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/align/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "undo") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (canvasComp) {
                        canvasComp->undo();
                        bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    } else {
                        pd::Interface::undo(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    }
                });
            } else {
                sendReply("/pd/undo/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "redo") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (canvasComp) {
                        canvasComp->redo();
                        bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                    } else {
                        pd::Interface::redo(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                    }
                });
            } else {
                sendReply("/pd/redo/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }
}

void MCPBridge::handleParamDomain(const juce::String& /*paramName*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /param <name> <value...> — the receiver name is the FIRST
    // argument, never the address path.
    auto paramName = getArgString(msg[0]);
    if (paramName.isEmpty() || paramName == "nil") return;

    if (msg.size() == 2) {
        auto const& v = msg[1];
        if (v.isFloat32() || v.isInt32()) {
            processor->sendFloat(paramName.toRawUTF8(), getArgFloat(v));
        } else {
            auto s = getArgString(v);
            if (s.startsWith("s:")) s = s.substring(2);
            processor->sendSymbol(paramName.toRawUTF8(), s.toRawUTF8());
        }
    } else if (msg.size() > 2) {
        SmallArray<pd::Atom> atoms;
        for (int i = 1; i < msg.size(); ++i) {
            auto const& arg = msg[i];
            if (arg.isFloat32() || arg.isInt32()) {
                atoms.add(pd::Atom(getArgFloat(arg)));
            } else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                atoms.add(pd::Atom(processor->generateSymbol(s)));
            }
        }
        processor->sendList(paramName.toRawUTF8(), atoms);
    }
}

void MCPBridge::handleTriggerDomain(const juce::String& triggerAction, const juce::OSCMessage& msg)
{
    if (!processor) return;

    if (triggerAction == "note" && msg.size() >= 3) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int pitch = static_cast<int>(getArgFloat(msg[1]));
        int vel = static_cast<int>(getArgFloat(msg[2]));
        processor->sendNoteOn(ch, pitch, vel);
        return;
    }

    // Lua-parity: /trigger <name> — the receiver name is the FIRST argument.
    auto bangName = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    if (bangName.isEmpty() || bangName == "nil") return;
    processor->sendBang(bangName.toRawUTF8());
}

void MCPBridge::handleTelemetryDomain(const juce::String& /*telAction*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /telemetry <action> [args...] — the action is the FIRST argument.
    auto action = getArgString(msg[0]);

    if (action == "dsp" && msg.size() >= 2) {
        float val = getArgFloat(msg[1]);
        if (val > 0.5f) processor->startDSP();
        else processor->releaseDSP();
    } else if (action == "delete" || action == "delete_on") {
        // [telemetry, delete, index]           → canvas defaults to main
        // [telemetry, delete, canvas, index]   → explicit canvas
        int idx = -1;
        juce::String canvasName = "pd-main";
        if (msg.size() == 2) {
            idx = static_cast<int>(getArgFloat(msg[1]));
        } else if (msg.size() >= 3) {
            canvasName = normalizeCanvas(getArgString(msg[1]));
            idx = static_cast<int>(getArgFloat(msg[2]));
        }
        if (idx >= 0) {
            sys_lock();
            t_canvas* canvas = processor->getCanvasBySymbol(canvasName);
            if (canvas) {
                t_gobj* obj = glistObjectAt(canvas, idx);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    pd::Interface::removeObjects(canvas, toDelete);
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();
        }
    }
}

static t_garray* findGArrayInCanvas(t_canvas* cnv, t_symbol* nameSym)
{
    if (!cnv) return nullptr;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (pd_class(&y->g_pd) == garray_class) {
            t_garray* ga = reinterpret_cast<t_garray*>(y);
            t_symbol* sym = nullptr;
            if (garray_getname(ga, &sym) && sym == nameSym) {
                return ga;
            }
        } else if (pd_class(&y->g_pd) == canvas_class) {
            t_canvas* sub = reinterpret_cast<t_canvas*>(y);
            t_garray* ga = findGArrayInCanvas(sub, nameSym);
            if (ga) return ga;
        }
    }
    return nullptr;
}

void MCPBridge::handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 2) return;

    // Two conventions:
    //   write/read: /array/<action> <name> <subpatch> <corrId> ...  (name = msg[0])
    //   stats:      /array ["stats", name, sampleRate, corrId]      (name = msg[1])
    auto arrayName = arrayAction == "stats" ? getArgString(msg[1]) : getArgString(msg[0]);
    auto canvasName = normalizeCanvas(getArgString(msg[1]));

    // /array/load <name> <subpatch> <filePath> <corrId>
    // Loads a WAV/AIFF from disk into the array natively (JUCE AudioFormatReader).
    // One message, no [soundfiler] object, no bang-and-wait dance. The disk read
    // happens OUTSIDE sys_lock; only resize+fill run under sys_lock. Reply arg0 = size,
    // or a negative error code.
    if (arrayAction == "load") {
        auto filePath      = msg.size() > 2 ? getArgString(msg[2]) : "";
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";
        juce::String replyAddr = "/array/load/reply/" + correlationId;

        juce::File file(filePath);
        if (!file.existsAsFile()) { sendReply(replyAddr, -2.0f); return; }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (!reader) { sendReply(replyAddr, -3.0f); return; }

        int const numChannels = static_cast<int>(reader->numChannels);
        int const numSamples  = static_cast<int>(reader->lengthInSamples);
        if (numChannels <= 0 || numSamples <= 0) { sendReply(replyAddr, -4.0f); return; }

        // Mono downmix (JUCE downmixes stereo→mono when the destination is 1ch).
        juce::AudioBuffer<float> fileBuf(1, numSamples);
        if (!reader->read(&fileBuf, 0, numSamples, 0, true, true)) { sendReply(replyAddr, -5.0f); return; }
        const float* src = fileBuf.getReadPointer(0);

        sys_lock();
        auto nameSym = gensym(arrayName.toRawUTF8());
        t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
        if (!garray) {
            for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                garray = findGArrayInCanvas(c, nameSym);
                if (garray) break;
            }
        }

        // Auto-create the array if it doesn't exist yet.
        if (!garray) {
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                int nobj = 0;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
                int stagger = (nobj * 14) % 420;
                juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                    + arrayName + " " + juce::String(numSamples) + " float 2;\n#X coords 0 1 "
                    + juce::String(numSamples > 1 ? numSamples - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                    + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
                pd::Interface::paste(cnv, pasta.toRawUTF8());
                garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
                if (!garray) {
                    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                        garray = findGArrayInCanvas(c, nameSym);
                        if (garray) break;
                    }
                }
                if (garray) {
                    canvas_dirty(cnv, 1);
                    processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
                }
            }
        }

        if (!garray) { sys_unlock(); sendReply(replyAddr, -6.0f); return; }

        garray_resize_long(garray, numSamples);
        int size = 0;
        t_word* vec = nullptr;
        if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
            sys_unlock();
            sendReply(replyAddr, -7.0f);
            return;
        }

        int const copy = size < numSamples ? size : numSamples;
        for (int i = 0; i < copy; ++i) vec[i].w_float = src[i];
        garray_redraw(garray);
        sys_unlock();

        sendReply(replyAddr, static_cast<float>(copy));
        return;
    }

    sys_lock();
    auto nameSym = gensym(arrayName.toRawUTF8());
    t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
    if (!garray) {
        for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
            garray = findGArrayInCanvas(c, nameSym);
            if (garray) break;
        }
    }

    // PlugData instantiates arrays through the GUI; arrays created via the
    // MCP text-object path may never bind their garray. If a write targets a
    // missing array, create it as a graph-on-parent via paste (the same
    // mechanism pd::Patch::createObject uses for arrays).
    if (!garray && (arrayAction == "write" || arrayAction == "write_bulk") && msg.size() >= 4) {
        int reqSize = 2048;
        if (arrayAction == "write" && msg.size() >= 5) {
            int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 5;
            reqSize = std::max(2048, chunkIndex * 128 + dataCount);
        } else if (arrayAction == "write_bulk" && msg.size() >= 4) {
            int offset = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 4;
            reqSize = std::max(2048, offset + dataCount);
        }

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (cnv) {
            // Stagger each auto-created array so they don't stack on top of
            // each other in the top-left corner.
            int nobj = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
            int stagger = (nobj * 14) % 420;
            juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                + arrayName + " " + juce::String(reqSize) + " float 2;\n#X coords 0 1 "
                + juce::String(reqSize > 1 ? reqSize - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
            pd::Interface::paste(cnv, pasta.toRawUTF8());
            garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
            if (!garray) {
                for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                    garray = findGArrayInCanvas(c, nameSym);
                    if (garray) break;
                }
            }
            if (garray) {
                canvas_dirty(cnv, 1);
                processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            }
        }
    }

    if (!garray) {
        sys_unlock();
        return;
    }

    int size = 0;
    t_word* vec = nullptr;
    if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
        sys_unlock();
        return;
    }

    if (arrayAction == "write_bulk" && msg.size() >= 4) {
        // [name, subpatch, corrId, offset, data...]
        auto correlationId = getArgString(msg[2]);
        int offset = static_cast<int>(getArgFloat(msg[3]));
        int dataStart = 4;
        int dataCount = msg.size() - dataStart;

        int requiredSize = offset + dataCount;
        if (requiredSize > size) {
            garray_resize_long(garray, requiredSize);
            if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
                sys_unlock();
                juce::OSCMessage errReply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
                errReply.addArgument(static_cast<int32>(-1));
                errReply.addArgument(static_cast<int32>(0));
                sender.send(errReply);
                return;
            }
        }

        int written = 0;
        for (int i = dataStart; i < msg.size() && (offset + written) < size; ++i) {
            vec[offset + written].w_float = getArgFloat(msg[i]);
            written++;
        }
        garray_redraw(garray);
        sys_unlock();

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
        reply.addArgument(static_cast<int32>(written));
        reply.addArgument(static_cast<int32>(size));
        sender.send(reply);
        return;
    } else if (arrayAction == "read_bulk") {
        // Format: [arrayName, subpatch, correlationId, offset, limit]
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : size;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        // Cap at 8192 samples per single reply (32KB OSC payload)
        readCount = std::min(readCount, 8192);

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/read_bulk/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));         // total array size
        reply.addArgument(static_cast<int32>(readStart));    // actual start
        reply.addArgument(static_cast<int32>(readCount));    // actual count
        for (int i = readStart; i < readStart + readCount; ++i) {
            reply.addArgument(vec[i].w_float);
        }
        sys_unlock();
        sender.send(reply);
        return;
    } else if (arrayAction == "write" && msg.size() >= 5) {
        // [name, subpatch, corrId, chunkIndex, totalChunks, data...]
        auto correlationId = getArgString(msg[2]);
        int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
        int const CHUNK = 128;
        int offset = chunkIndex * CHUNK;
        int dataStart = 5;

        for (int i = dataStart; i < msg.size() && (offset + i - dataStart) < size; ++i) {
            vec[offset + i - dataStart].w_float = getArgFloat(msg[i]);
        }
        garray_redraw(garray);
        sys_unlock();

        sendReply("/array/write/reply/" + correlationId, static_cast<float>(size));
    } else if (arrayAction == "read") {
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : 512;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        int const CHUNK = 4096;
        int totalChunks = (readCount + CHUNK - 1) / CHUNK;

        for (int c = 0; c < totalChunks; ++c) {
            int start = readStart + c * CHUNK;
            int stop = std::min(start + CHUNK, readStart + readCount);

            juce::OSCMessage chunkMsg { juce::OSCAddressPattern("/array/read/chunk/" + correlationId) };
            chunkMsg.addArgument(static_cast<int32>(c));
            chunkMsg.addArgument(static_cast<int32>(totalChunks));
            for (int i = start; i < stop; ++i) {
                chunkMsg.addArgument(vec[i].w_float);
            }
            sender.send(chunkMsg);
        }
        sys_unlock();

        juce::OSCMessage doneMsg { juce::OSCAddressPattern("/array/read/done/" + correlationId) };
        doneMsg.addArgument(arrayName);
        doneMsg.addArgument(static_cast<int32>(size));
        sender.send(doneMsg);
    } else if (arrayAction == "stats") {
        float sampleRate = msg.size() > 2 ? getArgFloat(msg[2]) : 44100.0f;
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";

        float minVal = 0.0f, maxVal = 0.0f, sumSq = 0.0f;
        int zeroCrossings = 0;
        float prevVal = 0.0f;

        if (size > 0) {
            minVal = vec[0].w_float;
            maxVal = vec[0].w_float;
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
                sumSq += v * v;

                if (i > 0) {
                    if ((prevVal < 0 && v >= 0) || (prevVal > 0 && v <= 0)) {
                        zeroCrossings++;
                    }
                }
                prevVal = v;
            }
        }
        sys_unlock();

        float rms = size > 0 ? std::sqrt(sumSq / static_cast<float>(size)) : 0.0f;
        float estimatedPitch = size > 0 ? (static_cast<float>(zeroCrossings) / 2.0f) * (sampleRate / static_cast<float>(size)) : 0.0f;

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/stats/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));
        reply.addArgument(minVal);
        reply.addArgument(maxVal);
        reply.addArgument(rms);
        reply.addArgument(static_cast<int32>(zeroCrossings));
        reply.addArgument(estimatedPitch);
        sender.send(reply);
    } else if (arrayAction == "mutate") {
        // /array/mutate <name> <subpatch> <corrId> <reverse|invert|crush>
        // In-place array mutation under a single sys_lock — zero OSC data round-trip,
        // so it never floods the bridge queue (the cause of live audio drops when
        // the TS server shuttles 427k floats through ~150 chunked messages).
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        auto op = msg.size() > 3 ? getArgString(msg[3]) : "reverse";

        if (op == "reverse") {
            for (int i = 0; i < size / 2; ++i) {
                float tmp = vec[i].w_float;
                vec[i].w_float = vec[size - 1 - i].w_float;
                vec[size - 1 - i].w_float = tmp;
            }
        } else if (op == "invert") {
            for (int i = 0; i < size; ++i) vec[i].w_float = -vec[i].w_float;
        } else if (op == "crush") {
            int const bits = 8;
            float const q = 1.0f / static_cast<float>(1 << (bits - 1));
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                vec[i].w_float = std::round(v / q) * q;
            }
        }

        garray_redraw(garray);
        sys_unlock();
        sendReply("/array/mutate/reply/" + correlationId, static_cast<float>(size));
        return;
    } else {
        sys_unlock();
    }
}

void MCPBridge::handleBridgeDomain(const juce::String& bridgeAction, const juce::OSCMessage& msg)
{
    if (bridgeAction == "connect") {
        if (msg.size() >= 2) {
            auto ip = getArgString(msg[0]);
            int port = static_cast<int>(getArgFloat(msg[1]));
            if (port > 0) {
                sender.disconnect();
                sender.connect(ip.isNotEmpty() ? ip : "127.0.0.1", port);
            }
        }
        sendReply("/bridge/connect/ack", "127.0.0.1");
    } else if (bridgeAction == "capabilities") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        juce::OSCMessage reply { juce::OSCAddressPattern("/bridge/capabilities/reply") };
        reply.addArgument(juce::String("11.8"));
        reply.addArgument(juce::String("create_batch"));
        reply.addArgument(juce::String("delete_batch"));
        reply.addArgument(juce::String("connect_batch"));
        reply.addArgument(juce::String("disconnect_batch"));
        reply.addArgument(juce::String("ping"));
        reply.addArgument(juce::String("clear"));
        reply.addArgument(juce::String("dump"));
        reply.addArgument(juce::String("load"));
        reply.addArgument(juce::String("param"));
        reply.addArgument(juce::String("param_symbol"));
        reply.addArgument(juce::String("param_list"));
        reply.addArgument(juce::String("trigger"));
        reply.addArgument(juce::String("telemetry"));
        reply.addArgument(juce::String("array_io"));
        reply.addArgument(juce::String("morph"));
        reply.addArgument(juce::String("adaptive_dump"));
        reply.addArgument(juce::String("finalize"));
        reply.addArgument(juce::String("array_stats"));
        reply.addArgument(juce::String("get_mappings"));
        reply.addArgument(juce::String("move_batch_id"));
        reply.addArgument(juce::String("connect_batch_id"));
        reply.addArgument(juce::String("disconnect_batch_id"));
        reply.addArgument(juce::String("create_batch_id"));
        reply.addArgument(juce::String("delete_batch_id"));
        reply.addArgument(juce::String("meter"));
        reply.addArgument(juce::String("meter_query"));
        reply.addArgument(juce::String("inline_mappings"));
        reply.addArgument(juce::String("array_bulk"));
        reply.addArgument(juce::String("census"));
        reply.addArgument(juce::String("typeof"));
        reply.addArgument(juce::String("ports"));
        reply.addArgument(juce::String("identity_snapshot"));
        reply.addArgument(juce::String("identity_version"));
        // PRD Phase 2 §2.6: the bridge mints readable tempIds natively
        // (census auto-adopt). TS is a read-only mirror on bridges that
        // advertise this; legacy bridges keep the TS adoption path.
        reply.addArgument(juce::String("auto_adopt"));
        // PRD Phase 3 §2.6: C++-native save/load with identity sidecar —
        // tempIds travel with the file and are re-registered by the engine.
        reply.addArgument(juce::String("save_patch"));
        reply.addArgument(juce::String("load_patch"));
        // PRD Phase 3 §2.5: multi-tab — open in new tab + enumerate tabs
        reply.addArgument(juce::String("open_patch"));
        reply.addArgument(juce::String("list_tabs"));
        reply.addArgument(juce::String("focus_tab"));
        reply.addArgument(juce::String("close_tab"));
        reply.addArgument(juce::String("connections"));
        reply.addArgument(juce::String("spectral"));
        reply.addArgument(juce::String("batch_atomic"));
        reply.addArgument(juce::String("transport"));
        reply.addArgument(juce::String("seq"));
        reply.addArgument(juce::String("boot:" + bootToken));
        sender.send(reply);
    }
}

void MCPBridge::handleTransportDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // Sample-accurate transport clock. Position derives from a running sample
    // counter (advanced in audioTick()), so bar/beat/step and the next downbeat
    // are exact — no Date.now()/wall-clock drift.

    if (action == "set_bpm") {
        if (msg.size() >= 1) {
            double bpm = getArgFloat(msg[0]);
            if (bpm < 1.0) bpm = 1.0;
            if (bpm > 400.0) bpm = 400.0;
            double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;
            double const oldBpm = transport.bpm.load(std::memory_order_relaxed);
            double const samplesPerBeat = sampleRate * 60.0 / oldBpm;
            int64_t const counter = transport.sampleCounter.load(std::memory_order_relaxed);
            int64_t const anchorSample = transport.anchorSample.load(std::memory_order_relaxed);
            double const anchorBeat = transport.anchorBeat.load(std::memory_order_relaxed);
            // Anchor current position so a tempo change preserves the beat position.
            double const curBeat = samplesPerBeat > 0.0 ? anchorBeat + (double)(counter - anchorSample) / samplesPerBeat : anchorBeat;
            transport.anchorBeat.store(curBeat, std::memory_order_relaxed);
            transport.anchorSample.store(counter, std::memory_order_relaxed);
            transport.bpm.store((float)bpm, std::memory_order_relaxed);
        }
        return;
    }

    if (action == "set_subdivision") {
        if (msg.size() >= 1) {
            int sub = (int)getArgFloat(msg[0]);
            if (sub < 1) sub = 1;
            if (sub > 64) sub = 64;
            transport.subdivision.store(sub, std::memory_order_relaxed);
        }
        return;
    }

    if (action == "set_mode") {
        if (msg.size() >= 1) {
            juce::String const mode = getArgString(msg[0]);
            transport.modeClock.store(mode == "clock", std::memory_order_relaxed);
        }
        return;
    }

    if (action == "start") {
        transport.running.store(true, std::memory_order_relaxed);
        return;
    }

    if (action == "pause") {
        transport.running.store(false, std::memory_order_relaxed);
        return;
    }

    if (action == "status") {
        auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;
        double const bpm = transport.bpm.load(std::memory_order_relaxed);
        int const subdivision = transport.subdivision.load(std::memory_order_relaxed);
        int const beatsPerBar = transport.beatsPerBar.load(std::memory_order_relaxed);
        bool const running = transport.running.load(std::memory_order_relaxed);
        bool const modeClock = transport.modeClock.load(std::memory_order_relaxed);
        int64_t const counter = transport.sampleCounter.load(std::memory_order_relaxed);
        int64_t const anchorSample = transport.anchorSample.load(std::memory_order_relaxed);
        double const anchorBeat = transport.anchorBeat.load(std::memory_order_relaxed);

        double const samplesPerBeat = sampleRate * 60.0 / bpm;
        double const totalBeats = samplesPerBeat > 0.0 ? anchorBeat + (double)(counter - anchorSample) / samplesPerBeat : anchorBeat;
        double const barD = std::floor(totalBeats / beatsPerBar);
        int const bar = (int)barD;
        int const beatInBar = (int)std::floor(totalBeats) - bar * beatsPerBar;
        int const stepInBeat = (int)std::floor((totalBeats - std::floor(totalBeats)) * subdivision);
        int const stepsPerBar = beatsPerBar * subdivision;
        int const stepInBar = ((int)std::floor(totalBeats * subdivision)) % stepsPerBar;

        double const nextBarBeat = (barD + 1.0) * beatsPerBar;
        int64_t const samplesToNextBar = (int64_t)((nextBarBeat - totalBeats) * samplesPerBeat);

        juce::OSCMessage reply { juce::OSCAddressPattern("/transport/status/" + correlationId) };
        reply.addArgument((float)bpm);
        reply.addArgument(subdivision);
        reply.addArgument(beatsPerBar);
        reply.addArgument(bar);
        reply.addArgument(beatInBar);
        reply.addArgument(stepInBeat);
        reply.addArgument(stepInBar);
        reply.addArgument((float)samplesToNextBar);
        reply.addArgument((float)sampleRate);
        reply.addArgument(running ? 1 : 0);
        reply.addArgument(modeClock ? juce::String("clock") : juce::String("free"));
        sender.send(reply);
        return;
    }
}

void MCPBridge::handleSeqDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // /seq/start <jobId> <corrId> <json>
    // /seq/stop  <jobId>
    if (action == "start") {
        auto const jobId = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        auto const correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        auto const jsonStr = msg.size() > 2 ? getArgString(msg[2]) : juce::String();
        juce::String const replyAddr = "/seq/start/reply/" + correlationId;

        if (jobId.isEmpty() || jsonStr.isEmpty()) { sendReply(replyAddr, 0.0f); return; }

        juce::var const parsed = juce::JSON::parse(jsonStr);
        if (parsed.isVoid()) { sendReply(replyAddr, 0.0f); return; }

        SeqJob job;
        job.jobId = jobId;
        job.bpm = varToFloat(parsed.getProperty("bpm", 120.0), 120.0f);
        job.subdivision = varToInt(parsed.getProperty("subdivision", 4.0), 4);
        job.swing = varToFloat(parsed.getProperty("swing", 0.0), 0.0f);
        job.syncToClock = varToBool(parsed.getProperty("syncToClock", true), true);
        job.drift = varToFloat(parsed.getProperty("drift", 0.0), 0.0f);
        job.nextStepSample = -1;  // sentinel: anchor to current position on first tick
        job.jobSampleCounter = 0;
        job.stepIndex = 0;
        job.active = true;

        if (auto* tracksObj = parsed.getProperty("tracks", juce::var()).getDynamicObject()) {
            int maxLen = 0;
            for (auto const& kv : tracksObj->getProperties()) {
                juce::String const name = kv.name.toString();
                SeqTrack track;
                track.name = name;
                track.isBang = isBangTrack(name);
                if (auto* arr = kv.value.getArray()) {
                    for (auto const& stepVar : *arr) {
                        if (stepVar.isBool()) track.steps.push_back(stepVar.toString().equalsIgnoreCase("true") ? 1.0f : 0.0f);
                        else track.steps.push_back(varToFloat(stepVar, 0.0f));
                    }
                }
                maxLen = std::max(maxLen, (int)track.steps.size());
                job.tracks.push_back(track);
            }
            job.patternLength = maxLen;
        }

        if (job.patternLength <= 0 || job.tracks.empty()) { sendReply(replyAddr, 0.0f); return; }

        {
            juce::ScopedLock sl(seqLock);
            for (auto& existing : seqJobs) {
                if (existing.jobId == jobId) {
                    existing.active = false;
                    existing = job;
                    sendReply(replyAddr, 1.0f);
                    return;
                }
            }
            seqJobs.push_back(job);
        }
        sendReply(replyAddr, 1.0f);
        return;
    }

    if (action == "stop") {
        auto const jobId = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        juce::ScopedLock sl(seqLock);
        for (auto& job : seqJobs)
            if (job.jobId == jobId || jobId.isEmpty())
                job.active = false;
        if (jobId.isEmpty()) seqJobs.clear();
        return;
    }
}

void MCPBridge::advanceSequencer(int blockSize)
{
    double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;

    juce::ScopedLock sl(seqLock);
    for (auto& job : seqJobs) {
        if (!job.active) continue;

        // Free jobs advance their own sample counter; locked jobs read the transport.
        job.jobSampleCounter += blockSize;
        int64_t const pos = job.syncToClock
            ? transport.sampleCounter.load(std::memory_order_relaxed)
            : job.jobSampleCounter;

        if (job.nextStepSample < 0)
            job.nextStepSample = pos;  // first tick: anchor to now

        double const effBpm = job.bpm * (1.0 + job.drift / 100.0);
        double const stepSamples = sampleRate * 60.0 / (effBpm * job.subdivision);

        int guard = 0;
        while (job.nextStepSample <= pos && guard++ < 64) {
            // Fire every non-rest step value across all tracks for this step.
            for (auto const& track : job.tracks) {
                if (track.steps.empty()) continue;
                int const ti = job.stepIndex % (int)track.steps.size();
                float const val = track.steps[ti];
                if (val == 0.0f) continue;
                enqueueSeqFire(track.isBang, track.name, val);
            }

            job.stepIndex = (job.stepIndex + 1) % job.patternLength;

            double len = stepSamples;
            if (job.swing > 0.0f && (job.stepIndex % 2) == 1)
                len += stepSamples * job.swing * 0.5;
            job.nextStepSample += (int64_t)len;
        }
    }
}

void MCPBridge::enqueueSeqFire(bool isBang, const juce::String& name, float value)
{
    PluginProcessor* p = processor;
    std::string const n = name.toStdString();
    // Defer the actual libpd dispatch to the message phase (sendMessagesFromQueue)
    // before the next block's performDSP — safe + block-accurate, zero UDP.
    p->enqueueFunctionAsync([p, isBang, n, value] {
        if (isBang) p->sendBang(n.c_str());
        else p->sendFloat(n.c_str(), value);
    });
}

void MCPBridge::handleMorphDomain(const juce::String& morphAction, const juce::OSCMessage& msg)
{
    if (morphAction == "run" && msg.size() >= 5) {
        auto correlationId = getArgString(msg[0]);
        int durationMs = static_cast<int>(getArgFloat(msg[1]));
        int steps = static_cast<int>(getArgFloat(msg[2]));
        int paramCount = static_cast<int>(getArgFloat(msg[3]));

        if (steps <= 0) steps = 20;
        int interval = std::max(1, durationMs / steps);

        MorphJob job;
        job.correlationId = correlationId;
        job.totalSteps = steps;
        job.currentStep = 0;
        job.intervalMs = interval;

        int cursor = 4;
        for (int i = 0; i < paramCount && cursor + 2 < msg.size(); i++) {
            MorphParam p;
            p.name = getArgString(msg[cursor++]);
            p.from = getArgFloat(msg[cursor++]);
            p.to = getArgFloat(msg[cursor++]);
            job.params.push_back(p);
        }

        {
            const juce::ScopedLock sl(morphLock);
            morphJobs.push_back(job);
        }

        if (!isTimerRunning()) {
            startTimer(15);
        }
    }
}

void MCPBridge::audioTick()
{
    probeManager.audioTick();

    // Advance the native transport clock — one PD block per call, so the running
    // sample counter gives sample-accurate bar/beat/step position.
    int const blockSize = pd::Instance::getBlockSize();
    if (blockSize > 0) {
        if (transport.running.load(std::memory_order_relaxed))
            transport.sampleCounter.fetch_add(blockSize, std::memory_order_relaxed);
        advanceSequencer(blockSize);
    }
}

static float estimateFrequency(const float* buf, int n, float sampleRate)
{
    if (!buf || n < 128 || sampleRate <= 0.0f) return 0.0f;

    int minLag = std::max(2, static_cast<int>(sampleRate / 4000.0f));  // max freq 4kHz
    int maxLag = std::min(n / 2, static_cast<int>(sampleRate / 40.0f));  // min freq 40Hz

    if (minLag >= maxLag) return 0.0f;

    std::vector<float> corrs(maxLag + 1, 0.0f);

    for (int lag = minLag; lag <= maxLag; lag++) {
        float sumCross = 0.0f;
        float sumSq0 = 0.0f;
        float sumSqLag = 0.0f;
        int count = n - lag;
        for (int i = 0; i < count; i++) {
            float a = buf[i];
            float b = buf[i + lag];
            sumCross += a * b;
            sumSq0 += a * a;
            sumSqLag += b * b;
        }
        float denom = std::sqrt(sumSq0 * sumSqLag);
        if (denom > 1e-6f) {
            corrs[lag] = sumCross / denom;
        }
    }

    int peakLag = 0;
    for (int lag = minLag + 1; lag < maxLag; lag++) {
        if (corrs[lag] > 0.75f && corrs[lag] >= corrs[lag - 1] && corrs[lag] >= corrs[lag + 1]) {
            peakLag = lag;
            break;
        }
    }

    if (peakLag > 0) {
        float alpha = corrs[peakLag - 1];
        float beta = corrs[peakLag];
        float gamma = corrs[peakLag + 1];
        float denom = (alpha - 2.0f * beta + gamma);
        float delta = (std::abs(denom) > 1e-9f) ? (0.5f * (alpha - gamma) / denom) : 0.0f;
        float trueLag = static_cast<float>(peakLag) + delta;
        return (trueLag > 0.0f) ? (sampleRate / trueLag) : 0.0f;
    }

    return 0.0f;
}

ProbeManager::ProbeManager(MCPBridge* owner)
    : bridge(owner)
{
}

void ProbeManager::audioTick()
{
    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) continue;

        auto* oc = probe.outconnect.load(std::memory_order_acquire);
        if (!oc) continue;

        auto* signal = outconnect_get_signal(oc);
        if (!signal || !signal->s_vec) continue;

        int n = signal->s_n;
        if (n <= 0) continue;
        float* vec = signal->s_vec;

        float sumSq = 0.0f, peak = 0.0f;
        for (int i = 0; i < n; i++) {
            float s = vec[i];
            float abs_s = std::abs(s);
            sumSq += s * s;
            if (abs_s > peak) peak = abs_s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(n));

        ProbeResult result { probe.probeId, rms, peak, n };
        probe.resultQueue.try_enqueue(result);

        int writePos = probe.ringWritePos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            probe.ringBuffer[(writePos + i) % PROBE_RING_SIZE] = vec[i];
        }
        probe.ringWritePos.store((writePos + n) % PROBE_RING_SIZE, std::memory_order_release);
    }
}

int ProbeManager::startProbe(t_outconnect* oc, const juce::String& canvasName, const juce::String& tempId, int outletIndex, const juce::String& correlationId, int durationMs)
{
    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) {
            ProbeResult discard;
            while (probe.resultQueue.try_dequeue(discard)) {}

            probe.probeId = nextProbeId.fetch_add(1);
            if (probe.probeId == 0) probe.probeId = nextProbeId.fetch_add(1);
            probe.correlationId = correlationId;
            probe.canvasName = canvasName;
            probe.tempId = tempId;
            probe.outletIndex = outletIndex;
            probe.durationMs = std::max(20, durationMs);
            probe.startTimeMs = juce::Time::getMillisecondCounter();
            probe.accRms = 0.0f;
            probe.accPeak = 0.0f;
            probe.accBlocks = 0;
            probe.ringWritePos.store(0, std::memory_order_relaxed);
            probe.ringBuffer.fill(0.0f);
            probe.outconnect.store(oc, std::memory_order_release);
            probe.active.store(true, std::memory_order_release);
            return static_cast<int>(probe.probeId);
        }
    }
    return -1;
}

void ProbeManager::stopProbe(uint32_t probeId, const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed) && probe.probeId == probeId) {
            ProbeResult item;
            while (probe.resultQueue.try_dequeue(item)) {
                probe.accRms += item.rms * item.rms;
                if (item.peak > probe.accPeak) probe.accPeak = item.peak;
                probe.accBlocks++;
            }

            float meanRms = (probe.accBlocks > 0) ? std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks)) : 0.0f;
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;

            float sampleRate = 44100.0f;
            if (bridge && bridge->processor) {
                sampleRate = static_cast<float>(bridge->processor->getSampleRate());
            }
            float freq = estimateFrequency(probe.ringBuffer.data(), PROBE_RING_SIZE, sampleRate);

            auto corr = correlationId.isNotEmpty() ? correlationId : probe.correlationId;
            if (corr.isNotEmpty() && bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + corr) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

void ProbeManager::stopAllProbes(const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed)) {
            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            if (probe.correlationId.isNotEmpty() && bridge) {
                bridge->sendReply("/meter/error/" + probe.correlationId, "Probe cancelled");
            }
        }
    }
    if (correlationId.isNotEmpty() && bridge) {
        bridge->sendRawReply("/meter/stop_all/reply/" + correlationId);
    }
}

void ProbeManager::collectResults()
{
    auto now = juce::Time::getMillisecondCounter();
    float sampleRate = 44100.0f;
    if (bridge && bridge->processor) {
        sampleRate = static_cast<float>(bridge->processor->getSampleRate());
    }

    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) continue;

        ProbeResult item;
        while (probe.resultQueue.try_dequeue(item)) {
            probe.accRms += item.rms * item.rms;
            if (item.peak > probe.accPeak) probe.accPeak = item.peak;
            probe.accBlocks++;
        }

        if (now - probe.startTimeMs >= static_cast<juce::int64>(probe.durationMs)) {
            if (probe.accBlocks == 0) {
                juce::String errCorr = probe.correlationId;
                bool wasSpectral = probe.spectral;
                probe.spectral = false;
                probe.active.store(false, std::memory_order_release);
                probe.outconnect.store(nullptr, std::memory_order_release);
                if (bridge) {
                    juce::String errAddr = wasSpectral ? "/meter/spectral/error/" : "/meter/error/";
                    bridge->sendReply(errAddr + errCorr, "Timeout: No audio blocks processed (is DSP running?)");
                }
                continue;
            }

            float meanRms = std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks));
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;
            std::array<float, PROBE_RING_SIZE> linearBuf;
            int wPos = probe.ringWritePos.load(std::memory_order_acquire);
            for (int i = 0; i < PROBE_RING_SIZE; i++) {
                linearBuf[i] = probe.ringBuffer[(wPos + i) % PROBE_RING_SIZE];
            }
            float freq = estimateFrequency(linearBuf.data(), PROBE_RING_SIZE, sampleRate);

            if (probe.spectral && bridge) {
                // === SPECTRAL ANALYSIS (Phase 7) ===
                // Apply Hann window
                constexpr int N = PROBE_RING_SIZE;
                std::array<float, N> windowed;
                for (int i = 0; i < N; i++) {
                    float w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(N - 1)));
                    windowed[i] = linearBuf[i] * w;
                }

                // Run real FFT via FFTW3
                constexpr int NBINS = N / 2 + 1;
                std::array<float, N> fftInput;
                std::copy(windowed.begin(), windowed.end(), fftInput.begin());

                // Use fftwf (single precision)
                std::array<fftwf_complex, NBINS> fftOutput;
                fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, fftInput.data(),
                    reinterpret_cast<fftwf_complex*>(fftOutput.data()), FFTW_ESTIMATE);
                fftwf_execute(plan);
                fftwf_destroy_plan(plan);

                // Compute magnitude spectrum (dB)
                std::array<float, NBINS> magnitudes;
                float binHz = sampleRate / static_cast<float>(N);
                float sumMag = 0.0f;
                float sumWeightedFreq = 0.0f;
                float sumLogMag = 0.0f;
                float maxMag = 0.0f;
                int maxBin = 0;

                for (int i = 0; i < NBINS; i++) {
                    float re = fftOutput[i][0];
                    float im = fftOutput[i][1];
                    float mag = std::sqrt(re * re + im * im) / static_cast<float>(N);
                    magnitudes[i] = mag;

                    if (i > 0) { // Skip DC bin for spectral features
                        sumMag += mag;
                        sumWeightedFreq += mag * (static_cast<float>(i) * binHz);
                        if (mag > 1e-10f) sumLogMag += std::log(mag);
                        else sumLogMag += std::log(1e-10f);
                        if (mag > maxMag) { maxMag = mag; maxBin = i; }
                    }
                }

                // Spectral centroid (Hz)
                float spectralCentroid = (sumMag > 1e-10f) ? (sumWeightedFreq / sumMag) : 0.0f;

                // Spectral flatness (0 = tonal, 1 = noise)
                int numBins = NBINS - 1; // exclude DC
                float geometricMean = std::exp(sumLogMag / static_cast<float>(numBins));
                float arithmeticMean = sumMag / static_cast<float>(numBins);
                float spectralFlatness = (arithmeticMean > 1e-10f) ? (geometricMean / arithmeticMean) : 0.0f;
                spectralFlatness = std::min(1.0f, std::max(0.0f, spectralFlatness));

                // Crest factor (peak / RMS)
                float crestFactor = (meanRms > 1e-7f) ? (peakVal / meanRms) : 0.0f;

                // Peak frequency bin
                float peakFreq = static_cast<float>(maxBin) * binHz;

                // Spectral rolloff (frequency below which 85% of energy lives)
                float totalEnergy = 0.0f;
                for (int i = 1; i < NBINS; i++) totalEnergy += magnitudes[i] * magnitudes[i];
                float rolloffThreshold = totalEnergy * 0.85f;
                float accumEnergy = 0.0f;
                float rolloffFreq = 0.0f;
                for (int i = 1; i < NBINS; i++) {
                    accumEnergy += magnitudes[i] * magnitudes[i];
                    if (accumEnergy >= rolloffThreshold) {
                        rolloffFreq = static_cast<float>(i) * binHz;
                        break;
                    }
                }

                // Top 8 frequency peaks (for harmonic analysis)
                struct FreqPeak { float freq; float magDb; };
                std::array<FreqPeak, 8> topPeaks {};
                std::array<float, NBINS> magCopy;
                std::copy(magnitudes.begin(), magnitudes.end(), magCopy.begin());
                for (int p = 0; p < 8; p++) {
                    int best = 1;
                    for (int i = 2; i < NBINS - 1; i++) {
                        if (magCopy[i] > magCopy[best]) best = i;
                    }
                    if (magCopy[best] < 1e-10f) break;
                    topPeaks[p].freq = static_cast<float>(best) * binHz;
                    topPeaks[p].magDb = 20.0f * std::log10(magCopy[best]);
                    // Zero out neighborhood to find next peak
                    for (int k = std::max(1, best - 3); k <= std::min(NBINS - 1, best + 3); k++) {
                        magCopy[k] = 0.0f;
                    }
                }

                // Build JSON response
                auto* rootObj = new juce::DynamicObject();
                rootObj->setProperty("rmsDb", rmsDb);
                rootObj->setProperty("peakDb", peakDb);
                rootObj->setProperty("fundamental", freq);
                rootObj->setProperty("spectralCentroid", spectralCentroid);
                rootObj->setProperty("spectralFlatness", spectralFlatness);
                rootObj->setProperty("spectralRolloff", rolloffFreq);
                rootObj->setProperty("crestFactor", crestFactor);
                rootObj->setProperty("peakFrequency", peakFreq);
                rootObj->setProperty("sampleRate", sampleRate);
                rootObj->setProperty("fftSize", N);
                rootObj->setProperty("binHz", binHz);
                rootObj->setProperty("blocks", probe.accBlocks);

                juce::Array<juce::var> peaksArray;
                for (int p = 0; p < 8 && topPeaks[p].freq > 0.0f; p++) {
                    auto* pk = new juce::DynamicObject();
                    pk->setProperty("freq", topPeaks[p].freq);
                    pk->setProperty("dB", topPeaks[p].magDb);
                    peaksArray.add(juce::var(pk));
                }
                rootObj->setProperty("peaks", peaksArray);

                juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);

                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/spectral/result/" + probe.correlationId) };
                rep.addArgument(jsonString);
                bridge->sender.send(rep);

                probe.spectral = false;
            } else if (bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + probe.correlationId) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
        }
    }
}

int ProbeManager::getActiveProbeCount() const
{
    int count = 0;
    for (auto const& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed)) {
            count++;
        }
    }
    return count;
}

bool MCPBridge::activateProbing()
{
    if (!plugdata_debugging_enabled()) {
        set_plugdata_debugging_enabled(1);
        sys_lock();
        canvas_update_dsp();
        sys_unlock();
        probeManager.setDebugEnabledByUs(true);
        return true;
    }
    return false;
}

t_outconnect* MCPBridge::resolveProbeTarget(const juce::String& canvasName, const juce::String& targetId, int outletIndex, juce::String& errorOut)
{
    if (!processor) {
        errorOut = "Processor unavailable";
        return nullptr;
    }

    t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (!cnv) {
        errorOut = "Canvas not found: " + canvasName;
        return nullptr;
    }

    t_gobj* gobj = processor->resolveStableId(canvasName, targetId);

    if (!gobj) {
        bool isNumber = targetId.isNotEmpty();
        for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
            if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                isNumber = false;
                break;
            }
        }
        if (isNumber) {
            int idx = targetId.getIntValue();
            gobj = glistObjectAt(cnv, idx);
        }
    }

    if (!gobj) {
        errorOut = "Object not found: " + targetId;
        return nullptr;
    }

    t_object* obj = pd::Interface::checkObject(gobj);
    if (!obj) {
        errorOut = "Target is not a valid Pd object: " + targetId;
        return nullptr;
    }

    t_outlet* outlet = obj->ob_outlet;
    for (int i = 0; i < outletIndex && outlet; i++) {
        outlet = outlet->o_next;
    }
    if (!outlet) {
        errorOut = "Outlet " + juce::String(outletIndex) + " not found on object " + targetId;
        return nullptr;
    }

    if (outlet->o_sym != gensym("signal")) {
        errorOut = "Outlet " + juce::String(outletIndex) + " is not a signal outlet (control rate)";
        return nullptr;
    }

    if (!outlet->o_connections) {
        errorOut = "Signal outlet has no connections";
        return nullptr;
    }

    return outlet->o_connections;
}

void MCPBridge::handleMeterDomain(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;

    if (meterAction == "start") {
        if (msg.size() < 2) return;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto tempId = getArgString(msg[1]);
        int outletIndex = 0;
        int durationMs = 500;
        juce::String correlationId = "0";

        if (msg.size() == 3) {
            correlationId = getArgString(msg[2]);
        } else if (msg.size() == 4) {
            outletIndex = static_cast<int>(getArgFloat(msg[2]));
            correlationId = getArgString(msg[3]);
        } else if (msg.size() >= 5) {
            outletIndex = static_cast<int>(getArgFloat(msg[2]));
            durationMs = static_cast<int>(getArgFloat(msg[3]));
            correlationId = getArgString(msg[4]);
        }

        activateProbing();

        sys_lock();
        juce::String errorOut;
        t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
        sys_unlock();

        if (!oc) {
            sendReply("/meter/error/" + correlationId, errorOut);
            return;
        }

        int probeId = probeManager.startProbe(oc, canvasName, tempId, outletIndex, correlationId, durationMs);
        if (probeId <= 0) {
            sendReply("/meter/error/" + correlationId, "Max active probes exceeded (limit: " + juce::String(MAX_PROBES) + ")");
            return;
        }

        if (!isTimerRunning()) {
            startTimer(15);
        }

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/start/reply/" + correlationId) };
        rep.addArgument(static_cast<int32>(probeId));
        sender.send(rep);
        return;
    }

    if (meterAction == "stop") {
        if (msg.size() < 1) return;
        uint32_t probeId = static_cast<uint32_t>(getArgFloat(msg[0]));
        juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "";
        probeManager.stopProbe(probeId, correlationId);
        return;
    }

    if (meterAction == "stop_all") {
        juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "";
        probeManager.stopAllProbes(correlationId);
        return;
    }

    if (meterAction == "query") {
        if (msg.size() < 2) return;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto tempId = getArgString(msg[1]);
        int outletIndex = 0;
        juce::String correlationId = "0";

        if (msg.size() == 3) {
            correlationId = getArgString(msg[2]);
        } else if (msg.size() >= 4) {
            outletIndex = static_cast<int>(getArgFloat(msg[2]));
            correlationId = getArgString(msg[3]);
        }

        bool justActivated = activateProbing();
        if (justActivated) {
            juce::Thread::sleep(5);
        }

        sys_lock();
        juce::String errorOut;
        t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
        if (!oc) {
            sys_unlock();
            sendReply("/meter/error/" + correlationId, errorOut);
            return;
        }

        t_signal* signal = outconnect_get_signal(oc);
        if (!signal || !signal->s_vec) {
            sys_unlock();
            sendReply("/meter/error/" + correlationId, "Signal not ready (is DSP running?)");
            return;
        }

        int n = signal->s_n;
        int nchans = signal->s_nchans;
        float* vec = signal->s_vec;
        float sumSq = 0.0f, peak = 0.0f;
        for (int i = 0; i < n; i++) {
            float s = vec[i];
            float abs_s = std::abs(s);
            sumSq += s * s;
            if (abs_s > peak) peak = abs_s;
        }
        sys_unlock();

        float rms = (n > 0) ? std::sqrt(sumSq / static_cast<float>(n)) : 0.0f;
        float rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
        float peakDb = (peak > 1e-7f) ? (20.0f * std::log10(peak)) : -100.0f;

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/query/reply/" + correlationId) };
        rep.addArgument(rmsDb);
        rep.addArgument(peakDb);
        rep.addArgument(static_cast<int32>(nchans));
        rep.addArgument(static_cast<int32>(n));
        sender.send(rep);
        return;
    }

    if (meterAction == "spectral") {
        // /meter/spectral <canvasName> <tempId> [outletIndex] [durationMs] <correlationId>
        // Starts a probe that captures audio into the ring buffer, then runs FFT
        // on completion. Returns spectral centroid, flatness, RMS, peak, fundamental,
        // crest factor, and top frequency bins.
        if (msg.size() < 2) return;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto tempId = getArgString(msg[1]);
        int outletIndex = 0;
        int durationMs = 200; // ~4400 samples at 44.1kHz, fills ring buffer multiple times
        juce::String correlationId = "0";

        if (msg.size() == 3) {
            correlationId = getArgString(msg[2]);
        } else if (msg.size() == 4) {
            outletIndex = static_cast<int>(getArgFloat(msg[2]));
            correlationId = getArgString(msg[3]);
        } else if (msg.size() >= 5) {
            outletIndex = static_cast<int>(getArgFloat(msg[2]));
            durationMs = static_cast<int>(getArgFloat(msg[3]));
            correlationId = getArgString(msg[4]);
        }

        if (durationMs < 50) durationMs = 50;
        if (durationMs > 2000) durationMs = 2000;

        activateProbing();

        sys_lock();
        juce::String errorOut;
        t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
        sys_unlock();

        if (!oc) {
            sendReply("/meter/spectral/error/" + correlationId, errorOut);
            return;
        }

        int probeId = probeManager.startProbe(oc, canvasName, tempId, outletIndex, correlationId, durationMs);
        if (probeId <= 0) {
            sendReply("/meter/spectral/error/" + correlationId, "Max active probes exceeded");
            return;
        }

        // Mark this probe as spectral so collectResults runs FFT
        for (auto& probe : probeManager.probes) {
            if (probe.probeId == static_cast<uint32_t>(probeId)) {
                probe.spectral = true;
                break;
            }
        }

        if (!isTimerRunning()) {
            startTimer(15);
        }

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/spectral/started/" + correlationId) };
        rep.addArgument(static_cast<int32>(probeId));
        sender.send(rep);
        return;
    }

    if (meterAction == "status") {
        juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        int activeCount = probeManager.getActiveProbeCount();
        int debugEnabled = plugdata_debugging_enabled();

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/status/reply/" + correlationId) };
        rep.addArgument(static_cast<int32>(activeCount));
        rep.addArgument(static_cast<int32>(debugEnabled));
        sender.send(rep);
        return;
    }
}

void MCPBridge::timerCallback()
{
    probeManager.collectResults();

    std::vector<MorphJob> activeJobs;
    std::vector<juce::String> completedIds;

    {
        const juce::ScopedLock sl(morphLock);
        for (auto& job : morphJobs) {
            job.currentStep++;
            float t = static_cast<float>(job.currentStep) / static_cast<float>(job.totalSteps);
            if (t > 1.0f) t = 1.0f;

            for (auto const& p : job.params) {
                float val = p.from + (p.to - p.from) * t;
                if (processor) {
                    processor->sendFloat(p.name.toRawUTF8(), val);
                }
            }

            if (job.currentStep >= job.totalSteps) {
                completedIds.push_back(job.correlationId);
            } else {
                activeJobs.push_back(job);
            }
        }
        morphJobs = activeJobs;
    }

    for (auto const& id : completedIds) {
        sendRawReply("/morph/run/done/" + id);
    }

    {
        const juce::ScopedLock sl(morphLock);
        if (morphJobs.empty() && probeManager.getActiveProbeCount() == 0) {
            stopTimer();
        }
    }
}

void MCPBridge::sendSelectionTelemetry(const juce::String& selector, const SmallArray<pd::Atom>& list)
{
    if (!active.load()) return;

    juce::String addr;
    if (selector.startsWith("/")) addr = selector;
    else if (selector == "selected_object") addr = "/pd/ui/selection";
    else if (selector == "selection_count") addr = "/pd/ui/count";
    else if (selector == "selection_indices") addr = "/pd/ui/indices";
    else addr = "/pd/ui/" + selector;

    juce::OSCMessage msg { juce::OSCAddressPattern(addr) };
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].isFloat()) {
            msg.addArgument(list[i].getFloat());
        } else if (list[i].isSymbol()) {
            msg.addArgument(juce::String::fromUTF8(list[i].getSymbol()->s_name));
        }
    }
    sender.send(msg);
}

void MCPBridge::sendConsoleLog(const juce::String& message, bool isError)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(isError ? "/pd/error" : "/pd/log") };
    msg.addArgument(message);
    sender.send(msg);
}

void MCPBridge::sendPrompt(const juce::String& promptText)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/mcp_prompt") };
    msg.addArgument(promptText);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::Array<juce::var>& args)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    for (auto const& v : args) {
        if (v.isDouble() || v.isInt() || v.isInt64()) {
            msg.addArgument(static_cast<float>(v));
        } else {
            msg.addArgument(v.toString());
        }
    }
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, float val)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(val);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::String& str)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(str);
    sender.send(msg);
}

void MCPBridge::sendRawReply(const juce::String& addressPattern)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    sender.send(msg);
}
