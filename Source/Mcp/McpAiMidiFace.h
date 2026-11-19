// The face of [ai.midi] in the Bauhaus family look: where everything sits, what the take looks like as ink blocks, and the words.
// Header-only and JUCE-free: no Pd, no JUCE (Tests/mcp/ai_midi_face_test.cpp). AiMidiObject.h draws with these numbers and
// hit-tests the buttons with the very same rectangles, so what is drawn and what answers a click cannot drift apart.
//
// The family rules (owner, 2026-11-19; memory ai-overlays-bauhaus-calm): black on cream; cream ECE5D3, ink 1A1A1A, grey 8C8576,
// red DC291F only where it means "recording"; 1 px lines, 2 px only for the active thing (here: the playhead), 2 px radius; a 24 px
// header strip top-left (the name 13 px medium, then a grey hint); 12 px padding; buttons are 1 px outlined lower-case words,
// "on" is inverted (ink fill, cream text); the AI chip sits bottom-right; no glow, no gradient.
//
//   default 240 x 64 (the object's own default size)         taller faces (h >= 80) only get more air
//   +--------------------------------------------------+     0
//   | ai.midi  [dot] recording, 12 notes      120 bpm  |     header strip, 24 px
//   |                                                  |
//   |  7    ||  | ||   |  |||                          |     take as ink blocks (a tiny piano roll, one bar = a grid cell)
//   |       [rec][play][clear][sound]          [AI...] |     buttons; the AI chip at the bottom right
//   +--------------------------------------------------+     h
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "McpAiMidiVoice.h" // kAiMidiDefaultNoteMs (a note nobody ends), kAiMidiMaxNoteMs
#include "McpAiStyle.h"      // the family's one style table (colours and sizes)

namespace mcp {

// The values are the family's one style table (McpAiStyle.h): cream aiCream, ink aiInk, grey aiGrey, red aiRed.
constexpr int kAiMidiHeaderH = aiHeaderHeight;
constexpr int kAiMidiPad = aiPadding;
constexpr int kAiMidiGap = 4;        // between two buttons
constexpr float kAiMidiNameSize = aiNameSize;
constexpr float kAiMidiHintSize = aiHintSize;
constexpr float kAiMidiRadius = aiRadius;
constexpr float kAiMidiLine = aiLineWidth;
constexpr float kAiMidiActiveLine = aiLineWidthActive;

struct AiMidiRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// The same order as AiMidiObject::Button.
enum AiMidiButtonId { kAiMidiRec = 0, kAiMidiPlay, kAiMidiClear, kAiMidiAi, kAiMidiSound, kAiMidiNumButtons };

struct AiMidiFace {
    AiMidiRect header;
    AiMidiRect numeral;    // the big take number, left column
    float numeralSize = 24.0f;
    AiMidiRect take;       // the ink blocks; empty when there is no room
    AiMidiRect button[kAiMidiNumButtons];
};

namespace face_detail {
// Natural width of a button at 11 px text: the word plus 6 px each side. The chip is "AI..." and goes to the right edge.
constexpr int kNatural[kAiMidiNumButtons] = { 24, 30, 36, 28, 38 }; // rec play clear ai sound
constexpr int kMinButtonW = 12;
inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace face_detail

// Every rectangle of the face for a widget of w x h pixels. Pure integer geometry; nothing leaves [0,w] x [0,h].
inline AiMidiFace aiMidiFaceLayout(int w, int h)
{
    using face_detail::clampInt;
    AiMidiFace f;
    w = std::max(w, 1);
    h = std::max(h, 1);
    f.header = { 0, 0, w, std::min(kAiMidiHeaderH, h) };

    bool const tall = h >= 80;
    int const bottomPad = tall ? 12 : (h >= 60 ? 8 : 6); // the default 64 px high face cannot afford the full 12
    int btnH = tall ? 20 : 16;
    int btnY = h - bottomPad - btnH;
    int const minBtnY = kAiMidiHeaderH + 2;
    if (btnY < minBtnY) { // a very low widget: the buttons keep what is left under the header
        btnY = std::min(minBtnY, h - 1);
        btnH = std::max(1, std::min(btnH, h - btnY - 2));
    }

    // Left column: the take number, large and plain. Hidden when the widget is too narrow for it.
    int const bodyTop = kAiMidiHeaderH;
    int const bodyH = std::max(0, h - bottomPad - bodyTop);
    float const size = std::max(14.0f, std::min(34.0f, static_cast<float>(bodyH) * 0.8f));
    int const numW = w >= 190 ? static_cast<int>(std::ceil(size * 1.25f)) : 0;
    f.numeralSize = size;
    f.numeral = { kAiMidiPad, bodyTop, numW, bodyH };

    int const x0 = numW > 0 ? kAiMidiPad + numW + 10 : kAiMidiPad;
    int const x1 = std::max(x0 + 1, w - kAiMidiPad);

    // The take as ink blocks: the band between the header and the buttons.
    int const takeTop = bodyTop + (tall ? 4 : 2);
    int const takeBottom = btnY - (tall ? 8 : 4);
    if (takeBottom - takeTop >= 6)
        f.take = { x0, takeTop, x1 - x0, takeBottom - takeTop };

    // Buttons: rec play clear sound on the left in their natural widths, the AI chip at the right edge. Too narrow: all shrink alike.
    int const leftIds[4] = { kAiMidiRec, kAiMidiPlay, kAiMidiClear, kAiMidiSound };
    int natural = 0;
    for (int id : leftIds)
        natural += face_detail::kNatural[id];
    natural += face_detail::kNatural[kAiMidiAi];
    int const gaps = 4 * kAiMidiGap; // three between the four left buttons, one before the chip
    float const scale = std::min(1.0f, std::max(0.5f, static_cast<float>(x1 - x0 - gaps) / static_cast<float>(natural)));
    auto widthOf = [&](int id) { return std::max(face_detail::kMinButtonW, static_cast<int>(std::floor(static_cast<float>(face_detail::kNatural[id]) * scale))); };
    int x = x0;
    for (int id : leftIds) {
        int const bw = widthOf(id);
        f.button[id] = { x, btnY, bw, btnH };
        x += bw + kAiMidiGap;
    }
    int const chipW = widthOf(kAiMidiAi);
    f.button[kAiMidiAi] = { std::max(x, x1 - chipW), btnY, chipW, btnH };
    return f;
}

// ── The take as ink blocks ──────────────────────────────────────────────────────────────────────────────────────────

constexpr double kAiMidiLoopTailMs = 500.0; // the sequencer waits this long after the last entry before it loops
constexpr int kAiMidiMaxBlocks = 600;       // more entries than this are drawn at a stride

// The loop's length in ms: exactly when the audition sequencer wraps (last entry + 500 ms). 0 for an empty take.
template<class Note>
double aiMidiLoopMs(Note const* notes, int count)
{
    return (notes && count > 0) ? static_cast<double>(notes[count - 1].time_ms) + kAiMidiLoopTailMs : 0.0;
}

// The time the blocks' width covers: whole bars (4 beats each), at least one, long enough for the loop and for the recording head.
inline double aiMidiSpanMs(double loopMs, double headMs, double beatMs)
{
    double const barMs = std::max(1.0, beatMs * 4.0);
    double const need = std::max(std::max(loopMs, headMs), 0.0);
    double const bars = std::max(1.0, std::ceil(need / barMs - 1e-9));
    return bars * barMs;
}

struct AiMidiBlock {
    float x, y, w, h; // pixels, relative to the take region's top-left
};

// Calls emit(AiMidiBlock) for the take's notes: x by start time over `spanMs`, y by pitch (higher = higher up), width by length.
// A note-on lasts until the next entry of its pitch (the take's own off, or a re-strike); one nobody ends lasts its dur_ms (200 if
// unusable). One pass, no allocation. `Note` has pitch, vel, time_ms, dur_ms.
template<class Note, class Emit>
void aiMidiForEachBlock(Note const* notes, int count, double spanMs, int regionW, int regionH, Emit&& emit)
{
    if (!notes || count <= 0 || regionW < 2 || regionH < 2 || !(spanMs > 0.0))
        return;
    int lo = 127, hi = 0;
    for (int i = 0; i < count; ++i) {
        if (static_cast<int>(notes[i].vel) <= 0)
            continue;
        int const p = face_detail::clampInt(static_cast<int>(notes[i].pitch), 0, 127);
        lo = std::min(lo, p);
        hi = std::max(hi, p);
    }
    if (hi < lo)
        return;
    double dlo = lo, dhi = hi;
    if (dhi - dlo < 11.0) { // a short melody keeps a calm, centred band instead of stretching to the full height
        double const mid = (dlo + dhi) * 0.5;
        dlo = mid - 5.5;
        dhi = mid + 5.5;
    }
    float const bh = std::max(3.0f, std::min(8.0f, static_cast<float>(regionH) * 0.4f));
    int const stride = std::max(1, (count + kAiMidiMaxBlocks - 1) / kAiMidiMaxBlocks);

    double openAt[128];
    int openIdx[128];
    for (int p = 0; p < 128; ++p) {
        openAt[p] = 0.0;
        openIdx[p] = -1;
    }

    auto put = [&](int idx, double t0, double t1) {
        if (idx % stride != 0)
            return;
        int const p = face_detail::clampInt(static_cast<int>(notes[idx].pitch), 0, 127);
        float x = static_cast<float>(t0 / spanMs * regionW);
        x = std::max(0.0f, std::min(x, static_cast<float>(regionW) - 2.0f));
        float bw = static_cast<float>((t1 - t0) / spanMs * regionW) - 1.0f;
        bw = std::max(2.0f, std::min(bw, static_cast<float>(regionW) - x));
        float const y = (static_cast<float>(regionH) - bh) * static_cast<float>(1.0 - (static_cast<double>(p) - dlo) / (dhi - dlo));
        emit(AiMidiBlock { x, y, bw, bh });
    };
    auto closeAt = [&](int p, double t) { // the entry that started the open note of pitch p ends at t
        put(openIdx[p], openAt[p], std::max(t, openAt[p]));
        openIdx[p] = -1;
    };
    for (int i = 0; i < count; ++i) {
        int const p = face_detail::clampInt(static_cast<int>(notes[i].pitch), 0, 127);
        double const t = static_cast<double>(notes[i].time_ms);
        if (openIdx[p] >= 0)
            closeAt(p, t);
        if (static_cast<int>(notes[i].vel) > 0) {
            openIdx[p] = i;
            openAt[p] = t;
        }
    }
    for (int p = 0; p < 128; ++p) { // never ended: its own length
        if (openIdx[p] < 0)
            continue;
        double const dur = static_cast<double>(notes[openIdx[p]].dur_ms);
        bool const usable = std::isfinite(dur) && dur > 0.0 && dur <= kAiMidiMaxNoteMs;
        closeAt(p, openAt[p] + (usable ? dur : kAiMidiDefaultNoteMs));
    }
}

// x of the playhead / recording head in pixels (relative to the take region), clamped to the region.
inline float aiMidiHeadX(double headMs, double spanMs, int regionW)
{
    if (!(spanMs > 0.0) || regionW <= 0)
        return 0.0f;
    double const f = std::max(0.0, std::min(headMs / spanMs, 1.0));
    return static_cast<float>(f * (regionW - 1));
}

// ── The words ───────────────────────────────────────────────────────────────────────────────────────────────────────

// The take number as the face shows it: the digits after the last "_" of the id, modulo 100 ("take_7" -> 7, "take_105" -> 5).
inline int aiMidiTakeNumber(char const* takeId)
{
    if (!takeId)
        return 0;
    char const* start = takeId;
    for (char const* c = takeId; *c; ++c)
        if (*c == '_')
            start = c + 1;
    long v = 0;
    for (char const* c = start; *c >= '0' && *c <= '9'; ++c) {
        v = v * 10 + (*c - '0');
        if (v > 100000000L)
            break;
    }
    return static_cast<int>(v % 100);
}

// The grey hint after the name in the header strip (lower case, no shouting).
inline std::string aiMidiHintText(bool counting, bool recording, bool playing, int noteCount)
{
    std::string const notes = std::to_string(noteCount) + (noteCount == 1 ? " note" : " notes");
    if (counting)
        return "counting in";
    if (recording)
        return "recording, " + notes;
    if (playing)
        return "playing, " + notes;
    if (noteCount > 0)
        return notes;
    return "empty, press rec";
}

} // namespace mcp
