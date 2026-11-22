/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Alphi calm (2026-11-22): when the selection pill's animator repaints, and how far around the pill. JUCE-free
// (Tests/mcp/pill_motion_test.cpp); the timer is CanvasMcpLayer::SelectionPillAnimator (CanvasMcpPill.cpp).
//
// The animator ran at 6 Hz for as long as the pill was visible and repainted the pill grown by 200 x 56 px, on a
// canvas that was not on screen too and with nothing on the pill that moves. What the pill's paint really animates
// (CanvasMcpLayer.cpp):
//  - the caret blink: the focused prompt field's caret (a JUCE caret drawn inside the NVG frame, so it shows only when
//    the frame is redrawn) and the ambient caret drawn while the field is unfocused and empty. The ink-mood strip has
//    no prompt field, so neither caret is drawn there;
//  - the record pulse: the voice border glow and level (voice / beatbox capture, any layout) and the MIDI box and pulse
//    (only in the strip);
//  - the strip's hint chip: drawn until its time runs out, then one more repaint to erase it.
// Everything else on the pill is still (typed text, an unfocused field), so the animator repaints nothing then. The
// timer itself drops to a slow look-in (watchHz: one state check, no repaint), not to a stop: voice capture can be
// started from the bridge and the field's focus and text change from anywhere, none of which tells this layer.

#include <cstdint>

namespace mcp {

struct PillAnimInput {
    bool showing = false;         // the pill is visible on a canvas that is on screen (not a hidden tab)
    bool moodStrip = false;       // the ink-mood strip layout (no prompt field)
    bool focused = false;         // the prompt field has the keyboard
    bool textEmpty = true;        // the prompt field holds no text
    bool voiceCapturing = false;  // voice / beatbox capture runs
    bool midiCapturing = false;   // MIDI capture runs
    std::int64_t nowMs = 0;       // a steady clock in ms (juce::Time::getMillisecondCounter)
    std::int64_t hintUntilMs = 0; // when the strip's hint chip stops showing; 0 = no hint
};

struct PillAnimPlan {
    bool run = false; // repaint this tick
    int growX = 0;    // canvas units the pill's frame grows by for the repaint, sideways
    int growY = 0;    // ... and up / down
};

struct PillAnimTiming {
    static constexpr int animateHz = 6; // the blink and the pulse: as before
    static constexpr int watchHz = 2;   // idle: only look whether something started to move
    static constexpr int bigGrowX = 200; // the record box above (340 px wide), the hint chip below: as before
    static constexpr int bigGrowY = 56;
    static constexpr int caretGrow = 3; // a caret lives inside the pill
    static constexpr std::int64_t hintGraceMs = 250; // one more repaint after the hint's time, to erase it
};

inline PillAnimPlan pillAnimPlan(PillAnimInput const& in)
{
    using T = PillAnimTiming;
    if (!in.showing) return {};
    bool const caret = !in.moodStrip && (in.focused || in.textEmpty);
    bool const record = in.voiceCapturing || (in.moodStrip && in.midiCapturing);
    bool const hint = in.moodStrip && in.hintUntilMs > 0 && in.nowMs < in.hintUntilMs + T::hintGraceMs;
    if (record || hint) return { true, T::bigGrowX, T::bigGrowY };
    if (caret) return { true, T::caretGrow, T::caretGrow };
    return {};
}

} // namespace mcp
