/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: parameter, trigger, telemetry and chat (Plan 3 P3.3): the domain functions moved unchanged out of MCPBridge.cpp.

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeBound.h"          // truth slice 3: send_checked's receivers
#include "MCPBridgeOwnReceivers.h"   // mcpIsBridgeOwnReceiverClass: the bridge's own probes (control, ai-ask, widget state) are no receivers
#include "MCPBridgePdAccessors.h"
#include "Mcp/McpWidgetCallJson.h"  // phase 3 (widget_call): the reply JSON, built after the collector is unbound

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
    // /chat/lane <deep|quick> (capability pill_lane): the server's sticky pill lane, pushed at connect and at every switch,
    // so the QUICK / DEEP chip on the pill shows the word the artist typed. State only: nothing is posted to the chat. The
    // chat domain takes every verb, so no table row or schema row is needed; anything but the two words is ignored, and an
    // older fork that sees a one-argument message here ignores it (it reads two).
    if (action == "lane") {
        auto const word = msg.size() >= 1 ? getArgString(msg[0]) : juce::String();
        if (word == "deep" || word == "quick") {
            bool const deep = word == "deep";
            bool const changed = isPillDeepLane() != deep;
            setPillDeepLane(deep);
            // The chip is drawn from this flag: a switch the server made (a typed `deep` / `quick`, a reconnect) repaints the pills.
            if (changed && processor)
                postMessage([p = processor] { p->getMcp()->gui().repaintAll(); });
        }
        return;
    }
    // /chat/widget_watch <on|off> (capability widget_state): the server switches the live widget history on or off. State only: nothing
    // is posted to the chat. While it is on, a widget whose state has been quiet for 1 s with a new rev is one event
    // `/pd/widget_ask state main <widget> <rev>` (MCPBridge::pumpWidgetAsks, Source/Mcp/McpWidgetState.h). Off at start and after every
    // stop: a server that does not send `on` (an older one) never sees a state event. Like /chat/lane, a verb of the chat domain (which
    // takes every verb: no table row, no schema row, no new hash); any other word, or none, is ignored.
    if (action == "widget_watch") {
        auto const word = msg.size() >= 1 ? getArgString(msg[0]) : juce::String();
        if (word == "on" || word == "off")
            setWidgetStateWatch(word == "on");
        return;
    }
    // /chat/msg <role> <text> [<mark>]: the optional third word is what the turn's tool results proved for an AI answer
    // ("changed=4 measured=-18.0", "explained"; the server's services/talk-mark.ts, parsed by McpCopilotTalk.h). An older server
    // sends two arguments, and an older fork reads two: both keep working.
    if (msg.size() >= 2) {
        auto role = getArgString(msg[0]);
        auto text = getArgString(msg[1]);
        auto mark = msg.size() >= 3 ? getArgString(msg[2]) : juce::String();
        postChatMessage(role, text, mark);
    }
}

// ── Truth slice 3 (capability send_checked): /pd/send_checked <canvas> <corrId> <name> <argc> <atoms...> ─────────────
// A send to a Pd name that says what happened, from C++ reads: who listens (MCPBridgeBound.h), whether a console line
// came while it was delivered (Pd prints synchronously), and per receiver what it holds now. Refuses Pd's own names
// (fail closed: `pd`, `pd-*`, `#*`, `.*`, `mcp_host`, and any name that also reaches something that is not a box: a
// canvas by namecanvas, an array, a dialog stub); an unbound name is answered without sending. A pd-lua `pd.Receive`
// proxy ("pdlua proxy receive", not a box) is a listener: counted as `proxies` (never read as a t_object, review H5), the
// name is sent to when there are boxes or proxies, and only the boxes are read back (a name with proxies alone has
// `bound: true`, `receivers: []`, and `delivered` says a Lua object took it and no console line came). Runs in the Pd context
// (message thread under the Pd lock). Realtime /param and /trigger stay as they are (sequences). Atoms as obj_set:
// floats as floats, strings as symbols ("s:" prefix optional). Reply /pd/send_checked/reply/<corrId>:
// {name, bound, proxies, receivers:[{tempId, class, readBack:{value, source, reason}}], delivered, console:[{tempId, text}],
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

// pd-lua's `pd.Receive` proxy (pdlua.c t_pdlua_proxyreceive: a t_pd + owner + name, 24 bytes, its class made with flags 0 so
// pd_checkobject passes on it). It is a listener of the name, not a box: reading it as a t_object overreads the heap
// (review H5), so it is recognised by its class NAME alone (`*b` is the class pointer: no struct field is read), counted
// and delivered to, never read.
bool isLuaProxyReceive(t_pd* b)
{
    char const* const cls = b ? class_getname(*b) : nullptr;
    return cls && std::strcmp(cls, "pdlua proxy receive") == 0;
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
    root->setProperty("proxies", 0);
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
        // pd-lua proxy receives are listeners that are not boxes: counted here, never read as a t_object (review H5), and
        // kept out of the placed-box walk (they are on no canvas, so the walk could never stop early for them).
        int proxies = 0;
        std::vector<t_pd*> candidates;
        for (auto* b : binders) {
            if (mcpIsBridgeOwnReceiverClass(*b)) continue; // the bridge's own probes (control window, ai-ask, widget state): no listener, never refused
            if (isLuaProxyReceive(b)) ++proxies;
            else candidates.push_back(b);
        }
        std::vector<t_object*> receivers;
        auto const placed = mcp::bound::placedAmong(candidates); // review H5: only a placed box is read as a t_object
        for (auto* b : candidates) {
            t_object* o = (*b == canvas_class || placed.found.count(b) == 0) ? nullptr : pd_checkobject(b);
            if (!o && *b != canvas_class && placed.walkLimit) {
                // The walk stopped at its cap before it found this binder: it may be a box; not known, so not sent.
                root->setProperty("refused", "walk limit");
                root->setProperty("refusedClass", juce::String::fromUTF8(class_getname(*b)));
                return;
            }
            if (!o) {
                // Review M6, fail closed: the name also reaches something that is not a box (a canvas by namecanvas,
                // an array, a dialog stub, any other class that sits on no canvas: review H5): never sent. (pd-lua's
                // pd.Receive proxy is the one such class that is a listener: counted above.)
                root->setProperty("refused", "non-object binder"); // server review B: the exact word
                root->setProperty("refusedClass", juce::String::fromUTF8(class_getname(*b)));
                return;
            }
            receivers.push_back(o);
        }
        root->setProperty("bound", !receivers.empty() || proxies > 0);
        root->setProperty("proxies", proxies);
        if (receivers.empty() && proxies == 0) return; // nothing listens: nothing is sent

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

// ── Phase 3 of the ai_widget plan (capability widget_call): /pd/widget_call <corrId> <widget> <paddr|-> <argc> <atoms...> ──────────────
// Reads and proposes on ONE ai.* widget in one call, without the shared control window. Under the Pd lock, on the Pd context (the message
// thread, like /pd/send_checked): a collector is bound to the widget's STATE name (`ai-draw-state`, a looper's `ai-loop_<name>-state`), the
// atoms are sent to its IN name (`ai-draw-in`) exactly the way /pd/send_checked sends (the same selector rule, pd_typedmess on the name's
// bound thing, so a pd-lua widget's own `pd.Receive` proxy hears it as well as a canvas [r]), the collector is unbound again, and the frames
// it caught, cut into sets at each `end`, are the reply (Source/Mcp/McpWidgetCallJson.h). `paddr` names one instance (`to <paddr>` is put in
// front of the atoms: only that object acts, the others stay silent); `-` sends the message as it is.
//
// The floor (mcp::widgetCallPlan, Source/Mcp/McpWidgetCall.h): the first word of the message is dump, ghost, ghostname or unghost, and nothing
// else gets through (do, clear, flow, beat, ...): the AI proposes and reads, it never drives. The widget name must be ai.<word>; the in-name
// has to be listened on by boxes and pd-lua proxies only (the same fail-closed sort as send_checked: an array, a canvas by namecanvas or any
// other binder that is not a box makes the whole call refused, nothing is sent). The collector is made before and freed after the bind by a
// guard, never reachable from the audio thread (the Pd lock is held from the bind to the unbind) and it allocates nothing while bound: the log
// is one fixed object made BEFORE the Pd context is entered.
//
// Reply /pd/widget_call/reply/<corrId>: { widget, listeners, sets: [{ rev, at, frames }], truncated, unended[, refused][, error] }. A Pd-context
// timeout says `listeners: null` and `error`. Nothing listening is `listeners: 0` and no sets.
namespace {

// What listens on a widget's in-name, sorted like send_checked sorts a name's binders (fail closed): the bridge's own receivers are no
// listeners; a pd-lua `pd.Receive` proxy is one (counted, never read as a t_object, review H5); a box is one only when it is found placed on a
// canvas (placedAmong), and is never read here; anything else, or a walk that stopped at its cap before it found a binder, refuses the call.
struct WidgetCallListeners {
    int count = 0;
    std::string refused;
};

WidgetCallListeners sortWidgetCallBinders(t_symbol* in)
{
    WidgetCallListeners out;
    int proxies = 0;
    std::vector<t_pd*> candidates;
    for (auto* b : mcp::bound::bindersOf(in)) {
        if (mcpIsBridgeOwnReceiverClass(*b)) continue;
        if (isLuaProxyReceive(b)) ++proxies;
        else candidates.push_back(b);
    }
    auto const placed = mcp::bound::placedAmong(candidates); // review H5: only a placed box is a box
    int boxes = 0;
    for (auto* b : candidates) {
        bool const isBox = *b != canvas_class && placed.found.count(b) != 0 && pd_checkobject(b) != nullptr;
        if (!isBox) {
            out.refused = (*b != canvas_class && placed.walkLimit) ? "walk limit" : "non-object binder"; // review M6, fail closed
            return out;
        }
        ++boxes;
    }
    out.count = proxies + boxes;
    return out;
}

// One atom of the request, kept until the call is done (the floor reads the words in place).
struct WidgetCallWire {
    bool isSymbol = false;
    double num = 0.0;
    std::string text;
};

std::string widgetCallText(juce::OSCArgument const& arg)
{
    auto s = getArgString(arg);
    if (s.startsWith("s:")) s = s.substring(2); // as send_checked: an optional marker that a string is a word
    return s.toStdString();
}

// The request's atoms (OSC floats and ints are numbers, strings are words), `argc` of them from index 4 on; the caller checked the message holds them.
std::vector<WidgetCallWire> readWidgetCallWire(juce::OSCMessage const& msg, int argc)
{
    std::vector<WidgetCallWire> wire(static_cast<std::size_t>(argc));
    for (int a = 0; a < argc; ++a) {
        auto const& arg = msg[4 + a];
        auto& w = wire[static_cast<std::size_t>(a)];
        if (arg.isFloat32() || arg.isInt32()) {
            w.num = static_cast<double>(getArgFloat(arg));
        } else {
            w.isSymbol = true;
            w.text = widgetCallText(arg);
        }
    }
    return wire;
}

// The Pd atoms of the call: `to <paddr>` first when the address came as an argument, then the request. gensym: the caller holds the Pd lock.
std::vector<t_atom> widgetCallAtoms(std::vector<WidgetCallWire> const& wire, std::string const& addressPrefix)
{
    std::vector<t_atom> atoms;
    atoms.reserve(wire.size() + 2);
    if (!addressPrefix.empty()) {
        t_atom to;
        SETSYMBOL(&to, gensym("to"));
        atoms.push_back(to);
        t_atom who;
        SETSYMBOL(&who, gensym(addressPrefix.c_str()));
        atoms.push_back(who);
    }
    for (auto const& w : wire) {
        t_atom at;
        if (w.isSymbol) SETSYMBOL(&at, gensym(w.text.c_str()));
        else SETFLOAT(&at, static_cast<t_float>(w.num));
        atoms.push_back(at);
    }
    return atoms;
}

// The selector /pd/send_checked picks for a list of atoms, so the widget's receivers see the same message either way.
t_symbol* widgetCallSelector(std::vector<t_atom> const& atoms)
{
    return atoms.empty() ? &s_bang
        : atoms.size() == 1 && atoms[0].a_type == A_FLOAT ? &s_float
        : atoms.size() == 1 && atoms[0].a_type == A_SYMBOL ? &s_symbol
        : &s_list;
}

} // namespace

void MCPBridge::pdWidgetCall(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (!processor || msg.size() < 4) return;
    auto const correlationId = getArgString(msg[0]);
    auto const address = "/pd/widget_call/reply/" + correlationId;
    auto const widget = widgetCallText(msg[1]);
    auto const paddr = widgetCallText(msg[2]);
    float const rawArgc = getArgFloat(msg[3]);
    int const argc = rawArgc >= 1.0f ? (rawArgc > 100000.0f ? 100000 : static_cast<int>(rawArgc)) : 0; // a huge or nan count is never cast

    mcp::WidgetCallReplyInfo info;
    info.widget = widget.c_str();
    info.listeners = 0;
    auto const reply = [&](mcp::WidgetCallLog const* log) {
        sendReply(address, juce::String::fromUTF8(mcp::widgetCallReplyJson(info, log).c_str()));
    };
    auto const refuse = [&](char const* why) {
        info.refused = why;
        reply(nullptr);
    };
    if (!mcp::widgetAskNameOk(widget.c_str())) return refuse(mcp::widgetCallRefusalText(mcp::WidgetCallRefusal::NoWidget));
    if (argc > mcp::kWidgetCallMaxArgs) return refuse(mcp::widgetCallRefusalText(mcp::WidgetCallRefusal::TooManyAtoms));
    if (static_cast<int>(msg.size()) < 4 + argc) return refuse("the message carries fewer atoms than its argc says");

    auto const wire = readWidgetCallWire(msg, argc);
    std::vector<mcp::WidgetCallArg> args(wire.size()); // after `wire` is complete: the words are read in place
    for (std::size_t i = 0; i < wire.size(); ++i) {
        args[i].isSymbol = wire[i].isSymbol;
        args[i].num = wire[i].num;
        args[i].sym = wire[i].isSymbol ? wire[i].text.c_str() : nullptr;
    }
    auto const plan = mcp::widgetCallPlan(args.data(), argc, paddr.c_str()); // the floor: before anything reaches Pd
    if (plan.refusal != mcp::WidgetCallRefusal::None) return refuse(mcp::widgetCallRefusalText(plan.refusal));

    char inName[mcp::kWidgetAskNameLen + 8] = {};
    char stateName[mcp::kWidgetAskNameLen + 12] = {};
    if (!mcp::widgetAskInName(widget.c_str(), inName, sizeof(inName)) || !mcp::widgetStateSymbolName(widget.c_str(), stateName, sizeof(stateName)))
        return refuse(mcp::widgetCallRefusalText(mcp::WidgetCallRefusal::NoWidget));
    // The address argument (the atoms did not say it: the floor checked) goes in front as `to <paddr>`; "-" sends the atoms as they are.
    std::string const addressPrefix = paddr == "-" ? std::string() : paddr;

    auto log = std::make_unique<mcp::WidgetCallLog>(); // the room, reserved before the Pd context is entered
    std::string refusedHere;
    auto* host = processor->getMcp();
    bool const ran = host->pdWait([&] {
        t_symbol* const in = mcp::bound::findSymbolNoCreate(inName);
        auto const listeners = sortWidgetCallBinders(in);
        if (!listeners.refused.empty()) {
            refusedHere = listeners.refused;
            return;
        }
        info.listeners = listeners.count;
        if (listeners.count == 0) return; // nothing listens: nothing is sent

        auto atoms = widgetCallAtoms(wire, addressPrefix);
        t_symbol* const selector = widgetCallSelector(atoms);
        t_symbol* const state = gensym(stateName); // made before the bind: nothing is made while the collector is bound
        unsigned int const seqBefore = mcp_last_seq();
        {
            McpCallCollectorGuard const collector(*log, state); // bound from here to the closing brace, under the Pd lock
            pd_typedmess(in->s_thing, selector, static_cast<int>(atoms.size()), atoms.data());
        }
        host->noteAiSeqRange(seqBefore, mcp_last_seq()); // what the message made downstream is the AI's, as for send_checked
    }, 3000);

    if (!refusedHere.empty()) return refuse(refusedHere.c_str());
    if (!ran) {
        // The Pd context did not answer (the job never ran and never will: pdWait abandons it): nothing is known.
        info.listeners = -1;
        info.error = "the Pd context did not answer in time";
        reply(nullptr);
        return;
    }
    reply(log.get());
}
