// Unit tests for the pasted-picture helpers (Source/Mcp/McpRefsName.h), JUCE-free:
//  - the file name `<patch>-<n>.png`: the stem is the patch's name made safe, n is the lowest free number, a name that exists is
//    never returned (so a paste never overwrites a file);
//  - the stored size: at most 960 px on the longest side, the ratio kept, never enlarged;
//  - the pasted object: one `[pic -open refs/<file>]` line, relative to the patch's folder;
//  - the name a [pic] keeps and SAVES once it opened its file: `refs/<file>` stays (a bare name would no longer open from the
//    patch's folder after the next load: the 2026-11-17 review's finding 1), an absolute path cuts down to its file name.
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

// What the saved patch line holds for a pasted picture: ELSE's pic_save writes `pic <outline> <x_filename> <send> <receive> <size>
// <latch>` and its creation reads the filename from the second argument, so what must survive is the kept name.
void savedNameReopens()
{
    std::string const file = "tape-echo-1.png";
    std::string const location = "refs/" + file; // what the pasted `[pic -open refs/<file>]` carried
    auto const kept = mcp::picKeptFilename(location, file); // PictureObject::openFile: location + the found file's bare name
    CHECK(kept == location);
    CHECK("pic 0 " + kept + " empty empty 0 0" == "pic 0 refs/tape-echo-1.png empty empty 0 0");
    // the old behaviour (the bare name) is exactly what lost the folder
    CHECK(file != kept);
}

void keptFilenames()
{
    CHECK(mcp::picKeptFilename("refs/a.png", "a.png") == "refs/a.png");
    CHECK(mcp::picKeptFilename("./refs/a.png", "a.png") == "./refs/a.png");
    CHECK(mcp::picKeptFilename("../shared/a.png", "a.png") == "../shared/a.png");
    CHECK(mcp::picKeptFilename("a.png", "a.png") == "a.png"); // bare: unchanged
    CHECK(mcp::picKeptFilename("/home/me/pics/a.png", "a.png") == "a.png"); // absolute: upstream's bare name
    CHECK(mcp::picKeptFilename("~/pics/a.png", "a.png") == "a.png");
    CHECK(mcp::picKeptFilename("C:\\pics\\a.png", "a.png") == "a.png");
    CHECK(mcp::picKeptFilename("c:/pics/a.png", "a.png") == "a.png");
    CHECK(mcp::picKeptFilename("\\\\server\\share\\a.png", "a.png") == "a.png");
    CHECK(mcp::picKeptFilename("/x/a.png", "") == "/x/a.png"); // no bare name to keep: the location stays
    CHECK(mcp::picKeptFilename("", "a.png") == "a.png");
    CHECK(!mcp::picIsAbsolute("refs/a.png"));
    CHECK(!mcp::picIsAbsolute("x"));                     // a lone letter is not a drive
    CHECK(!mcp::picIsAbsolute("1:/a.png"));              // a digit is not a drive letter
    CHECK(mcp::picIsAbsolute("Z:a.png"));
}

} // namespace

int main()
{
    stems();
    names();
    sizes();
    objectText();
    keptFilenames();
    savedNameReopens();
    if (failures != 0) {
        std::printf("refs_name_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("refs_name_test: ok\n");
    return 0;
}
