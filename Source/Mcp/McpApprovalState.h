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

struct ApprovalChip {
    std::string nonce;
    std::string canvas;
    std::string reason;
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
        if (nonce.empty())
            return false;
        std::lock_guard<std::mutex> const l(lock);
        purgeLocked(nowMs);
        ApprovalChip c;
        c.nonce = nonce;
        c.canvas = canvas;
        c.reason = reason;
        c.targets = targets;
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

    // A paint drew the chips: start their on-screen time.
    void markPainted(std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const l(lock);
        for (auto& c : chips)
            if (c.firstPaintedMs < 0)
                c.firstPaintedMs = nowMs;
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

struct ApprovalRect {
    int x = 0, y = 0, w = 0, h = 0;
};

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
