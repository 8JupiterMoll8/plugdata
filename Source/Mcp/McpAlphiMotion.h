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

#include <algorithm>
#include <cmath>

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

} // namespace mcp
