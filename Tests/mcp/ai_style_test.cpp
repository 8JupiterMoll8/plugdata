// Unit tests for the one AI style table (Source/Mcp/McpAiStyle.h), JUCE-free (design round 2026-11-19, step A):
//  - the palette and the sizes are the owner's numbers (picture "AI overlays, language A", grey regions);
//  - aiTagOf maps the kind words the server sends: the seven new words, the old ones (info -> guess, change -> changed,
//    warn -> look, artist -> none) and anything unknown or empty -> guess (an unmarked claim is never shown as measured);
//  - every tag glyph is 10 px or smaller (the order disc of the spotlight is its own, larger size);
//  - aiRegionRects: padded rectangles that never intersect, never cut a member box, keep a 4 px gap where the members
//    allow it, and give a region whose members lie on an earlier region's members an empty rectangle (title only);
//  - aiTagWidthPx is the one frame width the tag's paint and its hit test share;
//  - aiCardMark gives the card's mark for the chapter words the server really sends (SENT, WAITING, THINKING, WORKING,
//    DONE, ERROR, ...) and for the AI's own titles (built, measured, silent); anything else: no mark.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAiStyle.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::AiCardMark;
using mcp::AiRgba;
using mcp::AiTag;
using mcp::ApprovalRect;

namespace {

bool same(AiRgba c, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    return c.r == r && c.g == g && c.b == b && c.a == a;
}

void paletteIsTheOwnersNumbers()
{
    CHECK(same(mcp::aiCream, 0xEC, 0xE5, 0xD3));
    CHECK(same(mcp::aiInk, 0x1A, 0x1A, 0x1A));
    CHECK(same(mcp::aiGrey, 0x8C, 0x85, 0x76));
    CHECK(same(mcp::aiRed, 0xDC, 0x29, 0x1F));
    CHECK(same(mcp::aiYellow, 0xF7, 0xC2, 0x14));
    CHECK(same(mcp::aiBlue, 0x14, 0x3D, 0x9E));
    CHECK(same(mcp::aiNoteYellow, 0xF6, 0xE7, 0xA6));
    // The region tint is the grey at 14 % (36 of 255).
    CHECK(mcp::aiRegionTint.r == mcp::aiGrey.r && mcp::aiRegionTint.g == mcp::aiGrey.g && mcp::aiRegionTint.b == mcp::aiGrey.b);
    CHECK(mcp::aiRegionTint.a == 36);
    // Opaque on purpose: the colours never carry their own alpha, a caller scales it.
    CHECK(mcp::aiCream.a == 255 && mcp::aiInk.a == 255 && mcp::aiGrey.a == 255);
}

void sizesAreTheOwnersNumbers()
{
    CHECK(mcp::aiLineWidth == 1.0f);
    CHECK(mcp::aiLineWidthActive == 2.0f);
    CHECK(mcp::aiRadius == 2.0f);
    CHECK(mcp::aiHeaderHeight == 24);
    CHECK(mcp::aiPadding == 12);
    CHECK(mcp::aiNameSize == 13.0f);
    CHECK(mcp::aiHintSize == 11.0f);
    CHECK(mcp::aiTagTextSize == 12.0f);
    CHECK(mcp::aiTagGlyphSize == 9.0f);
    CHECK(mcp::aiGhostAlpha == 0.6f);
    CHECK(mcp::aiRegionGap == 4);
}

void tagKindWords()
{
    CHECK(mcp::aiTagOf("fact") == AiTag::Fact);
    CHECK(mcp::aiTagOf("guess") == AiTag::Guess);
    CHECK(mcp::aiTagOf("changed") == AiTag::Changed);
    CHECK(mcp::aiTagOf("proposed") == AiTag::Proposed);
    CHECK(mcp::aiTagOf("look") == AiTag::Look);
    CHECK(mcp::aiTagOf("ask") == AiTag::Ask);
    CHECK(mcp::aiTagOf("step") == AiTag::Step);
    // The case of the word never matters.
    CHECK(mcp::aiTagOf("FACT") == AiTag::Fact);
    CHECK(mcp::aiTagOf("Changed") == AiTag::Changed);
}

void oldKindWordsMap()
{
    CHECK(mcp::aiTagOf("info") == AiTag::Guess);
    CHECK(mcp::aiTagOf("change") == AiTag::Changed);
    CHECK(mcp::aiTagOf("warn") == AiTag::Look);
    CHECK(mcp::aiTagOf("artist") == AiTag::None);
}

void unknownIsNeverMeasured()
{
    CHECK(mcp::aiTagOf("") == AiTag::Guess);
    CHECK(mcp::aiTagOf("measured") == AiTag::Guess); // not a kind word: a claim without its mark stays a guess
    CHECK(mcp::aiTagOf("banana") == AiTag::Guess);
    CHECK(mcp::aiTagOf("fact ") == AiTag::Guess); // no trimming: only the exact words count
    // The one way to Fact is the word itself.
    for (char const* w : { "info", "change", "warn", "artist", "", "x", "ok", "error", "debug" })
        CHECK(mcp::aiTagOf(w) != AiTag::Fact);
}

void glyphSizesAreSmall()
{
    for (AiTag t : { AiTag::Fact, AiTag::Guess, AiTag::Changed, AiTag::Proposed, AiTag::Look, AiTag::Ask })
        CHECK(mcp::aiGlyphSize(t) > 0.0f && mcp::aiGlyphSize(t) <= 10.0f);
    CHECK(mcp::aiGlyphSize(AiTag::None) == 0.0f);
    // The order disc of the spotlight carries a number: bigger than a tag glyph, still small.
    CHECK(mcp::aiGlyphSize(AiTag::Step) == mcp::aiStepDiscSize);
    CHECK(mcp::aiStepDiscSize > 10.0f && mcp::aiStepDiscSize <= 14.0f);
    CHECK(mcp::aiTagGlyphSize <= 10.0f);
}

void tagFrameIsOneFunction()
{
    CHECK(mcp::aiTagWidthPx(0) == mcp::aiTagGlyphSlotPx);
    CHECK(mcp::aiTagWidthPx(10) == mcp::aiTagGlyphSlotPx + 10 * mcp::aiTagCharPx);
    CHECK(mcp::aiTagWidthPx(11) > mcp::aiTagWidthPx(10));
    CHECK(mcp::aiTagGlyphSlotPx == 16.0f && mcp::aiTagCharPx == 6.4f && mcp::aiTagHeightPx == 18.0f);
}

// What the region rectangles must always be.
bool inside(ApprovalRect outer, ApprovalRect inner)
{
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

bool isEmpty(ApprovalRect r) { return r.w <= 0 || r.h <= 0; }

// The two properties the owner cares about, for any input: no two rectangles cross, none cuts a member box of the
// others. `members` = the input; `rects` = the output; `emptied` = the indexes expected to have no rectangle.
void expectSane(std::vector<ApprovalRect> const& members, std::vector<ApprovalRect> const& rects, int line)
{
    if (rects.size() != members.size()) {
        std::printf("  FAIL line %d: %zu rectangles for %zu regions\n", line, rects.size(), members.size());
        ++failures;
        return;
    }
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (isEmpty(rects[i]))
            continue;
        if (!inside(rects[i], members[i])) {
            std::printf("  FAIL line %d: rectangle %zu does not hold its own members\n", line, i);
            ++failures;
        }
        for (std::size_t j = 0; j < rects.size(); ++j) {
            if (i == j || isEmpty(rects[j]))
                continue;
            if (i < j && mcp::rectsIntersect(rects[i], rects[j])) {
                std::printf("  FAIL line %d: rectangles %zu and %zu cross\n", line, i, j);
                ++failures;
            }
            if (mcp::rectsIntersect(rects[i], members[j])) {
                std::printf("  FAIL line %d: rectangle %zu cuts the members of region %zu\n", line, i, j);
                ++failures;
            }
        }
    }
}

void paddedAndApart()
{
    // The picture's columns: far apart, each just its members plus the padding (12 on three sides, 28 on top).
    std::vector<ApprovalRect> const members { { 80, 200, 110, 380 }, { 400, 200, 100, 420 }, { 740, 200, 110, 330 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(r.size() == 3);
    CHECK(r[0].x == 68 && r[0].y == 172 && r[0].w == 134 && r[0].h == 420);
    CHECK(r[1].x == 388 && r[1].y == 172 && r[1].w == 124 && r[1].h == 460);
    CHECK(r[2].x == 728 && r[2].y == 172 && r[2].w == 134 && r[2].h == 370);
    expectSane(members, r, __LINE__);
}

void sideBySideGetsTheGap()
{
    // Members 10 apart: the padded rectangles meet, both shrink toward their members to a 4 px gap.
    std::vector<ApprovalRect> const members { { 0, 0, 100, 100 }, { 110, 0, 100, 100 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(!isEmpty(r[0]) && !isEmpty(r[1]));
    CHECK(r[1].x - (r[0].x + r[0].w) == mcp::aiRegionGap);
    CHECK(r[0].x + r[0].w >= 100 && r[1].x <= 110); // neither cuts a member
    CHECK(r[0].x == -12 && r[1].x + r[1].w == 222);  // the outer edges keep their padding
    CHECK(r[0].y == -28 && r[0].h == 140 && r[1].y == -28 && r[1].h == 140);
    expectSane(members, r, __LINE__);
}

void membersCloserThanTheGap()
{
    // 2 px between the members: the gap cannot be 4 without cutting a member; the rectangles touch the members, not cross.
    std::vector<ApprovalRect> const members { { 0, 0, 100, 100 }, { 102, 0, 100, 100 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(r[0].x + r[0].w == 100 && r[1].x == 102);
    expectSane(members, r, __LINE__);
    // Touching members: the rectangles meet on one edge, no area shared.
    std::vector<ApprovalRect> const touching { { 0, 0, 100, 100 }, { 100, 0, 100, 100 } };
    auto const t = mcp::aiRegionRects(touching);
    CHECK(!isEmpty(t[0]) && !isEmpty(t[1]));
    CHECK(t[0].x + t[0].w == 100 && t[1].x == 100);
    expectSane(touching, t, __LINE__);
}

void stackedKeepsTheLowerTitle()
{
    // A region below another keeps the room for its title as long as the space between the members allows it.
    std::vector<ApprovalRect> const members { { 0, 100, 100, 50 }, { 0, 190, 100, 50 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(r[1].y == 190 - 28); // the lower one's title room is whole
    CHECK(r[1].y - (r[0].y + r[0].h) == mcp::aiRegionGap);
    CHECK(r[0].y + r[0].h >= 150); // the upper one still holds its members
    expectSane(members, r, __LINE__);
    // Less space: the lower region's title room is cut, never its members, and the rectangles still do not cross.
    std::vector<ApprovalRect> const tight { { 0, 100, 100, 50 }, { 0, 170, 100, 50 } };
    auto const t = mcp::aiRegionRects(tight);
    CHECK(t[1].y <= 170 && t[0].y + t[0].h >= 150);
    CHECK(t[1].y - (t[0].y + t[0].h) >= mcp::aiRegionGap);
    expectSane(tight, t, __LINE__);
}

void diagonalNeighbours()
{
    std::vector<ApprovalRect> const members { { 0, 0, 100, 100 }, { 110, 110, 100, 100 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(!isEmpty(r[0]) && !isEmpty(r[1]));
    expectSane(members, r, __LINE__);
}

void overlappingMembersGetNoRectangle()
{
    // The later region's members lie on the earlier one's: the later one is title only (an empty rectangle at the place
    // its rectangle would start), the earlier one keeps its whole padded rectangle.
    std::vector<ApprovalRect> const members { { 0, 0, 100, 100 }, { 50, 50, 100, 100 }, { 300, 0, 40, 40 } };
    auto const r = mcp::aiRegionRects(members);
    CHECK(r[0].x == -12 && r[0].y == -28 && r[0].w == 124 && r[0].h == 140);
    CHECK(isEmpty(r[1]));
    CHECK(r[1].x == 50 - 12 && r[1].y == 50 - 28); // where the title goes
    CHECK(!isEmpty(r[2]));
    expectSane(members, r, __LINE__);
    // Identical member sets: the second region is title only too.
    std::vector<ApprovalRect> const twins { { 10, 10, 60, 60 }, { 10, 10, 60, 60 } };
    auto const t = mcp::aiRegionRects(twins);
    CHECK(!isEmpty(t[0]) && isEmpty(t[1]));
    // A region nested in an earlier one: title only as well.
    std::vector<ApprovalRect> const nested { { 0, 0, 200, 200 }, { 50, 50, 20, 20 } };
    auto const n = mcp::aiRegionRects(nested);
    CHECK(!isEmpty(n[0]) && isEmpty(n[1]));
    // Every earlier region counts, an emptied one too: a third region on the second one's members is title only (its
    // members lie on member boxes that are drawn, tinted or not).
    std::vector<ApprovalRect> const chain { { 0, 0, 100, 100 }, { 80, 0, 100, 100 }, { 170, 0, 100, 100 } };
    auto const c = mcp::aiRegionRects(chain);
    CHECK(!isEmpty(c[0]) && isEmpty(c[1]) && isEmpty(c[2]));
}

void oddInput()
{
    CHECK(mcp::aiRegionRects({}).empty());
    // A member rectangle without area has no rectangle.
    auto const r = mcp::aiRegionRects({ { 10, 10, 0, 40 }, { 200, 10, 50, 50 } });
    CHECK(isEmpty(r[0]) && !isEmpty(r[1]));
    // One region: padded, nothing to resolve.
    auto const one = mcp::aiRegionRects({ { 5, 5, 20, 20 } });
    CHECK(one.size() == 1 && one[0].x == -7 && one[0].y == -23 && one[0].w == 44 && one[0].h == 60);
}

// A small deterministic generator: the property test needs no <random> state of the platform.
struct Lcg {
    std::uint32_t s;
    int next(int n)
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<int>((s >> 8) % static_cast<std::uint32_t>(n));
    }
};

void anyLayoutIsSane()
{
    Lcg rng { 12345u };
    for (int round = 0; round < 400; ++round) {
        int const n = 1 + rng.next(7);
        std::vector<ApprovalRect> members;
        for (int i = 0; i < n; ++i)
            members.push_back({ rng.next(600), rng.next(400), 20 + rng.next(120), 20 + rng.next(120) });
        auto const r = mcp::aiRegionRects(members);
        // Regions that kept a rectangle are those whose members touch no earlier region's members.
        std::vector<ApprovalRect> keptMembers, keptRects;
        for (std::size_t i = 0; i < members.size(); ++i) {
            bool lies = false;
            for (std::size_t j = 0; j < i; ++j)
                if (mcp::rectsIntersect(members[i], members[j]))
                    lies = true;
            CHECK(lies == isEmpty(r[i]));
            if (!isEmpty(r[i])) {
                keptMembers.push_back(members[i]);
                keptRects.push_back(r[i]);
            }
        }
        expectSane(keptMembers, keptRects, 1000 + round);
        // Where the members leave 4 px or more, the gap between two rectangles side by side is at least 4.
        for (std::size_t i = 0; i < keptRects.size(); ++i)
            for (std::size_t j = i + 1; j < keptRects.size(); ++j) {
                auto const a = keptRects[i];
                auto const b = keptRects[j];
                auto const ma = keptMembers[i];
                auto const mb = keptMembers[j];
                bool const sharesRows = std::min(a.y + a.h, b.y + b.h) > std::max(a.y, b.y);
                bool const sharesCols = std::min(a.x + a.w, b.x + b.w) > std::max(a.x, b.x);
                if (sharesRows && ma.x + ma.w + mcp::aiRegionGap <= mb.x)
                    CHECK(b.x - (a.x + a.w) >= mcp::aiRegionGap);
                if (sharesRows && mb.x + mb.w + mcp::aiRegionGap <= ma.x)
                    CHECK(a.x - (b.x + b.w) >= mcp::aiRegionGap);
                if (sharesCols && ma.y + ma.h + mcp::aiRegionGap <= mb.y)
                    CHECK(b.y - (a.y + a.h) >= mcp::aiRegionGap);
                if (sharesCols && mb.y + mb.h + mcp::aiRegionGap <= ma.y)
                    CHECK(a.y - (b.y + b.h) >= mcp::aiRegionGap);
            }
    }
}

void cardMarks()
{
    // The chapter words the server sends (src/services/*): the card says "asked / waiting" with an empty ring.
    for (char const* w : { "SENT", "Sent to Claude Code", "WAITING", "THINKING", "WORKING" })
        CHECK(mcp::aiCardMark(w) == AiCardMark::Ring);
    // The AI's own titles on a card.
    CHECK(mcp::aiCardMark("Built: bass") == AiCardMark::RedDot);
    CHECK(mcp::aiCardMark("BUILT") == AiCardMark::RedDot);
    CHECK(mcp::aiCardMark("Measured: it plays") == AiCardMark::BlueSquare);
    CHECK(mcp::aiCardMark("Silent: nothing reaches the speaker") == AiCardMark::YellowTriangle);
    CHECK(mcp::aiCardMark("ERROR") == AiCardMark::YellowTriangle);
    CHECK(mcp::aiCardMark("DENIED") == AiCardMark::YellowTriangle);
    // Anything else: no mark. In particular a bare DONE claims neither a change nor a measurement.
    for (char const* w : { "DONE", "MODE", "CHAT", "BACK", "KEEP", "BAG", "SONG", "VOICE", "FALLBACK", "", "hello", "  " })
        CHECK(mcp::aiCardMark(w) == AiCardMark::None);
    // Only the first word decides: a word inside a title is not a mark.
    CHECK(mcp::aiCardMark("MODE sent") == AiCardMark::None);
    CHECK(mcp::aiCardMark("done: built it") == AiCardMark::None);
    // Leading blanks and a trailing colon do not matter.
    CHECK(mcp::aiCardMark("  measured:") == AiCardMark::BlueSquare);
}

} // namespace

int main()
{
    paletteIsTheOwnersNumbers();
    sizesAreTheOwnersNumbers();
    tagKindWords();
    oldKindWordsMap();
    unknownIsNeverMeasured();
    glyphSizesAreSmall();
    tagFrameIsOneFunction();
    paddedAndApart();
    sideBySideGetsTheGap();
    membersCloserThanTheGap();
    stackedKeepsTheLowerTitle();
    diagonalNeighbours();
    overlappingMembersGetNoRectangle();
    oddInput();
    anyLayoutIsSane();
    cardMarks();
    std::printf(failures ? "ai_style_test: %d FAILED\n" : "ai_style_test: OK\n", failures);
    return failures ? 1 : 0;
}
