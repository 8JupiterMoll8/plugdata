/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// A bool that paint may read for free: the owner computes it on events (resized, parentHierarchyChanged, a window
// state change) and when dueForRefresh() says so (a timer), never from get(). Used for PluginEditor's "wants rounded
// corners" (lag fix, Fable's diagnosis 2026-11-23, fix #1): the computation asks the X server twice (is the window
// maximised, is a compositor running), and paint used to ask on every frame. JUCE-free
// (Tests/mcp/refreshed_flag_test.cpp). Message thread only.

#include <cstdint>

namespace mcp {

class RefreshedFlag {
public:
    explicit RefreshedFlag(bool const initial, std::uint32_t const maxAgeMs = 2000)
        : value(initial)
        , maxAge(maxAgeMs)
    {
    }

    // Free: paint reads this.
    bool get() const { return value; }

    // True when the value is older than maxAgeMs (or was never computed). The millisecond counter may wrap.
    bool dueForRefresh(std::uint32_t const nowMs) const { return !everSet || nowMs - setAtMs >= maxAge; }

    // Store a freshly computed value. Returns true when it differs from the one paint was reading.
    bool set(bool const newValue, std::uint32_t const nowMs)
    {
        bool const changed = newValue != value;
        value = newValue;
        setAtMs = nowMs;
        everSet = true;
        return changed;
    }

private:
    bool value;
    std::uint32_t maxAge;
    std::uint32_t setAtMs = 0;
    bool everSet = false;
};

} // namespace mcp
