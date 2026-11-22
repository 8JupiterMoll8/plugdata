// Unit tests for the split-view plan (Source/Mcp/McpTabSplit.h), JUCE-free (PARKED 2026-11-22, "split view for the AI"):
//  - the request words: left, right, and none / unsplit / off for one view again, any case, blanks trimmed; anything else is no request;
//  - planMove mirrors what the tab menu's "Split left / Split right" allow (TabComponent: canSplitTab = a split exists or the bar holds
//    two tabs): with no split, either side needs two tabs (the tab goes to its side, the rest to the other); with a split, a tab
//    already on the asked side is left alone and one on the other side moves over; unsplit needs a split;
//  - the words the reply uses: sideWord (bar 0 left, bar 1 right) and the sentence for each refusal.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpTabSplit.h"

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

using mcp::tabsplit::Layout;
using mcp::tabsplit::Plan;
using mcp::tabsplit::Request;

namespace {

void theRequestWords()
{
    CHECK(mcp::tabsplit::parseRequest("left") == Request::Left);
    CHECK(mcp::tabsplit::parseRequest("right") == Request::Right);
    CHECK(mcp::tabsplit::parseRequest("  Right\n") == Request::Right);
    CHECK(mcp::tabsplit::parseRequest("LEFT") == Request::Left);
    CHECK(mcp::tabsplit::parseRequest("none") == Request::Unsplit);
    CHECK(mcp::tabsplit::parseRequest("unsplit") == Request::Unsplit);
    CHECK(mcp::tabsplit::parseRequest("off") == Request::Unsplit);
    CHECK(!mcp::tabsplit::parseRequest("").has_value());
    CHECK(!mcp::tabsplit::parseRequest("up").has_value());
    CHECK(!mcp::tabsplit::parseRequest("left right").has_value());
}

void noSplitNeedsTwoTabs()
{
    Layout const one { false, 1, 0 };
    Layout const two { false, 2, 0 };
    CHECK(mcp::tabsplit::planMove(Request::Left, one, 0) == Plan::TooFewTabs);
    CHECK(mcp::tabsplit::planMove(Request::Right, one, 0) == Plan::TooFewTabs);
    CHECK(mcp::tabsplit::planMove(Request::Left, two, 0) == Plan::MoveLeft);   // this tab left, the other one right
    CHECK(mcp::tabsplit::planMove(Request::Right, two, 0) == Plan::MoveRight); // this tab right, the other one left
}

void aSplitMovesOnlyWhatIsOnTheOtherSide()
{
    Layout const split { true, 2, 1 };
    CHECK(mcp::tabsplit::planMove(Request::Left, split, 0) == Plan::AlreadyThere);
    CHECK(mcp::tabsplit::planMove(Request::Right, split, 1) == Plan::AlreadyThere);
    CHECK(mcp::tabsplit::planMove(Request::Right, split, 0) == Plan::MoveRight);
    CHECK(mcp::tabsplit::planMove(Request::Left, split, 1) == Plan::MoveLeft);
    // The only tab of a bar can still move: the bars then merge (the menu does the same, then closes the empty split).
    Layout const lonely { true, 1, 1 };
    CHECK(mcp::tabsplit::planMove(Request::Right, lonely, 0) == Plan::MoveRight);
    CHECK(mcp::tabsplit::planMove(Request::Left, lonely, 1) == Plan::MoveLeft);
}

void unsplitNeedsASplit()
{
    CHECK(mcp::tabsplit::planMove(Request::Unsplit, Layout { true, 1, 2 }, 0) == Plan::Unsplit);
    CHECK(mcp::tabsplit::planMove(Request::Unsplit, Layout { false, 3, 0 }, 0) == Plan::NothingToUnsplit);
    CHECK(mcp::tabsplit::planMove(Request::Unsplit, Layout { false, 0, 0 }, 0) == Plan::NothingToUnsplit);
}

void theWords()
{
    CHECK(std::string(mcp::tabsplit::sideWord(0)) == "left");
    CHECK(std::string(mcp::tabsplit::sideWord(1)) == "right");
    CHECK(std::string(mcp::tabsplit::sideWord(-1)) == "left"); // a view with no split reports its one side as left
    CHECK(std::string(mcp::tabsplit::planWords(Plan::TooFewTabs)).find("two tabs") != std::string::npos);
    CHECK(std::string(mcp::tabsplit::planWords(Plan::NothingToUnsplit)).find("no split") != std::string::npos);
    CHECK(std::string(mcp::tabsplit::planWords(Plan::AlreadyThere)).find("already") != std::string::npos);
    // The moves and the unsplit are no refusals: no words.
    CHECK(std::string(mcp::tabsplit::planWords(Plan::MoveLeft)).empty());
    CHECK(std::string(mcp::tabsplit::planWords(Plan::MoveRight)).empty());
    CHECK(std::string(mcp::tabsplit::planWords(Plan::Unsplit)).empty());
}

} // namespace

int main()
{
    theRequestWords();
    noSplitNeedsTwoTabs();
    aSplitMovesOnlyWhatIsOnTheOtherSide();
    unsplitNeedsASplit();
    theWords();
    if (failures != 0) {
        std::printf("tab_split_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("tab_split_test: OK\n");
    return 0;
}
