/*
 // Pd-side field reads of GUI externals (Plan 3 P3.2 S6). See McpGuiFields.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>

#include "Mcp/McpGuiFields.h"
#include "Utility/Config.h"
#include <g_all_guis.h>
#include "AllGuis.h"
extern "C" void knob_get_snd(void* x);
extern "C" void knob_get_rcv(void* x);

// Live `props` for a GUI object (shared by obj_get / obj_get_batch).
juce::DynamicObject* mcpBuildGuiProps(const juce::String& className, t_object* obj)
{
    auto* props = new juce::DynamicObject();
    if (!obj) return props;
    juce::String cls = className.toLowerCase();

    if (cls == "knob" || cls == "else/knob") {
        knob_get_rcv(reinterpret_cast<void*>(obj));
        knob_get_snd(reinterpret_cast<void*>(obj));
        auto* k = reinterpret_cast<t_fake_knob*>(obj);
        props->setProperty("min", (double) k->x_lower);
        props->setProperty("max", (double) k->x_upper);
        props->setProperty("value", (double) k->x_fval);
        props->setProperty("log", k->x_log != 0);
        if (k->x_snd_raw) props->setProperty("send", juce::String::fromUTF8(k->x_snd_raw->s_name));
        if (k->x_rcv_raw) props->setProperty("receive", juce::String::fromUTF8(k->x_rcv_raw->s_name));
        if (k->x_param && k->x_param != gensym("empty"))
            props->setProperty("param", juce::String::fromUTF8(k->x_param->s_name));
    } else if (cls == "floatatom" || cls == "numbox") {
        auto* g = reinterpret_cast<t_fake_gatom*>(obj);
        props->setProperty("min", (double) g->a_draglo);
        props->setProperty("max", (double) g->a_draghi);
        if (g->a_symto) props->setProperty("send", juce::String::fromUTF8(g->a_symto->s_name));
        if (g->a_symfrom) props->setProperty("receive", juce::String::fromUTF8(g->a_symfrom->s_name));
    } else if (cls == "hsl" || cls == "vsl") {
        auto* s = reinterpret_cast<t_slider*>(obj);
        props->setProperty("min", (double) s->x_min);
        props->setProperty("max", (double) s->x_max);
        props->setProperty("value", (double) s->x_fval);
        props->setProperty("log", s->x_lin0_log1 != 0);
        props->setProperty("orientation", (int) s->x_orientation);
        if (s->x_gui.x_snd && s->x_gui.x_snd != gensym("empty")) props->setProperty("send", juce::String::fromUTF8(s->x_gui.x_snd->s_name));
        if (s->x_gui.x_rcv && s->x_gui.x_rcv != gensym("empty")) props->setProperty("receive", juce::String::fromUTF8(s->x_gui.x_rcv->s_name));
    } else if (cls == "tgl") {
        auto* t = reinterpret_cast<t_toggle*>(obj);
        props->setProperty("value", (double) t->x_on);
        props->setProperty("nonzero", t->x_nonzero != 0);
    } else if (cls == "hradio" || cls == "vradio") {
        auto* r = reinterpret_cast<t_radio*>(obj);
        props->setProperty("index", r->x_on);
        props->setProperty("value", (double) r->x_fval);
    } else if (cls == "nbx") {
        auto* n = reinterpret_cast<t_my_numbox*>(obj);
        props->setProperty("value", (double) n->x_val);
        props->setProperty("min", (double) n->x_min);
        props->setProperty("max", (double) n->x_max);
        props->setProperty("log", n->x_lin0_log1 != 0);
    } else if (cls == "vu") {
        auto* v = reinterpret_cast<t_vu*>(obj);
        props->setProperty("rms", (double) v->x_fr);
        props->setProperty("peak", (double) v->x_fp);
    } else if (cls == "button") {
        auto* b = reinterpret_cast<t_fake_button*>(obj);
        props->setProperty("value", b->x_state);
        props->setProperty("mode", b->x_mode);
    } else if (cls == "function") {
        auto* f = reinterpret_cast<t_fake_function*>(obj);
        props->setProperty("min", (double) f->x_min);
        props->setProperty("max", (double) f->x_max);
        props->setProperty("n_states", f->x_n_states);
    } else if (cls == "scope~") {
        auto* sc = reinterpret_cast<t_fake_scope*>(obj);
        props->setProperty("min", (double) sc->x_min);
        props->setProperty("max", (double) sc->x_max);
        props->setProperty("frozen", sc->x_frozen != 0);
    } else if (cls == "keyboard") {
        auto* kb = reinterpret_cast<t_fake_keyboard*>(obj);
        props->setProperty("octaves", kb->x_octaves);
        props->setProperty("first_c", kb->x_first_c);
        props->setProperty("velocity", kb->x_velocity);
    }
    return props;
}

// Derive a named param from ANY scalar GUI widget — not just knobs. The NAME
// comes from the widget's MERDA `$0-<sym>` send (else/knob also honors
// @param/x_param); a resolved "1003-freq" send is stripped of its instance id.
// Used by /pd/params + param_set_name so props params / set[param] cover
// hsl/vsl/tgl/bng/button/hradio/vradio/nbx, not only else/knob.
bool mcpGetGuiParam(const juce::String& className, t_object* obj, McpGuiParam& out)
{
    if (!obj) return false;
    juce::String cls = className.toLowerCase();
    t_symbol* raw = nullptr;
    if (cls == "knob" || cls == "else/knob") {
        knob_get_snd(reinterpret_cast<void*>(obj));
        auto* k = reinterpret_cast<t_fake_knob*>(obj);
        if (k->x_param && k->x_param != gensym("empty"))
            out.name = juce::String::fromUTF8(k->x_param->s_name);
        raw = k->x_snd_raw;
        out.min = (double) k->x_lower; out.max = (double) k->x_upper; out.value = (double) k->x_fval;
    } else if (cls == "hsl" || cls == "vsl") {
        auto* s = reinterpret_cast<t_slider*>(obj);
        raw = s->x_gui.x_snd_unexpanded;
        out.min = (double) s->x_min; out.max = (double) s->x_max; out.value = (double) s->x_fval;
    } else if (cls == "tgl") {
        auto* t = reinterpret_cast<t_toggle*>(obj);
        raw = t->x_gui.x_snd_unexpanded;
        out.min = 0; out.max = 1; out.value = (double) t->x_on;
    } else if (cls == "hradio" || cls == "vradio") {
        auto* r = reinterpret_cast<t_radio*>(obj);
        raw = r->x_gui.x_snd_unexpanded;
        out.max = (double) (r->x_number > 0 ? r->x_number - 1 : 1); out.value = (double) r->x_fval;
    } else if (cls == "nbx") {
        // numbox is t_my_numbox (NOT t_fake_gatom — mis-casting crashed).
        auto* n = reinterpret_cast<t_my_numbox*>(obj);
        raw = n->x_gui.x_snd_unexpanded;
        out.min = n->x_min; out.max = n->x_max; out.value = n->x_val;
    } else if (cls == "floatatom" || cls == "numbox") {
        auto* g = reinterpret_cast<t_fake_gatom*>(obj);
        raw = g->a_symto;
        out.min = (double) g->a_draglo; out.max = (double) g->a_draghi;
    } else {
        return false;
    }
    if (out.name.isEmpty() && raw && raw->s_name && raw->s_name[0]) {
        juce::String s = juce::String::fromUTF8(raw->s_name);
        if (s.startsWith("\\$0-")) s = s.substring(4);
        else if (s.startsWith("$0-")) s = s.substring(3);
        else {
            int dash = s.indexOfChar('-');
            if (dash > 0 && s.substring(0, dash).containsOnly("0123456789")) s = s.substring(dash + 1);
            else return false;
        }
        if (s.startsWith("set-")) s = s.substring(4);
        out.name = s;
    }
    return out.name.isNotEmpty();
}

juce::String mcpKnobLabelName(t_gobj* y)
{
    juce::String semantic;
    auto* knb = reinterpret_cast<t_fake_knob*>(y);
    knob_get_rcv(knb);
    knob_get_snd(knb);
    auto valid = [](t_symbol* s) -> bool {
        return s && s->s_name && s->s_name[0]
            && juce::String::fromUTF8(s->s_name) != "empty";
    };
    if (valid(knb->x_rcv_raw))      semantic = juce::String::fromUTF8(knb->x_rcv_raw->s_name);
    else if (valid(knb->x_snd_raw)) semantic = juce::String::fromUTF8(knb->x_snd_raw->s_name);
    return semantic;
}
