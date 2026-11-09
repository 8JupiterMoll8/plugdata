/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: parameter, trigger, telemetry and chat (Plan 3 P3.3): the domain functions moved unchanged out of MCPBridge.cpp.

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeBound.h"          // truth slice 3: send_checked's receivers
#include "MCPBridgeControlWindow.h"  // mcpCtrlReceiverClass: the bridge's own probes are no receivers
#include "MCPBridgePdAccessors.h"

void MCPBridge::handleParamDomain(const juce::String& /*paramName*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /param <name> <value...> — the receiver name is the FIRST
    // argument, never the address path.
    auto paramName = getArgString(msg[0]);
    if (paramName.isEmpty() || paramName == "nil") return;

    std::vector<t_atom> shadow; // P4: record last value per bus for /pd/param_get
    if (msg.size() == 2) {
        auto const& v = msg[1];
        if (v.isFloat32() || v.isInt32()) {
            t_atom a; SETFLOAT(&a, getArgFloat(v)); shadow.push_back(a);
            processor->sendFloat(paramName.toRawUTF8(), getArgFloat(v));
        } else {
            auto s = getArgString(v);
            if (s.startsWith("s:")) s = s.substring(2);
            t_atom a; SETSYMBOL(&a, gensym(s.toRawUTF8())); shadow.push_back(a);
            processor->sendSymbol(paramName.toRawUTF8(), s.toRawUTF8());
        }
    } else if (msg.size() > 2) {
        SmallArray<pd::Atom> atoms;
        for (int i = 1; i < msg.size(); ++i) {
            auto const& arg = msg[i];
            if (arg.isFloat32() || arg.isInt32()) {
                t_atom a; SETFLOAT(&a, getArgFloat(arg)); shadow.push_back(a);
                atoms.add(pd::Atom(getArgFloat(arg)));
            } else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                t_atom a; SETSYMBOL(&a, gensym(s.toRawUTF8())); shadow.push_back(a);
                atoms.add(pd::Atom(processor->generateSymbol(s)));
            }
        }
        processor->sendList(paramName.toRawUTF8(), atoms);
    }

    {
        std::lock_guard<std::mutex> lk(s_paramShadowMutex);
        s_paramShadow[paramName.toStdString()] = shadow;
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

    if ((triggerAction == "cc" || triggerAction == "controlchange") && msg.size() >= 3) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int cc = static_cast<int>(getArgFloat(msg[1]));
        int val = static_cast<int>(getArgFloat(msg[2]));
        processor->sendControlChange(ch, cc, val);
        return;
    }

    if ((triggerAction == "bend" || triggerAction == "pitchbend") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendPitchBend(ch, val);
        return;
    }

    if ((triggerAction == "aftertouch" || triggerAction == "touch") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendAfterTouch(ch, val);
        return;
    }

    if ((triggerAction == "program" || triggerAction == "programchange") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendProgramChange(ch, val);
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
            t_canvas* canvas = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (canvas) {
                t_gobj* obj = glistObjectAt(canvas, idx);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    mcp::PdOps::removeObjects(canvas, toDelete);
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();
        }
    }
}

void MCPBridge::handleChatDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // /chat/msg <role> <text>
    if (msg.size() >= 2) {
        auto role = getArgString(msg[0]);
        auto text = getArgString(msg[1]);
        postChatMessage(role, text);
    }
}

// ── Truth slice 3 (capability send_checked): /pd/send_checked <canvas> <corrId> <name> <argc> <atoms...> ─────────────
// A send to a Pd name that says what happened, from C++ reads: who listens (MCPBridgeBound.h), whether a console line
// came while it was delivered (Pd prints synchronously), and per receiver what it holds now. Refuses Pd's own names
// (fail closed: `pd`, `pd-*`, `#*`, `.*`, `mcp_host`, and any name that also reaches something that is not a box: a
// canvas by namecanvas, an array, a dialog stub); an unbound name is answered without sending. Runs in the Pd context
// (message thread under the Pd lock). Realtime /param and /trigger stay as they are (sequences). Atoms as obj_set:
// floats as floats, strings as symbols ("s:" prefix optional). Reply /pd/send_checked/reply/<corrId>:
// {name, bound, receivers:[{tempId, class, readBack:{value, source, reason}}], delivered, console:[{tempId, text}],
// refused[, refusedClass]}. A delivered send also writes the /param shadow (/pd/param_get). On a Pd-context timeout
// bound is null (not known) and `error` says so.
namespace {

constexpr int kMaxSendReceivers = 64;

juce::String refusedName(juce::String const& name)
{
    if (name.isEmpty()) return "no name";
    if (name == "pd" || name.startsWith("pd-")) return "a Pd system name (pd, pd-<canvas>): never sent from the AI";
    if (name.startsWith("#")) return "a reserved name (#...): never sent from the AI";
    if (name.startsWith(".")) return "a reserved name (.x...: Pd's own window names): never sent from the AI";
    if (name == "mcp_host") return "the bridge's own name: never sent from the AI";
    return {};
}

// What a receiver holds after the send: a GUI's value; else the record this send left on one of its outlet wires.
juce::var receiverReadBack(t_object* o, unsigned int seqBefore, unsigned int seqAfter)
{
    auto* rb = new juce::DynamicObject();
    rb->setProperty("value", juce::var());
    rb->setProperty("source", juce::var());
    rb->setProperty("reason", "");
    double gui = 0.0;
    if (mcpGuiValue(o, gui)) {
        rb->setProperty("value", gui);
        rb->setProperty("source", "gui");
        return juce::var(rb);
    }
    for (int n = 0; n < obj_noutlets(o); ++n) {
        t_outlet* op = nullptr;
        for (t_outconnect* oc = obj_starttraverseoutlet(o, &op, n); oc;) {
            t_symbol* sel = nullptr;
            int argc = 0;
            t_atom atoms[kMcpLastAtoms] {};
            unsigned int seq = 0;
            if (mcp_outconnect_last(oc, &sel, &argc, atoms, &seq)) {
                unsigned int const at = seq - seqBefore;
                if (at >= 1 && at <= seqAfter - seqBefore) {
                    if (argc >= 1 && atoms[0].a_type == A_FLOAT) rb->setProperty("value", static_cast<double>(atoms[0].a_w.w_float));
                    else if (argc >= 1 && atoms[0].a_type == A_SYMBOL && atoms[0].a_w.w_symbol)
                        rb->setProperty("value", juce::String::fromUTF8(atoms[0].a_w.w_symbol->s_name));
                    else rb->setProperty("value", juce::String::fromUTF8(sel && sel->s_name ? sel->s_name : ""));
                    rb->setProperty("source", "last");
                    return juce::var(rb);
                }
            }
            t_object* dest = nullptr;
            t_inlet* in = nullptr;
            int which = 0;
            oc = obj_nexttraverseoutlet(oc, &dest, &in, &which);
        }
    }
    rb->setProperty("reason", "no GUI value and no wire carried it on (nothing downstream recorded this send)");
    return juce::var(rb);
}

} // namespace

void MCPBridge::pdSendChecked(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 3) return;
    auto const correlationId = getArgString(msg[1]);
    auto const name = getArgString(msg[2]);
    int const argc = msg.size() > 3 ? std::max(0, static_cast<int>(getArgFloat(msg[3]))) : 0;
    auto const address = "/pd/send_checked/reply/" + correlationId;

    auto* root = new juce::DynamicObject();
    juce::var const rootVar(root);
    root->setProperty("name", name);
    root->setProperty("bound", false);
    root->setProperty("receivers", juce::Array<juce::var>());
    root->setProperty("delivered", false);
    root->setProperty("console", juce::Array<juce::var>());
    root->setProperty("refused", "");
    if (auto const why = refusedName(name); why.isNotEmpty()) {
        root->setProperty("refused", why);
        sendReply(address, juce::JSON::toString(rootVar, true));
        return;
    }

    auto* host = processor->getMcp();
    bool const ran = host->pdWait([&] {
        std::vector<t_atom> atoms;
        for (int a = 0, cursor = 4; a < argc && cursor < static_cast<int>(msg.size()); ++a, ++cursor) {
            auto const& arg = msg[static_cast<size_t>(cursor)];
            t_atom at;
            if (arg.isFloat32() || arg.isInt32()) SETFLOAT(&at, getArgFloat(arg));
            else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                SETSYMBOL(&at, gensym(s.toRawUTF8()));
            }
            atoms.push_back(at);
        }
        t_symbol* sym = mcp::bound::findSymbolNoCreate(name.toRawUTF8());
        auto const binders = mcp::bound::bindersOf(sym);
        std::vector<t_object*> receivers;
        auto const placed = mcp::bound::placedAmong(binders); // review H5: only a placed box is read as a t_object
        for (auto* b : binders) {
            if (mcpCtrlReceiverClass && *b == mcpCtrlReceiverClass) continue; // the bridge's own probe
            t_object* o = (*b == canvas_class || placed.found.count(b) == 0) ? nullptr : pd_checkobject(b);
            if (!o && *b != canvas_class && placed.walkLimit) {
                // The walk stopped at its cap before it found this binder: it may be a box; not known, so not sent.
                root->setProperty("refused", "walk limit");
                root->setProperty("refusedClass", juce::String::fromUTF8(class_getname(*b)));
                return;
            }
            if (!o) {
                // Review M6, fail closed: the name also reaches something that is not a box (a canvas by namecanvas,
                // an array, a dialog stub, a pd-lua pd.Receive proxy: review H5): never sent.
                root->setProperty("refused", "non-object binder"); // server review B: the exact word
                root->setProperty("refusedClass", juce::String::fromUTF8(class_getname(*b)));
                return;
            }
            receivers.push_back(o);
        }
        root->setProperty("bound", !receivers.empty());
        if (receivers.empty()) return; // nothing listens: nothing is sent

        t_symbol* selector = atoms.empty() ? &s_bang
            : atoms.size() == 1 && atoms[0].a_type == A_FLOAT ? &s_float
            : atoms.size() == 1 && atoms[0].a_type == A_SYMBOL ? &s_symbol
            : &s_list;
        uint64_t const consoleBefore = host->consoleSeqNow();
        unsigned int const seqBefore = mcp_last_seq();
        pd_typedmess(sym->s_thing, selector, static_cast<int>(atoms.size()), atoms.empty() ? nullptr : atoms.data());
        unsigned int const seqAfter = mcp_last_seq();
        host->noteAiSeqRange(seqBefore, seqAfter);
        {
            // Server review A: the same shadow /param keeps, so /pd/param_get stays true after a send_checked.
            std::lock_guard<std::mutex> lk(s_paramShadowMutex);
            s_paramShadow[name.toStdString()] = atoms;
        }

        // Console: every error line the delivery printed, and any line a receiver printed. (obj_set_batch's delivered
        // counts only lines from the set object, error or not: review L9, see mcpPutDelivery.)
        juce::Array<juce::var> console;
        for (auto const& hit : host->consoleSince(consoleBefore)) {
            bool const fromReceiver = std::find(receivers.begin(), receivers.end(), static_cast<t_object const*>(hit.object)) != receivers.end();
            if (!hit.error && !fromReceiver) continue;
            auto* c = new juce::DynamicObject();
            c->setProperty("tempId", hit.tempId);
            c->setProperty("text", hit.text);
            console.add(juce::var(c));
        }
        root->setProperty("console", console);

        juce::Array<juce::var> list;
        bool live = true;
        for (auto* o : receivers) {
            if (list.size() >= kMaxSendReceivers) break;
            // A receiver the send deleted (a [r] that closes its own patch) is no longer bound: read nothing from it.
            auto const now = mcp::bound::bindersOf(sym);
            if (std::find(now.begin(), now.end(), &o->ob_pd) == now.end()) {
                live = false;
                continue;
            }
            auto* r = new juce::DynamicObject();
            r->setProperty("tempId", host->getStableId(&o->ob_g));
            r->setProperty("class", juce::String::fromUTF8(class_getname(pd_class(&o->ob_pd))));
            r->setProperty("readBack", receiverReadBack(o, seqBefore, seqAfter));
            list.add(juce::var(r));
        }
        root->setProperty("receivers", list);
        root->setProperty("delivered", live && console.isEmpty());
    }, 3000);
    if (!ran) {
        // Server review D: nothing is known (it may or may not have run): bound is null, never false.
        root->setProperty("bound", juce::var());
        root->setProperty("error", "the Pd context did not answer in time");
    }
    sendReply(address, juce::JSON::toString(rootVar, true));
}
