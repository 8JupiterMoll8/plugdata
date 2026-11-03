/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The canvas approval state (P12c). JUCE-free so it is unit-tested on its own (Tests/mcp/approval_state_test.cpp).
//
// One chip per pending approval record of the MCP server, keyed by the server's nonce. Only the server's
// core/approval.ts sets it, through /pd/approval_show and /pd/approval_clear; HUD text no longer shows chips (H5). The
// artist's click / Enter / Esc on a chip sends /pd/approval/answer <nonce> yes|no for that record only (H6). Enter only
// approves when exactly one record is pending and its chip has been on screen for kEnterMinVisibleMs (M5). A chip
// drops itself after its TTL, so a server that went away leaves nothing behind (M6). The bar is placed by
// approvalBarRect: under its anchor or at the centre of the visible viewport, and always clamped inside it (L5).

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct ApprovalRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct ApprovalChip {
    std::string nonce;
    std::string canvas;
    std::string reason;
    std::string title;  // line 1: the server's fixed words (verb, consequence, canvas); never cut
    std::string detail; // line 2: model-chosen names (file, snapshot, array); the only line that may be cut
    std::uintptr_t canvasId = 0; // the t_canvas the question is about (0: none / not open); the outline goes only there
    std::vector<std::string> targets; // the server-built ghost preview: tempIds, or "*" for the whole canvas
    std::int64_t expiresAtMs = 0;
    std::uint64_t seq = 0;
    // Where each canvas view last drew this chip fully on screen, and since when (round 3 items 3 and 6). A view that
    // draws it outside its area drops only its own entry; a moved bar starts that view's time again.
    struct Seen {
        ApprovalRect bar;
        std::int64_t since = 0;
    };
    std::map<std::uintptr_t, Seen> seen;
    // On screen since (the earliest view that has it fully visible), or -1: on screen nowhere.
    std::int64_t onScreenSince() const
    {
        std::int64_t best = -1;
        for (auto const& entry : seen)
            if (best < 0 || entry.second.since < best) best = entry.second.since;
        return best;
    }
};

class ApprovalState {
public:
    static constexpr std::int64_t kEnterMinVisibleMs = 400;
    static constexpr std::int64_t kMinTtlMs = 1000;
    static constexpr std::int64_t kMaxTtlMs = 10 * 60 * 1000;
    static constexpr std::size_t kMaxChips = 32;

    // Show (or replace) the chip for this nonce. Refused without a nonce. A replaced chip counts as new on screen.
    bool show(std::string const& nonce, std::string const& canvas, std::string const& reason,
        std::vector<std::string> const& targets, std::int64_t ttlMs, std::int64_t nowMs)
    {
        ApprovalChip c;
        c.nonce = nonce;
        c.canvas = canvas;
        c.reason = reason;
        c.title = reason;
        c.targets = targets;
        return show(std::move(c), ttlMs, nowMs);
    }

    // The full chip (title, detail, canvasId set by the caller).
    bool show(ApprovalChip c, std::int64_t ttlMs, std::int64_t nowMs)
    {
        if (c.nonce.empty())
            return false;
        auto const nonce = c.nonce;
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        c.seen.clear();
        c.expiresAtMs = nowMs + std::clamp<std::int64_t>(ttlMs, kMinTtlMs, kMaxTtlMs);
        c.seq = ++seqCounter;
        auto it = std::find_if(chips.begin(), chips.end(), [&](ApprovalChip const& x) { return x.nonce == nonce; });
        if (it != chips.end()) {
            c.seq = it->seq; // keeps its place in the stack
            *it = std::move(c);
            return true;
        }
        if (chips.size() >= kMaxChips)
            chips.erase(chips.begin()); // the oldest goes; the server still holds its record
        chips.push_back(std::move(c));
        return true;
    }

    bool clear(std::string const& nonce)
    {
        std::lock_guard<std::mutex> const l(lock);
        auto const before = chips.size();
        chips.erase(std::remove_if(chips.begin(), chips.end(), [&](ApprovalChip const& x) { return x.nonce == nonce; }), chips.end());
        return chips.size() != before;
    }

    // Remove and return the chip the artist answered (so a double click cannot answer twice).
    std::optional<ApprovalChip> take(std::string const& nonce, std::int64_t nowMs = 0)
    {
        std::lock_guard<std::mutex> const l(lock);
        if (nowMs > 0)
            purgeLocked(nowMs);
        auto it = std::find_if(chips.begin(), chips.end(), [&](ApprovalChip const& x) { return x.nonce == nonce; });
        if (it == chips.end())
            return std::nullopt;
        auto c = *it;
        chips.erase(it);
        return c;
    }

    // The chips still on screen, oldest first.
    std::vector<ApprovalChip> visible(std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        auto v = chips;
        std::sort(v.begin(), v.end(), [](ApprovalChip const& a, ApprovalChip const& b) { return a.seq < b.seq; });
        return v;
    }

    bool any(std::int64_t nowMs) { return !visible(nowMs).empty(); }

    // A paint drew this chip FULLY inside the visible area: start its on-screen time. A chip drawn off-screen (or cut
    // by the edge) never starts it, so neither Enter nor a click can answer a chip the artist could not see.
    bool markPaintedIfInside(std::string const& nonce, std::uintptr_t viewId, ApprovalRect const& bar, ApprovalRect const& view, std::int64_t nowMs)
    {
        bool const inside = view.w > 0 && view.h > 0 && bar.x >= view.x && bar.y >= view.y
            && bar.x + bar.w <= view.x + view.w && bar.y + bar.h <= view.y + view.h;
        std::lock_guard<std::mutex> const l(lock);
        for (auto& c : chips)
            if (c.nonce == nonce) {
                if (!inside) {
                    c.seen.erase(viewId); // only this view's entry: another view may still show it
                    return false;
                }
                auto it = c.seen.find(viewId);
                bool const same = it != c.seen.end() && it->second.bar.x == bar.x && it->second.bar.y == bar.y
                    && it->second.bar.w == bar.w && it->second.bar.h == bar.h;
                if (!same)
                    c.seen[viewId] = { bar, nowMs }; // new here, or moved (a chip above went away): the time starts again
                return true;
            }
        return false;
    }

    // One view (tests).
    bool markPaintedIfInside(std::string const& nonce, ApprovalRect const& bar, ApprovalRect const& view, std::int64_t nowMs)
    {
        return markPaintedIfInside(nonce, 0, bar, view, nowMs);
    }

    // A click answers a chip only once it has been fully on screen for kEnterMinVisibleMs (a chip that just appeared
    // under the cursor is not clickable yet).
    bool clickAllowed(std::string const& nonce, std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        for (auto const& c : chips)
            if (c.nonce == nonce) {
                auto const since = c.onScreenSince();
                return since >= 0 && nowMs - since >= kEnterMinVisibleMs;
            }
        return false;
    }

    // Enter answers yes only for exactly one pending record whose chip has been on screen long enough.
    std::optional<std::string> enterTarget(std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        if (chips.size() != 1)
            return std::nullopt;
        auto const& c = chips.front();
        auto const since = c.onScreenSince();
        if (since < 0 || nowMs - since < kEnterMinVisibleMs)
            return std::nullopt;
        return c.nonce;
    }

    // When the next chip expires (ms clock), or none: the canvas schedules a repaint then, so an expired chip leaves
    // the glass on time instead of at the next unrelated repaint.
    std::optional<std::int64_t> nextExpiryMs()
    {
        std::lock_guard<std::mutex> const l(lock);
        std::optional<std::int64_t> next;
        for (auto const& c : chips)
            if (!next || c.expiresAtMs < *next) next = c.expiresAtMs;
        return next;
    }

    // Esc cancels: every chip on screen (cancelling is the safe direction).
    std::vector<std::string> escTargets(std::int64_t nowMs)
    {
        std::vector<std::string> out;
        for (auto const& c : visible(nowMs))
            out.push_back(c.nonce);
        return out;
    }

private:
    void purgeLocked(std::int64_t nowMs)
    {
        chips.erase(std::remove_if(chips.begin(), chips.end(), [&](ApprovalChip const& x) { return x.expiresAtMs <= nowMs; }), chips.end());
    }

    std::mutex lock;
    std::vector<ApprovalChip> chips;
    std::uint64_t seqCounter = 0;
};

// Line 2 cut to what fits, at a UTF-8 code point, with an ellipsis; `fits(text)` says whether a text fits. Line 1 is
// never passed through this (it is the server's fixed wording; the bar grows or the font shrinks for it).
template<typename Fits>
std::string approvalEllipsize(std::string const& text, Fits fits)
{
    if (fits(text))
        return text;
    static std::string const ell = "\xe2\x80\xa6";
    std::string cut = text;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
            cut.pop_back(); // a continuation byte: keep cutting to the code point start
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80) != 0 && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0)
            cut.pop_back(); // a lead byte whose continuation is gone
        if (fits(cut + ell))
            return cut + ell;
    }
    return ell;
}

// How line 1 names the chip's canvas: "pd-main" is "main", "pd-x" is "x", a root file stays as it is.
inline std::string approvalCanvasLabel(std::string const& key)
{
    if (key == "pd-main") return "main";
    if (key.rfind("pd-", 0) == 0) return key.substr(3);
    return key;
}

// The visible area of a canvas view in canvas coordinates. CanvasViewport::getViewArea() already IS that (view
// position and size divided by the zoom): it is used as it is, never divided by the zoom a second time (P12c review:
// at zoom 0.5 / 1.5 / 2 the double division put the bar thousands of pixels off-screen).
inline ApprovalRect approvalVisibleArea(float x, float y, float w, float h)
{
    auto r = [](float v) { return static_cast<int>(v >= 0 ? v + 0.5f : v - 0.5f); };
    int const x0 = r(x), y0 = r(y);
    return { x0, y0, r(x + w) - x0, r(y + h) - y0 };
}

// Where the chip bar (a stack of `h` pixels) goes, in canvas coordinates: under its anchor (the pill or the
// selection), flipped above when it would leave the bottom; with no anchor at the centre of the VISIBLE viewport.
// Always clamped inside the viewport (with a margin when it fits); never at the canvas origin.
inline ApprovalRect approvalBarRect(std::optional<ApprovalRect> anchor, ApprovalRect view, int w, int h, int margin = 12, int gap = 14)
{
    if (view.w <= 0 || view.h <= 0)
        return { view.x - w / 2, view.y - h / 2, w, h }; // nothing laid out yet: centred on what there is
    int x, y;
    if (anchor) {
        x = anchor->x + anchor->w / 2 - w / 2;
        y = anchor->y + anchor->h + gap;
        if (y + h > view.y + view.h - margin)
            y = anchor->y - gap - h; // no room below: above the anchor
    } else {
        x = view.x + view.w / 2 - w / 2;
        y = view.y + view.h / 2 - h / 2;
    }
    auto clampAxis = [&](int v, int start, int len, int size) {
        if (len - 2 * margin < size)
            return len < size ? start : start + (len - size) / 2;
        return std::clamp(v, start + margin, start + len - margin - size);
    };
    x = clampAxis(x, view.x, view.w, w);
    y = clampAxis(y, view.y, view.h, h);
    return { x, y, w, h };
}

// P12a.1: two rects share at least one canvas unit.
inline bool rectsIntersect(ApprovalRect a, ApprovalRect b)
{
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// P12a.1: `place` moved off `keep` (Alphi, the flow toggle top left), staying inside `view`: to the right of it when
// the view is wide enough, else below it. Unchanged when the two do not meet, or when the view is too small for either
// (then the chips, which take clicks before Alphi and are drawn over it, still win).
inline ApprovalRect avoidRect(ApprovalRect place, ApprovalRect keep, ApprovalRect view, int margin)
{
    if (!rectsIntersect(place, keep))
        return place;
    int const right = view.x + view.w;
    int const bottom = view.y + view.h;
    int const toRight = keep.x + keep.w + margin;
    if (toRight + place.w <= right - margin)
        return { toRight, place.y, place.w, place.h };
    int const below = keep.y + keep.h + margin;
    for (int edge : { margin, 0 })
        if (below + place.h <= bottom - edge)
            return { std::clamp(place.x, view.x, std::max(view.x, right - place.w)), below, place.w, place.h };
    return place;
}

// The chip bar in DESIGN pixels: what one bar measures on screen, whatever the canvas zoom (live check 2026-11-02:
// drawn in canvas units the 440-wide bar outgrew the view from zoom ~1.6). Compact per the owner (2026-11-02, "easy to
// click but a bit big"): ~73% of the old 440x78 footprint, the buttons taller than before (36 px).
struct ApprovalBarMetrics {
    static constexpr int w = 340;      // minimum width; a long line 1 widens the bar (never cut)
    static constexpr int h = 74;
    static constexpr int gap = 6;      // between stacked bars
    static constexpr int more = 20;    // the "+N more waiting" line under the stack
    static constexpr int margin = 12;  // to the view edge
    static constexpr int anchorGap = 14;
    static constexpr int textX = 10;
    static constexpr float line1Y = 11.0f;
    static constexpr float line2Y = 25.0f;
    static constexpr int btnX = 8;
    static constexpr int btnY = 34;
    static constexpr int btnH = 36;
    static constexpr int btnGap = 8;
    static constexpr int maxBars = 4;
    static constexpr float minTitleFont = 9.0f;
};

// Width of one button (✓ or ✗) of a bar `barW` design pixels wide.
inline int approvalButtonW(int barW)
{
    return (barW - 2 * ApprovalBarMetrics::btnX - ApprovalBarMetrics::btnGap) / 2;
}

struct ApprovalBarLayout {
    ApprovalRect frame, yes, no; // canvas coordinates
};

struct ApprovalStackLayout {
    float scale = 1.0f;          // canvas units per design pixel: draw a bar at frame.x/y, scaled by this
    ApprovalRect frame;          // the whole stack, "+N more" line included
    std::vector<ApprovalBarLayout> bars;
};

// The chip stack in canvas coordinates at a fixed SCREEN size: one design pixel is 1/zoom canvas units, smaller only
// when the window cannot hold the stack (then it shrinks to fit). Placed by approvalBarRect (under the anchor or at the
// view centre) and clamped inside the view, so every bar is fully visible and its buttons can be hit at any zoom.
// The space of the full stack (maxBars) is reserved when the window holds it, so a new chip never moves the ones
// already on screen.
// P12a.1: `keepOut` (Alphi's rect) is a spot the stack never covers when the view has room elsewhere (avoidRect).
inline ApprovalStackLayout approvalStackLayout(std::optional<ApprovalRect> anchor, ApprovalRect view, float zoom, int barW, int shown, bool more,
    std::optional<ApprovalRect> keepOut = std::nullopt)
{
    using M = ApprovalBarMetrics;
    ApprovalStackLayout out;
    if (shown <= 0 || barW <= 0)
        return out;
    if (!(zoom > 0.01f)) zoom = 1.0f;
    float const k0 = 1.0f / zoom;
    float const stackPx = static_cast<float>(shown * M::h + (shown - 1) * M::gap + (more ? M::more : 0));
    float k = k0;
    if (view.w > 0 && view.h > 0) {
        float const m = M::margin * k0;
        auto avail = [&](int len) {
            float const a = static_cast<float>(len) - 2.0f * m;
            return a >= len * 0.5f ? a : static_cast<float>(len); // a tiny view: use all of it
        };
        k = std::min({ k0, avail(view.w) / barW, avail(view.h) / stackPx });
    }
    // Whole canvas units: a rect is never smaller than what is drawn in it (the scale follows the rounded width), and
    // the bars sit on a whole-unit pitch so neighbours never overlap. Rounding up can outgrow a tiny view: then shrink.
    int wc = 0, hc = 0, gapC = 0, moreC = 0, stackHc = 0;
    for (int tries = 0; tries < 32; ++tries) {
        wc = std::max(1, static_cast<int>(std::floor(barW * k)));
        k = static_cast<float>(wc) / static_cast<float>(barW);
        hc = static_cast<int>(std::ceil(M::h * k));
        gapC = static_cast<int>(std::lround(M::gap * k));
        moreC = more ? static_cast<int>(std::ceil(M::more * k)) : 0;
        stackHc = shown * hc + (shown - 1) * gapC + moreC;
        if (view.h <= 0 || stackHc <= view.h || wc == 1)
            break;
        k *= 0.95f;
    }
    int const fullHc = M::maxBars * hc + (M::maxBars - 1) * gapC + static_cast<int>(std::ceil(M::more * k));
    int const marginC = static_cast<int>(std::lround(M::margin * k));
    int const placeH = (fullHc + 2 * marginC <= view.h) ? fullHc : stackHc;
    auto place = approvalBarRect(anchor, view, wc, placeH, marginC, static_cast<int>(std::lround(M::anchorGap * k)));
    if (keepOut) {
        // The reserved space first (a new chip never moves the others); if only the real stack fits, that.
        auto moved = avoidRect(place, *keepOut, view, marginC);
        if (rectsIntersect(moved, *keepOut))
            moved = avoidRect(ApprovalRect { place.x, place.y, wc, stackHc }, *keepOut, view, marginC);
        place = { moved.x, moved.y, wc, placeH };
    }

    out.scale = k;
    out.frame = { place.x, place.y, wc, stackHc };
    int const half = approvalButtonW(barW);
    auto px = [&](float v) { return std::max(1, static_cast<int>(std::floor(v * k))); };
    for (int i = 0; i < shown; ++i) {
        int const y = place.y + i * (hc + gapC);
        ApprovalBarLayout b;
        b.frame = { place.x, y, wc, hc };
        int const by = y + static_cast<int>(std::ceil(M::btnY * k));
        int const bh = std::min(px(static_cast<float>(M::btnH)), y + hc - by);
        b.yes = { place.x + static_cast<int>(std::ceil(M::btnX * k)), by, px(static_cast<float>(half)), bh };
        b.no = { place.x + static_cast<int>(std::ceil((M::btnX + half + M::btnGap) * k)), by, px(static_cast<float>(half)), bh };
        out.bars.push_back(b);
    }
    return out;
}

} // namespace mcp
