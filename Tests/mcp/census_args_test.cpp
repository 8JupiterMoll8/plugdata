// Unit tests for the census's abstraction args (Source/Mcp/McpCensusArgs.h), JUCE-free (live check 2026-11-21):
//  - an abstraction box `[ai.loop~ a]` came back as type "pd", node_class "ai.loop~.pd", args ["ai.loop~.pd"]: the creation
//    args after the file name were lost, so `list` could not read a looper's name from the box;
//  - the args are now the file first (what every consumer already reads as args[0]), then the box's own args;
//  - a box with no args, no text, or only the class word gives the file alone (the old answer, unchanged);
//  - tokens split on blanks, a double-quoted span is one token (quotes kept, as the census does for every other box),
//    numbers and `$1` are kept as typed.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpCensusArgs.h"

#include <cstdio>
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

using Args = std::vector<std::string>;

namespace {

void theBoxArgsFollowTheFile()
{
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "ai.loop~ a") == (Args { "ai.loop~.pd", "a" }));
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "ai.loop~ voice 4 0.5") == (Args { "ai.loop~.pd", "voice", "4", "0.5" }));
    CHECK(mcp::abstractionCensusArgs("crusher.m~.pd", "crusher.m~ $1 -3") == (Args { "crusher.m~.pd", "$1", "-3" }));
}

void noArgsIsTheOldAnswer()
{
    // The file alone: exactly what the census gave before, so a consumer that reads args[0] sees no change.
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "ai.loop~") == (Args { "ai.loop~.pd" }));
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "") == (Args { "ai.loop~.pd" }));
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "   \n ") == (Args { "ai.loop~.pd" }));
    CHECK(mcp::abstractionCensusArgs("ai.loop~.pd", "ai.loop~   ") == (Args { "ai.loop~.pd" }));
}

void blanksAndQuotes()
{
    CHECK(mcp::abstractionCensusArgs("x.pd", "x \t a \n b") == (Args { "x.pd", "a", "b" }));
    CHECK(mcp::abstractionCensusArgs("x.pd", "x \"two words\" tail") == (Args { "x.pd", "\"two words\"", "tail" }));
    // An unclosed quote takes the rest of the text as one token, never loses it.
    CHECK(mcp::abstractionCensusArgs("x.pd", "x \"open end") == (Args { "x.pd", "\"open end" }));
}

void noFileIsNoArgs()
{
    // A plain [pd foo] subpatch has no abstraction file: the caller keeps its own answer; this function adds nothing.
    CHECK(mcp::abstractionCensusArgs("", "pd foo").empty());
}

} // namespace

int main()
{
    theBoxArgsFollowTheFile();
    noArgsIsTheOldAnswer();
    blanksAndQuotes();
    noFileIsNoArgs();
    if (failures != 0) {
        std::printf("census_args_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("census_args_test: OK\n");
    return 0;
}
