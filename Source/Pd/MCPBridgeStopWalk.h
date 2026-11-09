/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Truth slice 3 (F1): "where it stops" as facts. The armed trace taps the signal outlets and the sound judge
// (MCPBridgeSoundVerdict.h) gives each tap a verdict; this walk turns those readings into the place where the measured
// walk from the speakers ends. No weights, no advice, no "the one live end": every word is a reading or a wire count.
//
//   rows        every node upstream of an output (BFS over signal wires, `hop` = wires to the nearest output; a loop
//               is cut by the visited set), at most `cap`; then the live ends that are not on that walk (hop 0,
//               `liveEnd` true). rowsTruncated = the walk's rows left out.
//   nearestStop the lowest-hop node where the walk ends; at the same hop in this fixed order:
//                 dies_here     some signal input is audible, the output is silent or dc_only
//                 open_inlet    the output is silent (or dc_only) and a signal inlet has no wire at all (also an
//                               output whose signal inlets have no wire: hop 0)
//                 silent_source the output is silent (or dc_only) and no signal wire comes in
//               none: a signal wire into an output is audible (hop 0, the output). unread: an untapped node lies
//               nearer than any measured stop (or no stop was measured); `refocus` names the untapped nodes of the
//               walk to tap. No output on the canvas: no stop (`hasStop` false).
//   liveEnds    every node with an audible signal input, at least one signal outlet and no wire on any of them
//               (always listed, whatever the stop is: the reader connects the two, the walk does not).
//
// JUCE-free and Pd-free (Tests/mcp/stop_walk_test.cpp runs it with g++ alone). Message thread / worker, never audio.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace mcp::stops {

constexpr int kRowCap = 30;
constexpr int kRefocusCap = 16; // MAX_PROBES: a second armed read can tap no more

/** What the tap on one signal outlet said (Unread: not tapped; Unmeasured: tapped, no verdict). */
enum class Level { Unread, Unmeasured, Audible, Silent, DcOnly };

struct Inlet {
    bool signal = false;
    int wires = 0; // wires of ANY kind into this inlet (a control wire into a signal inlet sets its scalar)
};

struct Outlet {
    bool signal = false;
    int wires = 0;
    Level level = Level::Unread;
    float levelDb = -100.0f;
};

struct Node {
    std::string id;
    bool output = false; // dac~ / out~
    std::vector<Inlet> inlets;
    std::vector<Outlet> outlets;
};

/** One signal wire: node `src` outlet `srcOutlet` into node `dst` inlet `dstInlet`. */
struct Edge {
    int src = 0;
    int srcOutlet = 0;
    int dst = 0;
    int dstInlet = 0;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

enum class StopKind { DiesHere, OpenInlet, SilentSource, None, Unread };

inline char const* kindName(StopKind k)
{
    switch (k) {
    case StopKind::DiesHere: return "dies_here";
    case StopKind::OpenInlet: return "open_inlet";
    case StopKind::SilentSource: return "silent_source";
    case StopKind::None: return "none";
    case StopKind::Unread: return "unread";
    }
    return "unread";
}

struct Stop {
    StopKind kind = StopKind::Unread;
    int node = -1;
    int hop = 0;
    int inlet = -1; // open_inlet only
};

struct Row {
    int node = 0;
    int hop = 0;
    bool liveEnd = false; // a live end off the walk (hop 0)
};

struct Result {
    std::vector<int> outputs;
    bool hasStop = false;
    Stop stop;
    std::vector<Row> rows;
    int rowsTruncated = 0;
    std::vector<int> liveEnds;
    std::vector<int> refocus;
};

namespace detail {
inline bool valid(Graph const& g, int n) { return n >= 0 && static_cast<size_t>(n) < g.nodes.size(); }

inline bool validEdge(Graph const& g, Edge const& e)
{
    if (!valid(g, e.src) || !valid(g, e.dst) || e.srcOutlet < 0 || e.dstInlet < 0) return false;
    return static_cast<size_t>(e.srcOutlet) < g.nodes[static_cast<size_t>(e.src)].outlets.size();
}

inline Level edgeLevel(Graph const& g, Edge const& e)
{
    return g.nodes[static_cast<size_t>(e.src)].outlets[static_cast<size_t>(e.srcOutlet)].level;
}
} // namespace detail

/** The node's output as its taps read it: audible if any tapped signal outlet is, else dc_only, else silent. */
inline Level outLevel(Node const& n)
{
    bool read = false, dc = false, unmeasured = false;
    for (auto const& o : n.outlets) {
        if (!o.signal) continue;
        if (o.level == Level::Audible) return Level::Audible;
        if (o.level == Level::DcOnly) dc = true;
        if (o.level == Level::Silent || o.level == Level::DcOnly) read = true;
        if (o.level == Level::Unmeasured) unmeasured = true;
    }
    if (dc) return Level::DcOnly;
    if (read) return Level::Silent;
    return unmeasured ? Level::Unmeasured : Level::Unread;
}

inline bool deadOut(Level l) { return l == Level::Silent || l == Level::DcOnly; }

/** Some signal wire into node `n` comes from an outlet whose tap was audible. */
inline bool inputAudible(Graph const& g, int n)
{
    for (auto const& e : g.edges)
        if (e.dst == n && detail::validEdge(g, e) && detail::edgeLevel(g, e) == Level::Audible) return true;
    return false;
}

/** The signal inlets with no wire of any kind, in inlet order. */
inline std::vector<int> openSignalInlets(Node const& n)
{
    std::vector<int> out;
    for (size_t i = 0; i < n.inlets.size(); ++i)
        if (n.inlets[i].signal && n.inlets[i].wires == 0) out.push_back(static_cast<int>(i));
    return out;
}

inline int signalWiresIn(Graph const& g, int n)
{
    int count = 0;
    for (auto const& e : g.edges)
        if (e.dst == n && detail::validEdge(g, e)) ++count;
    return count;
}

/** Audible signal input, at least one signal outlet, no wire on any signal outlet. */
inline bool isLiveEnd(Graph const& g, int n)
{
    auto const& node = g.nodes[static_cast<size_t>(n)];
    if (node.output) return false;
    int signalOutlets = 0;
    for (auto const& o : node.outlets) {
        if (!o.signal) continue;
        ++signalOutlets;
        if (o.wires > 0) return false;
    }
    return signalOutlets > 0 && inputAudible(g, n);
}

inline Result walk(Graph const& g, std::vector<int> const& outputs, int cap = kRowCap)
{
    Result r;
    int const numNodes = static_cast<int>(g.nodes.size());
    for (int o : outputs)
        if (detail::valid(g, o) && std::find(r.outputs.begin(), r.outputs.end(), o) == r.outputs.end()) r.outputs.push_back(o);

    for (int n = 0; n < numNodes; ++n)
        if (isLiveEnd(g, n)) r.liveEnds.push_back(n);

    // BFS upstream from every output at once: hop = signal wires to the nearest output.
    std::vector<int> hop(static_cast<size_t>(numNodes), -1);
    std::vector<int> order; // walk nodes (not the outputs), in BFS order
    std::vector<int> frontier;
    for (int o : r.outputs) {
        hop[static_cast<size_t>(o)] = 0;
        frontier.push_back(o);
    }
    for (size_t head = 0; head < frontier.size(); ++head) {
        int const at = frontier[head];
        std::vector<Edge const*> in; // the wires into `at`, in inlet order (stable: wire order within an inlet)
        for (auto const& e : g.edges)
            if (e.dst == at && detail::validEdge(g, e)) in.push_back(&e);
        std::stable_sort(in.begin(), in.end(), [](Edge const* a, Edge const* b) { return a->dstInlet < b->dstInlet; });
        for (auto const* e : in) {
            if (hop[static_cast<size_t>(e->src)] >= 0) continue;
            hop[static_cast<size_t>(e->src)] = hop[static_cast<size_t>(at)] + 1;
            frontier.push_back(e->src);
            if (!g.nodes[static_cast<size_t>(e->src)].output) order.push_back(e->src);
        }
    }

    int const rowCap = cap < 0 ? 0 : cap;
    for (size_t k = 0; k < order.size(); ++k) {
        if (static_cast<int>(r.rows.size()) < rowCap)
            r.rows.push_back({ order[k], hop[static_cast<size_t>(order[k])], false });
        else
            ++r.rowsTruncated;
    }
    for (int n : r.liveEnds)
        if (hop[static_cast<size_t>(n)] < 0) r.rows.push_back({ n, 0, true });

    if (r.outputs.empty()) return r;
    r.hasStop = true;

    // Sound at an output: none.
    for (int o : r.outputs)
        if (inputAudible(g, o)) {
            r.stop = { StopKind::None, o, 0, -1 };
            return r;
        }

    // The measured candidates, nearest first; at one hop: dies_here, open_inlet, silent_source.
    Stop best;
    bool found = false;
    auto consider = [&](Stop const& s) {
        auto rank = [](StopKind k) { return k == StopKind::DiesHere ? 0 : k == StopKind::OpenInlet ? 1 : 2; };
        if (!found || s.hop < best.hop || (s.hop == best.hop && rank(s.kind) < rank(best.kind))) {
            best = s;
            found = true;
        }
    };
    for (int o : r.outputs)
        if (signalWiresIn(g, o) == 0) {
            auto const open = openSignalInlets(g.nodes[static_cast<size_t>(o)]);
            consider({ StopKind::OpenInlet, o, 0, open.empty() ? 0 : open.front() });
        }
    int nearestUnread = -1;
    for (int n : order) {
        auto const& node = g.nodes[static_cast<size_t>(n)];
        int const h = hop[static_cast<size_t>(n)];
        Level const out = outLevel(node);
        if (out == Level::Unread || out == Level::Unmeasured) {
            if (nearestUnread < 0 || h < hop[static_cast<size_t>(nearestUnread)]) nearestUnread = n;
            continue;
        }
        if (!deadOut(out)) continue;
        if (inputAudible(g, n)) {
            consider({ StopKind::DiesHere, n, h, -1 });
            continue;
        }
        auto const open = openSignalInlets(node);
        if (!open.empty()) {
            consider({ StopKind::OpenInlet, n, h, open.front() });
            continue;
        }
        if (signalWiresIn(g, n) == 0) consider({ StopKind::SilentSource, n, h, -1 });
    }

    if (nearestUnread >= 0 && (!found || hop[static_cast<size_t>(nearestUnread)] < best.hop)) {
        r.stop = { StopKind::Unread, nearestUnread, hop[static_cast<size_t>(nearestUnread)], -1 };
        for (int n : order) {
            Level const out = outLevel(g.nodes[static_cast<size_t>(n)]);
            if ((out == Level::Unread || out == Level::Unmeasured) && static_cast<int>(r.refocus.size()) < kRefocusCap)
                r.refocus.push_back(n);
        }
        return r;
    }
    if (found) {
        r.stop = best;
        return r;
    }
    // Every walked node is silent with wired inputs and no source was reached (should not happen on a finite graph,
    // but a malformed one must not invent a stop): unread at the nearest output.
    r.stop = { StopKind::Unread, r.outputs.front(), 0, -1 };
    return r;
}

} // namespace mcp::stops
