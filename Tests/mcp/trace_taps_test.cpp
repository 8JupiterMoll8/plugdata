// Unit tests for the armed trace's tap choice (T2b): JUCE-free, run by scripts/test-bridge-safety.sh.
#include "../../Source/Pd/MCPBridgeTraceTaps.h"

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

using namespace mcp::trace;

namespace {

// A chain of n objects: object i feeds object i + 1; every object but the last has one signal outlet.
void chain(int n, std::vector<SignalOutlet>& outlets, std::vector<SignalEdge>& edges)
{
    for (int i = 0; i + 1 < n; ++i) {
        outlets.push_back({ i, 0, "o" + std::to_string(i) });
        edges.push_back({ i, i + 1 });
    }
}

} // namespace

int main()
{
    // Flow order follows the wires, whatever the order the objects were created in.
    {
        std::vector<SignalOutlet> outlets { { 2, 0, "c" }, { 0, 0, "a" }, { 1, 0, "b" } };
        std::vector<SignalEdge> edges { { 0, 1 }, { 1, 2 }, { 2, 3 } };
        auto order = orderAlongSignal(outlets, edges, 4);
        CHECK(order.size() == 3);
        CHECK(outlets[static_cast<size_t>(order[0])].name == "a");
        CHECK(outlets[static_cast<size_t>(order[1])].name == "b");
        CHECK(outlets[static_cast<size_t>(order[2])].name == "c");
    }
    // A feedback loop does not hang the ordering; its outlets come last.
    {
        std::vector<SignalOutlet> outlets { { 1, 0, "loopA" }, { 2, 0, "loopB" }, { 0, 0, "src" } };
        std::vector<SignalEdge> edges { { 0, 1 }, { 1, 2 }, { 2, 1 } };
        auto order = orderAlongSignal(outlets, edges, 3);
        CHECK(order.size() == 3);
        CHECK(outlets[static_cast<size_t>(order[0])].name == "src");
    }
    // Everything fits: all of them, in order, not sampled.
    {
        std::vector<SignalOutlet> outlets;
        std::vector<SignalEdge> edges;
        chain(6, outlets, edges);
        auto order = orderAlongSignal(outlets, edges, 6);
        auto choice = chooseTaps(outlets, order, {}, 16);
        CHECK(!choice.sampled);
        CHECK(choice.picked.size() == 5);
    }
    // Too many: exactly `cap`, sampled, the first and the last always in, no duplicates, in flow order.
    {
        std::vector<SignalOutlet> outlets;
        std::vector<SignalEdge> edges;
        chain(41, outlets, edges); // 40 outlets
        auto order = orderAlongSignal(outlets, edges, 41);
        auto choice = chooseTaps(outlets, order, {}, 16);
        CHECK(choice.sampled);
        CHECK(choice.picked.size() == 16);
        CHECK(choice.picked.front() == order.front());
        CHECK(choice.picked.back() == order.back());
        for (size_t i = 1; i < choice.picked.size(); ++i)
            CHECK(choice.picked[i] > choice.picked[i - 1]);
    }
    // A focus object is always tapped, even if the even spread would skip it.
    {
        std::vector<SignalOutlet> outlets;
        std::vector<SignalEdge> edges;
        chain(41, outlets, edges);
        auto order = orderAlongSignal(outlets, edges, 41);
        auto choice = chooseTaps(outlets, order, { "o13", "o14" }, 16);
        CHECK(choice.picked.size() == 16);
        bool has13 = false, has14 = false;
        for (int i : choice.picked) {
            has13 = has13 || outlets[static_cast<size_t>(i)].name == "o13";
            has14 = has14 || outlets[static_cast<size_t>(i)].name == "o14";
        }
        CHECK(has13 && has14);
    }
    // A focus longer than the cap fills it and nothing else; a cap of one picks the last outlet.
    {
        std::vector<SignalOutlet> outlets;
        std::vector<SignalEdge> edges;
        chain(41, outlets, edges);
        auto order = orderAlongSignal(outlets, edges, 41);
        std::vector<std::string> focus;
        for (int i = 0; i < 30; ++i)
            focus.push_back("o" + std::to_string(i));
        auto many = chooseTaps(outlets, order, focus, 16);
        CHECK(many.picked.size() == 16);
        auto one = chooseTaps(outlets, order, {}, 1);
        CHECK(one.picked.size() == 1 && one.picked[0] == order.back());
        auto none = chooseTaps(outlets, order, {}, 0);
        CHECK(none.picked.empty() && none.sampled);
    }

    std::printf(failures ? "trace_taps_test: %d FAILED\n" : "trace_taps_test: OK\n", failures);
    return failures ? 1 : 0;
}
