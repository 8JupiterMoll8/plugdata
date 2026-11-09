/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Object value helpers of the object commands (Plan 3 P3.3 S11): the live-property set core, the inlet
// resolver and the obj_get JSON builders that MCPBridgeCmdObjects.cpp uses. Moved unchanged (static -> inline).

#pragma once

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgePdAccessors.h"
#include "MCPBridgeInletValues.h" // truth slice 3: read-back through the inlet facts

// Resolve the pd target for a given inlet index. <=0 -> the object itself. Walks Pd's inlet list through the fork's
// accessor (patches/submodules/pure-data/0004; review 2026-11-06: no mirrored struct _inlet). Inlet 0 is the object
// itself (not in ob_inlet).
inline t_pd *mcpResolveInlet(t_object *o, int inletIndex)
{
    if (!o) return nullptr;
    if (inletIndex <= 0) return reinterpret_cast<t_pd *>(o);
    t_inlet *in = o->ob_inlet;
    for (int i = 1; in && i < inletIndex; ++i) in = mcp_inlet_next(in);
    return in ? reinterpret_cast<t_pd *>(in) : nullptr;
}

// ── PRD_OBJ_GET_SET P5b: selectors that can trigger a DSP graph recompile ─────
inline bool mcpIsStructuralSelector(const juce::String& sel)
{
    static const std::unordered_set<std::string> s = {
        "resize", "block~", "switch~", "clone", "send~", "receive~"
    };
    return s.count(sel.toStdString()) > 0;
}

// Shared live-property set core (obj_set / obj_set_batch). Caller holds sys_lock.
// Returns "" on success, else an error string. Sets *warning on structural hit.
// Mark an IEM GUI scalar widget as "persist my value". Pd's iemgui savefns
// (slider_save / toggle_save / …) write the current value ONLY when x_loadinit
// is set; otherwise they write 0 and the widget resets to min on reload — the
// "slider went down / patch came up silent" trap. New MCP widgets default Init
// ON (McpIemArgs.h); this flips it on for widgets set via set[] so an EXISTING
// patch's knobs (created before the fix, Init off) start persisting too.
inline void mcpMarkGuiLoadinit(t_object* o, const juce::String& clsLower)
{
    if (!o) return;
    if (clsLower == "hsl" || clsLower == "vsl")
        reinterpret_cast<t_slider*>(o)->x_gui.x_isa.x_loadinit = 1;
    else if (clsLower == "tgl")
        reinterpret_cast<t_toggle*>(o)->x_gui.x_isa.x_loadinit = 1;
    else if (clsLower == "hradio" || clsLower == "vradio")
        reinterpret_cast<t_radio*>(o)->x_gui.x_isa.x_loadinit = 1;
    else if (clsLower == "nbx")
        reinterpret_cast<t_my_numbox*>(o)->x_gui.x_isa.x_loadinit = 1;
}

// Truth slice 3 (handMoved): a set that gives a GUI its value: inlet 0, float / set / list.
inline bool mcpIsGuiValueSet(int inlet, juce::String const& selector)
{
    return inlet <= 0 && (selector == "float" || selector == "set" || selector == "list");
}

inline juce::String mcpObjSetCore(t_gobj* g, const juce::String& tempId, int inlet,
                                   const juce::String& selector, std::vector<t_atom>& atoms,
                                   juce::String* warning)
{
    if (!g) return "error: unknown tempId '" + tempId + "'";
    t_object* o = pd::Interface::checkObject(g);
    if (!o) return "error: object not resolvable '" + tempId + "'";
    t_pd* dest = mcpResolveInlet(o, inlet);
    if (!dest) return "error: could not resolve inlet " + juce::String(inlet);
    // Guard 1.5 (connective rename): Pd's [receive]/[send] are CLASS_NOINLET and
    // register no runtime `set` method (see Libraries/pure-data/src/x_connective.c
    // — only bang/float/symbol/list/pointer/anything). A `receive`/`send`/`set`
    // selector therefore falls through to the class `anything` handler, which
    // FORWARDS the message out the receiver's outlet — a silent misroute that
    // surfaces as a confusing downstream error (e.g. "osc~: no method for
    // 'receive'"). Refuse clearly instead of leaking.
    {
        juce::String clsName = juce::String::fromUTF8(class_getname(pd_class(&o->ob_pd)));
        if ((clsName == "receive" || clsName == "send")
            && (selector == "receive" || selector == "send" || selector == "set")) {
            return "error: Pd's [" + clsName + "] cannot be renamed at runtime — use retype/edit to recreate it with the new name";
        }
    }
    if (warning && mcpIsStructuralSelector(selector))
        *warning = "structural:" + selector + " (may trigger a DSP recompile — prefer edit)";
    // Guard 1 (dead-knob safety net): a float/list sent to a signal-rate inlet
    // may be ignored by objects with no scalar/float handler on that inlet.
    // obj_issignalinlet() cannot distinguish a scalar-accepting signal inlet
    // (e.g. lop~ inlet 1) from a pure audio inlet, so this is ADVISORY, not a fix
    // and never a redirect — misrouting (e.g. osc~ phase) would be worse.
    // INDEX SPACE (R25, live-verified): `inlet` (N>=1) is the N-th EXPLICIT
    // inlet in physical order — mcpResolveInlet() walks ob_inlet, the same list
    // Pd lays out left-to-right (inlet_new appends), so it already IS the
    // 0-based physical index obj_issignalinlet() wants. The old "#72 known gap"
    // (from-the-end index) was a misdiagnosis: `set{inlet:1, 440}` on osc~ hits
    // the float PHASE inlet (d_osc.c osc_new: inlet_new(&ob_pd, &s_float, ft1)),
    // which accepts the float — a pitch probe just can't see the phase change.
    // Live proof: set{inlet:1, 200} on [bp~ 1000 5] moved the band 1kHz→215Hz
    // (center frequency = physical inlet 1), not Q.
    if (warning && inlet >= 0 && (selector == "float" || selector == "list")
        && obj_issignalinlet(o, inlet) != 0) {
        juce::String g1 = "inlet-signal: '" + selector + "' -> signal inlet " + juce::String(inlet)
            + " — if this inlet has no scalar/float handler the message is silently ignored; "
              "verify the param inlet (often inlet 1, varies by object)";
        *warning = warning->isEmpty() ? g1 : (*warning + "; " + g1);
    }
    // Persist-on-set: a GUI value set through the message lane must survive a
    // save/reload. Flip the widget's Init flag before dispatching the float so
    // the next save writes the live value (see mcpMarkGuiLoadinit).
    if (inlet == 0 && (selector == "float" || selector == "list")) {
        juce::String clsLower = juce::String::fromUTF8(class_getname(pd_class(&o->ob_pd))).toLowerCase();
        mcpMarkGuiLoadinit(o, clsLower);
    }
    pd_typedmess(dest, gensym(selector.toRawUTF8()), static_cast<int>(atoms.size()),
                 atoms.empty() ? nullptr : atoms.data());
    return {};
}

// ── Tier-2 value read ──────────────────────────────────────────────────
// For arg-objects the parameter IS the creation arg, and our set[] convention
// adds `relabel`, so the printed text stays truthful to the live value. Map a
// class to the index of the numeric arg that is its value (0 = first arg after
// the class name). Keep this list conservative and numeric-only.
inline int mcpArgValueIndexForClass(const juce::String& cls)
{
    static const std::unordered_map<std::string, int> kMap = {
        { "osc~", 0 }, { "phasor~", 0 }, { "saw~", 0 }, { "tri~", 0 },
        { "lop~", 0 }, { "hip~", 0 }, { "vcf~", 0 },
        { "*~", 0 }, { "+~", 0 }, { "-~", 0 }, { "/~", 0 },
        { "delread~", 1 },
    };
    auto it = kMap.find(cls.toStdString());
    return it == kMap.end() ? -1 : it->second;
}

inline bool mcpExtractArgValue(const juce::String& className, const juce::String& objectText, double& outValue)
{
    int const idx = mcpArgValueIndexForClass(className);
    if (idx < 0) return false;
    juce::StringArray toks;
    toks.addTokens(objectText, " ", "");
    int const tokIdx = 1 + idx;           // toks[0] is the class name
    if (tokIdx >= toks.size()) return false;
    auto const tok = toks[tokIdx];
    if (tok.isEmpty()) return false;
    auto cp = tok.getCharPointer();
    auto const start = cp;
    juce::CharacterFunctions::readDoubleValue(cp);
    if (cp == start) return false;        // leading token is not numeric
    outValue = tok.getDoubleValue();
    return true;
}

// Fill {type, text, props, value, valueSource} for one object. Caller holds sys_lock.
inline void mcpFillObjectJson(t_gobj* gobj, juce::DynamicObject* out)
{
    juce::String className = juce::String::fromUTF8(class_getname(pd_class(&gobj->g_pd)));
    if (pd_class(&gobj->g_pd) == canvas_class) {
        juce::String an;
        className = pd::getAbstractionFileName(gobj, an) ? an : juce::String("pd");
    }
    juce::String objectText;
    t_object* obj = pd::Interface::checkObject(gobj);
    if (obj) {
        char* text = nullptr; int len = 0;
        mcpGetObjectText(obj, &text, &len);
        if (text && len > 0) { objectText = juce::String::fromUTF8(text, len); freebytes(text, len); }
    }
    out->setProperty("type", className);
    out->setProperty("text", objectText);
    auto* props = mcpBuildGuiProps(className, obj);
    out->setProperty("props", juce::var(props));

    // Honest provenance: "stored" = live widget value; "arg" = parsed from the
    // creation text (truthful because set[] adds relabel); "none" = the object
    // publishes no readable value (a bare DSP object). The AI can then trust the
    // number it sees — and know when NOT to trust it.
    if (props->hasProperty("value")) {
        out->setProperty("value", props->getProperty("value"));
        out->setProperty("valueSource", "stored");
    } else {
        double v = 0.0;
        if (mcpExtractArgValue(className, objectText, v)) {
            out->setProperty("value", v);
            out->setProperty("valueSource", "arg");
        } else {
            out->setProperty("valueSource", "none");
        }
    }
}

// Serialize one object to a JSON string. Caller holds sys_lock.
inline juce::String mcpBuildObjJson(t_gobj* gobj)
{
    auto* root = new juce::DynamicObject();
    mcpFillObjectJson(gobj, root);
    return juce::JSON::toString(juce::var(root));
}

// Liveness guard: is `g` still in the canvas object list? A stale identity map
// (artist cleared/deleted objects via GUI) can hand obj_get/obj_set a dangling
// pointer. Callers resolve the id (member context) then check this.
inline bool mcpGobjIsLive(t_canvas* cnv, t_gobj* g)
{
    if (!cnv || !g) return false;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
        if (y == g) return true;
    return false;
}

// ── Truth slice 3 (capability set_delivery): what an AI set did, read back under the same Pd lock ──────────────────
// The bridge dispatches a value and then reads, without guessing:
//   delivered  the object still exists AND printed no console line while the message was delivered (Pd prints
//              synchronously: Instance.cpp processPrint -> McpHost::consoleMessage). A line Pd prints in a later tick
//              is outside this window: the fact is "no error during delivery".
//   readBack   {value, source, reason}: a GUI's value for inlet 0 (source "gui"); else the inlet's own copy
//              ("scalar" for a signal inlet's scalar, "floatSlot" for a float inlet, "symbolSlot"); else the record
//              pure-data/0005 kept for that inlet ("last") but ONLY when this very dispatch made it (its seq is inside
//              the dispatch's range); else null with the inlet's reason (e.g. forwarded: Pd keeps no copy).
inline juce::var mcpReadBack(McpHost* host, t_canvas* cnv, t_gobj* g, int inlet, unsigned int seqBefore, unsigned int seqAfter)
{
    auto* rb = new juce::DynamicObject();
    rb->setProperty("value", juce::var());
    rb->setProperty("source", juce::var());
    rb->setProperty("reason", "");
    t_object* o = (g && mcpGobjIsLive(cnv, g)) ? pd::Interface::checkObject(g) : nullptr;
    if (!o) {
        rb->setProperty("reason", "the object is gone");
        return juce::var(rb);
    }
    int const k = inlet < 0 ? 0 : inlet;
    double gui = 0.0;
    if (k == 0 && mcpGuiValue(o, gui)) {
        rb->setProperty("value", gui);
        rb->setProperty("source", "gui");
        return juce::var(rb);
    }
    std::unordered_set<t_object*> wanted { o };
    auto const wires = mcp::inlets::wiresInto(cnv, wanted);
    auto const wit = wires.find(o);
    auto const inlets = mcp::inlets::objectInletsBase(o, wit == wires.end() ? std::unordered_map<int, mcp::inlets::Wires> {} : wit->second);
    juce::var entry;
    if (auto const* list = inlets.getArray())
        for (auto const& e : *list)
            if (static_cast<int>(e["index"]) == k) entry = e;
    if (entry.isObject() && !entry["value"].isVoid()) {
        rb->setProperty("value", entry["value"]);
        rb->setProperty("source", entry["source"]);
        return juce::var(rb);
    }
    // The record of this inlet, if this dispatch made it.
    auto const last = mcp::inlets::lastInto(cnv, wanted, [](t_object*) { return juce::String(); });
    auto const lo = last.find(o);
    if (lo != last.end()) {
        auto const li = lo->second.find(k);
        if (li != lo->second.end() && li->second.has) {
            unsigned int const at = li->second.seq - seqBefore;
            if (at >= 1 && at <= seqAfter - seqBefore) {
                auto const number = mcp::inlets::lastNumber(li->second);
                if (number) rb->setProperty("value", *number);
                else if (li->second.argc >= 1 && li->second.atoms[0].a_type == A_SYMBOL && li->second.atoms[0].a_w.w_symbol)
                    rb->setProperty("value", juce::String::fromUTF8(li->second.atoms[0].a_w.w_symbol->s_name));
                rb->setProperty("source", "last");
                return juce::var(rb);
            }
        }
    }
    juce::ignoreUnused(host);
    rb->setProperty("reason", entry.isObject() && entry["reason"].toString().isNotEmpty() ? entry["reason"].toString()
        : juce::String("the inlet keeps no copy of what it got"));
    return juce::var(rb);
}

// delivered / console / readBack of one AI set. `consoleBefore` and the seq range were read around the dispatch.
inline void mcpPutDelivery(juce::DynamicObject* out, McpHost* host, t_canvas* cnv, t_gobj* g, int inlet,
    uint64_t consoleBefore, unsigned int seqBefore, unsigned int seqAfter)
{
    bool const live = g && mcpGobjIsLive(cnv, g);
    juce::Array<juce::var> console;
    if (live)
        for (auto const& line : host->consoleLinesSince(consoleBefore, g)) console.add(line);
    out->setProperty("delivered", live && console.isEmpty());
    out->setProperty("console", console);
    out->setProperty("readBack", mcpReadBack(host, cnv, g, inlet, seqBefore, seqAfter));
}
