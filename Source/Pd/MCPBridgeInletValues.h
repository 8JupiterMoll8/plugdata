/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// /pd/inlet_values (V1, 2026-11-06): the LIVE value each inlet of an object holds, so the doctor can say "the filter's
// cutoff is 0 Hz right now" instead of "the tone stops after the filter". Read only; message thread, caller holds the
// Pd lock; never on the audio thread.
//
// Where Pd keeps a value (m_obj.c at the plugdata-pd pin), read through the fork's accessors
// (patches/submodules/pure-data/0004, MCPBridgePdAccessors.h), each of which checks the inlet's class first:
//   - a signal inlet's scalar (used while no signal is connected): the main inlet's CLASS_MAINSIGNALIN field (offset
//     from the public t_class), a secondary signal inlet's value (mcp_inlet_signalscalar).
//   - a float inlet made with floatinlet_new(owner, &field): the field it points to (mcp_inlet_floatslot);
//     a symbol inlet (symbolinlet_new): the symbol it points to (mcp_inlet_symslot).
//   - an inlet made with inlet_new(owner, dest, s1, s2) only FORWARDS (e.g. osc~'s phase inlet, "ft1"): Pd keeps no
//     copy, so its value is null with that reason. So is the main inlet of a control object, and a pointer inlet.
// No mirrored struct: review 2026-11-06 (a mirror check cannot reject a bad layout for objects without signal
// inlets; a Pd bump could then read a wrong slot).
#pragma once

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgePdAccessors.h"

#include <functional>
#include <optional>

namespace mcp::inlets {

struct Wires {
    int signal = 0;
    int control = 0;
};

inline juce::String connectedName(Wires const& w)
{
    if (w.signal > 0 && w.control > 0) return "both";
    if (w.signal > 0) return "signal";
    if (w.control > 0) return "control";
    return "none";
}

/** The wires into each inlet of the objects in `wanted`, in one pass over the canvas. Caller holds the Pd lock. */
inline std::unordered_map<t_object*, std::unordered_map<int, Wires>> wiresInto(t_canvas* cnv, std::unordered_set<t_object*> const& wanted)
{
    std::unordered_map<t_object*, std::unordered_map<int, Wires>> out;
    if (!cnv) return out;
    t_linetraverser lt;
    linetraverser_start(&lt, cnv);
    while (linetraverser_next_nosize(&lt)) {
        if (!wanted.count(lt.tr_ob2)) continue;
        auto& w = out[lt.tr_ob2][lt.tr_inno];
        if (obj_issignaloutlet(lt.tr_ob, lt.tr_outno)) ++w.signal;
        else ++w.control;
    }
    return out;
}

inline juce::var number(t_float f) { return std::isfinite(f) ? juce::var(static_cast<double>(f)) : juce::var(); }

inline juce::String kindOf(t_symbol* s)
{
    if (s == gensym("float")) return "float";
    if (s == gensym("symbol")) return "symbol";
    if (s == gensym("pointer")) return "pointer";
    if (s == gensym("list")) return "list";
    if (s == gensym("signal")) return "signal";
    return "other";
}

// Truth slice 3 (capability inlet_last): the last control message an inlet got, from the records pure-data/0005 keeps
// on every wire (outconnect) and every secondary inlet. Inlet 0 has no record of its own (messages go straight to the
// object): its last is the newest record over the wires into it. An inlet >= 1 stores every message it gets, so its
// own record wins; else the newest wire. `by`: inside one of the bridge's own dispatch ranges (McpHost::seqIsAi) =
// "ai", else "other". Read only, under the Pd lock.
struct Last {
    bool has = false;
    t_symbol* sel = nullptr;
    int argc = 0;
    t_atom atoms[kMcpLastAtoms] {};
    unsigned int seq = 0;
    juce::String via; // "wire:<srcId>:<outlet>" or "inlet"
};

/** a is newer than b (the counter wraps: compared as a signed difference). */
inline bool newer(unsigned int a, unsigned int b) { return static_cast<int>(a - b) > 0; }

/** The first atom of a record as a number (nullopt: none, or not a float). */
inline std::optional<double> lastNumber(Last const& l)
{
    if (!l.has || l.argc < 1 || l.atoms[0].a_type != A_FLOAT) return std::nullopt;
    return static_cast<double>(l.atoms[0].a_w.w_float);
}

/**
 * The newest record per inlet of each object in `wanted`, in one pass over the canvas's wires plus each object's
 * inlet list. `nameOf` names a wire's source (tempId, else "class#index"). Caller holds the Pd lock.
 */
inline std::unordered_map<t_object*, std::unordered_map<int, Last>> lastInto(t_canvas* cnv, std::unordered_set<t_object*> const& wanted,
    std::function<juce::String(t_object*)> const& nameOf)
{
    std::unordered_map<t_object*, std::unordered_map<int, Last>> out;
    if (!cnv) return out;
    t_linetraverser lt;
    linetraverser_start(&lt, cnv);
    while (t_outconnect* oc = linetraverser_next_nosize(&lt)) {
        if (!wanted.count(lt.tr_ob2)) continue;
        Last l;
        if (!mcp_outconnect_last(oc, &l.sel, &l.argc, l.atoms, &l.seq)) continue;
        l.has = true;
        auto& cur = out[lt.tr_ob2][lt.tr_inno];
        if (cur.has && !newer(l.seq, cur.seq)) continue;
        l.via = "wire:" + nameOf(lt.tr_ob) + ":" + juce::String(lt.tr_outno);
        cur = l;
    }
    for (t_object* o : wanted) {
        int physical = pd_class(&o->ob_pd)->c_firstin ? 1 : 0;
        for (t_inlet* in = o->ob_inlet; in; in = mcp_inlet_next(in), ++physical) {
            Last l;
            if (!mcp_inlet_last(in, &l.sel, &l.argc, l.atoms, &l.seq)) continue;
            l.has = true;
            l.via = "inlet";
            out[o][physical] = l; // the inlet's own record: every message it got, by wire or sent to it
        }
    }
    return out;
}

/** {selector, atoms, seq, by, via[, truncated]} of a record. */
inline juce::var lastJson(Last const& l, bool ai)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("selector", juce::String::fromUTF8(l.sel && l.sel->s_name ? l.sel->s_name : ""));
    juce::Array<juce::var> atoms;
    for (int i = 0; i < std::min(l.argc, kMcpLastAtoms); ++i) {
        auto const& a = l.atoms[i];
        if (a.a_type == A_FLOAT) atoms.add(static_cast<double>(a.a_w.w_float));
        else if (a.a_type == A_SYMBOL && a.a_w.w_symbol) atoms.add(juce::String::fromUTF8(a.a_w.w_symbol->s_name));
        else if (a.a_type == A_POINTER) atoms.add("<pointer>");
        else atoms.add(juce::var());
    }
    o->setProperty("atoms", atoms);
    o->setProperty("seq", static_cast<juce::int64>(l.seq));
    o->setProperty("by", juce::String(ai ? "ai" : "other"));
    o->setProperty("via", l.via);
    if (l.argc > kMcpLastAtoms) o->setProperty("truncated", l.argc); // the full atom count; only the first 4 are kept
    return juce::var(o);
}

/** One inlet's entry. `value` stays null unless Pd holds it in a place this code can read for certain. */
inline juce::var inletJson(int index, juce::String const& kind, Wires const& w, juce::var value, juce::String const& source, juce::String const& reason)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("index", index);
    o->setProperty("kind", kind);
    o->setProperty("connected", connectedName(w));
    o->setProperty("signalWires", w.signal);
    o->setProperty("controlWires", w.control);
    o->setProperty("value", value);
    if (source.isNotEmpty()) o->setProperty("source", source);
    if (reason.isNotEmpty()) o->setProperty("reason", reason);
    return juce::var(o);
}

/** Every inlet of `o`, left to right, without the records. Caller holds the Pd lock. */
inline juce::var objectInletsBase(t_object* o, std::unordered_map<int, Wires> const& wires)
{
    juce::Array<juce::var> list;
    auto wiresOf = [&](int i) { auto it = wires.find(i); return it == wires.end() ? Wires {} : it->second; };
    t_class* cls = pd_class(&o->ob_pd);
    static juce::String const noSignal = "a signal is connected: Pd uses it, not the stored scalar";
    int physical = 0;
    if (cls->c_firstin) {
        auto const w = wiresOf(0);
        if (cls->c_floatsignalin > 0 && static_cast<size_t>(cls->c_floatsignalin) + sizeof(t_float) <= cls->c_size) {
            t_float const f = *reinterpret_cast<t_float*>(reinterpret_cast<char*>(o) + cls->c_floatsignalin);
            list.add(w.signal > 0 ? inletJson(0, "signal", w, {}, "scalar", noSignal) : inletJson(0, "signal", w, number(f), "scalar", {}));
        } else if (obj_issignalinlet(o, 0)) {
            list.add(inletJson(0, "signal", w, {}, {}, "a signal inlet without a stored scalar (the class declares none)"));
        } else {
            list.add(inletJson(0, "other", w, {}, {}, "the object's own inlet: its state is private to the object, Pd keeps no copy"));
        }
        physical = 1;
    }
    for (t_inlet* in = o->ob_inlet; in; in = mcp_inlet_next(in), ++physical) {
        auto const w = wiresOf(physical);
        t_symbol* from = nullptr;
        t_symbol* to = nullptr;
        if (t_float* scalar = mcp_inlet_signalscalar(in)) {
            list.add(w.signal > 0 ? inletJson(physical, "signal", w, {}, "scalar", noSignal) : inletJson(physical, "signal", w, number(*scalar), "scalar", {}));
        } else if (t_float* slot = mcp_inlet_floatslot(in)) {
            list.add(inletJson(physical, "float", w, number(*slot), "floatSlot", {}));
        } else if (t_symbol** sym = mcp_inlet_symslot(in)) {
            if (*sym && (*sym)->s_name) list.add(inletJson(physical, "symbol", w, juce::var(juce::String::fromUTF8((*sym)->s_name)), "symbolSlot", {}));
            else list.add(inletJson(physical, "symbol", w, {}, {}, "symbol inlet without a symbol"));
        } else if (mcp_inlet_ispointer(in)) {
            list.add(inletJson(physical, "pointer", w, {}, {}, "a pointer inlet holds a pointer, not a value"));
        } else if (mcp_inlet_generic(in, &from, &to)) {
            juce::String const target = (to && to->s_name) ? juce::String::fromUTF8(to->s_name) : juce::String("its own method");
            list.add(inletJson(physical, from ? kindOf(from) : juce::String("other"), w, {}, {},
                "forwarded to the object as '" + target + "': Pd keeps no copy in the inlet"));
        } else {
            list.add(inletJson(physical, "other", w, {}, {}, "an inlet kind the bridge does not know: not read"));
        }
    }
    return juce::var(list);
}

/**
 * Every inlet of `o`, left to right. Caller holds the Pd lock. With `last` (slice 3) each inlet that has a record
 * gets `last` ({selector, atoms, seq, by, via}); `isAi` says whose a seq was.
 */
inline juce::var objectInlets(t_object* o, std::unordered_map<int, Wires> const& wires,
    std::unordered_map<int, Last> const* last = nullptr, std::function<bool(unsigned int)> const& isAi = {})
{
    juce::var const out = objectInletsBase(o, wires);
    if (!last) return out;
    if (auto const* list = out.getArray())
        for (auto const& entry : *list) {
            auto const it = last->find(static_cast<int>(entry["index"]));
            if (it == last->end() || !it->second.has) continue;
            if (auto* obj = entry.getDynamicObject())
                obj->setProperty("last", lastJson(it->second, isAi && isAi(it->second.seq)));
        }
    return out;
}

} // namespace mcp::inlets
