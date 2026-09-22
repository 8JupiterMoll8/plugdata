#pragma once

#include <juce_core/juce_core.h>

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
        || c == "hradio" || c == "vradio" || c == "tgl" || c == "bng";
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
        return { "0","1","0","8", "empty","empty","empty", "0","-11", "0","12", "0","0","0", "0" };
    if (c == "bng")
        return { "15","250","50","0", "empty","empty","empty", "17","7", "0","12", "0","0","0" };
    if (c == "tgl")
        return { "15","0", "empty","empty","empty", "17","7", "0","12", "0","0","0", "0" };
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

    // ── Slider / numbox family: init lives at the END (arg 16), not position 5,
    //    so a positional pad would mis-map it. Use an explicit mapping.
    if (cls == "hsl" || cls == "vsl" || cls == "nbx" || cls == "numbox")
    {
        if (given < 4 || given > 6) return tokens; // 0 or full/other form
        const juce::String w    = tokens[1];
        const juce::String h    = tokens[2];
        const juce::String mn   = tokens[3];
        const juce::String mx   = tokens[4];
        const juce::String log  = given >= 5 ? tokens[5] : "0";
        const bool hasInit      = given >= 6;
        const juce::String init = hasInit ? tokens[6] : "0";
        const juce::String ldx  = (cls == "vsl") ? "0"  : "-2";
        const juce::String ldy  = (cls == "vsl") ? "-9" : "-8";
        juce::StringArray full;
        full.add(tokens[0]);            // class
        full.add(w);                    // 0  width
        full.add(h);                    // 1  height
        full.add(mn);                   // 2  min
        full.add(mx);                   // 3  max
        full.add(log);                  // 4  log
        full.add(hasInit ? "1" : "0");  // 5  loadinit (1 so init takes effect)
        full.add("empty");              // 6  send
        full.add("empty");              // 7  receive
        full.add("empty");              // 8  label
        full.add(ldx);                  // 9  label x
        full.add(ldy);                  // 10 label y
        full.add("0");                  // 11 font style
        full.add("12");                 // 12 font size
        full.add("0");                  // 13 bg colour
        full.add("0");                  // 14 fg colour
        full.add("0");                  // 15 label colour
        full.add(init);                 // 16 init value
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
