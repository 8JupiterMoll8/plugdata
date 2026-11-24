// Unit tests for the Copilot module rack's data and helpers (Source/Mcp/McpWidgetRack.h), JUCE-free:
//  - the table: every ai.* widget the rack lists once (the 13 widgets of the gallery plus the ai.note sticky), a one-line purpose,
//    a group, grouped in the groups' order, and an object text that is one valid `#X obj` body (no `;`, no `$`, no line break);
//  - the drag's patch text: `#X obj 0 0 <object>;\n`; ai.loop~ carries its name; ai.note is the sticky note's object text;
//  - the thumbnail file name: `<name>.png`, odd characters (the `~`) become `_`;
//  - a free looper name: the lowest loop<n> nobody uses;
//  - the artist's rack.json (Source/Mcp/McpRackFile.h): a good file adds cards in group order, a built-in wins unless the entry says
//    override, a broken / too big / unknown-version file leaves the built-ins and says one line, a bad entry is skipped with one line,
//    a patch entry is kept exactly, BLOCKS comes last, 64 entries and 256 KB are the limits, no input crashes the reader;
//  - docs/rack.json.example (the starter file the owner copies) reads with no warning.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpRackFile.h"
#include "../../Source/Mcp/McpWidgetRack.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
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

namespace {

void tableIsSound()
{
    CHECK(mcp::kRackWidgetCount == 14);
    std::set<std::string> names;
    std::size_t lastGroupIndex = 0;
    for (std::size_t i = 0; i < mcp::kRackWidgetCount; ++i) {
        auto const& w = mcp::kRackWidgets[i];
        CHECK(names.insert(std::string(w.name)).second); // listed once
        CHECK(w.name.substr(0, 3) == "ai.");
        CHECK(w.purpose.size() >= 10 && w.purpose.size() <= 80);
        CHECK(w.purpose.find('\n') == std::string_view::npos);
        // one `#X obj` body: no separator, no dollar argument, no line break, not empty
        CHECK(!w.object.empty());
        CHECK(w.object.find(';') == std::string_view::npos);
        CHECK(w.object.find('$') == std::string_view::npos);
        CHECK(w.object.find('\n') == std::string_view::npos);
        // grouped: the group index never goes back
        std::size_t groupIndex = 0;
        for (std::size_t g = 0; g < sizeof(mcp::kRackGroups) / sizeof(mcp::kRackGroups[0]); ++g)
            if (mcp::kRackGroups[g] == w.group) groupIndex = g;
        CHECK(groupIndex >= lastGroupIndex);
        lastGroupIndex = groupIndex;
    }
    for (char const* want : { "ai.sketch", "ai.tap", "ai.draw", "ai.loop~", "ai.knob", "ai.dice", "ai.paint", "ai.strip", "ai.listen", "ai.grid", "ai.jobs", "ai.midi", "ai.voice", "ai.note" })
        CHECK(names.count(want) == 1);
}

void everyGroupHasWidgetsAndATitle()
{
    for (auto const group : mcp::kRackGroups) {
        int n = 0;
        for (auto const& w : mcp::kRackWidgets)
            if (w.group == group) ++n;
        // BLOCKS has no built-in card: only rack.json fills it
        CHECK(group == mcp::RackGroup::Blocks ? n == 0 : n >= 1);
        CHECK(!mcp::rackGroupTitle(group).empty());
        CHECK((mcp::rackGroupColour(group) >> 24) == 0xff); // opaque
    }
    // distinct dot colours, so the groups read apart
    std::set<std::uint32_t> colours;
    for (auto const group : mcp::kRackGroups) colours.insert(mcp::rackGroupColour(group));
    CHECK(colours.size() == sizeof(mcp::kRackGroups) / sizeof(mcp::kRackGroups[0]));
}

mcp::RackWidget const* find(std::string_view name)
{
    for (auto const& w : mcp::kRackWidgets)
        if (w.name == name) return &w;
    return nullptr;
}

void patchText()
{
    auto const* sketch = find("ai.sketch");
    CHECK(sketch && mcp::rackPatchText(*sketch, "") == "#X obj 0 0 ai.sketch;\n");
    auto const* midi = find("ai.midi");
    CHECK(midi && mcp::rackPatchText(*midi, "ignored") == "#X obj 0 0 ai.midi 240 64;\n");
    auto const* loop = find("ai.loop~");
    CHECK(loop && mcp::rackPatchText(*loop, "loop3") == "#X obj 0 0 ai.loop~ loop3;\n");
    CHECK(loop && mcp::rackPatchText(*loop, "") == "#X obj 0 0 ai.loop~ loop1;\n"); // never a nameless looper
    auto const* note = find("ai.note");
    CHECK(note && mcp::rackPatchText(*note, "") == "#X obj 0 0 note 280 14 Inter empty 0 26 26 26 0 246 231 166 1 0 sticky note;\n");
}

// D1 (design round 2026-11-19): ai.note is a pale yellow card, 14 px dark ink, no outline. The argument order of ELSE [note]
// (note_new in pd-else): width, font size, font, receive, face (0 = no bold/italic/outline), text r g b, underline,
// background r g b, background flag, justification, then the text.
void noteSticky()
{
    auto const* note = find("ai.note");
    CHECK(note != nullptr);
    if (!note)
        return;
    std::string const object(note->object);
    std::vector<std::string> t;
    std::string cur;
    for (char const c : object) {
        if (c == ' ') {
            t.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    t.push_back(cur);
    CHECK(t.size() >= 16);
    if (t.size() < 16)
        return;
    CHECK(t[0] == "note");
    CHECK(t[1] == "280");  // width (the 12 px padding is inside it)
    CHECK(t[2] == "14");   // font size
    CHECK(t[3] == "Inter");
    CHECK(t[4] == "empty"); // no receive name
    CHECK(t[5] == "0");     // face: no bold, italic or outline
    CHECK(t[6] == "26" && t[7] == "26" && t[8] == "26"); // ink 1A1A1A
    CHECK(t[9] == "0");                                   // underline off
    CHECK(t[10] == "246" && t[11] == "231" && t[12] == "166"); // pale yellow F6E7A6
    CHECK(t[13] == "1");   // background on
    CHECK(t[14] == "0");   // left-justified
}

void thumbFileName()
{
    CHECK(mcp::rackThumbFileName("ai.sketch") == "ai.sketch.png");
    CHECK(mcp::rackThumbFileName("ai.loop~") == "ai.loop_.png");
    CHECK(mcp::rackThumbFileName("a b/c") == "a_b_c.png");
    CHECK(mcp::rackThumbFileName("../x") == ".._x.png"); // never a path: the separator is gone
    CHECK(mcp::rackThumbFileName("").empty());
}

void freeLooperName()
{
    CHECK(mcp::rackFreeLooperName({}) == "loop1");
    CHECK(mcp::rackFreeLooperName([](std::string const&) { return false; }) == "loop1");
    CHECK(mcp::rackFreeLooperName([](std::string const& n) { return n == "loop1" || n == "loop2"; }) == "loop3");
    CHECK(mcp::rackFreeLooperName([](std::string const& n) { return n == "loop1" || n == "loop3"; }) == "loop2");
    // everything taken: a name is still returned (never empty, never a hang)
    CHECK(mcp::rackFreeLooperName([](std::string const&) { return true; }) == "loop10000");
}

// ---- rack.json -----------------------------------------------------------------------------------------------------------------

using mcp::RackEntry;

RackEntry const* entryNamed(mcp::RackMerge const& m, std::string_view name)
{
    for (auto const& e : m.entries)
        if (e.name == name) return &e;
    return nullptr;
}

std::size_t indexOf(mcp::RackMerge const& m, std::string_view name)
{
    for (std::size_t i = 0; i < m.entries.size(); ++i)
        if (m.entries[i].name == name) return i;
    return m.entries.size();
}

std::string file(std::string const& widgets) { return "{ \"version\": 1, \"widgets\": [" + widgets + "] }"; }

std::string good(std::string const& name, std::string const& group = "SOUND", std::string const& extra = "\"object\": \"foo\"")
{
    return "{ \"name\": \"" + name + "\", \"purpose\": \"A good purpose\", \"group\": \"" + group + "\", " + extra + " }";
}

bool builtInsOnly(mcp::RackMerge const& m)
{
    if (m.entries.size() != mcp::kRackWidgetCount) return false;
    for (std::size_t i = 0; i < mcp::kRackWidgetCount; ++i)
        if (m.entries[i].name != mcp::kRackWidgets[i].name || m.entries[i].fromFile) return false;
    return true;
}

void groupTitles()
{
    CHECK(mcp::rackGroupFromTitle("BLOCKS") == mcp::RackGroup::Blocks);
    CHECK(mcp::rackGroupFromTitle("blocks") == mcp::RackGroup::Blocks);
    CHECK(mcp::rackGroupFromTitle("Idea Kit") == mcp::RackGroup::IdeaKit);
    CHECK(mcp::rackGroupFromTitle("melody") == mcp::RackGroup::Melody);
    CHECK(!mcp::rackGroupFromTitle("").has_value());
    CHECK(!mcp::rackGroupFromTitle("blocksx").has_value());
    CHECK(!mcp::rackGroupFromTitle("IDEAKIT").has_value());
    for (auto const g : mcp::kRackGroups) CHECK(mcp::rackGroupFromTitle(mcp::rackGroupTitle(g)) == g); // every title reads back
    CHECK(mcp::kRackGroups[sizeof(mcp::kRackGroups) / sizeof(mcp::kRackGroups[0]) - 1] == mcp::RackGroup::Blocks); // BLOCKS last
}

void noFileWidgets()
{
    auto const m = mcp::mergeRackFile(file(""));
    CHECK(builtInsOnly(m));
    CHECK(m.warnings.empty());
    CHECK(mcp::rackBuiltIns().size() == mcp::kRackWidgetCount);
}

void goodFileAddsInGroupOrder()
{
    auto const m = mcp::mergeRackFile(file(
        good("gbx.rec", "BLOCKS") + "," + good("ai.seq", "MELODY") + "," + good("gbx.out", "blocks") + "," + good("zap", "RHYTHM") + "," + good("dot", "IDEA KIT")));
    CHECK(m.warnings.empty());
    CHECK(m.entries.size() == mcp::kRackWidgetCount + 5);
    // the groups never go back
    std::size_t last = 0;
    for (auto const& e : m.entries) {
        std::size_t gi = 0;
        for (std::size_t g = 0; g < sizeof(mcp::kRackGroups) / sizeof(mcp::kRackGroups[0]); ++g)
            if (mcp::kRackGroups[g] == e.group) gi = g;
        CHECK(gi >= last);
        last = gi;
    }
    // in a group: built-ins first (table order), then the file's, in file order
    CHECK(indexOf(m, "ai.listen") < indexOf(m, "ai.seq"));
    CHECK(indexOf(m, "ai.seq") + 1 == indexOf(m, "ai.knob")); // ai.seq ends the MELODY group, SOUND follows
    CHECK(indexOf(m, "ai.sketch") < indexOf(m, "dot") && indexOf(m, "dot") < indexOf(m, "ai.tap")); // after the built-ins of IDEA KIT
    CHECK(indexOf(m, "ai.jobs") < indexOf(m, "gbx.rec"));     // BLOCKS after SYSTEM
    CHECK(indexOf(m, "gbx.rec") < indexOf(m, "gbx.out"));     // file order inside BLOCKS
    CHECK(m.entries.back().name == "gbx.out");
    // the built-ins keep their own order
    std::vector<std::string> builtIns;
    for (auto const& e : m.entries) if (!e.fromFile) builtIns.push_back(e.name);
    CHECK(builtIns.size() == mcp::kRackWidgetCount);
    for (std::size_t i = 0; i < builtIns.size(); ++i) CHECK(builtIns[i] == mcp::kRackWidgets[i].name);
    auto const* rec = entryNamed(m, "gbx.rec");
    CHECK(rec && rec->group == mcp::RackGroup::Blocks && rec->fromFile && rec->object == "foo" && rec->patch.empty());
    CHECK(rec && mcp::rackPatchText(*rec, "") == "#X obj 0 0 foo;\n");
}

void builtInWinsUnlessOverride()
{
    auto const wins = mcp::mergeRackFile(file(good("ai.sketch", "BLOCKS", "\"object\": \"other\"")));
    CHECK(builtInsOnly(wins)); // untouched, not even moved
    CHECK(wins.warnings.size() == 1 && wins.warnings[0].find("ai.sketch") != std::string::npos && wins.warnings[0].find("built-in") != std::string::npos);
    auto const falseOverride = mcp::mergeRackFile(file(good("ai.sketch", "BLOCKS", "\"object\": \"other\", \"override\": false")));
    CHECK(builtInsOnly(falseOverride) && falseOverride.warnings.size() == 1);

    auto const over = mcp::mergeRackFile(file(
        "{ \"name\": \"ai.midi\", \"purpose\": \"New words\", \"group\": \"BLOCKS\", \"object\": \"ai.midi 480 96\", \"override\": true }"));
    CHECK(over.warnings.empty());
    CHECK(over.entries.size() == mcp::kRackWidgetCount);
    auto const* midi = entryNamed(over, "ai.midi");
    CHECK(midi && midi->purpose == "New words" && midi->object == "ai.midi 480 96" && midi->fromFile);
    CHECK(midi && midi->group == mcp::RackGroup::Melody); // keeps its place and group
    CHECK(midi && mcp::rackPatchText(*midi, "") == "#X obj 0 0 ai.midi 480 96;\n");
    CHECK(indexOf(over, "ai.midi") == indexOf(mcp::mergeRackFile(file("")), "ai.midi"));
    // an override can carry a patch; and the looper's name still works for an overridden ai.loop~ object
    auto const patched = mcp::mergeRackFile(file(
        "{ \"name\": \"ai.loop~\", \"purpose\": \"Looper and out\", \"group\": \"RHYTHM\", \"object\": \"ai.loop~ {name} sync\", \"override\": true }"));
    auto const* looper = entryNamed(patched, "ai.loop~");
    CHECK(looper && mcp::rackNeedsLooperName(looper->object) && mcp::rackPatchText(*looper, "loop2") == "#X obj 0 0 ai.loop~ loop2 sync;\n");
    // an override that names no built-in is a plain new card
    auto const fresh = mcp::mergeRackFile(file(good("brandnew", "SOUND", "\"object\": \"foo\", \"override\": true")));
    CHECK(fresh.warnings.empty() && entryNamed(fresh, "brandnew") != nullptr);
}

void duplicatesInTheFile()
{
    auto const m = mcp::mergeRackFile(file(good("twice", "SOUND", "\"object\": \"one\"") + "," + good("twice", "BLOCKS", "\"object\": \"two\"")));
    CHECK(m.entries.size() == mcp::kRackWidgetCount + 1);
    CHECK(m.warnings.size() == 1 && m.warnings[0].find("entry 2") != std::string::npos);
    auto const* e = entryNamed(m, "twice");
    CHECK(e && e->object == "one"); // the first one stays
    // an overridden built-in named again: the second is a duplicate too
    auto const o = mcp::mergeRackFile(file(
        good("ai.dice", "SOUND", "\"object\": \"a\", \"override\": true") + "," + good("ai.dice", "SOUND", "\"object\": \"b\", \"override\": true")));
    CHECK(o.warnings.size() == 1);
    auto const* dice = entryNamed(o, "ai.dice");
    CHECK(dice && dice->object == "a");
}

void brokenFiles()
{
    for (std::string const& text : { std::string(""), std::string("   "), std::string("{"), std::string("not json"), std::string("{ \"widgets\": [ } "),
             std::string("{ \"widgets\": [] } trailing"), std::string("[]"), std::string("\"x\""), std::string("null"), std::string("{ \"version\": 1 }"),
             std::string("{ \"widgets\": {} }"), std::string("{ \"widgets\": [1,] }"), std::string("{ \"widgets\": [01] }"),
             std::string("{ 'widgets': [] }"), std::string("{ \"widgets\": [\"a\nb\"] }"), std::string("{ \"widgets\": [\"\\x\"] }"),
             std::string("{ \"widgets\": [\"\\ud800\"] }"), std::string("{ \"widgets\": [\"\\u0000\"] }"), std::string("{ \"widgets\": [\"\\u12\"] }"),
             std::string("{ \"version\": 2, \"widgets\": [] }"), std::string("{ \"version\": \"1\", \"widgets\": [] }"),
             std::string("{ \"version\": 1.0, \"widgets\": [] }"),
             std::string("{ \"widgets\": [\"\xff\"] }"), std::string("{ \"widgets\": [\"\xc3\"] }"), std::string("{ \"widgets\": [\"\xc0\x80\"] }"),
             std::string("{ \"widgets\": [\"\xed\xa0\x80\"] }"), std::string("{ \"widgets\": [\"\xf4\x90\x80\x80\"] }") }) {
        auto const m = mcp::mergeRackFile(text);
        CHECK(builtInsOnly(m));
        CHECK(m.warnings.size() == 1);
        CHECK(m.warnings.empty() || m.warnings[0].rfind("rack.json: ", 0) == 0);
    }
    // the one line says where a JSON error is
    auto const m = mcp::mergeRackFile("{\n  \"widgets\": [\n    oops\n  ]\n}");
    CHECK(m.warnings.size() == 1 && m.warnings[0].find("line 3") != std::string::npos);
    // a byte order mark and a UTF-8 purpose are fine
    auto const bom = mcp::mergeRackFile("\xEF\xBB\xBF" + file("{ \"name\": \"uml\", \"purpose\": \"Kl\xc3\xa4nge \\u00e4 \\ud83c\\udfb9\", \"group\": \"SOUND\", \"object\": \"foo\" }"));
    CHECK(bom.warnings.empty());
    auto const* uml = entryNamed(bom, "uml");
    CHECK(uml && uml->purpose == "Kl\xc3\xa4nge \xc3\xa4 \xf0\x9f\x8e\xb9");
    // whitespace around is fine
    CHECK(mcp::mergeRackFile("\n\t { \"widgets\" : [ ] } \r\n").warnings.empty());
}

void deepNestingIsRefused()
{
    std::string deep;
    for (int i = 0; i < 100000; ++i) deep += '[';
    auto const m = mcp::mergeRackFile(deep);
    CHECK(builtInsOnly(m) && m.warnings.size() == 1 && m.warnings[0].find("deep") != std::string::npos);
    std::string deepObj = "{ \"widgets\": [";
    for (int i = 0; i < 40; ++i) deepObj += "{ \"a\": [";
    CHECK(builtInsOnly(mcp::mergeRackFile(deepObj)));
    // nested within the limit is still read
    std::string ok = "{ \"widgets\": [ { \"name\": \"nest\", \"purpose\": \"Nested extra\", \"group\": \"SOUND\", \"object\": \"foo\", \"x\": [[[[[1]]]]] } ] }";
    CHECK(mcp::mergeRackFile(ok).warnings.empty());
}

void badEntriesAreSkipped()
{
    // each bad entry alone: skipped with exactly one line that names its number
    char const* bad[] = {
        "1",
        "\"text\"",
        "null",
        "[]",
        "{}",
        "{ \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",                                         // no name
        "{ \"name\": 5, \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",                           // name not text
        "{ \"name\": \"\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"has space\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"../x\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \".hidden\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"a/b\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"caf\\u00e9\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"abcdefghijklmnopqrstuvwxyzabcdefghijklmno\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }", // 41
        "{ \"name\": \"okname\", \"group\": \"SOUND\", \"object\": \"foo\" }",                                                     // no purpose
        "{ \"name\": \"okname\", \"purpose\": \"\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"okname\", \"purpose\": \"two\\nlines\", \"group\": \"SOUND\", \"object\": \"foo\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"object\": \"foo\" }",                                          // no group
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"NOPE\", \"object\": \"foo\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": 3, \"object\": \"foo\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\" }",                                          // neither
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\", \"patch\": \"#X obj 0 0 foo;\" }", // both
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"a; b\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"f $1\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"a\\nb\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": null }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"patch\": 7 }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"patch\": \"\" }",
        "{ \"name\": \"okname\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\", \"override\": \"yes\" }",
    };
    int n = 0;
    for (char const* b : bad) {
        ++n;
        auto const m = mcp::mergeRackFile(file(good("fine1", "SOUND") + "," + b + "," + good("fine2", "BLOCKS")));
        CHECK(m.entries.size() == mcp::kRackWidgetCount + 2); // the good ones around it stay
        CHECK(entryNamed(m, "fine1") && entryNamed(m, "fine2") && entryNamed(m, "okname") == nullptr);
        CHECK(m.warnings.size() == 1);
        if (m.warnings.size() == 1) {
            CHECK(m.warnings[0].find("entry 2") != std::string::npos);
            CHECK(m.warnings[0].find("skipped") != std::string::npos);
        } else {
            std::printf("  (bad entry %d)\n", n);
        }
    }
    // a long purpose, an overlong object, and a name with a control character in the log line
    CHECK(mcp::mergeRackFile(file(good("longp", "SOUND").replace(good("longp", "SOUND").find("A good purpose"), 14, std::string(161, 'x')))).warnings.size() == 1);
    CHECK(mcp::mergeRackFile(file(good("longo", "SOUND", "\"object\": \"" + std::string(201, 'x') + "\""))).warnings.size() == 1);
    CHECK(mcp::mergeRackFile(file(good("shortp", "SOUND").replace(good("shortp", "SOUND").find("A good purpose"), 14, std::string(160, 'x')))).warnings.empty());
    CHECK(mcp::mergeRackFile(file(good("fullo", "SOUND", "\"object\": \"" + std::string(200, 'x') + "\""))).warnings.empty());
    auto const odd = mcp::mergeRackFile(file("{ \"name\": \"a\\u0001b\\n\", \"purpose\": \"A good purpose\", \"group\": \"SOUND\", \"object\": \"foo\" }"));
    CHECK(odd.warnings.size() == 1 && odd.warnings[0].find('\n') == std::string::npos && odd.warnings[0].find('\x01') == std::string::npos);
    // extra keys are ignored
    CHECK(mcp::mergeRackFile(file(good("extra", "SOUND", "\"object\": \"foo\", \"colour\": \"red\", \"n\": [1, 2, {\"a\": null}]"))).warnings.empty());
}

void patchEntriesKeepTheirText()
{
    std::string const text = "#X obj 0 0 gbx.clock;\n#X obj 130 0 ai.seq bass;\n#X msg 0 40 \\; pd dsp 1;\n#X connect 0 0 1 0;";
    std::string const json = "{ \"name\": \"box\", \"purpose\": \"A wired box\", \"group\": \"BLOCKS\", \"patch\": \"#X obj 0 0 gbx.clock;\\n#X obj 130 0 ai.seq bass;\\n#X msg 0 40 \\\\; pd dsp 1;\\n#X connect 0 0 1 0;\" }";
    auto const m = mcp::mergeRackFile(file(json));
    CHECK(m.warnings.empty());
    auto const* box = entryNamed(m, "box");
    CHECK(box != nullptr);
    if (!box) return;
    CHECK(box->patch == text);                        // no change at all: no added newline, no trimming
    CHECK(box->object.empty());
    CHECK(mcp::rackPatchText(*box, "loop5") == text); // the drag pastes it as it is
    // {name} is the built-in looper's token only; a patch is not rewritten
    auto const tok = mcp::mergeRackFile(file(good("tok", "BLOCKS", "\"patch\": \"#X obj 0 0 ai.loop~ {name};\"")));
    auto const* t = entryNamed(tok, "tok");
    CHECK(t && mcp::rackPatchText(*t, "loop5") == "#X obj 0 0 ai.loop~ {name};");
    // CRLF text, a leading blank line, tabs and escaped dollars are fine
    CHECK(mcp::mergeRackFile(file(good("crlf", "BLOCKS", "\"patch\": \"\\r\\n#X obj 0 0 f \\\\$1;\\r\\n#X obj 0 30\\tt b;\\r\\n\""))).warnings.empty());
    // nothing that sends a message to Pd outside a #X statement
    for (char const* p : { "#X obj 0 0 foo;\\n; pd quit;", "pd quit;", "#X obj 0 0 foo;\\npd dsp 1;", "#X obj 0 0 foo", "#X obj 0 0 foo;\\n#X obj 0 0 bar", ";",
             "#X obj 0 0 foo;;", "#Xobj 0 0 foo;", "#N canvasx 0 0;", "  ;  ", "#X obj 0 0 a\\u0001b;" }) {
        auto const m2 = mcp::mergeRackFile(file(good("p", "BLOCKS", std::string("\"patch\": \"") + p + "\"")));
        CHECK(m2.warnings.size() == 1 && entryNamed(m2, "p") == nullptr);
    }
    // a subpatch and array data pass
    CHECK(mcp::mergeRackFile(file(good("sub", "BLOCKS", "\"patch\": \"#N canvas 0 0 450 300 sub 0;\\n#X obj 0 0 foo;\\n#X restore 10 10 pd sub;\\n#A 0 1 2;\""))).warnings.empty());
    // the patch size limit: 32 KB fits (blank space at the end is fine), one more byte does not
    std::string big;
    while (big.size() + 16 <= mcp::kRackPatchMax) big += "#X obj 0 0 foo;\n";
    big.resize(mcp::kRackPatchMax, ' ');
    std::string escaped;
    for (char const c : big) escaped += (c == '\n') ? std::string("\\n") : std::string(1, c);
    auto const fit = mcp::mergeRackFile(file(good("fit", "BLOCKS", "\"patch\": \"" + escaped + "\"")));
    CHECK(fit.warnings.empty() && entryNamed(fit, "fit") && entryNamed(fit, "fit")->patch == big);
    CHECK(mcp::mergeRackFile(file(good("over", "BLOCKS", "\"patch\": \"" + escaped + " \""))).warnings.size() == 1);
}

void entryCap()
{
    std::string many;
    for (int i = 0; i < 70; ++i) many += (i ? "," : "") + good("w" + std::to_string(i), "BLOCKS");
    auto const m = mcp::mergeRackFile(file(many));
    CHECK(m.entries.size() == mcp::kRackWidgetCount + 64);
    CHECK(entryNamed(m, "w63") != nullptr && entryNamed(m, "w64") == nullptr);
    CHECK(m.warnings.size() == 1 && m.warnings[0].find("64") != std::string::npos);
    // exactly 64 is fine
    std::string exact;
    for (int i = 0; i < 64; ++i) exact += (i ? "," : "") + good("w" + std::to_string(i), "BLOCKS");
    auto const e = mcp::mergeRackFile(file(exact));
    CHECK(e.entries.size() == mcp::kRackWidgetCount + 64 && e.warnings.empty());
    // the cap counts the entries read, bad ones too
    std::string withBad;
    for (int i = 0; i < 64; ++i) withBad += "7,";
    withBad += good("late", "SOUND");
    auto const b = mcp::mergeRackFile(file(withBad));
    CHECK(entryNamed(b, "late") == nullptr && b.warnings.size() == 65);
}

void sizeCap()
{
    std::string const base = file(good("fine", "SOUND"));
    // exactly 256 KB of text is read, one byte more is refused
    std::string pad = base;
    pad.append(mcp::kRackFileMaxBytes - pad.size(), ' ');
    CHECK(pad.size() == mcp::kRackFileMaxBytes);
    auto const atCap = mcp::mergeRackFile(pad);
    CHECK(atCap.warnings.empty() && entryNamed(atCap, "fine") != nullptr);
    auto const over = mcp::mergeRackFile(pad + " ");
    CHECK(builtInsOnly(over) && over.warnings.size() == 1 && over.warnings[0].find("256") != std::string::npos);
    // a big file of valid JSON is not parsed at all
    CHECK(builtInsOnly(mcp::mergeRackFile(std::string(10 * 1024 * 1024, '['))));
}

void missingOrUnreadable()
{
    for (auto const why : { mcp::RackFileProblem::Missing, mcp::RackFileProblem::Unreadable }) {
        auto const m = mcp::rackBuiltInsOnly(why);
        CHECK(builtInsOnly(m));
        CHECK(m.warnings.size() == 1 && m.warnings[0].rfind("rack.json: ", 0) == 0);
    }
}

void nameRules()
{
    CHECK(mcp::rackNameProblem("ai.seq").empty());
    CHECK(mcp::rackNameProblem("ai.loop~").empty());
    CHECK(mcp::rackNameProblem("a").empty());
    CHECK(mcp::rackNameProblem("gbx-rec_2.x").empty());
    CHECK(mcp::rackNameProblem("0day").empty());
    CHECK(!mcp::rackNameProblem("").empty());
    CHECK(!mcp::rackNameProblem("~x").empty());
    CHECK(!mcp::rackNameProblem("-x").empty());
    CHECK(!mcp::rackNameProblem("a b").empty());
    CHECK(!mcp::rackNameProblem("a\\b").empty());
    CHECK(!mcp::rackNameProblem(std::string(41, 'a')).empty());
    CHECK(mcp::rackNameProblem(std::string(40, 'a')).empty());
    // a good name always makes a file name inside the thumbs folder
    CHECK(mcp::rackThumbFileName("gbx.rec") == "gbx.rec.png");
}

// The starter file the owner copies to Abstractions/rack.json reads with no warning and has the entries the plan names.
void starterFile()
{
    // __FILE__ is `<root>/Tests/mcp/widget_rack_test.cpp` (absolute from scripts/test-bridge-safety.sh; relative from the repo root)
    std::string const here = __FILE__;
    auto const at = here.rfind("Tests/mcp/");
    std::string const path = (at == std::string::npos ? std::string() : here.substr(0, at)) + "docs/rack.json.example";
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty() && text.size() < 16 * 1024);
    auto const m = mcp::mergeRackFile(text);
    for (auto const& w : m.warnings) std::printf("  starter warning: %s\n", w.c_str());
    CHECK(m.warnings.empty());
    CHECK(m.entries.size() == mcp::kRackWidgetCount + 6);
    for (char const* name : { "ai.seq", "ai.sense8", "groovebox", "gbx.rec", "gesture", "gbx.out" }) CHECK(entryNamed(m, name) != nullptr);
    auto const* groove = entryNamed(m, "groovebox");
    CHECK(groove && groove->group == mcp::RackGroup::Blocks && groove->patch.rfind("#X obj 0 0 gbx.clock;\n", 0) == 0);
    CHECK(groove && groove->patch.find("#X connect 5 1 6 1;") != std::string::npos);
    CHECK(groove && groove->patch.back() == ';'); // the palette's text, exactly: no newline at the end
    auto const* gesture = entryNamed(m, "gesture");
    CHECK(gesture && gesture->group == mcp::RackGroup::Melody && gesture->patch.find("ai.draw") != std::string::npos);
    auto const* sense = entryNamed(m, "ai.sense8");
    CHECK(sense && sense->patch.find("sense.sim") != std::string::npos && sense->patch.find("ai.sense8") != std::string::npos && sense->patch.find("#X connect 0 0 1 0;") != std::string::npos);
    CHECK(entryNamed(m, "gbx.rec") && entryNamed(m, "gbx.rec")->object == "gbx.rec");
    CHECK(entryNamed(m, "ai.seq") && entryNamed(m, "ai.seq")->object == "ai.seq bass" && entryNamed(m, "ai.seq")->group == mcp::RackGroup::Melody);
}


// No input crashes the reader or hides a built-in: the starter file cut, flipped and padded at thousands of places (a fixed seed).
void mutatedStarterNeverCrashes()
{
    std::string const here = __FILE__;
    auto const at = here.rfind("Tests/mcp/");
    std::ifstream in((at == std::string::npos ? std::string() : here.substr(0, at)) + "docs/rack.json.example", std::ios::binary);
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty());
    if (text.empty())
        return;
    std::uint32_t seed = 12345u;
    auto const next = [&seed](std::uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    char const noise[] = { '"', '\\', '{', '}', '[', ']', ',', ':', ';', '\n', '\0', '\x80', '\xff', 'e', '-', '9' };
    for (int round = 0; round < 4000; ++round) {
        std::string t = text;
        switch (next(4)) {
        case 0: t.resize(next(static_cast<std::uint32_t>(t.size()))); break;
        case 1: t[next(static_cast<std::uint32_t>(t.size()))] = noise[next(sizeof(noise))]; break;
        case 2: t.insert(next(static_cast<std::uint32_t>(t.size())), 1, noise[next(sizeof(noise))]); break;
        default: t.erase(next(static_cast<std::uint32_t>(t.size())), 1 + next(8)); break;
        }
        auto const m = mcp::mergeRackFile(t);
        bool allThere = m.entries.size() >= mcp::kRackWidgetCount && m.entries.size() <= mcp::kRackWidgetCount + mcp::kRackFileMaxEntries;
        for (auto const& w : mcp::kRackWidgets)
            allThere = allThere && entryNamed(m, w.name) != nullptr;
        CHECK(allThere);
        for (auto const& e : m.entries) { // whatever came out of a file is a valid card
            CHECK(mcp::rackNameProblem(e.name).empty() && mcp::rackPurposeProblem(e.purpose).empty());
            CHECK(e.patch.empty() ? mcp::rackObjectProblem(e.object).empty() : mcp::rackPatchProblem(e.patch).empty());
        }
    }
}

} // namespace

int main()
{
    tableIsSound();
    everyGroupHasWidgetsAndATitle();
    patchText();
    noteSticky();
    thumbFileName();
    freeLooperName();
    groupTitles();
    noFileWidgets();
    goodFileAddsInGroupOrder();
    builtInWinsUnlessOverride();
    duplicatesInTheFile();
    brokenFiles();
    deepNestingIsRefused();
    badEntriesAreSkipped();
    patchEntriesKeepTheirText();
    entryCap();
    sizeCap();
    missingOrUnreadable();
    nameRules();
    starterFile();
    mutatedStarterNeverCrashes();
    if (failures != 0) {
        std::printf("widget_rack_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_rack_test: ok\n");
    return 0;
}
