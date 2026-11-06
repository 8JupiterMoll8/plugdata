/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// /pd/inlet_values (V1, 2026-11-06): the LIVE value each inlet of an object holds, so the doctor can say "the filter's
// cutoff is 0 Hz right now" instead of "the tone stops after the filter". Read only; message thread, caller holds the
// Pd lock; never on the audio thread.
//
// Where Pd keeps a value (m_obj.c at the plugdata-pd pin):
//   - a signal inlet's scalar (used while no signal is connected): the main inlet's CLASS_MAINSIGNALIN field, a
//     secondary signal inlet's iu_floatsignalvalue. Both are read through Pd's public obj_findsignalscalar().
//   - a float inlet made with floatinlet_new(owner, &field): the field it points to (i_floatslot);
//     a symbol inlet (symbolinlet_new): the symbol it points to.
//   - an inlet made with inlet_new(owner, dest, s1, s2) only FORWARDS (e.g. osc~'s phase inlet, "ft1"): Pd keeps no
//     copy, so its value is null with that reason. So is the main inlet of a control object, and a pointer inlet.
// t_inlet is private to m_obj.c. The bridge already mirrors its prefix (McpInletMirror, MCPBridgeObjectValues.h); this
// mirrors the whole struct and CHECKS the mirror on every object before trusting a slot: each inlet's owner field must
// be the object and every secondary signal inlet's scalar address must equal what obj_findsignalscalar() returns. A
// failed check reads every slot as null ("layout check failed"), never a guess. No Pd patch is needed.
#pragma once

#include "MCPBridgeCmdCommon.h"

namespace mcp::inlets {

// Pd's struct _inlet and union inletunion (m_obj.c:33-50), field for field.
struct InletMirror {
    t_pd i_pd;
    InletMirror* i_next;
    t_object* i_owner;
    t_pd* i_dest;
    t_symbol* i_symfrom;
    union {
        t_symbol* symto;
        t_gpointer* pointerslot;
        t_float* floatslot;
        t_symbol** symslot;
        t_float floatsignalvalue;
    } i_un;
};

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

/** The mirror check for one object (see the top of this file). */
inline bool layoutCheck(t_object* o)
{
    int physical = (pd_class(&o->ob_pd)->c_firstin) ? 1 : 0;
    for (auto* in = reinterpret_cast<InletMirror*>(o->ob_inlet); in; in = in->i_next, ++physical) {
        if (in->i_owner != o) return false;
        if (in->i_symfrom == &s_signal) {
            int const sig = obj_siginletindex(o, physical);
            if (sig < 0 || obj_findsignalscalar(o, sig) != &in->i_un.floatsignalvalue) return false;
        }
    }
    return true;
}

inline juce::var number(t_float f) { return std::isfinite(f) ? juce::var(static_cast<double>(f)) : juce::var(); }

inline juce::String kindOf(t_symbol* s)
{
    if (s == &s_float) return "float";
    if (s == &s_symbol) return "symbol";
    if (s == &s_pointer) return "pointer";
    if (s == &s_list) return "list";
    if (s == &s_signal) return "signal";
    return "other";
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

/** Every inlet of `o`, left to right. Caller holds the Pd lock. */
inline juce::var objectInlets(t_object* o, std::unordered_map<int, Wires> const& wires)
{
    juce::Array<juce::var> list;
    auto wiresOf = [&](int i) { auto it = wires.find(i); return it == wires.end() ? Wires {} : it->second; };
    t_class* cls = pd_class(&o->ob_pd);
    bool const layoutOk = layoutCheck(o);
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
    for (auto* in = reinterpret_cast<InletMirror*>(o->ob_inlet); in; in = in->i_next, ++physical) {
        auto const w = wiresOf(physical);
        if (!layoutOk) {
            list.add(inletJson(physical, obj_issignalinlet(o, physical) ? "signal" : "other", w, {}, {}, "inlet layout check failed: not read"));
            continue;
        }
        t_symbol* const from = in->i_symfrom;
        if (from == &s_signal) {
            int const sig = obj_siginletindex(o, physical);
            t_float const f = sig >= 0 ? *obj_findsignalscalar(o, sig) : 0.0f;
            if (sig < 0) list.add(inletJson(physical, "signal", w, {}, {}, "no signal index for this inlet"));
            else if (w.signal > 0) list.add(inletJson(physical, "signal", w, {}, "scalar", noSignal));
            else list.add(inletJson(physical, "signal", w, number(f), "scalar", {}));
        } else if (in->i_dest == nullptr && from == &s_float) {
            if (in->i_un.floatslot) list.add(inletJson(physical, "float", w, number(*in->i_un.floatslot), "floatSlot", {}));
            else list.add(inletJson(physical, "float", w, {}, {}, "float inlet without a slot"));
        } else if (in->i_dest == nullptr && from == &s_symbol) {
            t_symbol* const s = in->i_un.symslot ? *in->i_un.symslot : nullptr;
            if (s && s->s_name) list.add(inletJson(physical, "symbol", w, juce::var(juce::String::fromUTF8(s->s_name)), "symbolSlot", {}));
            else list.add(inletJson(physical, "symbol", w, {}, {}, "symbol inlet without a symbol"));
        } else if (in->i_dest == nullptr && from == &s_pointer) {
            list.add(inletJson(physical, "pointer", w, {}, {}, "a pointer inlet holds a pointer, not a value"));
        } else {
            juce::String const to = (in->i_un.symto && in->i_un.symto->s_name) ? juce::String::fromUTF8(in->i_un.symto->s_name) : juce::String("?");
            list.add(inletJson(physical, from ? kindOf(from) : juce::String("other"), w, {}, {},
                "forwarded to the object as '" + to + "': Pd keeps no copy in the inlet"));
        }
    }
    return juce::var(list);
}

} // namespace mcp::inlets
