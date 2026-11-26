// The docked selection pill (night/pill-docked): the parts that need neither Pd nor JUCE (Source/Mcp/McpPillDock.h).
// A widget's chip sends `ai-ask focus <widget> <rev> <ptr> [<x> <y> <w> <h> [subject <text...>]]`. The rect is the widget-local strip
// (unzoomed pixels) the pill docks in; the subject is the short label of its first chip ("3 notes", "the scene"). Both are optional and
// forgiving: a rect that does not parse or is out of bounds means the old, floating pill (and no subject), never a refused focus.
//  - pillDockParse: the atoms after the pointer -> a PillDock (rect checked and rounded, subject cleaned to printable ASCII, <= 24 chars);
//  - pillDockLayout: where the pill and its text column go on the canvas, and how it grows DOWNWARD when the text wraps (anchored at the
//    strip's top-left, never above it), then shrinks back;
//  - the chip widths of the row (dot, chips joined by "+").
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpPillDock.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

using mcp::DockAtom;
using mcp::PillDock;

namespace {

DockAtom num(double v)
{
    DockAtom a;
    a.type = DockAtom::Type::Float;
    a.num = v;
    return a;
}

DockAtom sym(char const* s)
{
    DockAtom a;
    a.type = DockAtom::Type::Symbol;
    a.sym = s;
    return a;
}

PillDock parse(std::vector<DockAtom> const& atoms)
{
    return mcp::pillDockParse(static_cast<int>(atoms.size()), atoms.data());
}

void rectParses()
{
    // the C1 strip of ai-midi5: x 14, y 293, w 432, h 24
    PillDock d = parse({ num(14), num(293), num(432), num(24) });
    CHECK(d.docked && d.x == 14 && d.y == 293 && d.w == 432 && d.h == 24);
    CHECK(!d.hasSubject());
    // pixels are whole: fractions round
    d = parse({ num(14.4), num(292.6), num(431.5), num(24.49) });
    CHECK(d.docked && d.x == 14 && d.y == 293 && d.w == 432 && d.h == 24);
    // nothing at all, or fewer than four numbers: the old pill
    CHECK(!mcp::pillDockParse(0, nullptr).docked);
    CHECK(!parse({ num(14), num(293), num(432) }).docked);
    CHECK(!parse({ num(14) }).docked);
    // a symbol among the four, or a symbol where a number belongs
    CHECK(!parse({ num(14), sym("y"), num(432), num(24) }).docked);
    CHECK(!parse({ sym("subject"), sym("3 notes") }).docked);
    // out of bounds: negative position, a pill narrower than a chip, a flat one, an absurd one, NaN, infinity
    CHECK(!parse({ num(-1), num(293), num(432), num(24) }).docked);
    CHECK(!parse({ num(14), num(-1), num(432), num(24) }).docked);
    CHECK(!parse({ num(14), num(293), num(39), num(24) }).docked);
    CHECK(!parse({ num(14), num(293), num(432), num(11) }).docked);
    CHECK(!parse({ num(14), num(293), num(4097), num(24) }).docked);
    CHECK(!parse({ num(14), num(293), num(432), num(513) }).docked);
    CHECK(!parse({ num(8193), num(293), num(432), num(24) }).docked);
    CHECK(!parse({ num(std::nan("")), num(293), num(432), num(24) }).docked);
    CHECK(!parse({ num(14), num(std::numeric_limits<double>::infinity()), num(432), num(24) }).docked);
    // the edges themselves are fine
    CHECK(parse({ num(0), num(0), num(40), num(12) }).docked);
    CHECK(parse({ num(8192), num(8192), num(4096), num(512) }).docked);
    // the first four atoms are the rect; extra numbers are dropped, they are no subject
    d = parse({ num(14), num(293), num(432), num(24), num(7), num(8) });
    CHECK(d.docked && !d.hasSubject());
}

void subjectParses()
{
    // one symbol with a space (a Lua string "3 notes"), or the words as the atoms Pd made of them (3 is a float)
    PillDock d = parse({ num(14), num(293), num(432), num(24), sym("subject"), sym("3 notes") });
    CHECK(d.docked && std::strcmp(d.subject, "3 notes") == 0);
    d = parse({ num(14), num(293), num(432), num(24), sym("subject"), num(3), sym("notes") });
    CHECK(d.docked && std::strcmp(d.subject, "3 notes") == 0);
    d = parse({ num(14), num(293), num(432), num(24), sym("subject"), sym("the"), sym("scene") });
    CHECK(std::strcmp(d.subject, "the scene") == 0);
    d = parse({ num(14), num(293), num(432), num(24), sym("subject"), sym("Alphi's"), sym("line") });
    CHECK(std::strcmp(d.subject, "Alphi's line") == 0);
    d = parse({ num(14), num(293), num(432), num(24), sym("subject"), num(2.5), sym("beats") });
    CHECK(std::strcmp(d.subject, "2.5 beats") == 0);
    // a word that is not `subject` after the rect: no subject (and the rect still docks)
    d = parse({ num(14), num(293), num(432), num(24), sym("label"), sym("3 notes") });
    CHECK(d.docked && !d.hasSubject());
    // `subject` and nothing after it: no chip
    d = parse({ num(14), num(293), num(432), num(24), sym("subject") });
    CHECK(d.docked && !d.hasSubject());
    // a subject without a usable rect is dropped with it: the old pill, no chip
    d = parse({ num(14), num(293), num(30), num(24), sym("subject"), sym("3 notes") });
    CHECK(!d.docked && !d.hasSubject());
}

void subjectIsCleaned()
{
    // control characters and anything outside printable ASCII are dropped, runs of blanks become one, the ends are trimmed
    PillDock d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("  3\t notes \n") });
    CHECK(std::strcmp(d.subject, "3 notes") == 0);
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("caf\xc3\xa9 line") });
    CHECK(std::strcmp(d.subject, "caf line") == 0);
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("\x01\x02\x7f") });
    CHECK(!d.hasSubject());
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("   ") });
    CHECK(!d.hasSubject());
    // cut at 24 characters, no trailing blank left by the cut, never overrun
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("abcdefghijklmnopqrstuvwxyz0123456789") });
    CHECK(static_cast<int>(std::strlen(d.subject)) == mcp::kPillSubjectChars);
    CHECK(std::strcmp(d.subject, "abcdefghijklmnopqrstuvwx") == 0);
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), sym("abcdefghijklmnopqrstuvw xyz") });
    CHECK(std::strcmp(d.subject, "abcdefghijklmnopqrstuvw") == 0); // the cut falls on the blank
    // many words: joined until the room is full
    std::vector<DockAtom> many { num(0), num(0), num(100), num(24), sym("subject") };
    for (int i = 0; i < 40; ++i)
        many.push_back(sym("word"));
    d = parse(many);
    CHECK(static_cast<int>(std::strlen(d.subject)) <= mcp::kPillSubjectChars);
    CHECK(std::strncmp(d.subject, "word word word", 14) == 0);
    // a null symbol pointer is an empty word, not a crash
    DockAtom nul;
    nul.type = DockAtom::Type::Symbol;
    nul.sym = nullptr;
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), nul, sym("ok") });
    CHECK(std::strcmp(d.subject, "ok") == 0);
    // a float that is not finite is skipped
    d = parse({ num(0), num(0), num(100), num(24), sym("subject"), num(std::nan("")), sym("ok") });
    CHECK(std::strcmp(d.subject, "ok") == 0);
}

void chipRowWidths()
{
    // the subject chip: 26 px of rim and note glyph plus the text, whole pixels
    CHECK(mcp::pillSubjectChipWidth(40.2f) == 66);
    CHECK(mcp::pillSubjectChipWidth(0.0f) == 26);
    // chips are joined by a small "+" (15 px each); no chips, no width
    int const one[] = { 66 };
    int const three[] = { 66, 16, 22 };
    CHECK(mcp::pillChipsWidth(0, nullptr) == 0);
    CHECK(mcp::pillChipsWidth(1, one) == 66);
    CHECK(mcp::pillChipsWidth(3, three) == 66 + 16 + 22 + 2 * mcp::kPillChipJoinPx);
}

void layoutOneLine()
{
    PillDock d = parse({ num(14), num(293), num(432), num(24) });
    // widget at canvas (100, 50): the pill sits at the widget's origin plus the strip, as wide as the strip, as high as the strip
    auto const l = mcp::pillDockLayout(100, 50, d, 66, 15, 15);
    CHECK(l.frame.x == 114 && l.frame.y == 343 && l.frame.w == 432 && l.frame.h == 24);
    // the text column starts after the dot, the chips and a small gap, runs to the right padding, is vertically centred in the strip
    int const left = mcp::kPillDockChipsX + 66 + mcp::kPillDockTextGap;
    CHECK(l.editor.x == 114 + left);
    CHECK(l.editor.w == 432 - left - mcp::kPillDockRightPad);
    CHECK(l.editor.h == 15 && l.editor.y == 343 + (24 - 15) / 2);
    CHECK(l.editor.x + l.editor.w <= l.frame.x + l.frame.w);
    // no chips: the text column starts right after the dot
    auto const bare = mcp::pillDockLayout(100, 50, d, 0, 15, 15);
    CHECK(bare.editor.x == 114 + mcp::kPillDockChipsX + mcp::kPillDockTextGap);
    // an empty field (text height 0) or a shorter one counts as one line
    CHECK(mcp::pillDockLayout(100, 50, d, 66, 0, 15).frame.h == 24);
    CHECK(mcp::pillDockLayout(100, 50, d, 66, 9, 15).frame.h == 24);
    CHECK(mcp::pillDockLayout(100, 50, d, 66, 0, 15).editor.h == 15);
}

void layoutGrowsDownward()
{
    PillDock d = parse({ num(14), num(293), num(432), num(24) });
    auto const one = mcp::pillDockLayout(100, 50, d, 66, 15, 15);
    auto const two = mcp::pillDockLayout(100, 50, d, 66, 30, 15);
    auto const three = mcp::pillDockLayout(100, 50, d, 66, 45, 15);
    // two lines: one line more room, and the top-left anchor never moves
    CHECK(two.frame.h == one.frame.h + 15 && three.frame.h == one.frame.h + 30);
    for (auto const* l : { &two, &three }) {
        CHECK(l->frame.x == one.frame.x && l->frame.y == one.frame.y && l->frame.w == one.frame.w);
        CHECK(l->editor.x == one.editor.x && l->editor.y == one.editor.y && l->editor.w == one.editor.w);
    }
    CHECK(two.editor.h == 30 && three.editor.h == 45);
    // the text column always sits inside the pill, whatever the wrap
    for (int textH = 0; textH <= 300; textH += 7) {
        auto const l = mcp::pillDockLayout(100, 50, d, 66, textH, 15);
        CHECK(l.editor.y >= l.frame.y && l.editor.y + l.editor.h <= l.frame.y + l.frame.h);
        CHECK(l.frame.y == 343); // never above the strip: the roll over it stays free
    }
    // growing is by the text only, so it shrinks back to the strip when the text is gone
    CHECK(mcp::pillDockLayout(100, 50, d, 66, 0, 15).frame.h == d.h);
    // a taller strip keeps its own height until the text needs more
    PillDock tall = parse({ num(0), num(0), num(300), num(40) });
    CHECK(mcp::pillDockLayout(0, 0, tall, 0, 15, 15).frame.h == 40);
    CHECK(mcp::pillDockLayout(0, 0, tall, 0, 45, 15).frame.h == 40 + 30);
    // the height is capped: a pasted page does not run the pill off the canvas
    auto const huge = mcp::pillDockLayout(0, 0, d, 66, 100000, 15);
    CHECK(huge.frame.h <= mcp::kPillDockMaxH);
    CHECK(huge.editor.y + huge.editor.h <= huge.frame.y + huge.frame.h);
}

void layoutNarrowStrip()
{
    PillDock d = parse({ num(0), num(0), num(100), num(24) });
    // chips that leave no room: the text column keeps a floor so the caret has somewhere to be
    auto const l = mcp::pillDockLayout(0, 0, d, 90, 15, 15);
    CHECK(l.editor.w >= mcp::kPillDockMinTextW);
    // the pill follows its widget: the same strip at another origin is the same shape shifted
    PillDock e = parse({ num(14), num(293), num(432), num(24) });
    auto const a = mcp::pillDockLayout(0, 0, e, 66, 30, 15);
    auto const b = mcp::pillDockLayout(-40, 25, e, 66, 30, 15);
    CHECK(b.frame.x == a.frame.x - 40 && b.frame.y == a.frame.y + 25 && b.frame.w == a.frame.w && b.frame.h == a.frame.h);
    CHECK(b.editor.x == a.editor.x - 40 && b.editor.y == a.editor.y + 25 && b.editor.w == a.editor.w && b.editor.h == a.editor.h);
}

} // namespace

int main()
{
    rectParses();
    subjectParses();
    subjectIsCleaned();
    chipRowWidths();
    layoutOneLine();
    layoutGrowsDownward();
    layoutNarrowStrip();
    if (failures) {
        std::printf("pill_dock_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pill_dock_test: ok\n");
    return 0;
}
