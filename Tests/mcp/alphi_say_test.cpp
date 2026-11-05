// Unit tests for Alphi's say board (Alphi drop 1, step W), JUCE-free (Source/Mcp/McpAlphiSay.h):
//  - the checks of /pd/alphi_say {text, kind, state, turn, ttlMs} (the TS zod type in src/services/alphi-bubble.ts
//    mirrors them);
//  - plain text: control and bidi characters become spaces, invalid UTF-8 is dropped, cut at 90 whole code points;
//  - two slots (N4): a busy say (building/measuring) sets only the activity slot; a final say (idle/failed) sets the
//    line slot and clears activity; a later busy refresh from the same turn never brings activity back over a final
//    line, and never touches the line;
//  - a say is dropped once its ttlMs has passed.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAlphiSay.h"

#include <cstdio>
#include <optional>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::AlphiSay;
using mcp::AlphiSayBoard;
using mcp::AlphiSayKind;
using mcp::AlphiSayPhase;

static std::optional<AlphiSay> say(char const* text, char const* kind, char const* state, char const* turn, double ttl = 3000)
{
    return mcp::alphiSayFrom(text, kind, state, turn, ttl);
}

static std::size_t codePoints(std::string const& s)
{
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

static void checks()
{
    std::printf("checks\n");
    CHECK(say("placed lop~", "busy", "building", "t1").has_value());
    CHECK(say("measuring", "busy", "measuring", "t1").has_value());
    CHECK(say("built, not heard yet", "plain", "idle", "t1").has_value());
    CHECK(say("didn't work: no dac~", "fail", "failed", "t1").has_value());
    CHECK(say("silent at the output", "fail", "idle", "t1").has_value());
    // kind and state agree: busy <=> building/measuring
    CHECK(!say("x", "plain", "building", "t1").has_value());
    CHECK(!say("x", "fail", "measuring", "t1").has_value());
    CHECK(!say("x", "busy", "idle", "t1").has_value());
    CHECK(!say("x", "busy", "failed", "t1").has_value());
    // unknown words
    CHECK(!say("x", "loud", "idle", "t1").has_value());
    CHECK(!say("x", "plain", "happy", "t1").has_value());
    CHECK(!say("x", "", "", "t1").has_value());
    // turn: 1..40 of [A-Za-z0-9_.:-]
    CHECK(!say("x", "plain", "idle", "").has_value());
    CHECK(!say("x", "plain", "idle", "t 1").has_value());
    CHECK(!say("x", "plain", "idle", "t<1>").has_value());
    CHECK(say("x", "plain", "idle", "boot-3f.turn:12_a").has_value());
    CHECK(say("x", "plain", "idle", std::string(40, 'a').c_str()).has_value());
    CHECK(!say("x", "plain", "idle", std::string(41, 'a').c_str()).has_value());
    // ttlMs: an integer 100..60000
    CHECK(say("x", "plain", "idle", "t1", 100).has_value());
    CHECK(say("x", "plain", "idle", "t1", 60000).has_value());
    CHECK(!say("x", "plain", "idle", "t1", 99).has_value());
    CHECK(!say("x", "plain", "idle", "t1", 60001).has_value());
    CHECK(!say("x", "plain", "idle", "t1", 1500.5).has_value());
    CHECK(!say("x", "plain", "idle", "t1", std::nan("")).has_value());
    CHECK(!say("x", "plain", "idle", "t1", HUGE_VAL).has_value());
    // empty text (or only what cleaning turns into spaces) is refused
    CHECK(!say("", "plain", "idle", "t1").has_value());
    CHECK(!say("   ", "plain", "idle", "t1").has_value());
    CHECK(!say("\n\t\x01", "plain", "idle", "t1").has_value());
    auto const k = say("x", "fail", "failed", "t9", 2500);
    CHECK(k && k->kind == AlphiSayKind::Fail && k->phase == AlphiSayPhase::Failed && k->turn == "t9" && k->ttlMs == 2500);
}

static void text()
{
    std::printf("plain text, cut at 90 code points\n");
    CHECK(mcp::alphiSayText("plays at \xe2\x88\x92" "18 dB") == "plays at \xe2\x88\x92" "18 dB"); // U+2212 kept
    CHECK(mcp::alphiSayText("a\nb\tc\rd") == "a b c d");
    CHECK(mcp::alphiSayText("a\x7f" "b") == "a b");
    CHECK(mcp::alphiSayText("a\xc2\x85" "b") == "a b");                     // C1 NEL
    CHECK(mcp::alphiSayText("a\xe2\x80\xae" "b") == "a b");                 // U+202E right-to-left override
    CHECK(mcp::alphiSayText("a\xe2\x81\xa6" "b") == "a b");                 // U+2066 left-to-right isolate
    CHECK(mcp::alphiSayText("a\xe2\x80\xa8" "b") == "a b");                 // U+2028 line separator
    CHECK(mcp::alphiSayText("a\xff\xfe" "b") == "ab");                      // invalid bytes dropped
    CHECK(mcp::alphiSayText("a\xe2\x88" "b") == "ab");                      // a cut-off sequence dropped
    CHECK(mcp::alphiSayText("a\xc0\xaf" "b") == "ab");                      // overlong '/' dropped
    CHECK(mcp::alphiSayText("a\xed\xa0\x80" "b") == "ab");                  // a UTF-16 surrogate dropped
    CHECK(mcp::alphiSayText("  hi  ") == "hi");
    CHECK(mcp::alphiSayText("<b>bold</b>") == "<b>bold</b>");               // shown literally, never interpreted

    std::string ascii(120, 'a');
    CHECK(mcp::alphiSayText(ascii) == std::string(90, 'a'));
    // 89 ASCII + a 4-byte emoji = 90 code points: kept whole
    std::string const emoji = "\xf0\x9f\x8e\x9b"; // U+1F39B
    auto const at90 = mcp::alphiSayText(std::string(89, 'a') + emoji + "zz");
    CHECK(codePoints(at90) == 90 && at90 == std::string(89, 'a') + emoji);
    // 90 three-byte code points + more: cut on a code point, never inside one
    std::string minus;
    for (int i = 0; i < 95; ++i) minus += "\xe2\x88\x92";
    auto const cut = mcp::alphiSayText(minus);
    CHECK(codePoints(cut) == 90 && cut.size() == 270);
    // a say carries the cleaned, cut text
    auto const s = say(std::string(200, 'b').c_str(), "plain", "idle", "t1");
    CHECK(s && s->text == std::string(90, 'b'));
}

static void slots()
{
    std::printf("two slots (N4)\n");
    AlphiSayBoard board;
    std::int64_t const t0 = 1000000;
    CHECK(!board.activity(t0) && !board.line(t0));

    CHECK(board.apply(*say("placed lop~", "busy", "building", "t1"), t0));
    auto a = board.activity(t0);
    CHECK(a && a->text == "placed lop~" && a->phase == AlphiSayPhase::Building && a->kind == AlphiSayKind::Busy);
    CHECK(!board.line(t0)); // a busy say never writes the line slot

    CHECK(board.apply(*say("measuring", "busy", "measuring", "t1"), t0 + 10));
    a = board.activity(t0 + 10);
    CHECK(a && a->text == "measuring" && a->phase == AlphiSayPhase::Measuring);
    // the same busy say again only extends its time: no visible change, no repaint asked
    CHECK(!board.apply(*say("measuring", "busy", "measuring", "t1"), t0 + 20));

    // the final line: sets the line, clears activity
    CHECK(board.apply(*say("plays at -18 dB", "plain", "idle", "t1", 6000), t0 + 30));
    CHECK(!board.activity(t0 + 30));
    auto l = board.line(t0 + 30);
    CHECK(l && l->text == "plays at -18 dB" && l->phase == AlphiSayPhase::Idle && l->turn == "t1");

    // a late busy refresh from the SAME turn: dropped (no sparks after the final line, line untouched)
    CHECK(!board.apply(*say("placed lop~", "busy", "building", "t1"), t0 + 40));
    CHECK(!board.activity(t0 + 40));
    l = board.line(t0 + 40);
    CHECK(l && l->text == "plays at -18 dB");

    // a busy say of the NEXT turn: activity again, the old line stays until its time is up
    CHECK(board.apply(*say("placed osc~", "busy", "building", "t2"), t0 + 50));
    CHECK(board.activity(t0 + 50) && board.activity(t0 + 50)->turn == "t2");
    CHECK(board.line(t0 + 50) && board.line(t0 + 50)->text == "plays at -18 dB");

    // a failed final of t2 replaces the line and clears activity
    CHECK(board.apply(*say("didn't work: no dac~", "fail", "failed", "t2", 6000), t0 + 60));
    CHECK(!board.activity(t0 + 60));
    l = board.line(t0 + 60);
    CHECK(l && l->kind == AlphiSayKind::Fail && l->phase == AlphiSayPhase::Failed);

    // after the t2 line expired, a late t2 busy refresh still cannot revive anything
    std::int64_t const late = t0 + 60 + 6000;
    CHECK(!board.line(late));
    CHECK(!board.apply(*say("placed osc~", "busy", "building", "t2"), late));
    CHECK(!board.activity(late) && !board.line(late));
    // nor can a late refresh of the OLDER turn t1 after t2's line
    CHECK(!board.apply(*say("placed lop~", "busy", "building", "t1"), late));
    CHECK(!board.activity(late));
}

static void ttl()
{
    std::printf("ttl drop\n");
    AlphiSayBoard board;
    std::int64_t const t0 = 5000;
    CHECK(board.apply(*say("placed lop~", "busy", "building", "t1", 3000), t0));
    CHECK(board.activity(t0 + 2999).has_value());
    CHECK(!board.activity(t0 + 3000).has_value());
    // a refresh before expiry extends it
    CHECK(board.apply(*say("placed lop~", "busy", "building", "t1", 3000), t0 + 2000) == false);
    CHECK(board.activity(t0 + 4999).has_value());
    CHECK(!board.activity(t0 + 5000).has_value());
    // a refresh after expiry shows it again: a visible change
    CHECK(board.apply(*say("placed lop~", "busy", "building", "t1", 3000), t0 + 6000));

    CHECK(board.apply(*say("built, not heard yet", "plain", "idle", "t1", 100), t0 + 7000));
    CHECK(board.line(t0 + 7099).has_value());
    CHECK(!board.line(t0 + 7100).has_value());
    // a clock that went back (the wall clock was set back) does not keep a say forever: expiry is now + ttl at apply
    AlphiSayBoard back;
    CHECK(back.apply(*say("x", "plain", "idle", "t1", 1000), 10000));
    CHECK(back.line(9000).has_value());
    CHECK(!back.line(11000).has_value());
    // the handler's clock never goes back
    auto const c0 = mcp::alphiSayNowMs();
    CHECK(mcp::alphiSayNowMs() >= c0);
}

int main()
{
    checks();
    text();
    slots();
    ttl();
    if (failures == 0)
        std::printf("alphi_say_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
