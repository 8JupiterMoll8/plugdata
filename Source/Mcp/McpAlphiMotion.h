/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Alphi drop 1 (docs/plans/alphi-drop1-alive-steps.md in the MCP repo): how Alphi's motion repaints (pulse, sparks,
// dots, spiral, the strip) reach the screen without touching his click gate or the dock. JUCE-free
// (Tests/mcp/alphi_gate_test.cpp).
//
// F0 (R5): the 400 ms click gate (alphiGateAfterPaint) restarts only when Alphi was covered / hidden, moved on the
// canvas (AlphiDock::apply) or moved on screen; a repaint never restarts it. A motion repaint invalidates Alphi's
// frame grown by repaintPad design px, so after the canvas's screen <-> canvas rounding it still redraws the whole
// frame (it can never leave a half-drawn Alphi whose gate does not start).
// D4 / review N3: a paint the motion timer asked for that stays near Alphi (or his strip) skips the dock pass
// (updateAlphiDock walks every box); the dock keeps its 10 Hz tick and every other paint.

#include "McpAlphiDock.h"
#include "McpAlphiSay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace mcp {

struct AlphiMotionMetrics {
    static constexpr int repaintPad = 3; // design px around the frame (or strip) a motion repaint invalidates
    static constexpr int scopePad = 6;   // design px: a region inside the frame (or strip) grown by this is Alphi-only
};

namespace alphimotion {

inline int padUnits(int designPx, float zoom)
{
    return static_cast<int>(std::ceil(static_cast<float>(designPx) / alphidock::safeZoom(zoom)));
}

inline ApprovalRect unite(ApprovalRect a, ApprovalRect b)
{
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int const x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    int const x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return { x0, y0, x1 - x0, y1 - y0 };
}

} // namespace alphimotion

// The canvas area a motion repaint of `frame` (Alphi's face, or his strip) invalidates (canvas coordinates).
inline ApprovalRect alphiMotionArea(ApprovalRect frame, float zoom)
{
    if (frame.w <= 0 || frame.h <= 0) return {};
    return alphidock::grown(frame, alphimotion::padUnits(AlphiMotionMetrics::repaintPad, zoom));
}

// Review N3: this paint (`region`, canvas coordinates) is Alphi's own: the motion timer asked for it, it stays inside
// Alphi's frame and his strip AS DRAWN (`strip`, empty when none; grown by scopePad), and nothing changed near him
// since the last full pass (`layoutSame`: AlphiZoneKey; review HIGH: a box created or dragged onto his face or strip
// in a paint coalesced with a motion repaint). Then the paint skips the dock pass. Any other paint does not.
inline bool alphiOnlyPaint(ApprovalRect region, bool motionAsked, ApprovalRect frame, ApprovalRect strip, float zoom,
    bool layoutSame)
{
    if (!layoutSame || !motionAsked || region.w <= 0 || region.h <= 0 || frame.w <= 0 || frame.h <= 0) return false;
    int const pad = alphimotion::padUnits(AlphiMotionMetrics::scopePad, zoom);
    auto const own = alphimotion::unite(alphidock::grown(frame, pad),
        strip.w > 0 && strip.h > 0 ? alphidock::grown(strip, pad) : ApprovalRect {});
    return alphiRectContains(own, region);
}

// Review HIGH: the zone near Alphi whose boxes the layout key watches: his frame and his drawn strip, grown by
// scopePad plus the dock's box pad (a box whose 8 px pad reaches him counts).
inline ApprovalRect alphiZone(ApprovalRect frame, ApprovalRect strip, float zoom)
{
    int const pad = alphimotion::padUnits(AlphiMotionMetrics::scopePad + AlphiDockMetrics::boxPad, zoom);
    return alphimotion::unite(frame.w > 0 && frame.h > 0 ? alphidock::grown(frame, pad) : ApprovalRect {},
        strip.w > 0 && strip.h > 0 ? alphidock::grown(strip, pad) : ApprovalRect {});
}

// A cheap layout key (no allocation, no full signature): the box count, every box inside `zone`, and whether a mouse
// button is held (a drag or a lasso is never skipped). Taken at each full dock pass and compared before a skip.
class AlphiZoneKey {
public:
    explicit AlphiZoneKey(ApprovalRect zone) : zone(zone) {}
    void add(ApprovalRect box)
    {
        ++count;
        if (rectsIntersect(box, zone)) {
            h = mix(h, static_cast<std::uint32_t>(box.x));
            h = mix(h, static_cast<std::uint32_t>(box.y));
            h = mix(h, static_cast<std::uint32_t>(box.w));
            h = mix(h, static_cast<std::uint32_t>(box.h));
        }
    }
    std::uint64_t value(bool buttonDown) const { return mix(mix(h, count), buttonDown ? 2u : 1u); }

private:
    static std::uint64_t mix(std::uint64_t a, std::uint64_t v) { return a ^ (v + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)); }
    ApprovalRect zone;
    std::uint64_t h = 1469598103934665603ULL;
    std::uint64_t count = 0;
};

// Review M: the strip's hit frame after a paint. It changes only when the render redrew the old and the new strip
// (`region` holds both); otherwise the drawn one stays (no click falls through a visible strip, no invisible click
// sink appears) and one more repaint is asked (unless old == new: nothing changed).
struct AlphiStripUpdate {
    ApprovalRect frame;
    bool repaint = false;
};
inline AlphiStripUpdate alphiStripAfterPaint(ApprovalRect prev, ApprovalRect next, ApprovalRect region)
{
    // Alphi calm, item 2: the same strip in the same place needs nothing, whatever this render covered (its content is
    // the say key's business: alphiStripRecordsDrawn). Before, every paint that missed a visible strip asked for another.
    if (prev.x == next.x && prev.y == next.y && prev.w == next.w && prev.h == next.h) return { next, false };
    bool const nextOk = next.w <= 0 || next.h <= 0 || alphiRectContains(region, next);
    bool const prevOk = prev.w <= 0 || prev.h <= 0 || alphiRectContains(region, prev);
    if (nextOk && prevOk) return { next, false };
    return { prev, true };
}

// Review L6: what a motion repaint invalidates: his face, plus the strip's spot only while a strip is drawn or the
// say board changed (a line may appear or go).
inline ApprovalRect alphiMotionRepaintArea(ApprovalRect face, ApprovalRect stripSpot, bool stripDrawn, bool sayChanged, float zoom)
{
    auto area = alphiMotionArea(face, zoom);
    if ((stripDrawn || sayChanged) && stripSpot.w > 0 && stripSpot.h > 0)
        area = alphimotion::unite(area, alphiMotionArea(stripSpot, zoom));
    return area;
}

// D4: the light ~30 Hz motion timer runs only while Alphi is drawn (shown, uncovered) and something on him moves:
// what the next paint would draw of the pulse differs from what was drawn (Alphi calm: alphiPulseHeld, the eye size
// on its 0.25 px grid, not the raw level), sparks / dots (busy), or the line's spiral or fade. It repaints his own rect
// (alphiMotionArea) and never touches the gate or the dock.
struct AlphiMotionInput {
    bool shown = false;      // drawn this paint (alphiShown, placed, not the edge tab)
    bool covered = false;    // a chip stack / the pill covers him
    float target = 0.0f;     // the pulse level the next paint would draw (alphiPulseHeld of the followed level)
    float drawn = 0.0f;      // the level last drawn
    bool busy = false;       // sparks (building) or dots (measuring)
    bool lineMoving = false; // the dizzy spiral or the line's fade (AlphiLineClock::moving)
};

struct AlphiMotionTiming {
    static constexpr int intervalMs = 33;   // ~30 Hz
    static constexpr float minStep = 0.02f; // the followed level this close to its target is "arrived" (the follow stops)
    static constexpr float sameLevel = 1e-4f; // two held levels closer than this are the same drawn size
};

inline bool alphiMotionRun(AlphiMotionInput const& in)
{
    if (!in.shown || in.covered) return false;
    if (in.busy || in.lineMoving) return true;
    return std::fabs(in.target - in.drawn) > AlphiMotionTiming::sameLevel;
}

// The 10 Hz tick wakes the motion timer when what the next paint would draw of the pulse differs from what was drawn
// (Alphi calm: a new eye size on the 0.25 px grid, or the last step to calm), not on every block or level wiggle
// (review L7: not on every new block).
inline bool alphiPulseWake(float drawnLevel, float level, AlphiLook const& look)
{
    return std::fabs(alphiPulseHeld(drawnLevel, level, look) - drawnLevel) > AlphiMotionTiming::sameLevel;
}

// Alphi calm, item 2: a paint records what the strip showed (the say key, the line's alpha, the spiral) only when it
// redrew the strip it drew (`drawnStrip`, empty when none): one that missed it left the screen as it was, so the motion
// timer must still ask for the change (alphiStripAfterPaint no longer repaints a strip that did not move).
inline bool alphiStripRecordsDrawn(ApprovalRect region, ApprovalRect drawnStrip)
{
    return drawnStrip.w <= 0 || drawnStrip.h <= 0 || alphiRectContains(region, drawnStrip);
}

// What the say board shows right now, as one key: a change (a new say, or a slot's ttl ran out; step W repaints only
// on a new say) makes the 10 Hz tick repaint Alphi and his strip.
inline std::string alphiSayKey(AlphiSayBoard const& board, std::int64_t nowMs)
{
    std::string k;
    auto const add = [&k](std::optional<AlphiSaySlot> const& s) {
        if (!s) {
            k += "-|";
            return;
        }
        k += std::to_string(static_cast<int>(s->phase)) + ":" + std::to_string(static_cast<int>(s->kind)) + ":" + s->turn
            + ":" + s->text + "|";
    };
    add(board.activity(nowMs));
    add(board.line(nowMs));
    return k;
}

// D6 / D8: the line slot's clock. A new final line shows at once for showMs, then fades over fadeMs; a failed one
// first spins him dizzy for dizzyMs (the fork shows the spiral whenever a failed final arrives; the server times the
// quiet over stdio), then shows the line. Seeing the same line again never restarts it (no re-show).
struct AlphiLineTiming {
    static constexpr std::int64_t dizzyMs = 2000;
    static constexpr std::int64_t showMs = 6000;
    static constexpr std::int64_t fadeMs = 500;
    static constexpr float spiralTurnsPerSecond = 1.5f;
};

class AlphiLineClock {
public:
    void observe(std::optional<AlphiSaySlot> const& line, std::int64_t nowMs)
    {
        if (!line) {
            has = false;
            return;
        }
        auto const key = std::to_string(static_cast<int>(line->phase)) + ":" + std::to_string(static_cast<int>(line->kind))
            + ":" + line->turn + ":" + line->text;
        if (!has || key != lastKey) {
            startMs = nowMs;
            failed = line->phase == AlphiSayPhase::Failed;
            lastKey = key;
        }
        has = true;
    }

    bool dizzy(std::int64_t nowMs) const
    {
        return has && failed && nowMs >= startMs && nowMs - startMs < AlphiLineTiming::dizzyMs;
    }

    float spiralTurns(std::int64_t nowMs) const
    {
        if (!dizzy(nowMs)) return 0.0f;
        return static_cast<float>(nowMs - startMs) / 1000.0f * AlphiLineTiming::spiralTurnsPerSecond;
    }

    float lineAlpha(std::int64_t nowMs) const
    {
        using T = AlphiLineTiming;
        if (!has) return 0.0f;
        std::int64_t const t0 = startMs + (failed ? T::dizzyMs : 0);
        if (nowMs < t0) return 0.0f;
        std::int64_t const dt = nowMs - t0;
        if (dt < T::showMs) return 1.0f;
        if (dt < T::showMs + T::fadeMs) return 1.0f - static_cast<float>(dt - T::showMs) / static_cast<float>(T::fadeMs);
        return 0.0f;
    }

    // The spiral turns or the line fades: the motion timer keeps running.
    bool moving(std::int64_t nowMs) const
    {
        float const a = lineAlpha(nowMs);
        return dizzy(nowMs) || (a > 0.0f && a < 1.0f);
    }

    void reset()
    {
        has = false;
        lastKey.clear();
    }

private:
    bool has = false;
    bool failed = false;
    std::int64_t startMs = 0;
    std::string lastKey;
};

// Alphi calm / repaint scope: the motion timer asked for the next paint (review N3: that paint skips the dock pass when it
// stays near Alphi). One frame can hold several canvas passes (NVGSurface paints far-apart damage as separate clipped
// passes): the request is seen by EVERY pass of the frame that first looks, whatever their order, and gone from the next
// frame on. The first pass used to consume it, so a pass elsewhere in front of Alphi's own made both walk every box.
// frameId is NVGSurface's recorded-frame counter; 0 = unknown frame: consumed by the first look.
class AlphiMotionAsk {
public:
    void ask()
    {
        asked_ = true;
        frame_ = 0;
    }

    bool seenBy(std::uint64_t frameId)
    {
        if (!asked_)
            return false;
        if (frameId == 0) {
            asked_ = false;
            return true;
        }
        if (frame_ == 0)
            frame_ = frameId;
        if (frame_ != frameId) {
            asked_ = false;
            frame_ = 0;
            return false;
        }
        return true;
    }

private:
    bool asked_ = false;
    std::uint64_t frame_ = 0;
};

} // namespace mcp
