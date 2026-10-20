/*
 // Stable-id map helpers for the MCP bridge (Plan 3 P3.2 S1, finding C6).
 // Kept free of JUCE and Pd so they can be unit-tested with plain g++:
 // see Tests/mcp/identity_rename_test.cpp and scripts/test-bridge-safety.sh.
 */
#pragma once

#include <string>
#include <utility>

namespace mcp::identity {

enum class RenameResult {
    Applied, // moved: the map changed, bump the identity version
    NoOp,    // from == to and present: counts as applied for the reply, nothing changed
    Missing, // `from` is not in the map
    Taken    // P3.5 S4 (C5): `to` already names an entry; nothing changed
};

// Moves the entry `from` to the key `to`. The old callers wrote `map[to] = map[from]` and then
// erased `from`, which with from == to erased the entry they had just written.
//
// A taken `to` is refused (finding C5, P3.5 S4): renaming onto another object's id silently dropped
// that object's id before.
template <typename Map>
RenameResult renameStableId(Map& map, std::string const& from, std::string const& to)
{
    auto it = map.find(from);
    if (it == map.end())
        return RenameResult::Missing;
    if (from == to)
        return RenameResult::NoOp;
    if (map.find(to) != map.end())
        return RenameResult::Taken;

    auto value = std::move(it->second);
    map.erase(it);
    map[to] = std::move(value);
    return RenameResult::Applied;
}

} // namespace mcp::identity
