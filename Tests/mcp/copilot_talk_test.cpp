// Unit tests for the Copilot panel's talk model (Source/Mcp/McpCopilotTalk.h), JUCE-free (design round 2026-11-19, step C1):
//  - role `waiting` adds a line marked waiting; the next line with role `ai`, `detail` or `error` REPLACES the newest waiting
//    line in place (same position, same id), it is not appended ("Claude Code has not answered yet" goes away when the answer
//    comes); a second waiting line while one is open replaces the first;
//  - a `user` line equal to the last user line is dropped (the echo of the artist's own prompt);
//  - `thought` (the working status) and unknown roles never become a line; blank text neither;
//  - at most 400 lines, the oldest dropped;
//  - talkMark: error -> Look, waiting -> None, an AI line ending in `?` -> Ask, everything else None (no guessing of
//    "measured" or "changed" from the words).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpCopilotTalk.h"

#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::TalkChange;
using mcp::TalkLine;
using mcp::TalkList;
using mcp::TalkMark;

namespace {

void plainLinesAppendInOrder()
{
    TalkList t;
    CHECK(t.empty());
    CHECK(t.add("user", "make it funkier", "ai.tap") == TalkChange::Appended);
    CHECK(t.add("ai", "Funk ghosts in.", "") == TalkChange::Appended);
    CHECK(t.size() == 2);
    CHECK(t.lines()[0].role == "user");
    CHECK(t.lines()[0].text == "make it funkier");
    CHECK(t.lines()[0].context == "ai.tap");
    CHECK(!t.lines()[0].waiting);
    CHECK(t.lines()[1].role == "ai");
    CHECK(t.lines()[1].context.empty());
}

void detailIsStoredAsAi()
{
    TalkList t;
    CHECK(t.add("detail", "The long answer.") == TalkChange::Appended);
    CHECK(t.size() == 1);
    CHECK(t.lines()[0].role == "ai");
}

void waitingAddsAMarkedLine()
{
    TalkList t;
    t.add("user", "a bassline for this beat");
    CHECK(t.add("waiting", "Claude Code has not answered yet") == TalkChange::Appended);
    CHECK(t.size() == 2);
    CHECK(t.lines()[1].waiting);
    CHECK(t.lines()[1].text == "Claude Code has not answered yet");
    CHECK(t.hasOpenWaiting());
}

void anAnswerReplacesTheWaitingLineInPlace()
{
    for (char const* role : { "ai", "detail" }) {
        TalkList t;
        t.add("user", "a bassline for this beat");
        t.add("waiting", "Claude Code has not answered yet");
        auto const waitingId = t.lines()[1].id;
        CHECK(t.add(role, "Bassline in [pd ai-bass], right of ai.tap.") == TalkChange::Replaced);
        CHECK(t.size() == 2); // replaced, not appended
        CHECK(t.lines()[1].id == waitingId); // the same place
        CHECK(!t.lines()[1].waiting);
        CHECK(t.lines()[1].role == "ai");
        CHECK(t.lines()[1].text == "Bassline in [pd ai-bass], right of ai.tap.");
        CHECK(!t.hasOpenWaiting());
        // the answer is a normal line now: the next one appends
        CHECK(t.add("ai", "Sliders: cutoff, level.") == TalkChange::Appended);
        CHECK(t.size() == 3);
    }
}

void anErrorReplacesTheWaitingLineAndKeepsItsRole()
{
    TalkList t;
    t.add("user", "go");
    t.add("waiting", "Claude Code has not answered yet");
    CHECK(t.add("error", "The bridge is not connected.") == TalkChange::Replaced);
    CHECK(t.size() == 2);
    CHECK(t.lines()[1].role == "error");
    CHECK(!t.lines()[1].waiting);
    CHECK(talkMark(t.lines()[1]) == TalkMark::Look);
}

void theAnswerFillsTheNewestWaitingLineAmongLaterLines()
{
    TalkList t;
    t.add("user", "one");
    t.add("waiting", "has not answered yet");
    t.add("user", "two"); // the artist wrote again while waiting
    CHECK(t.size() == 3);
    CHECK(t.add("ai", "Answer.") == TalkChange::Replaced);
    CHECK(t.size() == 3);
    CHECK(t.lines()[1].text == "Answer."); // at the waiting line's own position, before the later user line
    CHECK(t.lines()[2].text == "two");
}

void aSecondWaitingLineReplacesTheFirst()
{
    TalkList t;
    t.add("user", "go");
    t.add("waiting", "Claude Code has not answered yet");
    auto const id = t.lines()[1].id;
    CHECK(t.add("waiting", "Claude Code has not answered yet (2 min)") == TalkChange::Replaced);
    CHECK(t.size() == 2);
    CHECK(t.lines()[1].id == id);
    CHECK(t.lines()[1].waiting);
    CHECK(t.lines()[1].text == "Claude Code has not answered yet (2 min)");
    // the very same text again changes nothing
    CHECK(t.add("waiting", "Claude Code has not answered yet (2 min)") == TalkChange::None);
    CHECK(t.size() == 2);
}

void aNewWaitingLineAfterAnAnswerAppends()
{
    TalkList t;
    t.add("user", "one");
    t.add("waiting", "has not answered yet");
    t.add("ai", "Answer one.");
    t.add("user", "two");
    CHECK(t.add("waiting", "has not answered yet") == TalkChange::Appended);
    CHECK(t.size() == 4);
    CHECK(t.lines()[3].waiting);
    CHECK(!t.lines()[1].waiting);
}

void anAnswerWithoutWaitingAppends()
{
    TalkList t;
    t.add("user", "hello");
    CHECK(t.add("ai", "Hi.") == TalkChange::Appended);
    CHECK(t.size() == 2);
}

void aUserLineEqualToTheLastUserLineIsDropped()
{
    TalkList t;
    CHECK(t.add("user", "a bassline", "ai.tap") == TalkChange::Appended); // the pill's own line, with its context
    CHECK(t.add("user", "a bassline") == TalkChange::None);                // the echo from /chat/msg
    CHECK(t.add("user", "  a bassline \n") == TalkChange::None);           // blanks around it do not matter
    CHECK(t.size() == 1);
    CHECK(t.lines()[0].context == "ai.tap"); // the first line keeps its context
    t.add("ai", "Done.");
    // today's rule: the LAST user line decides, also past an AI line
    CHECK(t.add("user", "a bassline") == TalkChange::None);
    CHECK(t.size() == 2);
    CHECK(t.add("user", "something else") == TalkChange::Appended);
    CHECK(t.add("user", "a bassline") == TalkChange::Appended); // no longer the last user line
    CHECK(t.size() == 4);
}

void anIdenticalAiLineRightAfterItselfIsDropped()
{
    TalkList t;
    CHECK(t.add("ai", "Same.") == TalkChange::Appended);
    CHECK(t.add("ai", "Same.") == TalkChange::None);
    CHECK(t.add("detail", " Same.") == TalkChange::None); // detail is an ai line
    CHECK(t.size() == 1);
    CHECK(t.add("error", "Same.") == TalkChange::Appended); // another role
    CHECK(t.size() == 2);
}

void thoughtAndUnknownRolesAndBlankTextNeverBecomeLines()
{
    TalkList t;
    CHECK(t.add("thought", "Thinking... 3 s") == TalkChange::None);
    CHECK(t.add("system", "hello") == TalkChange::None);
    CHECK(t.add("", "no role") == TalkChange::None);
    CHECK(t.add("ai", "") == TalkChange::None);
    CHECK(t.add("ai", " \n\t ") == TalkChange::None);
    CHECK(t.add("waiting", "   ") == TalkChange::None);
    CHECK(t.add("user", "") == TalkChange::None);
    CHECK(t.empty());
    // a thought does not close an open waiting line either
    t.add("waiting", "has not answered yet");
    CHECK(t.add("thought", "Thinking...") == TalkChange::None);
    CHECK(t.hasOpenWaiting());
    CHECK(t.size() == 1);
}

void atMostFourHundredLinesOldestDropped()
{
    TalkList t;
    for (int i = 0; i < 405; ++i)
        CHECK(t.add("ai", "line " + std::to_string(i)) == TalkChange::Appended);
    CHECK(t.size() == TalkList::maxLines);
    CHECK(TalkList::maxLines == 400);
    CHECK(t.lines().front().text == "line 5");
    CHECK(t.lines().back().text == "line 404");
    // the ids still run up and never repeat
    std::set<std::uint64_t> ids;
    for (auto const& l : t.lines()) ids.insert(l.id);
    CHECK(ids.size() == t.size());
    for (std::size_t i = 1; i < t.lines().size(); ++i) CHECK(t.lines()[i].id > t.lines()[i - 1].id);
}

void aReplaceNeverGrowsTheList()
{
    TalkList t;
    for (int i = 0; i < 399; ++i) t.add("ai", "line " + std::to_string(i));
    t.add("waiting", "has not answered yet"); // the 400th
    CHECK(t.size() == 400);
    CHECK(t.add("ai", "the answer") == TalkChange::Replaced);
    CHECK(t.size() == 400);
    CHECK(t.lines().front().text == "line 0"); // nothing dropped
}

void clearEmptiesEverything()
{
    TalkList t;
    t.add("user", "x");
    t.add("waiting", "y");
    t.clear();
    CHECK(t.empty());
    CHECK(!t.hasOpenWaiting());
    CHECK(t.add("ai", "again") == TalkChange::Appended);
}

TalkLine line(std::string role, std::string text, bool waiting = false)
{
    TalkLine l;
    l.role = std::move(role);
    l.text = std::move(text);
    l.waiting = waiting;
    return l;
}

void talkMarkIsOnlyWhatTheSoftwareKnows()
{
    CHECK(talkMark(line("error", "Not connected.")) == TalkMark::Look);
    CHECK(talkMark(line("error", "Why?")) == TalkMark::Look); // an error is an error first
    CHECK(talkMark(line("ai", "Claude Code has not answered yet", true)) == TalkMark::None); // waiting is drawn as a dotted line
    CHECK(talkMark(line("ai", "Is it right?", true)) == TalkMark::None);
    CHECK(talkMark(line("ai", "Very low notes. Up an octave, or louder?")) == TalkMark::Ask);
    CHECK(talkMark(line("ai", "Louder?  \n")) == TalkMark::Ask);                              // blanks after the mark
    CHECK(talkMark(line("ai", "Louder? Or quieter.")) == TalkMark::None);                     // the line must END in the question
    CHECK(talkMark(line("user", "where is the bassline?")) == TalkMark::None);                // the artist's own words get no mark
    // no guessing of "measured" or "changed" from the words
    CHECK(talkMark(line("ai", "It plays: bass -11 dB, under the drums at -5 dB.")) == TalkMark::None);
    CHECK(talkMark(line("ai", "Changed the cutoff to 900 Hz.")) == TalkMark::None);
    CHECK(talkMark(line("ai", "Measured just now.")) == TalkMark::None);
    CHECK(talkMark(line("ai", "")) == TalkMark::None);
}

} // namespace

int main()
{
    plainLinesAppendInOrder();
    detailIsStoredAsAi();
    waitingAddsAMarkedLine();
    anAnswerReplacesTheWaitingLineInPlace();
    anErrorReplacesTheWaitingLineAndKeepsItsRole();
    theAnswerFillsTheNewestWaitingLineAmongLaterLines();
    aSecondWaitingLineReplacesTheFirst();
    aNewWaitingLineAfterAnAnswerAppends();
    anAnswerWithoutWaitingAppends();
    aUserLineEqualToTheLastUserLineIsDropped();
    anIdenticalAiLineRightAfterItselfIsDropped();
    thoughtAndUnknownRolesAndBlankTextNeverBecomeLines();
    atMostFourHundredLinesOldestDropped();
    aReplaceNeverGrowsTheList();
    clearEmptiesEverything();
    talkMarkIsOnlyWhatTheSoftwareKnows();
    if (failures != 0) {
        std::printf("copilot_talk_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("copilot_talk_test: ok\n");
    return 0;
}
