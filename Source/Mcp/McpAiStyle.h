/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The one AI style table (design round 2026-11-19, step A): every colour, size and tag shape of what the AI draws on
// the canvas lives here, in one place, for the C++ overlays (CanvasMcpLayer.cpp, CanvasMcpNotes.cpp, CanvasMcpPill.cpp,
// the Copilot panel). The Lua widgets cannot share a module; each carries a byte-identical palette block whose values a
// TS test compares with this file. JUCE-free, so Tests/mcp/ai_style_test.cpp compiles it alone; the drawing code turns
// an AiRgba into its own colour type.
//
// The look is the owner's picture "AI overlays, language A": one ink, three shapes for what the AI says (square = a
// measurement, circle = a change, triangle = look here). A part of the patch (owner, 2026-11-20, "a combination"): the 3 px
// ink bar and the name of option A over a see-through COLOUR per kind, no outline ("colored, transparent regions ... I could
// see in one moment what it was"; the grey of 2026-11-19 read as no colour, the bar alone too weak).

#include "McpApprovalState.h" // ApprovalRect, rectsIntersect

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

// -------------------------------------------------------------------------------------------------------- colours

struct AiRgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

constexpr AiRgba aiCream { 0xEC, 0xE5, 0xD3, 255 };  // paper
constexpr AiRgba aiInk { 0x1A, 0x1A, 0x1A, 255 };    // the one ink: text, lines, the artist's own marks
constexpr AiRgba aiGrey { 0x8C, 0x85, 0x76, 255 };   // hints, leader lines, region titles
constexpr AiRgba aiRed { 0xDC, 0x29, 0x1F, 255 };    // "I changed this" (a circle), "recording", "now"
constexpr AiRgba aiYellow { 0xF7, 0xC2, 0x14, 255 }; // "look here" (a triangle)
constexpr AiRgba aiBlue { 0x14, 0x3D, 0x9E, 255 };   // "a measurement" (a square), a message on a wire
constexpr AiRgba aiNoteYellow { 0xF6, 0xE7, 0xA6, 255 }; // the sticky note (ai.note)
constexpr float aiOffPathCordAlpha = 0.25f; // a cord off the lit path (control or signal) is drawn at 25 %

// -------------------------------------------------------------------------------------------------------- sizes

constexpr float aiLineWidth = 1.0f;       // every line
constexpr float aiLineWidthActive = 2.0f; // the active line
constexpr float aiRadius = 2.0f;          // every corner
constexpr int aiHeaderHeight = 24;        // a widget's header row
constexpr int aiPadding = 12;             // the air around content
constexpr float aiNameSize = 13.0f;       // a name (widget, card title), Inter-Medium
constexpr float aiHintSize = 11.0f;       // a hint, a second line
constexpr float aiTagTextSize = 12.0f;    // the text of a tag on the canvas
constexpr float aiTagGlyphSize = 9.0f;    // the shape before a tag's text
constexpr float aiGhostAlpha = 0.6f;      // a ghost is ink at 60 %
constexpr float aiStepDiscSize = 14.0f;   // the spotlight's order disc: it carries a number, so it is bigger than a tag glyph

// A tag is one frame: a leader, a glyph slot of 16 px, then the text. The width is an estimate by character count (an
// exact measure would need a font); the tag's paint AND its hit test (hover, close cross, chips drawer) call this one
// function, so a tag can never be drawn where it cannot be clicked.
constexpr float aiTagGlyphSlotPx = 16.0f;
constexpr float aiTagCharPx = 6.4f;
constexpr float aiTagHeightPx = 18.0f;
constexpr float aiTagLeaderPx = 14.0f; // the air between a tag and its boxes, and the longest a grey leader line ever is

constexpr float aiTagWidthPx(std::size_t characters)
{
    return aiTagGlyphSlotPx + static_cast<float>(characters) * aiTagCharPx;
}

// -------------------------------------------------------------------------------------------------------- tags

enum class AiTag { None, Fact, Guess, Changed, Proposed, Look, Ask, Step };

namespace aistyle_detail {

inline std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace aistyle_detail

/** The tag a kind word asks for. New words: fact, guess, changed, proposed, look, ask, step. Old words keep working
 *  (info -> guess, change -> changed, warn -> look, artist -> none: the artist's own note is plain ink text). Anything
 *  unknown or empty is a guess: an unmarked claim is never shown as measured. Only the exact word counts (no trimming);
 *  the case does not matter. */
inline AiTag aiTagOf(std::string_view kind)
{
    auto const k = aistyle_detail::lower(kind);
    if (k == "fact") return AiTag::Fact;
    if (k == "guess") return AiTag::Guess;
    if (k == "changed" || k == "change") return AiTag::Changed;
    if (k == "proposed") return AiTag::Proposed;
    if (k == "look" || k == "warn") return AiTag::Look;
    if (k == "ask") return AiTag::Ask;
    if (k == "step") return AiTag::Step;
    if (k == "artist") return AiTag::None;
    return AiTag::Guess; // "info", "" and everything the server does not know
}

/** The glyph's size in px: 9 for the tag shapes (never above 10), the disc's own size for a step, 0 for none. */
constexpr float aiGlyphSize(AiTag tag)
{
    switch (tag) {
    case AiTag::None: return 0.0f;
    case AiTag::Step: return aiStepDiscSize;
    default: return aiTagGlyphSize;
    }
}

// ------------------------------------------------------------------------------------------------------- regions

constexpr int aiRegionPadTop = 28;      // room for the title above the members
constexpr int aiRegionGap = 4;          // the least air between two regions' rectangles
constexpr int aiRegionMinBelow = 8;     // the least air under any member, where the neighbour's band allows it
constexpr float aiRegionBarWidth = 3.0f;        // the ink bar down the region's left edge
constexpr float aiRegionTitleAlpha = 0.85f;     // the name is ink at 85 %
constexpr float aiRegionFillAlpha = 0.16f;      // the see-through colour of the region's kind (behind the boxes, no outline)
constexpr float aiRegionTitleSize = 12.0f;      // the name, Inter-Medium, in patch px at zoom 1
constexpr float aiRegionTitleMinScreenPx = 11.0f; // ... but never smaller than this on the screen when zoomed out
constexpr float aiRegionTitleInset = 8.0f;      // from the rectangle's left edge (the bar and 5 px of air) to the name
constexpr float aiRegionTitleTop = 6.0f;        // from the rectangle's top edge to the name
constexpr float aiInterLineHeight = 1.21f;      // Inter's line box as a multiple of its font size (nvgTextBounds' height)
constexpr float aiRegionTitleMaxSize = 18.0f;   // the name's size cap: 18 x 1.21 = 21.8 px fits what the 28 px band leaves under the 6 px top (22)

namespace detail {
inline bool aiKindIs(std::string_view kind, std::string_view word)
{
    if (kind.size() != word.size())
        return false;
    for (std::size_t i = 0; i < kind.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(kind[i])) != word[i])
            return false;
    return true;
}
}

/** A region's hue by its kind (source green, fx blue, loop amber, output red; anything else is a group: violet grey).
 *  Drawn at aiRegionFillAlpha behind the boxes, no outline. */
inline AiRgba aiRegionTint(std::string_view kind)
{
    if (detail::aiKindIs(kind, "source"))
        return { 0x2E, 0x9E, 0x5B, 255 };
    if (detail::aiKindIs(kind, "fx"))
        return { 0x3A, 0x78, 0xE0, 255 };
    if (detail::aiKindIs(kind, "loop"))
        return { 0xE8, 0xA2, 0x1C, 255 };
    if (detail::aiKindIs(kind, "output"))
        return { 0xE0, 0x49, 0x3C, 255 };
    return { 0x8A, 0x86, 0xA0, 255 };
}

/** The name's size in patch px for a canvas zoom: 12 px, or more when the zoom would make it smaller than 11 screen px
 *  (a zoomed-out canvas must still show what a part is called), but never more than aiRegionTitleMaxSize (18): a bigger
 *  line would run past its 28 px title band over the members below, so under zoom 11/18 = 0.61 the name gets smaller on
 *  the screen than 11 px. A zoom that is not a positive number counts as 1. */
inline float aiRegionTitleSizeFor(float zoom)
{
    if (!(zoom > 0.0f) || !std::isfinite(zoom))
        return aiRegionTitleSize;
    return std::min(aiRegionTitleMaxSize, std::max(aiRegionTitleSize, aiRegionTitleMinScreenPx / zoom));
}

/** One rectangle per region, from the bounding box of the region's member boxes (patch coordinates; the caller leaves
 *  Pd comments out of the bounds):
 *  - padded by aiPadding (12) on three sides and aiRegionPadTop (28, the title) on top;
 *  - where two padded rectangles meet (or come closer than aiRegionGap), both shrink toward their members to a 4 px gap
 *    (to as much as the members allow: a rectangle never cuts a member box, so members closer than 4 px leave less).
 *    Side by side the free band is split in the middle; one above the other the squeeze is shared evenly between the
 *    upper region's air below and the lower region's title room, and the upper one keeps aiRegionMinBelow (8 px) under
 *    its members whenever the band holds 8 px plus the gap;
 *  - a region whose members lie on an EARLIER region's members (any earlier one, an emptied one too) gets an empty
 *    rectangle at the place its title goes (x = members.x - padding, y = members.y - 28, w = h = 0): title only;
 *  - a member box without area also gets an empty rectangle.
 *  The result has one entry per input, in the same order; an empty one has w == 0 and h == 0. */
inline std::vector<ApprovalRect> aiRegionRects(std::vector<ApprovalRect> const& memberBounds)
{
    std::size_t const n = memberBounds.size();
    std::vector<ApprovalRect> out(n);
    // lo/hi per axis (0 = x, 1 = y) of every padded rectangle; `kept` = the region has a rectangle.
    std::vector<std::array<int, 2>> lo(n), hi(n);
    std::vector<bool> kept(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        auto const m = memberBounds[i];
        out[i] = { m.x - aiPadding, m.y - aiRegionPadTop, 0, 0 }; // the title's place, whatever follows
        if (m.w <= 0 || m.h <= 0)
            continue;
        bool lies = false;
        for (std::size_t j = 0; j < i && !lies; ++j)
            lies = rectsIntersect(m, memberBounds[j]);
        if (lies)
            continue;
        kept[i] = true;
        lo[i] = { m.x - aiPadding, m.y - aiRegionPadTop };
        hi[i] = { m.x + m.w + aiPadding, m.y + m.h + aiPadding };
    }

    auto const mlo = [&](std::size_t i, int axis) { return axis == 0 ? memberBounds[i].x : memberBounds[i].y; };
    auto const mhi = [&](std::size_t i, int axis) { return axis == 0 ? memberBounds[i].x + memberBounds[i].w : memberBounds[i].y + memberBounds[i].h; };

    for (std::size_t i = 0; i < n; ++i) {
        if (!kept[i])
            continue;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!kept[j])
                continue;
            // The axis the members are best separated on (they never overlap; touching = gap 0).
            int const gapX = std::max(mlo(j, 0) - mhi(i, 0), mlo(i, 0) - mhi(j, 0));
            int const gapY = std::max(mlo(j, 1) - mhi(i, 1), mlo(i, 1) - mhi(j, 1));
            int const a = gapX >= gapY ? 0 : 1;
            // Two rectangles that are 4 px or more apart on either axis do not meet (negative = they overlap there).
            int const sepX = std::max(lo[j][0] - hi[i][0], lo[i][0] - hi[j][0]);
            int const sepY = std::max(lo[j][1] - hi[i][1], lo[i][1] - hi[j][1]);
            if (sepX >= aiRegionGap || sepY >= aiRegionGap)
                continue;
            std::size_t const first = mhi(i, a) <= mlo(j, a) ? i : j;
            std::size_t const second = first == i ? j : i;
            int const g = mlo(second, a) - mhi(first, a); // the free band between the two member sets
            if (a == 0) {
                // Side by side: the free band is split in the middle, the gap in the middle of it.
                int newFirstHi = g >= aiRegionGap ? std::min(hi[first][a], mhi(first, a) + (g - aiRegionGap) / 2) : mhi(first, a);
                newFirstHi = std::max(newFirstHi, mhi(first, a));
                int newSecondLo = std::max(lo[second][a], newFirstHi + aiRegionGap);
                newSecondLo = std::min(newSecondLo, mlo(second, a));
                hi[first][a] = newFirstHi;
                lo[second][a] = newSecondLo;
            } else {
                // One above the other: the squeeze is shared evenly (live review 2026-11-19: the lower one used to keep its
                // whole title room and leave the upper rectangle flush on its last box). `below` is the air under the
                // upper members as it stands, `above` the title room over the lower ones; together with the gap they need
                // more than the band g holds by `need`: the upper one gives half, the lower one the rest. The upper one
                // keeps aiRegionMinBelow (8 px) under its members when the band holds 8 + the gap; the lower one gets
                // what is left for its title (never a negative room, never a cut member).
                int const below = hi[first][a] - mhi(first, a);
                int const above = mlo(second, a) - lo[second][a];
                int const need = below + above + aiRegionGap - g;
                if (need > 0) {
                    int const floorBelow = std::min(below, std::min(aiRegionMinBelow, std::max(0, g - aiRegionGap)));
                    int const newBelow = std::max(below - need / 2, floorBelow);
                    int const newAbove = std::max(0, std::min(above, g - aiRegionGap - newBelow));
                    hi[first][a] = mhi(first, a) + newBelow;
                    lo[second][a] = mlo(second, a) - newAbove;
                }
            }
        }
    }

    for (std::size_t i = 0; i < n; ++i)
        if (kept[i])
            out[i] = { lo[i][0], lo[i][1], hi[i][0] - lo[i][0], hi[i][1] - lo[i][1] };
    return out;
}

// ---------------------------------------------------------------------------------------------------------- card

/** The one mark on the card at the bottom of the canvas (the same shapes as the tags). */
enum class AiCardMark { None, Ring, RedDot, BlueSquare, YellowTriangle };

/** The mark for a card's title, decided by its FIRST word (case and a leading blank or trailing colon do not matter):
 *  - sent, waiting, thinking, working (the server's chapter words while the AI has not answered): an empty ring;
 *  - built (the AI says it changed the patch): a red dot;
 *  - measured (the AI says it measured): a blue square. Only the AI's own title says this, with a reading in the same
 *    turn; the software never infers it;
 *  - silent, error, denied: a yellow triangle (look here);
 *  - anything else (DONE, MODE, CHAT, BACK, KEEP, BAG, SONG, VOICE, FALLBACK, a free text): no mark. A bare DONE
 *    claims neither a change nor a measurement. */
inline AiCardMark aiCardMark(std::string_view title)
{
    std::size_t begin = 0;
    while (begin < title.size() && std::isspace(static_cast<unsigned char>(title[begin])))
        ++begin;
    std::size_t end = begin;
    while (end < title.size() && std::isalpha(static_cast<unsigned char>(title[end])))
        ++end;
    auto const word = aistyle_detail::lower(title.substr(begin, end - begin));
    if (word == "sent" || word == "waiting" || word == "thinking" || word == "working")
        return AiCardMark::Ring;
    if (word == "built")
        return AiCardMark::RedDot;
    if (word == "measured")
        return AiCardMark::BlueSquare;
    if (word == "silent" || word == "error" || word == "denied")
        return AiCardMark::YellowTriangle;
    return AiCardMark::None;
}

} // namespace mcp
