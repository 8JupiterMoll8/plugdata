// Unit tests for the Copilot panel's talk model (Source/Mcp/McpCopilotTalk.h), JUCE-free (design round 2026-11-19, step C1):
//  - role `waiting` adds a line marked waiting; the next line with role `ai`, `detail` or `error` REPLACES the newest waiting
//    line in place (same position, same id), it is not appended ("Claude Code has not answered yet" goes away when the answer
//    comes); a second waiting line while one is open replaces the first;
//  - a `user` line equal to the last user line is dropped (the echo of the artist's own prompt);
//  - `thought` (the working status) and unknown roles never become a line; blank text neither;
//  - at most 400 lines, the oldest dropped;
//  - talkMark: error -> Look, waiting -> None, an AI line ending in `?` -> Ask, everything else None (no guessing of
//    "measured" or "changed" from the words);
//  - the server's mark (design round 2026-11-19, "who is who"): the optional third word of /chat/msg on an ai / detail line,
//    `changed=<n>` / `changed`, `explained`, `measured=<dB>`, parsed by parseTalkWork; it becomes the line's mark (red circle,
//    blue outline square, blue square) and the grey header "alphi · changed 4 · measured -14 dB"; a malformed word is no mark;
//  - the widget marks (ai_widget plan, phase 3): `used` (the artist took Alphi's proposal on a widget) and `refused` (the artist turned it
//    down) are two more words of the same wire word; each becomes a mark of its own (and the header "alphi · used" / "alphi · refused");
//    a malformed word is no mark, both at once say nothing, a change still leads;
//  - groups: one artist ask and the answers after it are one group (talkGroups); the grey header of an AI line shows once per
//    run of the same fact; the context label of an artist line ("random 8", "ai.tap", "hsl +2");
//  - canvas tags (roles tag / annotation / hud) never become a line; an answer is stored whole (no cut, no "->").
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
using mcp::TalkWork;
using mcp::TalkWorkKind;

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


// ── the server's mark ─────────────────────────────────────────────────────────────────────────────────────────────────

void theWireWordIsParsed()
{
    auto w = mcp::parseTalkWork("changed=4 measured=-18.0");
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 4 && w.hasDb && w.db == -18.0);
    w = mcp::parseTalkWork("changed");
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 0 && !w.hasDb); // a change whose count is not known
    w = mcp::parseTalkWork("explained");
    CHECK(w.kind == TalkWorkKind::Explained && !w.hasDb);
    w = mcp::parseTalkWork("explained measured=-14.3");
    CHECK(w.kind == TalkWorkKind::Explained && w.hasDb && w.db == -14.3);
    w = mcp::parseTalkWork("measured=-9.5");
    CHECK(w.kind == TalkWorkKind::Measured && w.hasDb && w.db == -9.5);
    w = mcp::parseTalkWork("measured=-100.0");
    CHECK(w.kind == TalkWorkKind::Measured && w.db == -100.0);
    w = mcp::parseTalkWork("  measured=3  changed=12  "); // any order, any blanks
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 12 && w.hasDb && w.db == 3.0);
    w = mcp::parseTalkWork("changed=2 explained"); // a fact that changed beats one that only explained
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 2);
}

void aMalformedWireWordIsNoMark()
{
    for (char const* bad : { "", "   ", "changed=0", "changed=-3", "changed=abc", "changed=1e3", "changed=", "measured=", "measured=loud",
                             "measured=-", "measured=--3", "measured=1.2.3", "measured=-500", "measured=900", "Changed=2", "measured=nan",
                             "measured=inf", "explained=1", "something else" }) {
        auto const w = mcp::parseTalkWork(bad);
        CHECK(w.kind == TalkWorkKind::None);
        CHECK(!w.hasDb);
    }
    // one bad token does not spoil a good one next to it
    auto w = mcp::parseTalkWork("changed=abc explained");
    CHECK(w.kind == TalkWorkKind::Explained);
    w = mcp::parseTalkWork("changed=3 measured=loud");
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 3 && !w.hasDb);
    // a huge count is no count
    w = mcp::parseTalkWork("changed=99999999999999999999");
    CHECK(w.kind == TalkWorkKind::None);
}

void theMarkRidesOnlyOnAnswerLines()
{
    TalkList t;
    t.add("user", "variate this");
    CHECK(t.add("ai", "Faster clock.", "", "changed=4 measured=-18.0") == TalkChange::Appended);
    CHECK(t.lines()[1].work.kind == TalkWorkKind::Changed && t.lines()[1].work.changes == 4);
    CHECK(t.add("detail", "More words.", "", "explained") == TalkChange::Appended);
    CHECK(t.lines()[2].work.kind == TalkWorkKind::Explained);
    // the artist's own line, an error and a waiting line never wear one, whatever the wire said
    CHECK(t.add("user", "more", "", "changed=9") == TalkChange::Appended);
    CHECK(t.lines()[3].work.kind == TalkWorkKind::None);
    CHECK(t.add("waiting", "has not answered yet", "", "changed=9") == TalkChange::Appended);
    CHECK(t.lines()[4].work.kind == TalkWorkKind::None);
}

void anAnswerReplacingAWaitingLineBringsItsMark()
{
    TalkList t;
    t.add("user", "a bassline");
    t.add("waiting", "Claude Code has not answered yet");
    CHECK(t.add("ai", "Bassline built.", "", "changed=7") == TalkChange::Replaced);
    CHECK(t.size() == 2);
    CHECK(!t.lines()[1].waiting);
    CHECK(t.lines()[1].work.kind == TalkWorkKind::Changed && t.lines()[1].work.changes == 7);
}

void anAnswerWithoutAMarkStaysUnmarked()
{
    TalkList t;
    t.add("user", "hello");
    t.add("ai", "I changed the cutoff to 900 Hz and it measured -11 dB.", "", "");
    CHECK(t.lines()[1].work.kind == TalkWorkKind::None); // the words are never read for a mark
    CHECK(talkMark(t.lines()[1]) == TalkMark::None);
    CHECK(mcp::talkHeaderText(t.lines()[1]) == "alphi");
    t.add("ai", "Done.", "", "garbage");
    CHECK(t.lines()[2].work.kind == TalkWorkKind::None);
}

void aRepeatedLineAdoptsAMarkItLacked()
{
    TalkList t;
    t.add("ai", "Same.");
    CHECK(t.add("ai", "Same.", "", "changed=2") == TalkChange::Replaced); // the same words, now with what the turn proved
    CHECK(t.size() == 1);
    CHECK(t.lines()[0].work.kind == TalkWorkKind::Changed);
    CHECK(t.add("ai", "Same.", "", "changed=2") == TalkChange::None);
    CHECK(t.add("ai", "Same.") == TalkChange::None);
}

void theMarkOfALine()
{
    auto const withWork = [](std::string wire, std::string role = "ai") {
        TalkLine l = line(std::move(role), "Done.");
        l.work = mcp::parseTalkWork(wire);
        return l;
    };
    CHECK(talkMark(withWork("changed=4")) == TalkMark::Changed);
    CHECK(talkMark(withWork("changed")) == TalkMark::Changed);
    CHECK(talkMark(withWork("explained")) == TalkMark::Explained);
    CHECK(talkMark(withWork("measured=-14.0")) == TalkMark::Measured);
    CHECK(talkMark(withWork("changed=1 measured=-14.0")) == TalkMark::Changed); // the change leads; the reading is in the words of the header
    // a known fact beats the question mark of the words; an error stays an error
    auto q = withWork("changed=2");
    q.text = "Done. Louder?";
    CHECK(talkMark(q) == TalkMark::Changed);
    CHECK(talkMark(withWork("changed=2", "error")) == TalkMark::Look);
}

void theHeaderTextSaysWhatTheTurnProved()
{
    auto const header = [](std::string wire, std::string text = "Done.", std::string role = "ai") {
        TalkLine l = line(std::move(role), std::move(text));
        l.work = mcp::parseTalkWork(wire);
        return mcp::talkHeaderText(l);
    };
    std::string const dot = " \xC2\xB7 ";
    CHECK(header("changed=4 measured=-18.0") == "alphi" + dot + "changed 4" + dot + "measured -18 dB");
    CHECK(header("changed=3 measured=-14.2") == "alphi" + dot + "changed 3" + dot + "measured -14.2 dB");
    CHECK(header("changed") == "alphi" + dot + "changed");
    CHECK(header("explained") == "alphi" + dot + "explained");
    CHECK(header("explained measured=-9.0") == "alphi" + dot + "explained" + dot + "measured -9 dB");
    CHECK(header("measured=-9.5") == "alphi" + dot + "measured -9.5 dB");
    CHECK(header("measured=-100.0") == "alphi" + dot + "measured silent"); // a silent reading is a reading
    CHECK(header("") == "alphi");                                        // unknown: nothing extra
    CHECK(header("", "Louder?") == "alphi" + dot + "asks");
    CHECK(header("", "The bridge is not connected.", "error") == "alphi" + dot + "look here");
}

// ── the widget marks: used / refused (ai_widget plan, phase 3) ────────────────────────────────────────────────────────

void theUsedAndRefusedWordsAreParsed()
{
    auto w = mcp::parseTalkWork("used");
    CHECK(w.kind == TalkWorkKind::Used && w.changes == 0 && !w.hasDb);
    w = mcp::parseTalkWork("refused");
    CHECK(w.kind == TalkWorkKind::Refused && w.changes == 0 && !w.hasDb);
    w = mcp::parseTalkWork("  refused  "); // any blanks
    CHECK(w.kind == TalkWorkKind::Refused);
    w = mcp::parseTalkWork("measured=-9.5 used"); // any order; a reading rides along
    CHECK(w.kind == TalkWorkKind::Used && w.hasDb && w.db == -9.5);
    w = mcp::parseTalkWork("used explained"); // the artist's answer leads a guess
    CHECK(w.kind == TalkWorkKind::Used);
    w = mcp::parseTalkWork("refused explained");
    CHECK(w.kind == TalkWorkKind::Refused);
    w = mcp::parseTalkWork("changed=3 used"); // a change of the turn still leads
    CHECK(w.kind == TalkWorkKind::Changed && w.changes == 3);
    w = mcp::parseTalkWork("changed refused");
    CHECK(w.kind == TalkWorkKind::Changed);
    // both at once say nothing about the artist (no guess); what else the word says still counts
    w = mcp::parseTalkWork("used refused");
    CHECK(w.kind == TalkWorkKind::None);
    w = mcp::parseTalkWork("refused used explained");
    CHECK(w.kind == TalkWorkKind::Explained);
}

void aMalformedUsedOrRefusedWordIsNoMark()
{
    for (char const* bad : { "used=1", "used=", "Used", "USED", "refused=now", "Refused", "unused", "reused", "refusedd", "use", "refuse", "used,", "used;", "us ed",
                             "used:1", "=used" }) {
        auto const w = mcp::parseTalkWork(bad);
        CHECK(w.kind == TalkWorkKind::None);
        CHECK(!w.hasDb);
    }
    // one bad token does not spoil a good one next to it, and does not become the other mark
    auto w = mcp::parseTalkWork("used=1 refused");
    CHECK(w.kind == TalkWorkKind::Refused);
    w = mcp::parseTalkWork("Used used");
    CHECK(w.kind == TalkWorkKind::Used);
}

void theMarkOfAUsedOrRefusedLine()
{
    auto const withWork = [](std::string wire, std::string role = "ai") {
        TalkLine l = line(std::move(role), "You used Alphi's line on ai.draw.");
        l.work = mcp::parseTalkWork(wire);
        return l;
    };
    CHECK(talkMark(withWork("used")) == TalkMark::Used);
    CHECK(talkMark(withWork("refused")) == TalkMark::Refused);
    CHECK(talkMark(withWork("used measured=-12.0")) == TalkMark::Used);
    CHECK(talkMark(withWork("changed=2 used")) == TalkMark::Changed);
    // the words are never read for these marks: a line that merely says "used" has none; an error stays an error; a waiting line wears none
    auto plain = line("ai", "You used the line.");
    CHECK(talkMark(plain) == TalkMark::None);
    CHECK(talkMark(withWork("used", "error")) == TalkMark::Look);
    auto waiting = withWork("used");
    waiting.waiting = true;
    CHECK(talkMark(waiting) == TalkMark::None);
}

void theHeaderSaysUsedOrRefused()
{
    auto const header = [](std::string wire, std::string text = "Done.", std::string role = "ai") {
        TalkLine l = line(std::move(role), std::move(text));
        l.work = mcp::parseTalkWork(wire);
        return mcp::talkHeaderText(l);
    };
    std::string const dot = " \xC2\xB7 ";
    CHECK(header("used") == "alphi" + dot + "used");
    CHECK(header("refused") == "alphi" + dot + "refused");
    CHECK(header("used measured=-9.5") == "alphi" + dot + "used" + dot + "measured -9.5 dB");
    CHECK(header("used", "Louder?") == "alphi" + dot + "used"); // what is known beats the question mark
    CHECK(header("used", "The bridge is not connected.", "error") == "alphi" + dot + "look here");
}

void theMarkRidesOnTheTalkLine()
{
    TalkList t;
    CHECK(t.add("ai", "You used Alphi's line on ai.draw.", "", "used") == TalkChange::Appended);
    CHECK(t.lines()[0].work.kind == TalkWorkKind::Used);
    CHECK(t.add("detail", "You turned Alphi's line on ai.tap down.", "", "refused") == TalkChange::Appended);
    CHECK(t.lines()[1].work.kind == TalkWorkKind::Refused);
    // the artist's own line and a waiting line never wear one, whatever the wire said
    CHECK(t.add("user", "more", "", "used") == TalkChange::Appended);
    CHECK(t.lines()[2].work.kind == TalkWorkKind::None);
    CHECK(t.add("waiting", "has not answered yet", "", "refused") == TalkChange::Appended);
    CHECK(t.lines()[3].work.kind == TalkWorkKind::None);
    // a repeated line adopts the mark it lacked
    TalkList u;
    u.add("ai", "Same.");
    CHECK(u.add("ai", "Same.", "", "used") == TalkChange::Replaced);
    CHECK(u.lines()[0].work.kind == TalkWorkKind::Used);
    // and an answer that replaces a waiting line brings it
    TalkList v;
    v.add("user", "a bassline");
    v.add("waiting", "Claude Code has not answered yet");
    CHECK(v.add("ai", "You refused it.", "", "refused") == TalkChange::Replaced);
    CHECK(v.lines()[1].work.kind == TalkWorkKind::Refused);
}

// ── groups ────────────────────────────────────────────────────────────────────────────────────────────────────────────

void anAskAndItsAnswersAreOneGroup()
{
    TalkList t;
    t.add("user", "one");
    t.add("ai", "answer one");
    t.add("detail", "more of one");
    t.add("user", "two");
    t.add("ai", "answer two");
    auto const g = mcp::talkGroups(t.lines());
    CHECK(g.size() == 2);
    CHECK(g[0].first == 0 && g[0].count == 3 && g[0].hasAsk);
    CHECK(g[1].first == 3 && g[1].count == 2 && g[1].hasAsk);
}

void answersBeforeAnyAskAreAGroupWithoutAsk()
{
    TalkList t;
    t.add("ai", "Picture saved as refs/x.png.");
    t.add("user", "go");
    t.add("ai", "Done.");
    auto const g = mcp::talkGroups(t.lines());
    CHECK(g.size() == 2);
    CHECK(g[0].first == 0 && g[0].count == 1 && !g[0].hasAsk);
    CHECK(g[1].first == 1 && g[1].count == 2 && g[1].hasAsk);
}

void twoAsksInARowAreTwoGroups()
{
    TalkList t;
    t.add("user", "Extremer");
    t.add("user", "Variiere die Klangfarbe");
    t.add("ai", "Varied.");
    auto const g = mcp::talkGroups(t.lines());
    CHECK(g.size() == 2);
    CHECK(g[0].first == 0 && g[0].count == 1 && g[0].hasAsk);
    CHECK(g[1].first == 1 && g[1].count == 2 && g[1].hasAsk);
}

void anEmptyListHasNoGroups()
{
    TalkList t;
    CHECK(mcp::talkGroups(t.lines()).empty());
}

void theHeaderShowsOncePerRunOfTheSameFact()
{
    TalkList t;
    t.add("user", "one");
    t.add("ai", "Short line.", "", "changed=2");
    t.add("detail", "The long answer.", "", "changed=2");
    t.add("user", "two");
    t.add("ai", "Explained.", "", "explained");
    t.add("ai", "And a thing I measured.", "", "measured=-12.0");
    t.add("ai", "No new fact.");
    auto const& l = t.lines();
    CHECK(mcp::talkHeaderShown(l, 1));  // the first answer after an ask
    CHECK(!mcp::talkHeaderShown(l, 2)); // the same fact again: no second header
    CHECK(mcp::talkHeaderShown(l, 4));
    CHECK(mcp::talkHeaderShown(l, 5));  // a different fact
    CHECK(!mcp::talkHeaderShown(l, 6)); // an unknown line after a known one adds nothing to say
}

void aWaitingLineAndAnArtistLineHaveNoHeader()
{
    TalkList t;
    t.add("user", "go");
    t.add("waiting", "Claude Code has not answered yet");
    CHECK(!mcp::talkHeaderShown(t.lines(), 0)); // the artist line
    CHECK(!mcp::talkHeaderShown(t.lines(), 1)); // the dotted waiting line
}

void aLeadingAnswerAndAnErrorAfterAnAnswerHaveAHeader()
{
    TalkList t;
    t.add("ai", "Picture saved.");
    CHECK(mcp::talkHeaderShown(t.lines(), 0)); // no ask before it: still its own header
    t.add("error", "Then it broke.");
    CHECK(mcp::talkHeaderShown(t.lines(), 1)); // another role
}

void theContextLabelNamesTheBoxTheAskWasAbout()
{
    CHECK(mcp::talkContextLabel("random 8", 1) == "random 8");
    CHECK(mcp::talkContextLabel("  random   8 ", 1) == "random 8");
    CHECK(mcp::talkContextLabel("osc~ 440", 1) == "osc~ 440");
    CHECK(mcp::talkContextLabel("hsl 128 15 0 127 0 0 empty empty empty -2 -8 0 10 -262144 -1 -1 0 1", 1) == "hsl"); // a long IEM box: its class
    CHECK(mcp::talkContextLabel("expr ($f1*7)%24+36 and more and more", 1) == "expr");
    CHECK(mcp::talkContextLabel("ai.tap 8 4", 1) == "ai.tap");  // an ai widget: its name
    CHECK(mcp::talkContextLabel("ai.tap", 1) == "ai.tap");
    CHECK(mcp::talkContextLabel("random 8", 3) == "random 8 +2"); // more boxes selected
    CHECK(mcp::talkContextLabel("", 1).empty());
    CHECK(mcp::talkContextLabel("   ", 2).empty());
    CHECK(mcp::talkContextLabel("random 8", 0) == "random 8");
}

// ── tags stay on the canvas; answers are whole ────────────────────────────────────────────────────────────────────────

void canvasTagsNeverBecomeTalkLines()
{
    TalkList t;
    // whatever role a tag, a note card or the HUD might arrive with, nothing becomes a line
    for (char const* role : { "tag", "annotation", "annotate", "hud", "note", "overlay", "ghost", "region" }) {
        CHECK(t.add(role, "rolls 0-7 every 250 ms") == TalkChange::None);
        CHECK(t.add(role, "jumps by itself: each note picks 500-3100 Hz", "", "explained") == TalkChange::None);
    }
    CHECK(t.empty());
    // a tag's text inside a real answer line is the answer's own words: only the role decides, the text is never matched
    CHECK(t.add("ai", "It rolls 0-7 every 250 ms.") == TalkChange::Appended);
    CHECK(t.size() == 1);
}

void aLongAnswerIsStoredWholeWithoutACut()
{
    std::string text = "[expr ($f1*7)%24+36] turns the dice number into a note: times 7 semitones makes steps of a fifth (0, 7, 14, 21, 28), "
                       "%24 folds them back into two octaves (28 becomes 4), +36 starts at C2. So you hear C, G, D, A, E: a pentatonic. "
                       "The note goes to mtof, which makes it a frequency for the saw.";
    TalkList t;
    CHECK(t.add("ai", text, "", "explained") == TalkChange::Appended);
    CHECK(t.lines()[0].text == text);
    CHECK(text.size() > 280);
    CHECK(t.lines()[0].text.find("->") == std::string::npos);
    // thousands of characters stay whole
    std::string big;
    for (int i = 0; i < 400; ++i) big += "word" + std::to_string(i) + " ";
    CHECK(big.size() > 2000);
    TalkList u;
    u.add("detail", big);
    CHECK(u.lines()[0].text == TalkList::trim(big));
    TalkList v;
    v.add("user", big);
    CHECK(v.lines()[0].text == TalkList::trim(big)); // the artist's long ask as well
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
    theWireWordIsParsed();
    aMalformedWireWordIsNoMark();
    theMarkRidesOnlyOnAnswerLines();
    anAnswerReplacingAWaitingLineBringsItsMark();
    anAnswerWithoutAMarkStaysUnmarked();
    aRepeatedLineAdoptsAMarkItLacked();
    theMarkOfALine();
    theHeaderTextSaysWhatTheTurnProved();
    theUsedAndRefusedWordsAreParsed();
    aMalformedUsedOrRefusedWordIsNoMark();
    theMarkOfAUsedOrRefusedLine();
    theHeaderSaysUsedOrRefused();
    theMarkRidesOnTheTalkLine();
    anAskAndItsAnswersAreOneGroup();
    answersBeforeAnyAskAreAGroupWithoutAsk();
    twoAsksInARowAreTwoGroups();
    anEmptyListHasNoGroups();
    theHeaderShowsOncePerRunOfTheSameFact();
    aWaitingLineAndAnArtistLineHaveNoHeader();
    aLeadingAnswerAndAnErrorAfterAnAnswerHaveAHeader();
    theContextLabelNamesTheBoxTheAskWasAbout();
    canvasTagsNeverBecomeTalkLines();
    aLongAnswerIsStoredWholeWithoutACut();
    if (failures != 0) {
        std::printf("copilot_talk_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("copilot_talk_test: ok\n");
    return 0;
}
