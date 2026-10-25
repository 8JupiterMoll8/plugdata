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
    std::int64_t firstPaintedMs = -1; // -1: not on screen yet
    std::uint64_t seq = 0;
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
        c.firstPaintedMs = -1;
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
    bool markPaintedIfInside(std::string const& nonce, ApprovalRect const& bar, ApprovalRect const& view, std::int64_t nowMs)
    {
        bool const inside = view.w > 0 && view.h > 0 && bar.x >= view.x && bar.y >= view.y
            && bar.x + bar.w <= view.x + view.w && bar.y + bar.h <= view.y + view.h;
        std::lock_guard<std::mutex> const l(lock);
        for (auto& c : chips)
            if (c.nonce == nonce) {
                if (!inside)
                    c.firstPaintedMs = -1; // left the screen: the on-screen time starts again when it is back
                else if (c.firstPaintedMs < 0)
                    c.firstPaintedMs = nowMs;
                return inside;
            }
        return false;
    }

    // A click answers a chip only once it has been fully on screen for kEnterMinVisibleMs (a chip that just appeared
    // under the cursor is not clickable yet).
    bool clickAllowed(std::string const& nonce, std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        for (auto const& c : chips)
            if (c.nonce == nonce)
                return c.firstPaintedMs >= 0 && nowMs - c.firstPaintedMs >= kEnterMinVisibleMs;
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
        if (c.firstPaintedMs < 0 || nowMs - c.firstPaintedMs < kEnterMinVisibleMs)
            return std::nullopt;
        return c.nonce;
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
inline ApprovalRect approvalBarRect(std::optional<ApprovalRect> anchor, ApprovalRect view, int w, int h)
{
    constexpr int margin = 12;
    constexpr int gap = 14;
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

} // namespace mcp
