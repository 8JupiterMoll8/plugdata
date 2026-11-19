// Unit tests for the face of [ai.midi] in the Bauhaus family look (Source/Mcp/McpAiMidiFace.h), JUCE-free:
//  - at the object's default size (240 x 64) the header strip is 24 px, the buttons sit below it, the AI chip is the right-most
//    button at the 12 px padding, rec/play/clear/sound do not touch it, and the take blocks sit between header and buttons;
//  - for every size from the smallest the editor allows up to a large one, every rectangle stays inside the widget, the buttons never
//    overlap each other or the header, and the click frames are the drawn rectangles (one source);
//  - the ink blocks: a note lasts until the take's own off or a re-strike, a note nobody ends lasts its dur_ms (200 ms if unusable),
//    higher pitch sits higher, x grows with time, nothing leaves the region, a huge take is drawn at a stride;
//  - the span is whole bars and long enough for the loop and the recording head; the head is clamped to the region;
//  - the words: take number from the id, the hint for each state.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAiMidiFace.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

namespace {

struct Note {
    int pitch;
    int vel;
    float time_ms;
    float dur_ms;
};

bool inside(mcp::AiMidiRect const& r, int w, int h) { return r.x >= 0 && r.y >= 0 && r.right() <= w && r.bottom() <= h; }
bool overlap(mcp::AiMidiRect const& a, mcp::AiMidiRect const& b)
{
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

void defaultFaceAnatomy()
{
    auto const f = mcp::aiMidiFaceLayout(240, 64);
    CHECK(f.header.x == 0 && f.header.y == 0 && f.header.w == 240 && f.header.h == 24); // the 24 px header strip
    for (int i = 0; i < mcp::kAiMidiNumButtons; ++i) {
        CHECK(f.button[i].y >= 24);                       // under the header
        CHECK(f.button[i].h >= 16);                       // a click target a hand can hit
        CHECK(f.button[i].bottom() <= 64 - 8);            // the default face keeps 8 px at the bottom
        CHECK(f.button[i].y == f.button[0].y);            // one row
    }
    auto const& chip = f.button[mcp::kAiMidiAi];
    CHECK(chip.right() == 240 - 12);                      // the AI chip is bottom-right, at the 12 px padding
    for (int i = 0; i < mcp::kAiMidiNumButtons; ++i)
        CHECK(i == mcp::kAiMidiAi || f.button[i].right() < chip.x);
    CHECK(f.button[mcp::kAiMidiRec].x < f.button[mcp::kAiMidiPlay].x);
    CHECK(f.button[mcp::kAiMidiPlay].x < f.button[mcp::kAiMidiClear].x);
    CHECK(f.button[mcp::kAiMidiClear].x < f.button[mcp::kAiMidiSound].x);
    CHECK(!f.take.empty() && f.take.bottom() <= f.button[0].y && f.take.y >= 24); // blocks between header and buttons
    CHECK(f.numeral.x == 12 && !f.numeral.empty() && f.numeral.right() <= f.take.x);
    CHECK(f.take.right() == 240 - 12);
}

void tallFaceGetsFullPadding()
{
    auto const f = mcp::aiMidiFaceLayout(300, 120);
    CHECK(f.button[mcp::kAiMidiAi].bottom() == 120 - 12);
    CHECK(f.button[mcp::kAiMidiAi].right() == 300 - 12);
    CHECK(f.take.h >= 20 && f.take.bottom() <= f.button[0].y);
}

void everySizeStaysInsideAndClear()
{
    for (int w = 120; w <= 520; w += 7) {
        for (int h = 40; h <= 200; h += 3) {
            auto const f = mcp::aiMidiFaceLayout(w, h);
            CHECK(inside(f.header, w, h));
            for (int i = 0; i < mcp::kAiMidiNumButtons; ++i) {
                CHECK(inside(f.button[i], w, h));
                CHECK(f.button[i].w >= 12 && f.button[i].h >= 1);
                CHECK(!overlap(f.button[i], f.header));
                for (int j = i + 1; j < mcp::kAiMidiNumButtons; ++j)
                    CHECK(!overlap(f.button[i], f.button[j]));
            }
            if (!f.take.empty()) {
                CHECK(inside(f.take, w, h));
                CHECK(!overlap(f.take, f.header));
                for (int i = 0; i < mcp::kAiMidiNumButtons; ++i)
                    CHECK(!overlap(f.take, f.button[i]));
            }
            if (!f.numeral.empty()) {
                CHECK(inside(f.numeral, w, h));
                CHECK(!overlap(f.numeral, f.header));
                for (int i = 0; i < mcp::kAiMidiNumButtons; ++i)
                    CHECK(!overlap(f.numeral, f.button[i]));
            }
        }
    }
}

void theSupportedSizesKeepTheirBlocks()
{
    CHECK(!mcp::aiMidiFaceLayout(240, 64).take.empty());
    CHECK(!mcp::aiMidiFaceLayout(200, 64).take.empty());
    CHECK(mcp::aiMidiFaceLayout(240, 48).take.empty()); // too low for blocks: the buttons and the number stay
}

using Blocks = std::vector<mcp::AiMidiBlock>;

Blocks blocksOf(std::vector<Note> const& take, double span, int w = 172, int h = 10)
{
    Blocks out;
    mcp::aiMidiForEachBlock(take.data(), static_cast<int>(take.size()), span, w, h, [&](mcp::AiMidiBlock const& b) { out.push_back(b); });
    return out;
}

void blocksFollowTheTake()
{
    // C4 held 0..400 ms by its own off; E4 a bare note-on with the stamped 200 ms; G4 struck twice (the first one ends at the re-strike)
    std::vector<Note> const take {
        { 60, 100, 0, 200 }, { 60, 0, 400, 200 }, { 64, 100, 500, 200 }, { 67, 100, 600, 200 }, { 67, 100, 800, 200 },
    };
    double const span = 2000.0;
    auto const b = blocksOf(take, span);
    CHECK(b.size() == 4);
    // emitted in the order they close: C4, G4 (first strike), then the unended ones by pitch
    float const pxPerMs = 172.0f / 2000.0f;
    CHECK(std::fabs(b[0].x - 0.0f) < 0.01f && std::fabs(b[0].w - (400.0f * pxPerMs - 1.0f)) < 0.01f);
    CHECK(std::fabs(b[1].x - 600.0f * pxPerMs) < 0.01f && std::fabs(b[1].w - (200.0f * pxPerMs - 1.0f)) < 0.01f); // G4, ended by the re-strike
    for (auto const& blk : b) {
        CHECK(blk.x >= 0.0f && blk.x + blk.w <= 172.0f + 0.01f);
        CHECK(blk.y >= 0.0f && blk.y + blk.h <= 10.0f + 0.01f);
        CHECK(blk.w >= 2.0f && blk.h >= 3.0f);
    }
    // pitch order: the highest note sits highest (smallest y)
    float yC = 0, yE = 0, yG = 0;
    for (auto const& blk : b) {
        if (std::fabs(blk.x - 0.0f) < 0.01f)
            yC = blk.y;
        else if (std::fabs(blk.x - 500.0f * pxPerMs) < 0.01f)
            yE = blk.y;
        else if (std::fabs(blk.x - 800.0f * pxPerMs) < 0.01f)
            yG = blk.y;
    }
    CHECK(yG < yE && yE < yC);
}

void unusableLengthsFallBackTo200()
{
    std::vector<Note> const take { { 60, 100, 0, 0 }, { 62, 100, 1000, -5 }, { 64, 100, 1500, 1e9f } };
    auto const b = blocksOf(take, 2000.0);
    CHECK(b.size() == 3);
    for (auto const& blk : b)
        CHECK(std::fabs(blk.w - (200.0f / 2000.0f * 172.0f - 1.0f)) < 0.01f);
}

void emptyAndBrokenTakesDrawNothing()
{
    CHECK(blocksOf({}, 2000.0).empty());
    std::vector<Note> const offsOnly { { 60, 0, 0, 200 }, { 62, 0, 100, 200 } };
    CHECK(blocksOf(offsOnly, 2000.0).empty());
    std::vector<Note> const one { { 60, 100, 0, 200 } };
    CHECK(blocksOf(one, 0.0).empty());
    CHECK(blocksOf(one, 2000.0, 1, 10).empty());
    Blocks none;
    mcp::aiMidiForEachBlock(static_cast<Note const*>(nullptr), 3, 1000.0, 100, 10, [&](mcp::AiMidiBlock const& b) { none.push_back(b); });
    CHECK(none.empty());
}

void aHugeTakeIsDrawnAtAStride()
{
    std::vector<Note> take;
    for (int i = 0; i < 5000; ++i) {
        take.push_back({ 40 + (i % 30), 100, static_cast<float>(i * 10), 200.0f });
        take.push_back({ 40 + (i % 30), 0, static_cast<float>(i * 10 + 5), 200.0f });
    }
    auto const b = blocksOf(take, 60000.0);
    CHECK(!b.empty() && static_cast<int>(b.size()) <= mcp::kAiMidiMaxBlocks + 1);
}

void spanIsWholeBars()
{
    double const beat = 500.0; // 120 bpm: a bar is 2000 ms
    CHECK(mcp::aiMidiSpanMs(0.0, 0.0, beat) == 2000.0);        // an empty take still shows one bar
    CHECK(mcp::aiMidiSpanMs(1500.0, 0.0, beat) == 2000.0);
    CHECK(mcp::aiMidiSpanMs(2000.0, 0.0, beat) == 2000.0);     // exactly one bar does not grow a second
    CHECK(mcp::aiMidiSpanMs(2001.0, 0.0, beat) == 4000.0);
    CHECK(mcp::aiMidiSpanMs(1000.0, 5300.0, beat) == 6000.0);  // the recording head runs past the notes
    CHECK(mcp::aiMidiSpanMs(-5.0, -5.0, beat) == 2000.0);
    CHECK(mcp::aiMidiSpanMs(1.0, 0.0, 0.0) > 0.0);             // a broken tempo cannot divide by zero
    std::vector<Note> const take { { 60, 100, 0, 200 }, { 62, 100, 1700, 200 } };
    CHECK(mcp::aiMidiLoopMs(take.data(), 2) == 2200.0);        // the sequencer's own loop length
    CHECK(mcp::aiMidiLoopMs(take.data(), 0) == 0.0);
    CHECK(mcp::aiMidiLoopMs(static_cast<Note const*>(nullptr), 2) == 0.0);
}

void theHeadStaysInTheRegion()
{
    CHECK(mcp::aiMidiHeadX(0.0, 2000.0, 100) == 0.0f);
    CHECK(std::fabs(mcp::aiMidiHeadX(1000.0, 2000.0, 101) - 50.0f) < 0.01f);
    CHECK(mcp::aiMidiHeadX(9000.0, 2000.0, 100) == 99.0f);
    CHECK(mcp::aiMidiHeadX(-50.0, 2000.0, 100) == 0.0f);
    CHECK(mcp::aiMidiHeadX(10.0, 0.0, 100) == 0.0f);
}

void theWords()
{
    CHECK(mcp::aiMidiTakeNumber("take_7") == 7);
    CHECK(mcp::aiMidiTakeNumber("take_105") == 5);
    CHECK(mcp::aiMidiTakeNumber("take_12") == 12);
    CHECK(mcp::aiMidiTakeNumber("take") == 0);
    CHECK(mcp::aiMidiTakeNumber("") == 0);
    CHECK(mcp::aiMidiTakeNumber(nullptr) == 0);
    CHECK(mcp::aiMidiTakeNumber("a_b_3x") == 3);
    CHECK(mcp::aiMidiHintText(false, false, false, 0) == "empty, press rec");
    CHECK(mcp::aiMidiHintText(false, false, false, 1) == "1 note");
    CHECK(mcp::aiMidiHintText(false, false, false, 12) == "12 notes");
    CHECK(mcp::aiMidiHintText(false, true, false, 3) == "recording, 3 notes");
    CHECK(mcp::aiMidiHintText(false, false, true, 8) == "playing, 8 notes");
    CHECK(mcp::aiMidiHintText(true, false, false, 0) == "counting in");
    CHECK(mcp::aiMidiHintText(true, true, true, 5) == "counting in"); // the count-in wins
}

void theFamilyValues()
{
    // the face reads the family's style table, not numbers of its own
    CHECK(mcp::aiCream.r == 0xEC && mcp::aiCream.g == 0xE5 && mcp::aiCream.b == 0xD3);
    CHECK(mcp::aiInk.r == 0x1A && mcp::aiGrey.r == 0x8C && mcp::aiRed.r == 0xDC);
    CHECK(mcp::kAiMidiHeaderH == 24 && mcp::kAiMidiPad == 12 && mcp::kAiMidiRadius == 2.0f);
    CHECK(mcp::kAiMidiLine == 1.0f && mcp::kAiMidiActiveLine == 2.0f);
    CHECK(mcp::kAiMidiNameSize == 13.0f && mcp::kAiMidiHintSize == 11.0f);
}

} // namespace

int main()
{
    defaultFaceAnatomy();
    tallFaceGetsFullPadding();
    everySizeStaysInsideAndClear();
    theSupportedSizesKeepTheirBlocks();
    blocksFollowTheTake();
    unusableLengthsFallBackTo200();
    emptyAndBrokenTakesDrawNothing();
    aHugeTakeIsDrawnAtAStride();
    spanIsWholeBars();
    theHeadStaysInTheRegion();
    theWords();
    theFamilyValues();
    if (failures) {
        std::printf("ai_midi_face_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("ai_midi_face_test: all passed\n");
    return 0;
}
