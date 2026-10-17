// Unit tests for the stable-id rename helper (Plan 3 P3.2 S1, finding C6).
// Build + run: scripts/test-bridge-safety.sh
//
// The old callers did `map[to] = map[from]; map.erase(from)`, which with from == to wrote the
// entry and then erased it: renaming an object to its own id made it vanish from the map.
#include "../../Source/Pd/MCPIdentity.h"

#include <cstdio>
#include <string>
#include <unordered_map>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::identity::RenameResult;
using mcp::identity::renameStableId;

int main()
{
    // ── a normal rename moves the value ──
    {
        std::unordered_map<std::string, int> map { { "osc1", 7 }, { "dac1", 9 } };
        CHECK(renameStableId(map, "osc1", "carrier") == RenameResult::Applied);
        CHECK(map.size() == 2);
        CHECK(map.count("osc1") == 0);
        CHECK(map.at("carrier") == 7);
        CHECK(map.at("dac1") == 9);
    }

    // ── self-rename: present -> NoOp, the entry survives untouched (the C6 defect) ──
    {
        std::unordered_map<std::string, int> map { { "k_osc", 7 } };
        CHECK(renameStableId(map, "k_osc", "k_osc") == RenameResult::NoOp);
        CHECK(map.size() == 1);
        CHECK(map.at("k_osc") == 7);
    }

    // ── missing source: nothing changes, also for from == to ──
    {
        std::unordered_map<std::string, int> map { { "a", 1 } };
        CHECK(renameStableId(map, "nope", "b") == RenameResult::Missing);
        CHECK(renameStableId(map, "nope", "nope") == RenameResult::Missing);
        CHECK(map.size() == 1);
        CHECK(map.at("a") == 1);
        CHECK(map.count("b") == 0);
    }

    // ── no clash check here (that is C5, P3.5): an existing target is replaced, as before ──
    {
        std::unordered_map<std::string, int> map { { "a", 1 }, { "b", 2 } };
        CHECK(renameStableId(map, "a", "b") == RenameResult::Applied);
        CHECK(map.size() == 1);
        CHECK(map.at("b") == 1);
    }

    // ── ids with quotes and backslashes are just keys ──
    {
        std::unordered_map<std::string, int> map { { "q\"x\\y", 3 } };
        CHECK(renameStableId(map, "q\"x\\y", "q\"x\\y") == RenameResult::NoOp);
        CHECK(map.at("q\"x\\y") == 3);
    }

    // ── the value type does not matter (the bridge stores t_gobj*) ──
    {
        int object = 0;
        std::unordered_map<std::string, int*> map { { "obj", &object } };
        CHECK(renameStableId(map, "obj", "obj2") == RenameResult::Applied);
        CHECK(map.at("obj2") == &object);
    }

    if (failures == 0) std::printf("identity_rename_test: ok\n");
    return failures == 0 ? 0 : 1;
}
