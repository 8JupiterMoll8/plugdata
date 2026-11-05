/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// P12a.1: Alphi, the small robot face in a corner of every canvas view while the MCP bridge runs (big Alphi 1a0: the
// corner no box, chip stack, pill or drawer lies under, default bottom right; McpAlphiDock.h). It IS the flow-mode
// switch (owner decision 2026-11-03, docs/plans/p12-flow-mode-steps.md): one click turns flow mode on, the next one
// off. JUCE-free (Tests/mcp/alphi_test.cpp); drawn and clicked in CanvasMcpFlow.cpp, switched in McpHost::alphiClicked.
//
// - The click is the artist's mouse on the canvas; no verb, HUD text, note or patch text reaches alphiClicked
//   (scripts/test-bridge-safety.sh: artistTurnOn is called only there, alphiClicked only from the canvas click).
// - A click counts once Alphi has been on screen for minVisibleMs (as the approval chips), and a double click flips once.
// - The approval chips are laid out around Alphi (approvalStackLayout keepOut), so a ✓ never sits on it.

#include "McpApprovalState.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace mcp {

// In DESIGN (screen) pixels: Alphi is the same size on screen at any zoom.
struct AlphiMetrics {
    static constexpr int w = 48;      // the whole face (antenna + head) = the touch target
    static constexpr int h = 56;
    static constexpr int margin = 12; // from the two edges of its corner of the view
    static constexpr std::int64_t minVisibleMs = 400;
    static constexpr std::int64_t debounceMs = 350;
    static constexpr std::int64_t brakeFloorMs = 150;
    static constexpr std::int64_t flowOffGapMs = 600;
};

// Alphi's rect in canvas coordinates at the top-left corner of the visible view (canvas units), screen-constant size.
// 1a0: the canvas uses alphiCornerRect (McpAlphiDock.h) for the corner it docks in; this is its top-left case.
inline ApprovalRect alphiRect(ApprovalRect view, float zoom)
{
    using A = AlphiMetrics;
    if (!(zoom > 0.01f) || !std::isfinite(zoom))
        zoom = 1.0f;
    float const k = 1.0f / zoom;
    return { view.x + static_cast<int>(std::lround(A::margin * k)), view.y + static_cast<int>(std::lround(A::margin * k)),
        static_cast<int>(std::ceil(A::w * k)), static_cast<int>(std::ceil(A::h * k)) };
}

// A click counts only once Alphi has been on screen (painted) for minVisibleMs (`shownSinceMs` 0 = not on screen), and
// not within minVisibleMs after any approval chip left the glass (review M2: `lastChipGoneMs`, 0 = never): a chip the AI
// clears from on top of Alphi just before the artist's ✓ click never turns that click into flow ON. Review of
// 2a13ec760: nor within flowOffGapMs after flow went off by ANY door (`lastFlowOffMs`, 0 = never): the second half of
// a slow double click on the brake (past the 350 ms double-click guard), or a click on a face still drawn ON after
// another door switched flow off, never turns flow back on.
inline bool alphiClickAllowed(std::int64_t shownSinceMs, std::int64_t nowMs, std::int64_t lastChipGoneMs = 0,
    std::int64_t lastFlowOffMs = 0)
{
    if (shownSinceMs <= 0 || nowMs - shownSinceMs < AlphiMetrics::minVisibleMs)
        return false;
    if (lastFlowOffMs > 0 && nowMs - lastFlowOffMs < AlphiMetrics::flowOffGapMs)
        return false;
    return lastChipGoneMs <= 0 || nowMs - lastChipGoneMs >= AlphiMetrics::minVisibleMs;
}

// What a click on a point of Alphi's face (alphiOwnsPoint) does. The brake (owner rule, d1 live check 2026-11-05):
// while flow is ON, a click on the face drawn whole at its spot (`shownSinceMs` > 0) for brakeFloorMs turns flow OFF.
// The floor (not the 400 ms of the way ON, and no wait after a chip left) keeps a face that slid under a canvas click
// from eating it; a gate that restarted under a face that stayed in view never holds the brake longer. The face just
// out of the edge tab (`peekOut`, D11) keeps the full minVisibleMs: a double click on the tab is never flow. The
// 350 ms double-click guard (McpHost's AlphiClickGate) applies to the brake as to every Alphi click. Flow OFF -> ON
// keeps the full rule (alphiClickAllowed, with `lastFlowOffMs`). A face not drawn (gate 0) takes nothing.
enum class AlphiClickAct { None, Brake, TurnOn };
inline AlphiClickAct alphiClickAct(bool flowOn, std::int64_t shownSinceMs, std::int64_t nowMs, std::int64_t lastChipGoneMs = 0,
    std::int64_t lastFlowOffMs = 0, bool peekOut = false)
{
    if (flowOn) {
        std::int64_t const floor = peekOut ? AlphiMetrics::minVisibleMs : AlphiMetrics::brakeFloorMs;
        return shownSinceMs > 0 && nowMs - shownSinceMs >= floor ? AlphiClickAct::Brake : AlphiClickAct::None;
    }
    return alphiClickAllowed(shownSinceMs, nowMs, lastChipGoneMs, lastFlowOffMs) ? AlphiClickAct::TurnOn : AlphiClickAct::None;
}

// Review M1: a point inside Alphi's frame (canvas coordinates) belongs to Alphi, never to the canvas (no double-click
// box, no lasso). An empty frame (Alphi not drawn) holds nothing.
inline bool alphiHit(ApprovalRect frame, int x, int y)
{
    return frame.w > 0 && frame.h > 0 && x >= frame.x && y >= frame.y && x < frame.x + frame.w && y < frame.y + frame.h;
}

// Review M2: something on the glass (a chip stack that could not move off Alphi, the pill, its drawers) covers Alphi:
// then Alphi is not drawn and not clickable, and its on-screen time starts again once it is uncovered.
inline bool alphiCovered(ApprovalRect alphi, std::initializer_list<ApprovalRect> covers)
{
    for (auto const& c : covers)
        if (rectsIntersect(alphi, c))
            return true;
    return false;
}

// The state a click on Alphi leads to: on when off, off when on.
inline bool alphiNextOn(bool isOn) { return !isOn; }

// One flip per click: a second click within debounceMs of the last counted one (a double click) is ignored.
class AlphiClickGate {
public:
    bool accept(std::int64_t nowMs)
    {
        if (nowMs <= 0)
            return false;
        auto prev = last.load(std::memory_order_acquire);
        do {
            if (prev != 0 && nowMs - prev < AlphiMetrics::debounceMs && nowMs >= prev)
                return false;
        } while (!last.compare_exchange_weak(prev, nowMs, std::memory_order_acq_rel));
        return true;
    }

private:
    std::atomic<std::int64_t> last { 0 };
};

struct AlphiRgba {
    unsigned char r, g, b, a;
};

// What Alphi looks like (Teenage Engineering: white head, dark screen, two dot eyes).
struct AlphiLook {
    AlphiRgba eye;
    float eyeRadius;  // design pixels
    bool sleepy;      // off: the eyes are flat calm dashes
    bool glow;        // on: a warm halo around the eyes
    bool antennaLit;  // on: the antenna tip glows
    AlphiRgba antenna;
    bool label;       // on: "FLOW" on the chin
    char const* labelText;
};

inline AlphiLook alphiLook(bool on)
{
    if (on)
        return { { 255, 204, 51, 255 }, 3.6f, false, true, true, { 255, 196, 40, 255 }, true, "FLOW" };
    return { { 120, 135, 150, 170 }, 2.6f, true, false, false, { 150, 150, 146, 255 }, false, "" };
}

// Alphi drop 1, F1 (D2, D3; docs/plans/alphi-drop1-alive-steps.md in the MCP repo): the eyes pulse with the sound
// that really leaves PlugData. The output ceiling publishes the loudest channel's block mean square after its breaker
// gain and a block counter (OutputCeiling::readPulse, audio thread, no AI); the canvas follows it with a 30 ms attack /
// 150 ms release (alphiPulseFollow) and draws the eyes from the level (alphiEyesAt). Calm eyes mean "nothing
// measured" (silence, below -60 dBFS, no new block for 200 ms: device stopped, ceiling off, plugin build), never a
// claim that it is silent.
struct AlphiPulseMetrics {
    static constexpr float floorDb = -60.0f;    // at or below: the resting look
    static constexpr float topDb = -6.0f;       // at or above: the full pulse
    static constexpr std::int64_t staleMs = 200; // no new block for longer: calm
    static constexpr float attackMs = 30.0f;    // level 0 -> 1 at most this fast
    static constexpr float releaseMs = 150.0f;  // level 1 -> 0 at most this fast
    static constexpr float dashRest = 2.2f;     // off look: the flat dash's height (design px) at rest ...
    static constexpr float dashFull = 5.0f;     // ... and at the full pulse
    static constexpr float radiusGrow = 1.45f;  // on look: the eye radius at the full pulse, times the resting one
};

struct AlphiEyes {
    float level = 0.0f;      // 0..1
    float eyeRadius = 0.0f;  // design px (the on look's dots; the off look keeps its own)
    float dashHeight = AlphiPulseMetrics::dashRest; // design px (the off look's flat dashes)
    bool calm = true;        // the resting look
};

// The eyes at a level 0..1 for this look (D3): off = the dash grows 2.2 -> 5 px, on = the radius grows.
inline AlphiEyes alphiEyesAt(float level, AlphiLook const& look)
{
    using P = AlphiPulseMetrics;
    if (!(level > 0.0f)) level = 0.0f; // NaN and below 0
    if (level > 1.0f) level = 1.0f;
    AlphiEyes e;
    e.level = level;
    e.calm = level <= 0.0f;
    e.eyeRadius = look.sleepy ? look.eyeRadius : look.eyeRadius * (1.0f + (P::radiusGrow - 1.0f) * level);
    e.dashHeight = look.sleepy ? P::dashRest + (P::dashFull - P::dashRest) * level : P::dashRest;
    return e;
}

// The level a mean square (`ms`, 0..1 = 0 dBFS) stands for: dB-linear from -60 dBFS (0) to -6 dBFS (1); 0 when the
// value is not a positive finite number.
inline float alphiPulseLevel(float ms)
{
    using P = AlphiPulseMetrics;
    if (!(ms > 0.0f) || !std::isfinite(ms)) return 0.0f;
    float const db = 10.0f * std::log10(ms);
    float const l = (db - P::floorDb) / (P::topDb - P::floorDb);
    return l <= 0.0f ? 0.0f : (l >= 1.0f ? 1.0f : l);
}

// The eyes for a mean square that is `ageMs` old (since the ceiling's block counter last moved).
inline AlphiEyes alphiPulse(float ms, std::int64_t ageMs, AlphiLook const& look)
{
    bool const fresh = ageMs >= 0 && ageMs <= AlphiPulseMetrics::staleMs;
    return alphiEyesAt(fresh ? alphiPulseLevel(ms) : 0.0f, look);
}

// The canvas's attack / release: from `level` toward `target` (both 0..1) over `dtMs`, linear in the level (= in dB).
inline float alphiPulseFollow(float level, float target, float dtMs)
{
    using P = AlphiPulseMetrics;
    if (!(level >= 0.0f)) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (!(target >= 0.0f)) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    if (!(dtMs > 0.0f) || !std::isfinite(dtMs)) return level;
    if (target > level) return std::fmin(target, level + dtMs / P::attackMs);
    return std::fmax(target, level - dtMs / P::releaseMs);
}

// Alphi drop 1, F2 (D5): his eyes look toward the selection. The offset (design px) points from his frame's centre to
// the selection's centre, 1 px per 40 design px of screen distance, clamped to +-maxPx on the larger axis (direction
// kept); no selection, or none of his frame: straight ahead. Screen-constant: the zoom comes from his frame's size.
struct AlphiGazeMetrics {
    static constexpr float maxPx = 3.0f;          // design px, per axis
    static constexpr float designPxPerPx = 40.0f; // screen distance (design px) per px of gaze
};

struct AlphiGaze {
    float dx = 0.0f, dy = 0.0f; // design px
};

inline AlphiGaze alphiGaze(ApprovalRect frame, std::optional<ApprovalRect> selection)
{
    using G = AlphiGazeMetrics;
    if (!selection || selection->w <= 0 || selection->h <= 0 || frame.w <= 0 || frame.h <= 0) return {};
    double const unitsPerDesignPx = static_cast<double>(frame.w) / AlphiMetrics::w;
    double const vx = ((selection->x + selection->w * 0.5) - (frame.x + frame.w * 0.5)) / unitsPerDesignPx / G::designPxPerPx;
    double const vy = ((selection->y + selection->h * 0.5) - (frame.y + frame.h * 0.5)) / unitsPerDesignPx / G::designPxPerPx;
    if (!std::isfinite(vx) || !std::isfinite(vy)) return {};
    double const big = std::fmax(std::fabs(vx), std::fabs(vy));
    double const scale = big > G::maxPx ? G::maxPx / big : 1.0;
    return { static_cast<float>(vx * scale), static_cast<float>(vy * scale) };
}

} // namespace mcp
