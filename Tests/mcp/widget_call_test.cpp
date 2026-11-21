// Phase 3 of the ai_widget plan (docs/plans/AI-WIDGET-TOOL-PLAN-2026-11-21.md in the MCP repo): the widget_call core. JUCE-free and
// Pd-free (Source/Mcp/McpWidgetCall.h and McpWidgetCallJson.h). `/pd/widget_call <corrId> <widget> <paddr|-> <argc> <atoms...>`
// binds a collector to the widget's state name for the length of ONE call, sends the atoms to the widget's in-name and answers the
// frames the widget sent back, cut into sets at each `end`:
//  - the floor: the first word (after an optional `to <paddr>`) is dump, ghost, ghostname or unghost; anything else is refused
//    here (do, clear, flow, beat, ...), the address is `p` and 1..16 lower-case hex digits, nothing else gets through;
//  - the log: at most 64 frames of at most 32 atoms (the 65th frame and the 33rd atom are cut and flagged `truncated`); a symbol
//    longer than the room is cut and flagged too; the storage is fixed (nothing is allocated while the collector is bound);
//  - the sets: one per `end` frame (the end frame is the last frame of its set), `rev` and `at` read from it; frames after the
//    last `end` are never served (never half a state), only counted;
//  - the reply JSON: { widget, listeners, sets: [{ rev, at, frames }], truncated, unended, refused? error? }, bounded in size.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetCall.h"
#include "../../Source/Mcp/McpWidgetCallJson.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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

using mcp::WidgetCallArg;
using mcp::WidgetCallLog;
using mcp::WidgetCallRefusal;
using mcp::WidgetCallSet;

namespace {

WidgetCallArg sym(char const* s)
{
    WidgetCallArg a;
    a.isSymbol = true;
    a.sym = s;
    return a;
}

WidgetCallArg num(double v)
{
    WidgetCallArg a;
    a.num = v;
    return a;
}

WidgetCallRefusal planOf(std::vector<WidgetCallArg> const& args, char const* paddr = "-")
{
    return mcp::widgetCallPlan(args.data(), static_cast<int>(args.size()), paddr).refusal;
}

// A frame as a widget sends it: the marker word, then numbers (given as doubles) or words.
void frame(WidgetCallLog& log, char const* selector, std::vector<double> const& numbers = {})
{
    log.beginFrame(selector);
    for (double n : numbers)
        log.addNumber(n);
}

void end(WidgetCallLog& log, double rev, char const* addr = nullptr)
{
    log.beginFrame("end");
    log.addNumber(rev);
    if (addr) log.addSymbol(addr);
}

void addressesAndWords()
{
    CHECK(mcp::widgetCallAddrOk("p1"));
    CHECK(mcp::widgetCallAddrOk("p55d0c8a3b2a0"));
    CHECK(mcp::widgetCallAddrOk("pffffffffffffffff")); // 16 digits
    CHECK(mcp::widgetCallAddrOk("p0"));
    CHECK(!mcp::widgetCallAddrOk(nullptr));
    CHECK(!mcp::widgetCallAddrOk(""));
    CHECK(!mcp::widgetCallAddrOk("p"));                  // no digit
    CHECK(!mcp::widgetCallAddrOk("55d0"));               // no p
    CHECK(!mcp::widgetCallAddrOk("P55d0"));              // upper-case p
    CHECK(!mcp::widgetCallAddrOk("p55D0"));              // upper-case digits: a widget writes lower case
    CHECK(!mcp::widgetCallAddrOk("p0x55d0"));            // a 0x is not a digit
    CHECK(!mcp::widgetCallAddrOk("p55g0"));
    CHECK(!mcp::widgetCallAddrOk("pf0000000000000000")); // 17 digits
    CHECK(!mcp::widgetCallAddrOk("p55d0 "));
    CHECK(!mcp::widgetCallAddrOk("p55d0;"));
    CHECK(!mcp::widgetCallAddrOk("p-1"));

    for (char const* w : { "dump", "ghost", "ghostname", "unghost" })
        CHECK(mcp::widgetCallWordOk(w));
    for (char const* w : { "do", "clear", "flow", "beat", "drive", "undo", "back", "reload", "tempo", "set", "float", "list", "to", "askable", "ask",
                           "focused", "Dump", "dump ", "", "ghosts", "ghostnames", "feel", "save", "load" })
        CHECK(!mcp::widgetCallWordOk(w));
    CHECK(!mcp::widgetCallWordOk(nullptr));
}

void theFloorAllowsFourWordsAndNothingElse()
{
    // the four words, bare
    CHECK(planOf({ sym("dump") }) == WidgetCallRefusal::None);
    CHECK(planOf({ sym("unghost") }) == WidgetCallRefusal::None);
    CHECK(planOf({ sym("ghost"), num(7), num(0.5), num(-1) }) == WidgetCallRefusal::None);
    CHECK(planOf({ sym("ghostname"), sym("warm"), sym("bass") }) == WidgetCallRefusal::None);

    // refused words: the floor is the word list, whatever follows
    for (char const* w : { "do", "clear", "flow", "beat", "drive", "undo", "back", "reload", "tempo", "speaker", "feel", "save", "load", "set", "float", "bang" })
        CHECK(planOf({ sym(w) }) == WidgetCallRefusal::Word);
    CHECK(planOf({ sym("do"), sym("undo") }) == WidgetCallRefusal::Word);
    CHECK(planOf({ sym("clear") }, "p1f") == WidgetCallRefusal::Word); // with an address too
    CHECK(planOf({ sym("flow"), num(1) }) == WidgetCallRefusal::Word);
    CHECK(planOf({ num(0) }) == WidgetCallRefusal::Word);              // a number is no word
    CHECK(planOf({ num(1), sym("dump") }) == WidgetCallRefusal::Word);
    CHECK(planOf({ sym("Dump") }) == WidgetCallRefusal::Word);         // exact words only
    // a refused word hidden behind a good one is the second atom: that is an argument, the widget checks it (the floor is the first word)
    CHECK(planOf({ sym("ghostname"), sym("do") }) == WidgetCallRefusal::None);

    // nothing to send
    CHECK(planOf({}) == WidgetCallRefusal::Empty);
    CHECK(planOf({}, "p1f") == WidgetCallRefusal::Empty);
}

void theAddressIsPrefixedOrGiven()
{
    // the address as an argument: the floor checks the inner message, the bridge puts `to <paddr>` in front
    auto plan = mcp::widgetCallPlan(std::vector<WidgetCallArg> { sym("dump") }.data(), 1, "p55d0");
    CHECK(plan.refusal == WidgetCallRefusal::None && plan.bodyStart == 0 && !plan.addressInAtoms);
    plan = mcp::widgetCallPlan(std::vector<WidgetCallArg> { sym("dump") }.data(), 1, "-");
    CHECK(plan.refusal == WidgetCallRefusal::None && plan.bodyStart == 0 && !plan.addressInAtoms);
    plan = mcp::widgetCallPlan(std::vector<WidgetCallArg> { sym("dump") }.data(), 1, nullptr);
    CHECK(plan.refusal == WidgetCallRefusal::None && !plan.addressInAtoms);

    // a bad address argument
    for (char const* bad : { "", "p", "55d0", "P55d0", "p55G0", "pf0000000000000000", "p55d0;", "to", "p 1" })
        CHECK(planOf({ sym("dump") }, bad) == WidgetCallRefusal::BadAddress);

    // the `to <paddr>` prefix inside the atoms (the form the server sends today): the floor is the word after it
    auto const withTo = std::vector<WidgetCallArg> { sym("to"), sym("p55d0"), sym("ghost"), num(4), num(0.25) };
    plan = mcp::widgetCallPlan(withTo.data(), static_cast<int>(withTo.size()), "-");
    CHECK(plan.refusal == WidgetCallRefusal::None && plan.addressInAtoms && plan.bodyStart == 2);
    CHECK(planOf({ sym("to"), sym("p55d0"), sym("do") }) == WidgetCallRefusal::Word);
    CHECK(planOf({ sym("to"), sym("p55d0"), sym("clear") }) == WidgetCallRefusal::Word);
    CHECK(planOf({ sym("to"), sym("p55d0") }) == WidgetCallRefusal::Empty);              // an address and no word
    CHECK(planOf({ sym("to") }) == WidgetCallRefusal::BadAddress);
    CHECK(planOf({ sym("to"), num(5), sym("dump") }) == WidgetCallRefusal::BadAddress);
    CHECK(planOf({ sym("to"), sym("zz"), sym("dump") }) == WidgetCallRefusal::BadAddress);
    CHECK(planOf({ sym("to"), sym("p55d0"), sym("dump") }, "p1f") == WidgetCallRefusal::AddressTwice); // an address in the argument AND in the atoms
    CHECK(planOf({ sym("to"), sym("p55d0"), sym("to"), sym("p1f"), sym("dump") }) == WidgetCallRefusal::Word); // `to` is not a word of the list
}

void theFloorRefusesBadAtoms()
{
    CHECK(planOf({ sym("ghost"), num(NAN) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghost"), num(INFINITY) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym("a\nb") }) == WidgetCallRefusal::BadAtom);   // a control character
    CHECK(planOf({ sym("ghostname"), sym("a\x7f") }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym("") }) == WidgetCallRefusal::BadAtom);       // an empty symbol is no atom
    // a request word is refused above what a captured frame can carry back (kWidgetCallSymLen - 1 = 47 bytes): a label the widget echoes
    // is then never read back cut short
    CHECK(mcp::kWidgetCallMaxArgChars == mcp::kWidgetCallSymLen - 1 && mcp::kWidgetCallMaxArgChars == 47);
    CHECK(planOf({ sym("ghostname"), sym(std::string(65, 'a').c_str()) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym(std::string(64, 'a').c_str()) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym(std::string(48, 'a').c_str()) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym(std::string(47, 'a').c_str()) }) == WidgetCallRefusal::None);
    // the cap counts bytes: 24 umlauts are 48 bytes (refused), 23 umlauts and an 'a' are 47 (taken)
    std::string umlauts48, umlauts47 = "a";
    for (int i = 0; i < 24; ++i) umlauts48 += "\xC3\xA4";
    for (int i = 0; i < 23; ++i) umlauts47 += "\xC3\xA4";
    CHECK(planOf({ sym("ghostname"), sym(umlauts48.c_str()) }) == WidgetCallRefusal::BadAtom);
    CHECK(planOf({ sym("ghostname"), sym(umlauts47.c_str()) }) == WidgetCallRefusal::None);
    WidgetCallArg nullSym;
    nullSym.isSymbol = true;
    nullSym.sym = nullptr;
    CHECK(planOf({ sym("ghost"), nullSym }) == WidgetCallRefusal::BadAtom);

    std::vector<WidgetCallArg> many { sym("ghost") };
    while (many.size() < static_cast<std::size_t>(mcp::kWidgetCallMaxArgs)) many.push_back(num(1));
    CHECK(planOf(many) == WidgetCallRefusal::None);                                   // exactly the cap
    many.push_back(num(1));
    CHECK(planOf(many) == WidgetCallRefusal::TooManyAtoms);                           // one more
    CHECK(planOf({ sym("dump") }, "p1f") == WidgetCallRefusal::None);

    // every refusal says something a person can read, and the codes are told apart
    CHECK(std::strlen(mcp::widgetCallRefusalText(WidgetCallRefusal::Word)) > 10);
    CHECK(std::strcmp(mcp::widgetCallRefusalText(WidgetCallRefusal::Word), mcp::widgetCallRefusalText(WidgetCallRefusal::Empty)) != 0);
    CHECK(std::strcmp(mcp::widgetCallRefusalText(WidgetCallRefusal::None), "") == 0);
}

void twoSetsAreSplitAtEachEnd()
{
    auto log = std::make_unique<WidgetCallLog>();
    frame(*log, "d1", { 4, 2, 1 });
    frame(*log, "G", { 0 });
    end(*log, 4, "p55d0");
    frame(*log, "d1", { 9, 1, 1 });
    end(*log, 9, "p77aa");
    CHECK(log->frameCount() == 5);
    CHECK(!log->truncated());

    WidgetCallSet sets[mcp::kWidgetCallMaxFrames];
    int unended = -1;
    int const n = log->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended);
    CHECK(n == 2 && unended == 0);
    CHECK(sets[0].first == 0 && sets[0].count == 3);
    CHECK(sets[0].hasRev && sets[0].rev == 4);
    CHECK(sets[0].hasAddr && std::strcmp(sets[0].addr, "p55d0") == 0);
    CHECK(sets[1].first == 3 && sets[1].count == 2);
    CHECK(sets[1].hasRev && sets[1].rev == 9);
    CHECK(sets[1].hasAddr && std::strcmp(sets[1].addr, "p77aa") == 0);
    // the end frame is the last frame of its set, exactly as caught
    CHECK(std::strcmp(log->frame(sets[0].first + sets[0].count - 1).selector, "end") == 0);
    CHECK(log->frame(0).argc == 3 && !log->frame(0).atoms[0].isSymbol && log->frame(0).atoms[0].num == 4);
}

void aSetWithoutEndIsNotServed()
{
    auto log = std::make_unique<WidgetCallLog>();
    frame(*log, "t1", { 1 });
    end(*log, 2, "p1");
    frame(*log, "t1", { 3 }); // a dump that was cut: frames, no end
    frame(*log, "K", { 4 });
    WidgetCallSet sets[mcp::kWidgetCallMaxFrames];
    int unended = 0;
    int n = log->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended);
    CHECK(n == 1 && unended == 2);
    CHECK(sets[0].first == 0 && sets[0].count == 2);

    // frames and no end at all: nothing is served
    auto bare = std::make_unique<WidgetCallLog>();
    frame(*bare, "t1", { 1 });
    n = bare->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended);
    CHECK(n == 0 && unended == 1);

    // nothing came: no sets, nothing unended
    auto none = std::make_unique<WidgetCallLog>();
    n = none->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended);
    CHECK(n == 0 && unended == 0 && none->frameCount() == 0 && !none->truncated());
}

void anEndWithoutRevOrAddressStillCloses()
{
    // an old widget (an old copy in the artist's folder) says `end <rev>` with no address; a broken one says no rev
    auto log = std::make_unique<WidgetCallLog>();
    frame(*log, "d1", { 1 });
    end(*log, 5); // no address
    frame(*log, "d1", { 2 });
    log->beginFrame("end"); // no atoms at all
    frame(*log, "d1", { 3 });
    end(*log, 1.5, "p1"); // not a whole number
    frame(*log, "d1", { 4 });
    end(*log, 2, "55d0"); // not an address
    frame(*log, "d1", { 5 });
    end(*log, -3, "p2"); // negative
    WidgetCallSet sets[mcp::kWidgetCallMaxFrames];
    int unended = 0;
    int const n = log->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended);
    CHECK(n == 5 && unended == 0);
    CHECK(sets[0].hasRev && sets[0].rev == 5 && !sets[0].hasAddr);
    CHECK(!sets[1].hasRev && !sets[1].hasAddr);
    CHECK(!sets[2].hasRev && sets[2].hasAddr && std::strcmp(sets[2].addr, "p1") == 0);
    CHECK(sets[3].hasRev && sets[3].rev == 2 && !sets[3].hasAddr);
    CHECK(!sets[4].hasRev && sets[4].hasAddr);
}

void theSixtyFifthFrameIsTruncated()
{
    auto log = std::make_unique<WidgetCallLog>();
    for (int i = 0; i < mcp::kWidgetCallMaxFrames; ++i)
        frame(*log, "d1", { static_cast<double>(i) });
    CHECK(log->frameCount() == 64 && !log->truncated());
    frame(*log, "d1", { 64 }); // the 65th
    CHECK(log->frameCount() == 64 && log->truncated());
    log->addNumber(1); // an atom of the dropped frame goes nowhere
    CHECK(log->frameCount() == 64 && log->frame(63).argc == 1);
    end(*log, 3); // an end after the cap is dropped too: the 64 frames hold no end, so no set is served
    WidgetCallSet sets[mcp::kWidgetCallMaxFrames];
    int unended = 0;
    CHECK(log->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended) == 0 && unended == 64);

    // 63 frames and an end make the 64th: nothing is cut
    auto fits = std::make_unique<WidgetCallLog>();
    for (int i = 0; i < 63; ++i)
        frame(*fits, "d1", { static_cast<double>(i) });
    end(*fits, 1, "p1");
    CHECK(fits->frameCount() == 64 && !fits->truncated());
    CHECK(fits->splitSets(sets, mcp::kWidgetCallMaxFrames, &unended) == 1 && sets[0].count == 64);

    // clear gives the room back (the collector is one per call, but the log may be reused)
    log->clear();
    CHECK(log->frameCount() == 0 && !log->truncated());
    frame(*log, "d1", { 1 });
    CHECK(log->frameCount() == 1 && !log->truncated());
}

void theThirtyThirdAtomIsCutAndFlagged()
{
    auto log = std::make_unique<WidgetCallLog>();
    log->beginFrame("g1");
    for (int i = 0; i < mcp::kWidgetCallMaxAtoms; ++i)
        log->addNumber(i);
    CHECK(log->frame(0).argc == 32 && !log->truncated());
    log->addNumber(99); // the 33rd
    CHECK(log->frame(0).argc == 32 && log->truncated());
    CHECK(log->frame(0).atoms[31].num == 31);
    log->addSymbol("more");
    CHECK(log->frame(0).argc == 32);
    // the next frame starts clean
    log->beginFrame("d1");
    log->addNumber(1);
    CHECK(log->frameCount() == 2 && log->frame(1).argc == 1);
    // an atom with no open frame (the log is full) goes nowhere and is no crash
    WidgetCallLog empty;
    empty.addNumber(1);
    empty.addSymbol("x");
    CHECK(empty.frameCount() == 0);
}

void aLongSymbolIsCutAndFlagged()
{
    auto log = std::make_unique<WidgetCallLog>();
    log->beginFrame("label");
    log->addSymbol("warm");
    CHECK(!log->truncated() && std::strcmp(log->frame(0).atoms[0].sym, "warm") == 0 && log->frame(0).atoms[0].isSymbol);
    std::string const longWord(200, 'x');
    log->addSymbol(longWord.c_str());
    CHECK(log->truncated());
    CHECK(std::strlen(log->frame(0).atoms[1].sym) == static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1));
    CHECK(log->frame(0).argc == 2);
    // a long selector is cut and flagged too
    auto log2 = std::make_unique<WidgetCallLog>();
    log2->beginFrame(longWord.c_str());
    CHECK(log2->truncated() && std::strlen(log2->frame(0).selector) == static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1));
    // a null symbol is a question mark, as the control window writes it
    auto log3 = std::make_unique<WidgetCallLog>();
    log3->beginFrame("x");
    log3->addSymbol(nullptr);
    CHECK(std::strcmp(log3->frame(0).atoms[0].sym, "?") == 0);
}

// Valid UTF-8 as juce::String::fromUTF8 wants it: every lead byte has exactly its continuation bytes, no stray continuation byte.
bool validUtf8(char const* s)
{
    auto const* p = reinterpret_cast<unsigned char const*>(s);
    while (*p) {
        int extra = 0;
        if (*p < 0x80) extra = 0;
        else if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false; // a stray continuation byte (or an impossible lead)
        ++p;
        for (int i = 0; i < extra; ++i, ++p)
            if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}

void aCutNeverSplitsAMultibyteCharacter()
{
    CHECK(validUtf8("warm \xC3\xA4 \xE2\x82\xAC \xF0\x9F\x8E\x9B") && !validUtf8("a\xA4") && !validUtf8("a\xC3") && !validUtf8("a\xE2\x82"));
    // the umlaut straddles the cut: 46 bytes, then C3 A4 (bytes 47 and 48); 47 bytes are kept, so the lead byte alone would remain
    std::string const straddle = std::string(mcp::kWidgetCallSymLen - 2, 'a') + "\xC3\xA4" + "tail";
    auto log = std::make_unique<WidgetCallLog>();
    log->beginFrame("label");
    log->addSymbol(straddle.c_str());
    CHECK(log->truncated());
    CHECK(validUtf8(log->frame(0).atoms[0].sym));
    CHECK(std::strcmp(log->frame(0).atoms[0].sym, std::string(mcp::kWidgetCallSymLen - 2, 'a').c_str()) == 0); // the whole character is dropped, not half
    // every character width at every cut position: the symbol is always valid, never longer than the room, and loses at most 3 bytes
    for (char const* ch : { "\xC3\xA4", "\xE2\x82\xAC", "\xF0\x9F\x8E\x9B" }) {
        std::size_t const width = std::strlen(ch);
        for (int pad = 0; pad < 8; ++pad) {
            std::string text(static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1 - 6 + pad), 'a');
            while (text.size() < 90) text += ch;
            auto l = std::make_unique<WidgetCallLog>();
            l->beginFrame(text.c_str()); // the selector is cut the same way
            l->addSymbol(text.c_str());
            CHECK(l->truncated());
            CHECK(validUtf8(l->frame(0).selector) && validUtf8(l->frame(0).atoms[0].sym));
            std::size_t const kept = std::strlen(l->frame(0).atoms[0].sym);
            CHECK(kept <= static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1) && kept + width > static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1));
            CHECK(std::strcmp(l->frame(0).selector, l->frame(0).atoms[0].sym) == 0);
        }
    }
    // a character that ENDS exactly at the cut stays whole and the symbol is not flagged: 45 bytes and an umlaut are 47
    std::string const fits = std::string(mcp::kWidgetCallSymLen - 3, 'a') + "\xC3\xA4";
    auto fit = std::make_unique<WidgetCallLog>();
    fit->beginFrame("label");
    fit->addSymbol(fits.c_str());
    CHECK(!fit->truncated() && std::strcmp(fit->frame(0).atoms[0].sym, fits.c_str()) == 0);
    // text that is cut right after a whole character keeps it: 45 bytes, the umlaut, more text
    auto after = std::make_unique<WidgetCallLog>();
    after->beginFrame("label");
    after->addSymbol((fits + "more").c_str());
    CHECK(after->truncated() && std::strcmp(after->frame(0).atoms[0].sym, fits.c_str()) == 0);
    // text that is not UTF-8 at all (a run of continuation bytes) is still cut at the room: the step back is bounded, nothing runs off the buffer
    auto junk = std::make_unique<WidgetCallLog>();
    junk->beginFrame("label");
    junk->addSymbol(std::string(100, '\xA4').c_str());
    CHECK(junk->truncated() && std::strlen(junk->frame(0).atoms[0].sym) <= static_cast<std::size_t>(mcp::kWidgetCallSymLen - 1));
}

// A request word the floor takes (47 bytes) comes back whole from a frame, umlauts included.
void aLabelTheFloorTakesRoundTrips()
{
    std::string const word47 = std::string(mcp::kWidgetCallMaxArgChars - 2, 'w') + "\xC3\xA4";
    CHECK(word47.size() == 47 && planOf({ sym("ghostname"), sym(word47.c_str()) }) == WidgetCallRefusal::None);
    auto log = std::make_unique<WidgetCallLog>();
    log->beginFrame("label");
    log->addSymbol(word47.c_str());
    CHECK(!log->truncated() && std::strcmp(log->frame(0).atoms[0].sym, word47.c_str()) == 0);
    std::string const plain47(47, 'x');
    CHECK(planOf({ sym("ghostname"), sym(plain47.c_str()) }) == WidgetCallRefusal::None);
    auto log2 = std::make_unique<WidgetCallLog>();
    log2->beginFrame("label");
    log2->addSymbol(plain47.c_str());
    CHECK(!log2->truncated() && std::strcmp(log2->frame(0).atoms[0].sym, plain47.c_str()) == 0);
}

void theRoomIsFixed()
{
    // nothing is allocated while the collector is bound: the whole log is one object of fixed size (64 frames of 32 atoms)
    CHECK(sizeof(WidgetCallLog) >= static_cast<std::size_t>(mcp::kWidgetCallMaxFrames) * mcp::kWidgetCallMaxAtoms * sizeof(double));
    CHECK(mcp::kWidgetCallMaxFrames == 64 && mcp::kWidgetCallMaxAtoms == 32);
}

void theReplyJsonHasTheAgreedShape()
{
    auto log = std::make_unique<WidgetCallLog>();
    frame(*log, "d1", { 4, 2, 0.5 });
    log->beginFrame("label");
    log->addSymbol("warm \"bass\"");
    end(*log, 4, "p55d0");
    frame(*log, "d1", { 9 });
    end(*log, 9);
    frame(*log, "K", { 1 }); // unended
    mcp::WidgetCallReplyInfo info;
    info.widget = "ai.draw";
    info.listeners = 2;
    std::string const json = mcp::widgetCallReplyJson(info, log.get());
    CHECK(json == R"({"widget":"ai.draw","listeners":2,"sets":[)"
                  R"({"rev":4,"at":"p55d0","frames":[["d1",4,2,0.5],["label","warm \"bass\""],["end",4,"p55d0"]]},)"
                  R"({"rev":9,"at":null,"frames":[["d1",9],["end",9]]}],"truncated":false,"unended":1})");

    // nothing listens: no sets, listeners 0
    mcp::WidgetCallReplyInfo none;
    none.widget = "ai.draw";
    none.listeners = 0;
    CHECK(mcp::widgetCallReplyJson(none, nullptr) == R"({"widget":"ai.draw","listeners":0,"sets":[],"truncated":false,"unended":0})");

    // refused: the reason, no sets
    mcp::WidgetCallReplyInfo refused;
    refused.widget = "ai.draw";
    refused.refused = "not an allowed widget word";
    CHECK(mcp::widgetCallReplyJson(refused, nullptr)
          == R"({"widget":"ai.draw","listeners":0,"sets":[],"truncated":false,"unended":0,"refused":"not an allowed widget word"})");

    // the Pd context did not answer: listeners is not known (null), the error says why
    mcp::WidgetCallReplyInfo timedOut;
    timedOut.widget = "ai.tap";
    timedOut.listeners = -1;
    timedOut.error = "the Pd context did not answer in time";
    CHECK(mcp::widgetCallReplyJson(timedOut, nullptr)
          == R"({"widget":"ai.tap","listeners":null,"sets":[],"truncated":false,"unended":0,"error":"the Pd context did not answer in time"})");

    // a log that was cut says so
    auto cut = std::make_unique<WidgetCallLog>();
    cut->beginFrame("g1");
    for (int i = 0; i < 33; ++i)
        cut->addNumber(1);
    end(*cut, 1, "p1");
    mcp::WidgetCallReplyInfo cutInfo;
    cutInfo.widget = "ai.draw";
    cutInfo.listeners = 1;
    std::string const cutJson = mcp::widgetCallReplyJson(cutInfo, cut.get());
    CHECK(cutJson.find("\"truncated\":true") != std::string::npos);
    CHECK(cutJson.find("\"rev\":1,\"at\":\"p1\"") != std::string::npos);
}

void numbersAreWrittenAsJsonNumbers()
{
    char buf[40];
    auto text = [&](double v) {
        mcp::widgetCallNumberText(v, buf, sizeof(buf));
        return std::string(buf);
    };
    CHECK(text(0) == "0");
    CHECK(text(-0.0) == "0");
    CHECK(text(4) == "4");
    CHECK(text(-1) == "-1");
    CHECK(text(127) == "127");
    CHECK(text(0.5) == "0.5");
    CHECK(text(static_cast<double>(0.1f)) == "0.1");      // a float32 from Pd: no 0.10000000149
    CHECK(text(static_cast<double>(0.3f)) == "0.3");
    CHECK(text(static_cast<double>(1.0f / 3.0f)) == "0.3333333");
    CHECK(text(-0.25) == "-0.25");
    CHECK(text(9999999) == "9999999");
    CHECK(text(NAN) == "null");                           // JSON has no nan
    CHECK(text(INFINITY) == "null");
    CHECK(text(-INFINITY) == "null");
    CHECK(text(1e12) == "1000000000000");
    CHECK(text(1.5e30).find('e') != std::string::npos);   // an exponent form is still a JSON number
}

void theReplyStaysUnderItsCap()
{
    // 64 frames of 32 long words would be about 100 KB: the reply (one OSC message) is cut at a set that still fits, and says so
    auto log = std::make_unique<WidgetCallLog>();
    std::string const word(40, 'w');
    for (int set = 0; set < 8; ++set) {
        for (int f = 0; f < 7; ++f) {
            log->beginFrame("words");
            for (int a = 0; a < mcp::kWidgetCallMaxAtoms; ++a)
                log->addSymbol(word.c_str());
        }
        end(*log, set, "p1");
    }
    CHECK(log->frameCount() == 64 && !log->truncated());
    mcp::WidgetCallReplyInfo info;
    info.widget = "ai.draw";
    info.listeners = 1;
    std::string const json = mcp::widgetCallReplyJson(info, log.get());
    CHECK(json.size() <= static_cast<std::size_t>(mcp::kWidgetCallMaxReplyBytes) + 200); // the envelope is outside the cap
    CHECK(json.find("\"truncated\":true") != std::string::npos);
    CHECK(json.find("\"rev\":0,") != std::string::npos);         // the first set fits and is served whole
    CHECK(json.find("\"rev\":7,") == std::string::npos);         // the last does not
    CHECK(json.back() == '}' && json.find("\"sets\":[") != std::string::npos);
}

} // namespace

int main()
{
    addressesAndWords();
    theFloorAllowsFourWordsAndNothingElse();
    theAddressIsPrefixedOrGiven();
    theFloorRefusesBadAtoms();
    aCutNeverSplitsAMultibyteCharacter();
    aLabelTheFloorTakesRoundTrips();
    twoSetsAreSplitAtEachEnd();
    aSetWithoutEndIsNotServed();
    anEndWithoutRevOrAddressStillCloses();
    theSixtyFifthFrameIsTruncated();
    theThirtyThirdAtomIsCutAndFlagged();
    aLongSymbolIsCutAndFlagged();
    theRoomIsFixed();
    theReplyJsonHasTheAgreedShape();
    numbersAreWrittenAsJsonNumbers();
    theReplyStaysUnderItsCap();
    if (failures != 0) {
        std::printf("widget_call_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_call_test: ok\n");
    return 0;
}
