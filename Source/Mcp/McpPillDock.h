/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The docked selection pill (night/pill-docked): the parts that need neither Pd nor JUCE (unit-tested by Tests/mcp/pill_dock_test.cpp
// through scripts/test-bridge-safety.sh).
//
// The artist's picture (ai-midi5 v16, "the pill with references"): the pill opens DOCKED inside the widget's bottom strip, a white,
// fully rounded pill with a thin ink outline, a small ink dot, then CHIPS (the subject chip first: "3 notes"), then her words and
// the caret. A widget asks for it by adding a tail to its focus word (McpWidgetAsk.h):
//
//   ai-ask focus <widget> <rev> <ptr> <x> <y> <w> <h> [subject <text...>]
//
//   x y w h   the strip in WIDGET-LOCAL pixels, unzoomed (the canvas zoom is the canvas's own transform, so a canvas-space rect is the
//             widget's content origin plus these numbers and the zoom scales it on screen with everything else). Checked and rounded
//             (pillDockParse); a tail that does not parse means the old, floating pill, never a refused focus.
//   subject   the first chip's label ("3 notes", "the scene", "Alphi's line"). Words after `subject` are joined with single blanks,
//             cleaned to printable ASCII and cut at kPillSubjectChars. It is shown, never sent: no free text from a patch reaches
//             the AI's prompt (review M4); the chip is the artist's own subject label on her own screen.
//
// The pill GROWS DOWNWARD when the chips and the text do not fit the strip: the text column wraps and the pill gets one line of height
// per wrapped line, anchored at the strip's top-left (it never covers the roll above the strip), and shrinks back as the text goes
// (pillDockLayout).
//
// The receiver (MCPBridgeWidgetAsk.h) runs where Pd dispatches, the audio thread included: no lock, no allocation, no printf here.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcp {

constexpr int kPillSubjectChars = 24; // the subject chip's label, the terminating NUL not counted

// Bounds of the strip (widget-local pixels after rounding): a pill narrower than a chip or flatter than a line is no pill.
constexpr int kPillDockMaxPos = 8192;
constexpr int kPillDockMinW = 40;
constexpr int kPillDockMaxW = 4096;
constexpr int kPillDockMinH = 12;
constexpr int kPillDockMaxH = 160; // the pill never grows beyond this (a pasted page must not run it off the canvas); a taller strip keeps its own height
constexpr int kPillDockMaxStripH = 512;

// The look of the row (px; the picture's numbers): the dot sits at x 12, the first chip starts at 22, chips are joined by a "+" of 15 px.
constexpr int kPillDockChipsX = 22;
constexpr int kPillDockTextGap = 6;
constexpr int kPillDockRightPad = 10;
constexpr int kPillDockMinTextW = 40;
constexpr int kPillChipJoinPx = 15;
constexpr int kPillChipRimPx = 26; // the subject chip: its rim and the red note glyph, before the text

struct PillDock {
    bool docked = false; // the tail held a usable rect
    int x = 0, y = 0, w = 0, h = 0; // widget-local, unzoomed pixels
    char subject[kPillSubjectChars + 1] = {};
    bool hasSubject() const { return subject[0] != 0; }
};

// One atom of the focus word's tail, as the receiver reads it (a Float, a Symbol with its text, anything else).
struct DockAtom {
    enum class Type { Float, Symbol, Other };
    Type type = Type::Other;
    double num = 0.0;
    char const* sym = nullptr;
};

namespace pilldock_detail {

inline bool wholeRectAtom(DockAtom const& a, double& out)
{
    if (a.type != DockAtom::Type::Float || !std::isfinite(a.num)) return false;
    out = a.num;
    return true;
}

inline int roundPx(double v)
{
    return static_cast<int>(std::floor(v + 0.5));
}

// Builds the subject text one character at a time: printable ASCII kept, tab / newline / CR / blank are ONE blank (never first, never
// trailing), everything else dropped; stops at kPillSubjectChars.
struct SubjectBuilder {
    char* out;
    int len = 0;
    bool pendingBlank = false;
    bool stopped = false; // the room ran out (a blank that would be the last character ends the text, nothing follows it)

    bool full() const { return stopped || len >= kPillSubjectChars; }

    void put(unsigned char c)
    {
        if (full()) return;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pendingBlank = len > 0;
            return;
        }
        if (c < 0x21 || c > 0x7e) return; // control characters and anything outside ASCII
        if (pendingBlank) {
            if (len + 2 > kPillSubjectChars) { // blank + this character do not both fit: the text ends here, no trailing blank
                stopped = true;
                return;
            }
            out[len++] = ' ';
            pendingBlank = false;
        }
        out[len++] = static_cast<char>(c);
    }

    void word(char const* s)
    {
        if (!s) return;
        for (; *s && !full(); ++s)
            put(static_cast<unsigned char>(*s));
    }

    // A whole number, or up to two decimals (2.5, 0.25); anything else (not finite, huge) is skipped. No printf: this runs on the Pd dispatch path.
    void number(double v)
    {
        if (!std::isfinite(v) || std::fabs(v) >= 1e9) return;
        if (v < 0.0) put('-');
        double const a = std::fabs(v);
        auto whole = static_cast<std::int64_t>(a);
        auto frac = static_cast<int>(std::floor((a - static_cast<double>(whole)) * 100.0 + 0.5));
        if (frac >= 100) {
            ++whole;
            frac -= 100;
        }
        char digits[24];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + whole % 10);
            whole /= 10;
        } while (whole > 0 && n < 20);
        while (n > 0)
            put(static_cast<unsigned char>(digits[--n]));
        if (frac > 0) {
            put('.');
            put(static_cast<unsigned char>('0' + frac / 10));
            if (frac % 10) put(static_cast<unsigned char>('0' + frac % 10));
        }
    }

    void finish() { out[len] = 0; }
};

} // namespace pilldock_detail

// The atoms after `<ptr>` of a focus word: `<x> <y> <w> <h> [subject <text...>]`. docked = false (and no subject) when the four numbers are
// not all there, not numbers, or out of bounds: the old pill. Extra atoms past the rect that are not `subject <text...>` are dropped.
inline PillDock pillDockParse(int argc, DockAtom const* argv)
{
    PillDock d;
    if (argc < 4 || !argv) return d;
    double v[4] = {};
    for (int i = 0; i < 4; ++i)
        if (!pilldock_detail::wholeRectAtom(argv[i], v[i])) return d;
    // bounds on the doubles first (so a huge float never meets an int cast), then on the rounded pixels
    if (v[0] < 0.0 || v[1] < 0.0 || v[0] > kPillDockMaxPos + 0.5 || v[1] > kPillDockMaxPos + 0.5 || v[2] > kPillDockMaxW + 0.5
        || v[3] > kPillDockMaxStripH + 0.5)
        return d;
    int const x = pilldock_detail::roundPx(v[0]);
    int const y = pilldock_detail::roundPx(v[1]);
    int const w = pilldock_detail::roundPx(v[2]);
    int const h = pilldock_detail::roundPx(v[3]);
    if (x > kPillDockMaxPos || y > kPillDockMaxPos || w < kPillDockMinW || w > kPillDockMaxW || h < kPillDockMinH || h > kPillDockMaxStripH)
        return d;
    d.docked = true;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    if (argc > 4 && argv[4].type == DockAtom::Type::Symbol && argv[4].sym && std::strcmp(argv[4].sym, "subject") == 0) {
        pilldock_detail::SubjectBuilder sb { d.subject };
        for (int i = 5; i < argc && !sb.full(); ++i) {
            // a word is separated from the one before by a blank, whatever atom type it was
            if (argv[i].type == DockAtom::Type::Symbol && argv[i].sym) {
                sb.put(' ');
                sb.word(argv[i].sym);
            } else if (argv[i].type == DockAtom::Type::Float && std::isfinite(argv[i].num)) {
                sb.put(' ');
                sb.number(argv[i].num);
            }
        }
        sb.finish();
    }
    return d;
}

// -------------------------------------------------------------------------------------------------------- the chip row

// The subject chip's width: the rim and the red note glyph (kPillChipRimPx) plus the label's measured width, in whole pixels. The label is
// measured by whoever draws it (a font); this is the one place that adds the chip's own parts, so the chip and the room before the text agree.
inline int pillSubjectChipWidth(float textPx)
{
    return kPillChipRimPx + static_cast<int>(std::floor(textPx + 0.5f));
}

// The row's chips side by side, joined by a "+": the room they take after the dot. No chips, no room.
inline int pillChipsWidth(int count, int const* widths)
{
    if (count <= 0 || !widths) return 0;
    int total = 0;
    for (int i = 0; i < count; ++i)
        total += widths[i];
    return total + (count - 1) * kPillChipJoinPx;
}

// -------------------------------------------------------------------------------------------------------- the layout

struct PillRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct PillDockLayout {
    PillRect frame;  // the pill itself, canvas coordinates
    PillRect editor; // the text column inside it (the prompt field), canvas coordinates
};

// Where the docked pill and its text column go. `originX/Y`: the widget's content origin on the canvas (its top-left, what the widget-local
// numbers are measured from). `chipsW`: pillChipsWidth of the row. `textH`: the height the wrapped text needs in the column (0 = empty),
// `lineH`: one line of it. The pill is the strip while the text fits on one line; each further wrapped line adds one line of height DOWNWARD
// from the strip's top-left (the roll above is never covered); at kPillDockMaxH (or the strip's own height, if taller) it stops growing.
inline PillDockLayout pillDockLayout(int originX, int originY, PillDock const& d, int chipsW, int textH, int lineH)
{
    PillDockLayout l;
    lineH = std::max(1, lineH);
    int const wanted = std::max(textH, lineH);
    int const cap = std::max(d.h, kPillDockMaxH);
    l.frame = { originX + d.x, originY + d.y, d.w, std::min(cap, d.h + (wanted - lineH)) };
    int const left = kPillDockChipsX + std::max(0, chipsW) + kPillDockTextGap;
    int const top = std::max(0, (d.h - lineH) / 2);
    l.editor.x = l.frame.x + left;
    l.editor.y = l.frame.y + top;
    l.editor.w = std::max(kPillDockMinTextW, d.w - left - kPillDockRightPad);
    l.editor.h = std::max(1, std::min(wanted, l.frame.h - top));
    return l;
}

} // namespace mcp
