/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// P12a.1: Alphi, the small robot face top left of every canvas view while the MCP bridge runs. It IS the flow-mode
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

namespace mcp {

// In DESIGN (screen) pixels: Alphi is the same size on screen at any zoom.
struct AlphiMetrics {
    static constexpr int w = 48;      // the whole face (antenna + head) = the touch target
    static constexpr int h = 56;
    static constexpr int margin = 12; // from the view's top-left corner
    static constexpr std::int64_t minVisibleMs = 400;
    static constexpr std::int64_t debounceMs = 350;
};

// Alphi's rect in canvas coordinates: the top-left corner of the visible view (canvas units), screen-constant size.
inline ApprovalRect alphiRect(ApprovalRect view, float zoom)
{
    using A = AlphiMetrics;
    if (!(zoom > 0.01f) || !std::isfinite(zoom))
        zoom = 1.0f;
    float const k = 1.0f / zoom;
    return { view.x + static_cast<int>(std::lround(A::margin * k)), view.y + static_cast<int>(std::lround(A::margin * k)),
        static_cast<int>(std::ceil(A::w * k)), static_cast<int>(std::ceil(A::h * k)) };
}

// A click counts only once Alphi has been on screen (painted) for minVisibleMs; `shownSinceMs` 0 = not on screen.
inline bool alphiClickAllowed(std::int64_t shownSinceMs, std::int64_t nowMs)
{
    return shownSinceMs > 0 && nowMs - shownSinceMs >= AlphiMetrics::minVisibleMs;
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

} // namespace mcp
