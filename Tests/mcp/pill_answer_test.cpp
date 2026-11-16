// Unit tests for the canvas pill's answer line (Source/Mcp/McpPillAnswer.h), JUCE-free:
//  - only role `ai` sets the line; `detail`, `thought` and `error` leave it alone; `user` (the next prompt) clears it;
//  - the line is one plain line: breaks and controls become single spaces, backticks are dropped, a long text is cut at
//    whole code points (never inside a UTF-8 sequence) and ends in "...";
//  - the line holds 7 s, fades over 1 s, then is gone; a new line restarts the clock;
//  - a cut-off UTF-8 sequence at the end of the text is read safely.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpPillAnswer.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::PillAnswer;

namespace {

void rolesDecideWhatShows()
{
    PillAnswer a;
    CHECK(a.line().empty());
    CHECK(!a.visible(0));

    CHECK(a.onChat("ai", "Warm saw, sub on the left.", 1000));
    CHECK(a.line() == "Warm saw, sub on the left.");
    CHECK(a.visible(1000));

    // The long answer, the working status and an error never replace the line.
    CHECK(!a.onChat("detail", "A long answer with many words.", 1500));
    CHECK(!a.onChat("thought", "Thinking... 3 s", 1500));
    CHECK(!a.onChat("error", "Something failed", 1500));
    CHECK(a.line() == "Warm saw, sub on the left.");

    // The artist's next prompt clears it; a second user line changes nothing more.
    CHECK(a.onChat("user", "more bite", 2000));
    CHECK(a.line().empty());
    CHECK(!a.visible(2000));
    CHECK(!a.onChat("user", "again", 2100));

    // An empty or whitespace-only ai line shows nothing.
    CHECK(a.onChat("ai", " \n\t ", 3000));
    CHECK(!a.visible(3000));
}

void oneLineIsPlain()
{
    CHECK(PillAnswer::oneLine("  Hello   world \n", 160) == "Hello world");
    CHECK(PillAnswer::oneLine("line one\n\nline two\r\nline three", 160) == "line one line two line three");
    CHECK(PillAnswer::oneLine("Used `[tanh~]` for grit", 160) == "Used [tanh~] for grit");
    CHECK(PillAnswer::oneLine("a\x01" "b\x7f" "c", 160) == "a b c");
    // U+2028 (e2 80 a8) and a C1 control (c2 85) are spaces; an ordinary non-ASCII letter is kept.
    CHECK(PillAnswer::oneLine("a\xE2\x80\xA8" "b\xC2\x85" "c", 160) == "a b c");
    CHECK(PillAnswer::oneLine("W\xC3\xA4rme", 160) == "W\xC3\xA4rme");
    CHECK(PillAnswer::oneLine("", 160).empty());
    CHECK(PillAnswer::oneLine("   ", 160).empty());
}

void cutsAtWholeCodePoints()
{
    CHECK(PillAnswer::oneLine("abcdefghij", 5) == "abcde...");
    CHECK(PillAnswer::oneLine("abcde", 5) == "abcde"); // exactly the limit: no cut
    // Two-byte letters: five code points are ten bytes, never half a letter.
    CHECK(PillAnswer::oneLine("\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4", 5) == "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4...");
    // A space counts as one point: "ab cd" is 5 points, so a limit of 4 cuts before "d" ("ab c" would be 4).
    CHECK(PillAnswer::oneLine("ab cd", 4) == "ab c...");
    // A space that would be the last allowed point, with nothing after it that fits, is not left dangling.
    CHECK(PillAnswer::oneLine("abcd efgh", 4) == "abcd...");
    // The default limit.
    std::string const longText(PillAnswer::maxCodePoints + 50, 'x');
    auto const cut = PillAnswer::oneLine(longText, PillAnswer::maxCodePoints);
    CHECK(cut.size() == PillAnswer::maxCodePoints + 3);
    CHECK(cut.substr(cut.size() - 3) == "...");
}

void truncatedUtf8IsSafe()
{
    // A lead byte whose continuation bytes are missing at the end of the text: kept as is, never read past the end.
    auto const s = PillAnswer::oneLine("ok \xE2\x80", 160);
    CHECK(s == "ok \xE2\x80");
    auto const t = PillAnswer::oneLine("\xF0", 160);
    CHECK(t == "\xF0");
}

void holdsThenFades()
{
    PillAnswer a;
    CHECK(a.onChat("ai", "Done.", 10000));
    CHECK(a.alpha(10000) == 1.0f);
    CHECK(a.alpha(10000 + PillAnswer::holdMs) == 1.0f);
    float const mid = a.alpha(10000 + PillAnswer::holdMs + PillAnswer::fadeMs / 2);
    CHECK(mid > 0.45f && mid < 0.55f);
    CHECK(a.alpha(10000 + PillAnswer::holdMs + PillAnswer::fadeMs) == 0.0f);
    CHECK(!a.visible(10000 + PillAnswer::holdMs + PillAnswer::fadeMs + 5));
    CHECK(PillAnswer::holdMs + PillAnswer::fadeMs == 8000); // "about 8 s"

    // A clock that went back (a wall clock step) keeps the line, it does not fade early.
    CHECK(a.alpha(9000) == 1.0f);

    // A new line restarts the clock.
    CHECK(a.onChat("ai", "Second line", 17000));
    CHECK(a.alpha(17000 + 100) == 1.0f);
    CHECK(a.line() == "Second line");

    a.clear();
    CHECK(!a.visible(17001));
}

} // namespace

int main()
{
    rolesDecideWhatShows();
    oneLineIsPlain();
    cutsAtWholeCodePoints();
    truncatedUtf8IsSafe();
    holdsThenFades();
    if (failures != 0) {
        std::printf("pill_answer_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pill_answer_test: ok\n");
    return 0;
}
