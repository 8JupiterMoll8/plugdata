/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include "MCPBridge.h"
#include "PluginProcessor.h"
#include "Pd/Interface.h"

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

void MCPBridge::handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 2) return;

    // Two conventions:
    //   write/read: /array/<action> <name> <subpatch> <corrId> ...  (name = msg[0])
    //   stats:      /array ["stats", name, sampleRate, corrId]      (name = msg[1])
    auto arrayName = arrayAction == "stats" ? getArgString(msg[1]) : getArgString(msg[0]);
    auto canvasName = normalizeCanvas(getArgString(msg[1]));

    sys_lock();
    t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(gensym(arrayName.toRawUTF8()), garray_class));

    // PlugData instantiates arrays through the GUI; arrays created via the
    // MCP text-object path may never bind their garray. If a write targets a
    // missing array, create it as a graph-on-parent via paste (the same
    // mechanism pd::Patch::createObject uses for arrays).
    if (!garray && arrayAction == "write" && msg.size() >= 5) {
        int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
        int dataCount = msg.size() - 5;
        int size = std::max(64, chunkIndex * 128 + dataCount);

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (cnv) {
            // Stagger each auto-created array so they don't stack on top of
            // each other in the top-left corner.
            int nobj = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
            int stagger = (nobj * 14) % 420;
            juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                + arrayName + " " + juce::String(size) + " float 2;\n#X coords 0 1 "
                + juce::String(size > 1 ? size - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
            pd::Interface::paste(cnv, pasta.toRawUTF8());
            garray = reinterpret_cast<t_garray*>(pd_findbyclass(gensym(arrayName.toRawUTF8()), garray_class));
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

    if (arrayAction == "write" && msg.size() >= 5) {
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

        int const CHUNK = 128;
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
        reply.addArgument(juce::String("boot:" + bootToken));
        sender.send(reply);
    }
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
                probe.active.store(false, std::memory_order_release);
                probe.outconnect.store(nullptr, std::memory_order_release);
                if (bridge) {
                    bridge->sendReply("/meter/error/" + errCorr, "Timeout: No audio blocks processed (is DSP running?)");
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

            if (bridge) {
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
