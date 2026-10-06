#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// -----------------------------------------------------------------------------
// IEM GUI creation-argument guard (shared by every MCP create path).
//
// Pd's IEM GUI classes only parse their creation arguments when the argument
// count matches their *full* form:
//     slider (hsl/vsl) / numbox (nbx) : 17 or 18 args
//     radio  (hradio/vradio)          : 15 args
//     bng                             : 14 args
//     toggle (tgl)                    : 13 or 14 args
//     vu                              : 12 args
// Any shorter list is SILENTLY IGNORED, so `[hsl 128 15 0 1000 0]`,
// `[hradio 15 1 0 3]`, `[tgl 15]`, `[bng 15 250 50]` all keep their defaults
// (wrong range / wrong cell count / wrong size) with no error at all.
//
// This helper expands the well-known short forms into the canonical full form
// so the requested size/range/number actually applies. Anything unrecognised is
// returned unchanged (Pd's current behaviour) rather than guessed at.
// -----------------------------------------------------------------------------

namespace mcp
{
inline bool isIemGuiClass(const juce::String& c)
{
    return c == "hsl" || c == "vsl" || c == "nbx" || c == "numbox"
        || c == "hradio" || c == "vradio" || c == "tgl" || c == "bng"
        || c == "knob";
}

// Full arg count + a full default argument template for the classes whose short
// forms are positional from the front (radio / toggle / bng). The template is
// overwritten left-to-right with the user's args, so only the tail differs.
inline int iemFullArgCount(const juce::String& c)
{
    if (c == "hsl" || c == "vsl" || c == "nbx" || c == "numbox") return 17;
    if (c == "hradio" || c == "vradio") return 15;
    if (c == "bng") return 14;
    if (c == "tgl") return 13;
    return 0;
}

inline juce::StringArray iemDefaultTemplate(const juce::String& c)
{
    //        a   b   c   d   snd    rcv    label  ldx ldy  fsty fs   bg fg lc  init
    if (c == "hradio" || c == "vradio")
        return { "0","1","0","8", "empty","empty","empty", "0","-11", "0","12", "#fcfcfc","#000000","#000000", "0" };
    if (c == "bng")
        return { "15","250","50","0", "empty","empty","empty", "17","7", "0","12", "#fcfcfc","#000000","#000000" };
    if (c == "tgl")
        return { "15","0", "empty","empty","empty", "17","7", "0","12", "#fcfcfc","#000000","#000000", "0" };
    return {};
}

// tokens[0] is the object class; the remaining tokens are the creation args.
// Returns an expanded token list when a known short form is detected, else the
// input unchanged.
inline juce::StringArray expandIemGuiShortForm(const juce::StringArray& tokens)
{
    if (tokens.size() < 2) return tokens;
    const juce::String cls = tokens[0].toLowerCase();
    if (!isIemGuiClass(cls)) return tokens;

    const int given = tokens.size() - 1; // args after the class name

    // ── Slider / numbox family: init lives at the END (arg 16), not position 5.
    //    Short-form convention (matches every AI-facing example):
    //        hsl <w> <h> <min> <max> [value] [log]
    //    The 5th arg IS the initial value (e.g. `hsl 128 15 0 1 0.35`), the
    //    6th is the log flag. The old mapping treated the 5th as `log`, so the
    //    documented example created a log slider with value 0 → a gain slider
    //    at zero → silent patch (the "range calibration muted it" trap).
    if (cls == "hsl" || cls == "vsl" || cls == "nbx" || cls == "numbox")
    {
        if (given < 4 || given > 6) return tokens; // 0 or full/other form
        const juce::String w    = tokens[1];
        const juce::String h    = tokens[2];
        const juce::String mn   = tokens[3];
        const juce::String mx   = tokens[4];
        const juce::String init = given >= 5 ? tokens[5] : "0";
        const juce::String log  = given >= 6 ? tokens[6] : "0";
        const juce::String ldx  = (cls == "vsl") ? "0"  : "-2";
        const juce::String ldy  = (cls == "vsl") ? "-9" : "-8";
        // hsl/vsl store arg 16 as x_val = PIXEL POSITION ×100, not the real
        // value (slider_getfval: fval = x_val*0.01*k + min). Handing it the raw
        // requested value (e.g. 1200) produced fval 937 — a slider that starts
        // nowhere near its documented value. Convert real value → position here.
        // nbx stores real values directly and is left untouched.
        juce::String initArg = init;
        if (cls == "hsl" || cls == "vsl") {
            const double len = (cls == "vsl") ? h.getDoubleValue() : w.getDoubleValue();
            double dmin = mn.getDoubleValue();
            double dmax = mx.getDoubleValue();
            const bool isLog = log.getDoubleValue() != 0.0;
            double val = init.getDoubleValue();
            // Replicate slider_check_minmax() normalization for log sliders.
            if (isLog) {
                if (dmin == 0.0 && dmax == 0.0) dmax = 1.0;
                if (dmax > 0.0) { if (dmin <= 0.0) dmin = 0.01 * dmax; }
                else { if (dmin > 0.0) dmax = 0.01 * dmin; }
            }
            const double span = (len > 1.0) ? (len - 1.0) : 1.0;
            if (val < dmin) val = dmin;
            if (val > dmax) val = dmax;
            double g = 0.0;
            if (isLog && dmin > 0.0 && val > 0.0 && dmax > dmin)
                g = std::log(val / dmin) / (std::log(dmax / dmin) / span);
            else if (dmax != dmin)
                g = (val - dmin) / ((dmax - dmin) / span);
            int xval = (int) std::lround(100.0 * g);
            const int maxPos = (int) std::lround(100.0 * span);
            if (xval < 0) xval = 0;
            if (xval > maxPos) xval = maxPos;
            initArg = juce::String(xval);
        }
        juce::StringArray full;
        full.add(tokens[0]);            // class
        full.add(w);                    // 0  width
        full.add(h);                    // 1  height
        full.add(mn);                   // 2  min
        full.add(mx);                   // 3  max
        full.add(log);                  // 4  log
        // 5 loadinit: ALWAYS on. Pd's slider_save writes the current value ONLY
        // when x_loadinit is set (`x_loadinit ? x_val : 0`, g_slider.c), and
        // slider_loadbang re-sends it on load only when set. With loadinit off
        // (the old default when no init arg was given) every MCP-created knob
        // silently reset to min on reload AND stayed silent — the "slider went
        // down / patch came up silent" trap. Init ON makes runtime values
        // persist and wakes the knob→line~ chain on load.
        full.add("1");
        full.add("empty");              // 6  send
        full.add("empty");              // 7  receive
        full.add("empty");              // 8  label
        full.add(ldx);                  // 9  label x
        full.add(ldy);                  // 10 label y
        full.add("0");                  // 11 font style
        full.add("12");                 // 12 font size
        full.add("#fcfcfc");            // 13 bg colour
        full.add("#000000");            // 14 fg colour (handle / thumb)
        full.add("#000000");            // 15 label colour
        full.add(initArg);              // 16 init (position×100 for hsl/vsl)
        return full;
    }

    // ── ELSE knob: <size> <min> <max> <exp> <load> <snd> <rcv> <bg> <mg> <fg>
    //    <square> <circular> <steps> <discrete> <arc> <angle> <offset> <jump>
    //    <arcstart> <param> <var> <number_mode> <n_size> <xpos> <ypos> <savestate>
    //    <lb> <ticks> <readonly>.  A short form (e.g. `knob 40 -50 50`) leaves
    //    arg 14 (arc) at 0 → the value indicator is HIDDEN (knob.c hides the arc
    //    unless arc && fval != arcstart) → the artist sees an EMPTY CIRCLE that
    //    shows no value. Pad to the canonical full form with arc=1.
    if (cls == "knob" || cls == "else/knob")
    {
        if (given == 0 || given > 5) return tokens; // full/other form
        const juce::String size = tokens[1];
        const juce::String mn   = given >= 2 ? tokens[2] : "0";
        const juce::String mx   = given >= 3 ? tokens[3] : "127";
        const juce::String exp  = given >= 4 ? tokens[4] : "0";
        const juce::String load = given >= 5 ? tokens[5] : "0";
        juce::StringArray full;
        full.add(tokens[0]);            // class
        full.add(size);                 // 0  size
        full.add(mn);                   // 1  min
        full.add(mx);                   // 2  max
        full.add(exp);                  // 3  exp
        full.add(load);                 // 4  load value
        full.add("empty");              // 5  send
        full.add("empty");              // 6  receive
        full.add("#dfdfdf");            // 7  background
        full.add("#7c7c7c");            // 8  arc colour
        full.add("black");              // 9  foreground
        full.add("0");                  // 10 square
        full.add("0");                  // 11 circular
        full.add("0");                  // 12 steps
        full.add("0");                  // 13 discrete
        full.add("1");                  // 14 ARC — 1 shows the value indicator
        full.add("320");                // 15 angle
        full.add("0");                  // 16 offset
        full.add("0");                  // 17 jump
        full.add(mn);                   // 18 arcstart (arc fills from min)
        full.add("empty");              // 19 param
        full.add("empty");              // 20 var
        full.add("0");                  // 21 number_mode
        full.add("12");                 // 22 n_size
        full.add("6");                  // 23 xpos
        full.add("-15");                // 24 ypos
        full.add("0");                  // 25 savestate
        full.add("1");                  // 26 loadbang
        full.add("0");                  // 27 ticks
        full.add("0");                  // 28 readonly
        return full;
    }

    // ── Radio / toggle / bng: short forms are positional from the front, so pad
    //    the tail with the class defaults.
    const int fullCount = iemFullArgCount(cls);
    if (fullCount == 0 || given == 0 || given >= fullCount) return tokens;
    juce::StringArray tmpl = iemDefaultTemplate(cls);
    if (tmpl.size() != fullCount) return tokens;
    juce::StringArray full;
    full.add(tokens[0]);
    for (int i = 0; i < fullCount; ++i)
        full.add(i < given ? tokens[i + 1] : tmpl[i]);
    return full;
}
} // namespace mcp
