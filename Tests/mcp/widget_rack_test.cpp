// Unit tests for the Copilot module rack's data and helpers (Source/Mcp/McpWidgetRack.h), JUCE-free:
//  - the table: every ai.* widget the rack lists once (the 13 widgets of the gallery plus the ai.note sticky), a one-line purpose,
//    a group, grouped in the groups' order, and an object text that is one valid `#X obj` body (no `;`, no `$`, no line break);
//  - the drag's patch text: `#X obj 0 0 <object>;\n`; ai.loop~ carries its name; ai.note is the sticky note's object text;
//  - the thumbnail file name: `<name>.png`, odd characters (the `~`) become `_`;
//  - a free looper name: the lowest loop<n> nobody uses.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetRack.h"

#include <cstdio>
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
        CHECK(n >= 1);
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

} // namespace

int main()
{
    tableIsSound();
    everyGroupHasWidgetsAndATitle();
    patchText();
    noteSticky();
    thumbFileName();
    freeLooperName();
    if (failures != 0) {
        std::printf("widget_rack_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_rack_test: ok\n");
    return 0;
}
