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

// Review N3: this paint (`region`, canvas coordinates) is Alphi's own: the motion timer asked for it and it stays
// inside Alphi's frame and strip (grown by scopePad). Then the paint skips the dock pass. Any other paint (a box moved
// under Alphi, a canvas repaint coalesced with his) is not.
inline bool alphiOnlyPaint(ApprovalRect region, bool motionAsked, ApprovalRect frame, ApprovalRect strip, float zoom)
{
    if (!motionAsked || region.w <= 0 || region.h <= 0 || frame.w <= 0 || frame.h <= 0) return false;
    int const pad = alphimotion::padUnits(AlphiMotionMetrics::scopePad, zoom);
    auto const own = alphimotion::unite(alphidock::grown(frame, pad),
        strip.w > 0 && strip.h > 0 ? alphidock::grown(strip, pad) : ApprovalRect {});
    return alphiRectContains(own, region);
}

// D4: the light ~30 Hz motion timer runs only while Alphi is drawn (shown, uncovered) and something on him moves:
// the pulse level differs from the drawn one by minStep or more (or the last step to calm), sparks / dots (busy), or
// the line's spiral or fade. It repaints his own rect (alphiMotionArea) and never touches the gate or the dock.
struct AlphiMotionInput {
    bool shown = false;      // drawn this paint (alphiShown, placed, not the edge tab)
    bool covered = false;    // a chip stack / the pill covers him
    float target = 0.0f;     // the pulse level the ceiling asks for (alphiPulse)
    float drawn = 0.0f;      // the level last drawn
    bool busy = false;       // sparks (building) or dots (measuring)
    bool lineMoving = false; // the dizzy spiral or the line's fade (AlphiLineClock::moving)
};

struct AlphiMotionTiming {
    static constexpr int intervalMs = 33;   // ~30 Hz
    static constexpr float minStep = 0.02f; // smaller level changes are not repainted
};

inline bool alphiMotionRun(AlphiMotionInput const& in)
{
    if (!in.shown || in.covered) return false;
    if (in.busy || in.lineMoving) return true;
    float const d = std::fabs(in.target - in.drawn);
    return d >= AlphiMotionTiming::minStep || (in.target <= 0.0f && in.drawn > 0.0f);
}

// The 10 Hz tick wakes the motion timer when the ceiling published a new block or the level crossed -60 dBFS
// (level 0 <-> above 0).
inline bool alphiPulseWake(std::uint64_t lastBlocks, std::uint64_t blocks, float lastLevel, float level)
{
    return blocks != lastBlocks || ((lastLevel > 0.0f) != (level > 0.0f));
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

} // namespace mcp
