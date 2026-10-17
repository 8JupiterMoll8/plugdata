/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: object values, parameters, census and wiring reads (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

// Prefix-only mirror of Pd's private struct _inlet (m_obj.c:42) — we read
// i_next only. Needed to address a specific physical inlet (>=1) without
// pulling in m_obj internals. Inlet 0 is the object itself (not in ob_inlet).
struct McpInletMirror { t_pd i_pd; McpInletMirror *i_next; };

// Resolve the pd target for a given inlet index. <=0 -> the object itself.
static t_pd *mcpResolveInlet(t_object *o, int inletIndex)
{
    if (!o) return nullptr;
    if (inletIndex <= 0) return reinterpret_cast<t_pd *>(o);
    auto *in = reinterpret_cast<McpInletMirror *>(o->ob_inlet);
    for (int i = 1; in && i < inletIndex; ++i) in = in->i_next;
    return in ? reinterpret_cast<t_pd *>(in) : nullptr;
}

// ── PRD_OBJ_GET_SET P5b: selectors that can trigger a DSP graph recompile ─────
static bool mcpIsStructuralSelector(const juce::String& sel)
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
static void mcpMarkGuiLoadinit(t_object* o, const juce::String& clsLower)
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

static juce::String mcpObjSetCore(t_gobj* g, const juce::String& tempId, int inlet,
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
static int mcpArgValueIndexForClass(const juce::String& cls)
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

static bool mcpExtractArgValue(const juce::String& className, const juce::String& objectText, double& outValue)
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
static void mcpFillObjectJson(t_gobj* gobj, juce::DynamicObject* out)
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
static juce::String mcpBuildObjJson(t_gobj* gobj)
{
    auto* root = new juce::DynamicObject();
    mcpFillObjectJson(gobj, root);
    return juce::JSON::toString(juce::var(root));
}

// Liveness guard: is `g` still in the canvas object list? A stale identity map
// (artist cleared/deleted objects via GUI) can hand obj_get/obj_set a dangling
// pointer. Callers resolve the id (member context) then check this.
static bool mcpGobjIsLive(t_canvas* cnv, t_gobj* g)
{
    if (!cnv || !g) return false;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
        if (y == g) return true;
    return false;
}

void MCPBridge::pdObjSet(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/obj_set <canvas> <corrId> <tempId> <inlet> <selector> <argc> <atoms...>
    // Live property/parameter set on a SPECIFIC object by tempId.
    //   inlet : <=0 = object inlet 0 (pd_typedmess); >=1 = physical inlet
    //   atoms : floats as-is; symbols tagged "s:name"
    // reply: 1.0 [optional "structural:<sel>" warning] | "error: ..."
    if (msg.size() >= 6 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        auto tempId = getArgString(msg[2]);
        int inlet = static_cast<int>(getArgFloat(msg[3]));
        auto selector = getArgString(msg[4]);
        if (selector.startsWith("s:")) selector = selector.substring(2);
        int argc = static_cast<int>(getArgFloat(msg[5]));
        int cursor = 6;

        std::vector<t_atom> atoms;
        if (argc > 0) atoms.reserve(static_cast<size_t>(argc));
        for (int a = 0; a < argc && cursor < msg.size(); a++) {
            auto const& arg = msg[cursor++];
            t_atom at;
            if (arg.isFloat32() || arg.isInt32()) SETFLOAT(&at, getArgFloat(arg));
            else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                SETSYMBOL(&at, gensym(s.toRawUTF8()));
            }
            atoms.push_back(at);
        }

        juce::String err, warning;
        {
            sys_lock();
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            t_gobj* g = processor->getMcp()->resolveStableId(canvasName, tempId);
            if (g && !mcpGobjIsLive(cnv, g)) g = nullptr; // stale → treat as unknown
            err = mcpObjSetCore(g, tempId, inlet, selector, atoms, &warning);
            sys_unlock();
        }

        juce::OSCMessage reply { juce::OSCAddressPattern("/pd/obj_set/reply/" + correlationId) };
        if (err.isNotEmpty()) reply.addArgument(err);
        else { reply.addArgument(1.0f); if (warning.isNotEmpty()) reply.addArgument(warning); }
        sender.send(reply);
    }
    return;
}

void MCPBridge::pdObjRelabel(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/obj_relabel <canvas> <corrId> <tempId> <text>
    // Rewrite a TEXT object's creation text (the printed label) WITHOUT recreating
    // it, so a live `set` can also make the number truthful — no recompile/dropout.
    // (getText() reads te_binbuf live, so the label repaints automatically.)
    // Reply: /pd/obj_relabel/reply/<corrId> <1|0>
    auto canvasName    = normalizeCanvas(getArgString(msg[0]));
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto tempId        = msg.size() > 2 ? getArgString(msg[2]) : juce::String();
    auto newText       = msg.size() > 3 ? getArgString(msg[3]) : juce::String();

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (!cnv) { sendReply("/pd/obj_relabel/reply/" + correlationId, 0.0f); return; }

    int ok = 0;
    sys_lock();
    t_gobj* g = processor->getMcp()->resolveStableId(canvasName, tempId);
    if (g && pd::Interface::isTextObject(g) && newText.isNotEmpty()) {
        auto* t = reinterpret_cast<t_text*>(g);
        if (t->te_binbuf) {
            binbuf_clear(t->te_binbuf);
            binbuf_text(t->te_binbuf, newText.toRawUTF8(), newText.getNumBytesAsUTF8());
            canvas_dirty(cnv, 1);
            ok = 1;
        }
    }
    sys_unlock();

    // NOTE: rewriting te_binbuf updates the STORED text (props/save). The
    // on-screen label is pulled lazily on the message thread by
    // TextBase::render()/updateTextLayout() via syncTextFromPd() — never
    // poke gui objects from the bridge (see 61c0c12f0 crash, reverted in
    // 0b75486d6). cc->repaint() (thread-safe) is enough to trigger it.
    processor->getMcp()->gui().repaintView(cnv);
    sendReply("/pd/obj_relabel/reply/" + correlationId, static_cast<float>(ok));
    return;
}

void MCPBridge::pdObjSetBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/obj_set_batch <canvas> <corrId> <count> [tempId inlet selector argc atoms...] * count
    // reply: JSON { ok, total, results:[{tempId,selector,ok,[detail|warning]}] }
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        int count = static_cast<int>(getArgFloat(msg[2]));
        int cursor = 3;
        int okCount = 0;
        juce::Array<juce::var> results;

        sys_lock();
        for (int i = 0; i < count && cursor < msg.size(); i++) {
            auto tempId = getArgString(msg[cursor++]);
            int inlet = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
            auto selector = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
            if (selector.startsWith("s:")) selector = selector.substring(2);
            int argc = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
            std::vector<t_atom> atoms;
            for (int a = 0; a < argc && cursor < msg.size(); a++) {
                auto const& arg = msg[cursor++];
                t_atom at;
                if (arg.isFloat32() || arg.isInt32()) SETFLOAT(&at, getArgFloat(arg));
                else {
                    auto s = getArgString(arg);
                    if (s.startsWith("s:")) s = s.substring(2);
                    SETSYMBOL(&at, gensym(s.toRawUTF8()));
                }
                atoms.push_back(at);
            }
            juce::String warning;
            t_gobj* g = processor->getMcp()->resolveStableId(canvasName, tempId);
            auto err = mcpObjSetCore(g, tempId, inlet, selector, atoms, &warning);
            auto* ro = new juce::DynamicObject();
            ro->setProperty("tempId", tempId);
            ro->setProperty("selector", selector);
            if (err.isNotEmpty()) { ro->setProperty("ok", false); ro->setProperty("detail", err); }
            else { ro->setProperty("ok", true); okCount++; if (warning.isNotEmpty()) ro->setProperty("warning", warning); }
            results.add(juce::var(ro));
        }
        sys_unlock();

        auto* root = new juce::DynamicObject();
        root->setProperty("ok", okCount);
        root->setProperty("total", count);
        root->setProperty("results", results);
        sendReply("/pd/obj_set_batch/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdParamGet(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/param_get <name> <corrId>
    // reply: JSON { name, found, atoms:[...] }  (last value posted via /param)
    if (msg.size() >= 1) {
        auto name = getArgString(msg[0]);
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
        std::vector<t_atom> atoms; bool found = false;
        {
            std::lock_guard<std::mutex> lk(s_paramShadowMutex);
            auto it = s_paramShadow.find(name.toStdString());
            if (it != s_paramShadow.end()) { atoms = it->second; found = true; }
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("name", name);
        root->setProperty("found", found);
        juce::Array<juce::var> arr;
        for (auto const& a : atoms) {
            if (a.a_type == A_SYMBOL) arr.add(juce::String::fromUTF8(a.a_w.w_symbol ? a.a_w.w_symbol->s_name : ""));
            else arr.add((double) a.a_w.w_float);
        }
        root->setProperty("atoms", arr);
        sendReply("/pd/param_get/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdParams(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/params <canvas> <corrId>
    // List knob-bound named parameters: [{ tempId, name, value, min, max }]
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        juce::Array<juce::var> arr;
        {
            sys_lock();
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            if (cnv) {
                // Collect knob-bound params, DESCENDING into placed
                // subpatches / abstractions (GOP/MERDA modules keep their
                // knobs inside the abstraction canvas, not on the root).
                // #43: without recursion, props params found 0 for every
                // generated module. Each descended param is tagged with its
                // OWNING module tempId so callers (auto_map) can scope to a
                // single module — C++ is the truth, TS must not re-derive.
                auto findTempId = [&](t_gobj* y) -> juce::String {
                    for (auto& [key, m] : processor->getMcp()->session.mcpStableObjectMap)
                        for (auto& [id, ptr] : m) if (ptr == y) return id;
                    return {};
                };
                std::function<void(t_canvas*, int, juce::String)> collect =
                    [&](t_canvas* c, int depth, juce::String owner) {
                        if (!c || depth > 4) return;
                        for (t_gobj* y = c->gl_list; y; y = y->g_next) {
                            if (pd_class(&y->g_pd) == canvas_class) {
                                juce::String subId = findTempId(y);
                                collect(reinterpret_cast<t_canvas*>(y), depth + 1,
                                        owner.isNotEmpty() ? owner : subId);
                                continue;
                            }
                            juce::String cn = juce::String::fromUTF8(class_getname(pd_class(&y->g_pd))).toLowerCase();
                            t_object* guiObj = pd::Interface::checkObject(y);
                            McpGuiParam gp;
                            if (!guiObj || !mcpGetGuiParam(cn, guiObj, gp)) continue;
                            auto* o = new juce::DynamicObject();
                            o->setProperty("tempId", findTempId(y));
                            o->setProperty("owner", owner);
                            o->setProperty("name", gp.name);
                            o->setProperty("value", gp.value);
                            o->setProperty("min", gp.min);
                            o->setProperty("max", gp.max);
                            arr.add(juce::var(o));
                        }
                    };
                collect(cnv, 0, {});
            }
            sys_unlock();
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("params", arr);
        sendReply("/pd/params/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdParamSetName(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/param_set_name <canvas> <corrId> <name> <value>
    if (msg.size() >= 4 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        auto name = getArgString(msg[2]);
        float value = getArgFloat(msg[3]);
        // Optional owner scope: when given, only knobs in THAT module are
        // set (avoids fan-out across modules sharing a param name).
        juce::String ownerFilter = msg.size() > 4 ? getArgString(msg[4]) : juce::String();
        int applied = 0;
        {
            sys_lock();
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            if (cnv) {
                // Descend into placed subpatches/abstractions so a param
                // INSIDE a GOP/MERDA module can be set by name (#43).
                auto findTempId = [&](t_gobj* y) -> juce::String {
                    for (auto& [key, m] : processor->getMcp()->session.mcpStableObjectMap)
                        for (auto& [id, ptr] : m) if (ptr == y) return id;
                    return {};
                };
                std::function<void(t_canvas*, int, juce::String)> applyParam =
                    [&](t_canvas* c, int depth, juce::String owner) {
                        if (!c || depth > 4) return;
                        for (t_gobj* y = c->gl_list; y; y = y->g_next) {
                            if (pd_class(&y->g_pd) == canvas_class) {
                                juce::String subId = findTempId(y);
                                applyParam(reinterpret_cast<t_canvas*>(y), depth + 1,
                                           owner.isNotEmpty() ? owner : subId);
                                continue;
                            }
                            if (ownerFilter.isNotEmpty() && owner != ownerFilter) continue;
                            juce::String cn = juce::String::fromUTF8(class_getname(pd_class(&y->g_pd))).toLowerCase();
                            t_object* o = pd::Interface::checkObject(y);
                            McpGuiParam gp;
                            if (!o || !mcpGetGuiParam(cn, o, gp)) continue;
                            if (gp.name != name) continue;
                            t_atom a; SETFLOAT(&a, value);
                            pd_typedmess(reinterpret_cast<t_pd*>(o), gensym("float"), 1, &a);
                            applied++;
                        }
                    };
                applyParam(cnv, 0, {});
            }
            sys_unlock();
        }
        if (applied > 0) {
            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/param_set_name/reply/" + correlationId) };
            reply.addArgument(static_cast<float>(applied));
            if (applied > 1)
                reply.addArgument(juce::String("fanout: matched ") + juce::String(applied) + " knobs");
            sender.send(reply);
        } else {
            sendReply("/pd/param_set_name/reply/" + correlationId,
                      juce::String("error: param '") + name + "' not found");
        }
    }
    return;
}

void MCPBridge::pdObjGetBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/obj_get_batch <canvas> <corrId> <count> [tempId...]
    // reply JSON: { values: { <tempId>: {type,text,props} | {error} } }
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        int count = static_cast<int>(getArgFloat(msg[2]));
        int cursor = 3;
        auto* values = new juce::DynamicObject();
        {
            sys_lock();
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            for (int i = 0; i < count && cursor < msg.size(); i++) {
                auto tempId = getArgString(msg[cursor++]);
                t_gobj* gobj = processor->getMcp()->resolveStableId(canvasName, tempId);
                if (gobj && !mcpGobjIsLive(cnv, gobj)) gobj = nullptr; // stale → unknown
                if (gobj) {
                    auto* o = new juce::DynamicObject();
                    mcpFillObjectJson(gobj, o);
                    values->setProperty(tempId, juce::var(o));
                } else {
                    auto* o = new juce::DynamicObject();
                    o->setProperty("error", "unknown tempId");
                    values->setProperty(tempId, juce::var(o));
                }
            }
            sys_unlock();
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("values", juce::var(values));
        sendReply("/pd/obj_get_batch/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdObjGet(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/obj_get <canvas> <corrId> <tempId>  ->  {type, text, props}
    // `text` = saved constructor args; `props` = live fields for known GUI classes.
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        auto tempId = getArgString(msg[2]);

        juce::String json;
        {
            sys_lock();
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            t_gobj* gobj = processor->getMcp()->resolveStableId(canvasName, tempId);
            if (gobj && !mcpGobjIsLive(cnv, gobj)) gobj = nullptr; // stale → unknown
            if (gobj) json = mcpBuildObjJson(gobj);
            sys_unlock();
        }

        if (json.isEmpty())
            sendReply("/pd/obj_get/reply/" + correlationId,
                      juce::String("error: unknown tempId '") + tempId + "'");
        else
            sendReply("/pd/obj_get/reply/" + correlationId, json);
    }
    return;
}

void MCPBridge::pdCensus(const juce::String& action, const juce::OSCMessage& msg)
{
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

void MCPBridge::pdTypeof(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/typeof <canvasName> <tempId_or_index> <correlationId>
    // Returns: /pd/typeof/reply/<corrId> <className> <objectText>
    // Reads the object's class name and binbuf text directly from structs.
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto targetId = getArgString(msg[1]);
        auto correlationId = getArgString(msg[2]);

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (!cnv) {
            sys_unlock();
            sendReply("/pd/typeof/error/" + correlationId, "Canvas not found: " + canvasName);
            return;
        }

        // Resolve by stableId first, fallback to numeric index
        t_gobj* gobj = processor->getMcp()->resolveStableId(canvasName, targetId);
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
            mcpGetObjectText(obj, &text, &len);
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

void MCPBridge::pdPorts(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/ports <canvasName> <tempId_or_index> <correlationId>
    // Returns: /pd/ports/reply/<corrId> <numInlets> <numOutlets> <inletTypes...> <outletTypes...>
    // inletTypes/outletTypes: "s" for signal, "c" for control
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto targetId = getArgString(msg[1]);
        auto correlationId = getArgString(msg[2]);

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (!cnv) {
            sys_unlock();
            sendReply("/pd/ports/error/" + correlationId, "Canvas not found: " + canvasName);
            return;
        }

        // Resolve by stableId first, fallback to numeric index
        t_gobj* gobj = processor->getMcp()->resolveStableId(canvasName, targetId);
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

void MCPBridge::pdConnections(const juce::String& action, const juce::OSCMessage& msg)
{
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
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
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
        if (processor->getMcp()->session.mcpStableObjectMap.count(canvasStr)) {
            for (auto const& [id, ptr] : processor->getMcp()->session.mcpStableObjectMap[canvasStr]) {
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

            bool srcIsSig = (t.tr_outlet && t.tr_outlet->o_sym == gensym("signal"));
            bool destIsSig = (t.tr_ob2 && obj_issignalinlet(t.tr_ob2, t.tr_inno) != 0);
            juce::String rate = (srcIsSig || destIsSig) ? "sig" : "ctl";

            auto* c = new juce::DynamicObject();
            c->setProperty("srcIndex", srcIt->second);
            c->setProperty("srcOut", t.tr_outno);
            c->setProperty("destIndex", destIt->second);
            c->setProperty("destIn", t.tr_inno);
            c->setProperty("srcId", ptrToId.count(srcGobj) ? ptrToId[srcGobj] : juce::String());
            c->setProperty("destId", ptrToId.count(destGobj) ? ptrToId[destGobj] : juce::String());
            c->setProperty("rate", rate);

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

void MCPBridge::pdDump(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto dumpFile = getArgString(msg[1]);
        auto dumpDir = getArgString(msg[2]);
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "";

        sys_lock();
        t_canvas* cnv = processor ? processor->getMcp()->getCanvasBySymbol(canvasName) : nullptr;
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
