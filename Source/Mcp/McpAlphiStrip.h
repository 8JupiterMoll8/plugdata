/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Alphi drop 1, F3 (D6; docs/plans/alphi-drop1-alive-steps.md in the MCP repo; reviews R4, R7, N1, N8): the line
// strip beside Alphi's face, where his one honest line (and the busy text) shows. JUCE-free
// (Tests/mcp/alphi_dock_test.cpp); drawn by CanvasMcpAlphiSay.cpp from session.alphiSay.
//
// - Left of the face in the right corners, right of it in the left corners; bottom-aligned in the bottom corners,
//   top-aligned in the top ones; at most two lines, screen-constant size.
// - Shown only when its rect lies inside the view and is free of padded boxes, covers (the chip stack, the pill, its
//   drawers, the minimap) and the pointer (it hides while the pointer is over it, N8; no hover re-show). Otherwise it
//   is not shown at all (the line still goes to the server's log).
// - It is never part of Alphi's hit frame, never a cover (alphiCoveredNow), never a dock input and never in the
//   chips' keep-out (N1): it never moves Alphi or a chip. A click on a shown strip is swallowed: never Alphi's (no
//   flow click), never the canvas's.

#include "McpAlphi.h"
#include "McpAlphiDock.h"

#include <cmath>
#include <string>
#include <vector>

namespace mcp {

struct AlphiStripMetrics {
    static constexpr int w = 250;            // design px
    static constexpr int h = 36;             // design px: two lines
    static constexpr int gap = 6;            // design px between the face and the strip
    static constexpr std::size_t perLine = 46; // code points per line (two lines hold the 90 of a say)
};

// The strip's rect (canvas coordinates) beside `face` in `corner`; empty when it does not fit inside the view.
inline ApprovalRect alphiStripRect(ApprovalRect face, AlphiCorner corner, ApprovalRect view, float zoom)
{
    using S = AlphiStripMetrics;
    if (face.w <= 0 || face.h <= 0) return {};
    float const k = 1.0f / alphidock::safeZoom(zoom);
    int const w = static_cast<int>(std::ceil(S::w * k));
    int const h = static_cast<int>(std::ceil(S::h * k));
    int const gap = static_cast<int>(std::lround(S::gap * k));
    bool const right = corner == AlphiCorner::bottomRight || corner == AlphiCorner::topRight;
    bool const bottom = corner == AlphiCorner::bottomRight || corner == AlphiCorner::bottomLeft;
    ApprovalRect const r { right ? face.x - gap - w : face.x + face.w + gap, bottom ? face.y + face.h - h : face.y, w, h };
    return alphiRectContains(view, r) ? r : ApprovalRect {};
}

struct AlphiStripInput {
    ApprovalRect strip;    // alphiStripRect
    ApprovalRect view;
    float zoom = 1.0f;
    AlphiMouse mouse {};   // the pointer over this canvas
    bool faceShown = true; // Alphi's face is drawn (not covered, not the edge tab)
};

// Whether the strip is shown in this paint (boxes: the visible boxes, padded here by the dock's 8 design px; covers:
// the chip stack, the pill, its drawers, PlugData's overlays).
inline bool alphiStripShown(AlphiStripInput const& in, std::vector<ApprovalRect> const& boxes,
    std::vector<ApprovalRect> const& covers)
{
    auto const& s = in.strip;
    if (!in.faceShown || s.w <= 0 || s.h <= 0 || !alphiRectContains(in.view, s)) return false;
    if (in.mouse.known && alphiHit(s, in.mouse.x, in.mouse.y)) return false;
    int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad / alphidock::safeZoom(in.zoom)));
    for (auto const& b : boxes)
        if (rectsIntersect(s, alphidock::grown(b, pad))) return false;
    for (auto const& c : covers)
        if (rectsIntersect(s, c)) return false;
    return true;
}

// A click at (x, y) on the strip as drawn in the last paint (`shownStrip`, empty when not shown) is swallowed, except
// inside a live box (8 design px pad, as alphiOwnsPoint): a box that arrived under a stale strip keeps its clicks.
inline bool alphiStripTakesClick(ApprovalRect shownStrip, std::vector<ApprovalRect> const& liveBoxes, int x, int y, float zoom)
{
    if (!alphiHit(shownStrip, x, y)) return false;
    int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad / alphidock::safeZoom(zoom)));
    for (auto const& b : liveBoxes)
        if (alphiHit(alphidock::grown(b, pad), x, y)) return false;
    return true;
}

namespace alphistrip {

inline std::vector<std::string> codePoints(std::string const& s)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 1;
        while (i + len < s.size() && (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80)
            ++len;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

inline std::string join(std::vector<std::string> const& cps, std::size_t from, std::size_t to)
{
    while (to > from && cps[to - 1] == " ")
        --to;
    std::string out;
    for (std::size_t i = from; i < to; ++i)
        out += cps[i];
    return out;
}

} // namespace alphistrip

inline std::size_t alphiStripCodePoints(std::string const& s) { return alphistrip::codePoints(s).size(); }

// The say's text (already clean, McpAlphiSay.h) in at most two lines of `perLine` code points, wrapped at a space
// (a word longer than a line is cut at whole code points); more than two lines: the second ends with "…".
inline std::vector<std::string> alphiStripLines(std::string const& text, std::size_t perLine = AlphiStripMetrics::perLine)
{
    std::vector<std::string> lines;
    if (perLine < 2) return lines;
    auto const cps = alphistrip::codePoints(text);
    std::size_t const n = cps.size();
    std::size_t i = 0;
    for (int line = 0; line < 2; ++line) {
        while (i < n && cps[i] == " ")
            ++i;
        if (i >= n) break;
        if (n - i <= perLine) {
            lines.push_back(alphistrip::join(cps, i, n));
            break;
        }
        if (line == 1) {
            std::size_t cut = i + perLine - 1; // room for the ellipsis
            std::size_t sp = cut;
            while (sp > i + perLine / 2 && cps[sp] != " ")
                --sp;
            if (cps[sp] == " ") cut = sp;
            lines.push_back(alphistrip::join(cps, i, cut) + "\xE2\x80\xA6");
            break;
        }
        std::size_t sp = i + perLine; // cps[i + perLine] exists: more than perLine are left
        while (sp > i && cps[sp] != " ")
            --sp;
        std::size_t const end = sp == i ? i + perLine : sp;
        lines.push_back(alphistrip::join(cps, i, end));
        i = end;
    }
    return lines;
}

} // namespace mcp
