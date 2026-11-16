// Unit tests for the pasted-picture helpers (Source/Mcp/McpRefsName.h), JUCE-free:
//  - the file name `<patch>-<n>.png`: the stem is the patch's name made safe, n is the lowest free number, a name that exists is
//    never returned (so a paste never overwrites a file);
//  - the stored size: at most 960 px on the longest side, the ratio kept, never enlarged;
//  - the pasted object: one `[pic -open refs/<file>]` line, relative to the patch's folder.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpRefsName.h"

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

namespace {

void stems()
{
    CHECK(mcp::refsStem("tape-echo") == "tape-echo");
    CHECK(mcp::refsStem("Tape Echo (v2)") == "tape-echo-v2");
    CHECK(mcp::refsStem("  --Weird__Name!!  ") == "weird__name");
    CHECK(mcp::refsStem("m\xC3\xBCller song") == "m-ller-song"); // a non-ASCII letter is a separator, not a broken byte
    CHECK(mcp::refsStem("") == "pasted");
    CHECK(mcp::refsStem("!!!") == "pasted");
    CHECK(mcp::refsStem("../../etc/passwd") == "etc-passwd"); // never a path
    auto const longStem = mcp::refsStem(std::string(100, 'a'));
    CHECK(longStem.size() == mcp::refsStemMax);
}

void names()
{
    std::set<std::string> there;
    auto const exists = [&there](std::string const& n) { return there.count(n) > 0; };
    CHECK(mcp::refsFileName("Tape Echo", exists) == "tape-echo-1.png");
    there.insert("tape-echo-1.png");
    CHECK(mcp::refsFileName("Tape Echo", exists) == "tape-echo-2.png");
    there.insert("tape-echo-3.png");
    CHECK(mcp::refsFileName("Tape Echo", exists) == "tape-echo-2.png"); // the lowest free number, a gap is filled
    there.insert("tape-echo-2.png");
    CHECK(mcp::refsFileName("Tape Echo", exists) == "tape-echo-4.png");
    // never an existing name, whatever exists
    for (int i = 0; i < 20; ++i) {
        auto const n = mcp::refsFileName("x", exists);
        CHECK(there.count(n) == 0);
        there.insert(n);
    }
    // no predicate: the first name
    CHECK(mcp::refsFileName("x", {}) == "x-1.png");
    // everything exists: no name (the caller refuses)
    CHECK(mcp::refsFileName("x", [](std::string const&) { return true; }).empty());
}

void sizes()
{
    CHECK((mcp::refsFitSize(800, 600) == std::pair<int, int> { 800, 600 })); // small: unchanged
    CHECK((mcp::refsFitSize(960, 540) == std::pair<int, int> { 960, 540 })); // exactly the edge
    CHECK((mcp::refsFitSize(3840, 2160) == std::pair<int, int> { 960, 540 }));
    CHECK((mcp::refsFitSize(1000, 4000) == std::pair<int, int> { 240, 960 }));
    CHECK((mcp::refsFitSize(10000, 1) == std::pair<int, int> { 960, 1 })); // never below one pixel
    CHECK((mcp::refsFitSize(0, 50) == std::pair<int, int> { 0, 50 }));     // nothing to scale: left as it is
    CHECK((mcp::refsFitSize(2000, 1000, 500) == std::pair<int, int> { 500, 250 }));
}

void objectText()
{
    CHECK(mcp::refsPicObject("tape-echo-1.png") == "pic -open refs/tape-echo-1.png");
    CHECK(mcp::refsPatchText("tape-echo-1.png") == "#X obj 0 0 pic -open refs/tape-echo-1.png;\n");
}

} // namespace

int main()
{
    stems();
    names();
    sizes();
    objectText();
    if (failures != 0) {
        std::printf("refs_name_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("refs_name_test: ok\n");
    return 0;
}
