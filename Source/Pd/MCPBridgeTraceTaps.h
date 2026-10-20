/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Which signal outlets an armed trace taps (T2b). JUCE-free and Pd-free, so Tests/mcp/trace_taps_test.cpp runs it
// with g++ alone. The bridge has MAX_PROBES probe slots; a canvas can have far more signal outlets, so the trace taps
// the ones that matter: the objects the caller asks about first (`focus`), then outlets spread evenly along the
// signal path (source to sink), so a silent stretch is bracketed by a tapped outlet that sounds and one that does not.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace mcp::trace {

/** A linear peak as dBFS; anything under -140 dBFS is -100 (the bridge's floor for "nothing"). */
inline float peakToDb(float peak)
{
    return peak > 1e-7f ? 20.0f * std::log10(peak) : -100.0f;
}

/** One signal outlet that has at least one connection. `obj` indexes the canvas's object list. */
struct SignalOutlet {
    int obj = 0;
    int outlet = 0;
    std::string name; // the object's tempId (or "class#index" when it has none)
};

/** A signal wire between two objects of the canvas (object indices). */
struct SignalEdge {
    int src = 0;
    int dst = 0;
};

/**
 * The outlets in signal-flow order (Kahn's algorithm over the objects: an object comes after everything that feeds
 * it). Objects in a feedback loop never reach in-degree 0; their outlets come last, in input order. Returns indices
 * into `outlets`.
 */
inline std::vector<int> orderAlongSignal(std::vector<SignalOutlet> const& outlets, std::vector<SignalEdge> const& edges, int numObjects)
{
    std::vector<int> indegree(static_cast<size_t>(std::max(numObjects, 0)), 0);
    std::vector<std::vector<int>> next(indegree.size());
    for (auto const& e : edges) {
        if (e.src < 0 || e.dst < 0 || e.src >= numObjects || e.dst >= numObjects || e.src == e.dst)
            continue;
        next[static_cast<size_t>(e.src)].push_back(e.dst);
        ++indegree[static_cast<size_t>(e.dst)];
    }
    std::vector<int> rank(indegree.size(), -1);
    std::vector<int> queue;
    for (int i = 0; i < numObjects; ++i)
        if (indegree[static_cast<size_t>(i)] == 0)
            queue.push_back(i);
    int step = 0;
    for (size_t head = 0; head < queue.size(); ++head) {
        int const o = queue[head];
        rank[static_cast<size_t>(o)] = step++;
        for (int d : next[static_cast<size_t>(o)])
            if (--indegree[static_cast<size_t>(d)] == 0)
                queue.push_back(d);
    }
    std::vector<int> order(outlets.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);
    auto rankOf = [&](int outletIndex) {
        int const o = outlets[static_cast<size_t>(outletIndex)].obj;
        int const r = (o >= 0 && o < numObjects) ? rank[static_cast<size_t>(o)] : -1;
        return r < 0 ? step + 1 : r; // loops and unknown objects last
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rankOf(a) < rankOf(b); });
    return order;
}

struct TapChoice {
    std::vector<int> picked; // indices into the outlet list, in signal order
    bool sampled = false;    // true when not every outlet could be tapped
};

/**
 * At most `cap` outlets of `ordered` (indices into `outlets`, in signal order). Everything fits: all of them, not
 * sampled. Otherwise the outlets of objects named in `focus` come first (in signal order), and the remaining slots
 * are spread evenly over the rest, always including the first (a source) and the last (nearest the sink).
 */
inline TapChoice chooseTaps(std::vector<SignalOutlet> const& outlets, std::vector<int> const& ordered, std::vector<std::string> const& focus, int cap)
{
    TapChoice choice;
    int const limit = std::max(cap, 0);
    if (static_cast<int>(ordered.size()) <= limit) {
        choice.picked = ordered;
        return choice;
    }
    choice.sampled = true;
    std::vector<char> taken(outlets.size(), 0);
    std::vector<int> rest;
    int used = 0;
    for (int idx : ordered) {
        bool const wanted = std::find(focus.begin(), focus.end(), outlets[static_cast<size_t>(idx)].name) != focus.end();
        if (wanted && used < limit) {
            taken[static_cast<size_t>(idx)] = 1;
            ++used;
        } else if (!wanted) {
            rest.push_back(idx);
        }
    }
    int const slots = limit - used;
    int const n = static_cast<int>(rest.size());
    if (slots > 0 && n > 0) {
        if (slots >= n) {
            for (int idx : rest)
                taken[static_cast<size_t>(idx)] = 1;
        } else if (slots == 1) {
            taken[static_cast<size_t>(rest.back())] = 1;
        } else {
            for (int k = 0; k < slots; ++k) {
                int const at = static_cast<int>((static_cast<long long>(k) * (n - 1) + (slots - 1) / 2) / (slots - 1));
                taken[static_cast<size_t>(rest[static_cast<size_t>(at)])] = 1;
            }
        }
    }
    for (int idx : ordered)
        if (taken[static_cast<size_t>(idx)])
            choice.picked.push_back(idx);
    return choice;
}

} // namespace mcp::trace
