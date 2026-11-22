// Unit tests for the selection pill's animator decision (Alphi calm, 2026-11-22), JUCE-free (Source/Mcp/McpPillMotion.h).
//
// The pill's 6 Hz animator (CanvasMcpPill.cpp SelectionPillAnimator) repainted the pill grown by 200 x 56 px for as long
// as the pill was visible, even on a canvas that was not on screen or with nothing on it that moves. What it really
// animates (CanvasMcpLayer.cpp, the pill's paint):
//  - the caret blink: the focused prompt field's caret (a JUCE caret drawn inside the NVG frame), and the ambient caret
//    drawn while the field is unfocused and empty (not in the ink-mood strip, which has no prompt field);
//  - the record pulse: the voice border glow and level (voice / beatbox capture), the MIDI box and pulse (in the strip);
//  - the mood strip's hint chip: drawn until its time runs out, then one more repaint to erase it.
// Everything else is still: pillAnimPlan says whether the animator repaints and how far around the pill.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpPillMotion.h"

#include <cstdint>
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::PillAnimInput;
using mcp::PillAnimTiming;

static PillAnimInput shown()
{
    PillAnimInput in;
    in.showing = true;
    in.nowMs = 100000;
    return in;
}

static void notShowing()
{
    std::printf("pill animator: nothing runs while the canvas is not showing\n");
    for (int bits = 0; bits < 32; ++bits) {
        PillAnimInput in = shown();
        in.showing = false;
        in.moodStrip = (bits & 1) != 0;
        in.focused = (bits & 2) != 0;
        in.textEmpty = (bits & 4) != 0;
        in.voiceCapturing = (bits & 8) != 0;
        in.midiCapturing = (bits & 16) != 0;
        in.hintUntilMs = in.nowMs + 1000;
        CHECK(!mcp::pillAnimPlan(in).run);
    }
}

static void caret()
{
    std::printf("pill animator: the caret blinks (focused, or ambient while unfocused and empty)\n");
    auto in = shown();
    in.textEmpty = false;
    in.focused = true;
    auto p = mcp::pillAnimPlan(in);
    CHECK(p.run); // the focused field's caret
    CHECK(p.growX == PillAnimTiming::caretGrow && p.growY == PillAnimTiming::caretGrow); // only the pill itself
    in.textEmpty = true;
    p = mcp::pillAnimPlan(in);
    CHECK(p.run);
    in.focused = false; // the ambient caret
    p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growX == PillAnimTiming::caretGrow);
    in.textEmpty = false; // typed text, field not focused: nothing moves
    p = mcp::pillAnimPlan(in);
    CHECK(!p.run);
}

static void moodStrip()
{
    std::printf("pill animator: the ink-mood strip has no prompt field, so no caret\n");
    auto in = shown();
    in.moodStrip = true;
    in.textEmpty = true;
    in.focused = false;
    CHECK(!mcp::pillAnimPlan(in).run); // the ambient caret is not drawn there
    in.focused = true;
    CHECK(!mcp::pillAnimPlan(in).run); // the parked 1 x 1 field's caret is never seen
}

static void recording()
{
    std::printf("pill animator: the record pulse runs, with the room its box and glow need\n");
    auto in = shown();
    in.textEmpty = false;
    in.voiceCapturing = true; // glow + level, in any layout
    auto p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growX == PillAnimTiming::bigGrowX && p.growY == PillAnimTiming::bigGrowY);
    in.moodStrip = true;
    p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growY == PillAnimTiming::bigGrowY);
    in.voiceCapturing = false;
    in.midiCapturing = true; // the MIDI box and pulse exist only in the strip
    p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growX == PillAnimTiming::bigGrowX);
    in.moodStrip = false;
    CHECK(!mcp::pillAnimPlan(in).run); // not drawn outside the strip: nothing to animate
    // Recording together with a caret: the larger room wins.
    in.voiceCapturing = true;
    in.focused = true;
    p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growX == PillAnimTiming::bigGrowX && p.growY == PillAnimTiming::bigGrowY);
}

static void hint()
{
    std::printf("pill animator: the hint chip runs until it expires, plus one repaint to erase it\n");
    auto in = shown();
    in.moodStrip = true;
    in.textEmpty = false;
    in.hintUntilMs = in.nowMs + 2000; // set 600 ms ago, 2600 ms long
    auto p = mcp::pillAnimPlan(in);
    CHECK(p.run && p.growY == PillAnimTiming::bigGrowY); // the chip sits below the strip
    in.hintUntilMs = in.nowMs - PillAnimTiming::hintGraceMs + 1; // just expired: the erase repaint
    CHECK(mcp::pillAnimPlan(in).run);
    in.hintUntilMs = in.nowMs - PillAnimTiming::hintGraceMs - 1; // erased: nothing left to move
    CHECK(!mcp::pillAnimPlan(in).run);
    in.hintUntilMs = 0; // no hint was ever set
    CHECK(!mcp::pillAnimPlan(in).run);
    in.hintUntilMs = in.nowMs + 2000;
    in.moodStrip = false; // the chip is drawn only in the strip
    CHECK(!mcp::pillAnimPlan(in).run);
}

// The animator as CanvasMcpPill.cpp runs it: it remembers the last plan, asks pillAnimStep what to repaint, and keeps this
// tick's plan for the next one.
namespace {
struct Animator {
    mcp::PillAnimPlan last;
    mcp::PillAnimStep tick(PillAnimInput const& in)
    {
        auto const now = mcp::pillAnimPlan(in);
        auto const st = mcp::pillAnimStep(last, now);
        last = now;
        return st;
    }
};
constexpr int glowReach = 4; // the voice border glow reaches 1 + (1.5 + 3.5 * level) / 2 = up to 3.5 px beyond the pill
} // namespace

static void transitions()
{
    std::printf("pill animator: the last repaint when it stops or shrinks (review: a frozen caret, a stuck record rim)\n");
    using mcp::PillAnimPlan;
    PillAnimPlan const idle {}, caret { true, PillAnimTiming::caretGrow, PillAnimTiming::caretGrow },
        big { true, PillAnimTiming::bigGrowX, PillAnimTiming::bigGrowY };
    auto const same = [](mcp::PillAnimStep const& s, PillAnimPlan const& p) { return s.repaint && s.growX == p.growX && s.growY == p.growY; };
    // The table: (last, now) -> what is repainted.
    CHECK(!mcp::pillAnimStep(idle, idle).repaint);               // idle stays idle: nothing
    CHECK(same(mcp::pillAnimStep(idle, caret), caret));           // a caret starts
    CHECK(same(mcp::pillAnimStep(caret, caret), caret));          // blinking
    CHECK(same(mcp::pillAnimStep(caret, big), big));              // a recording starts: the larger room at once
    CHECK(same(mcp::pillAnimStep(big, big), big));
    CHECK(same(mcp::pillAnimStep(big, caret), big));              // a recording ends under a caret: one more repaint of its room
    CHECK(same(mcp::pillAnimStep(big, idle), big));               // ... or under nothing at all
    CHECK(same(mcp::pillAnimStep(caret, idle), caret));           // the caret was visible: erase it once
    // Mixed rooms: the repaint covers both.
    auto const mixed = mcp::pillAnimStep(PillAnimPlan { true, 200, 3 }, PillAnimPlan { true, 3, 56 });
    CHECK(mixed.repaint && mixed.growX == 200 && mixed.growY == 56);

    // (a) Voice capture ends from elsewhere (watchdog, buffer full, the AI) with the field unfocused and holding text:
    // the red recording border must be erased by one more repaint of the whole recording room.
    {
        Animator a;
        auto in = shown();
        in.textEmpty = false;
        in.voiceCapturing = true;
        CHECK(a.tick(in).repaint);
        CHECK(a.tick(in).growX == PillAnimTiming::bigGrowX);
        in.voiceCapturing = false; // ended elsewhere: no repaint request came with it
        auto const last = a.tick(in);
        CHECK(!mcp::pillAnimPlan(in).run); // nothing animates any more ...
        CHECK(last.repaint && last.growX >= glowReach && last.growY >= glowReach); // ... but the rim is erased first
        CHECK(!a.tick(in).repaint);        // then it is quiet
        CHECK(!a.tick(in).repaint);
    }
    // (b) Focus leaves the field with its text kept: the caret frozen visible is erased by one more repaint.
    {
        Animator a;
        auto in = shown();
        in.textEmpty = false;
        in.focused = true;
        CHECK(a.tick(in).repaint);
        in.focused = false; // onFocusLost repaints nothing
        auto const last = a.tick(in);
        CHECK(!mcp::pillAnimPlan(in).run);
        CHECK(last.repaint && last.growX >= PillAnimTiming::caretGrow);
        CHECK(!a.tick(in).repaint);
    }
    // (c) Capture ends while a caret still blinks (empty field): the repaint is the recording's room, not the caret's
    // 3 px, so the glow (up to ~3.5 px beyond the pill) goes; the next ticks are caret-sized again.
    {
        Animator a;
        auto in = shown();
        in.textEmpty = true;
        in.voiceCapturing = true;
        CHECK(a.tick(in).growX == PillAnimTiming::bigGrowX);
        in.voiceCapturing = false;
        auto const last = a.tick(in);
        CHECK(mcp::pillAnimPlan(in).run && last.growX >= glowReach && last.growY >= glowReach);
        auto const next = a.tick(in);
        CHECK(next.repaint && next.growX == PillAnimTiming::caretGrow);
    }
    // The hint chip's last repaint (its grace) and a hidden tab: nothing repaints before or after.
    {
        Animator a;
        auto in = shown();
        in.moodStrip = true;
        in.hintUntilMs = in.nowMs + 1000;
        CHECK(a.tick(in).repaint);
        in.nowMs += 5000; // the hint is long gone
        CHECK(a.tick(in).repaint); // the one more repaint that erases it
        CHECK(!a.tick(in).repaint);
        in.showing = false;
        CHECK(!a.tick(in).repaint);
    }
}

static void rates()
{
    std::printf("pill animator: rates\n");
    CHECK(PillAnimTiming::animateHz == 6); // the look of the blink and the pulse is unchanged
    CHECK(PillAnimTiming::watchHz > 0 && PillAnimTiming::watchHz < PillAnimTiming::animateHz); // idle: a cheap look-in
    CHECK(PillAnimTiming::caretGrow < PillAnimTiming::bigGrowX && PillAnimTiming::caretGrow < PillAnimTiming::bigGrowY);
}

int main()
{
    notShowing();
    caret();
    moodStrip();
    recording();
    hint();
    transitions();
    rates();
    if (failures == 0)
        std::printf("pill_motion_test: all passed\n");
    else
        std::printf("pill_motion_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
