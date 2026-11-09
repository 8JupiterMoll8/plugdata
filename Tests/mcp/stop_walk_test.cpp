// Unit tests for the stop walk (truth slice 3, F1): where the measured walk from the speakers ends, as facts.
// JUCE-free and Pd-free, run by scripts/test-bridge-safety.sh.
#include "../../Source/Pd/MCPBridgeStopWalk.h"

#include <algorithm>
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

using namespace mcp::stops;

namespace {

// A small builder: nodes by id, wires by id. Every node gets the inlets and outlets it is declared with.
struct Builder {
    Graph g;
    int add(std::string id, int sigIn, int ctlIn, int sigOut, bool output = false)
    {
        Node n;
        n.id = std::move(id);
        n.output = output;
        for (int i = 0; i < sigIn; ++i) n.inlets.push_back({ true, 0 });
        for (int i = 0; i < ctlIn; ++i) n.inlets.push_back({ false, 0 });
        for (int i = 0; i < sigOut; ++i) n.outlets.push_back({ true, 0, Level::Unread, -100.0f });
        g.nodes.push_back(std::move(n));
        return static_cast<int>(g.nodes.size()) - 1;
    }
    // A signal wire; the source outlet's level is what its tap read.
    void wire(int src, int dst, int inlet = 0, int outlet = 0)
    {
        g.edges.push_back({ src, outlet, dst, inlet });
        ++g.nodes[static_cast<size_t>(src)].outlets[static_cast<size_t>(outlet)].wires;
        ++g.nodes[static_cast<size_t>(dst)].inlets[static_cast<size_t>(inlet)].wires;
    }
    // A control wire into an inlet (counts as a wire, carries no signal).
    void control(int dst, int inlet) { ++g.nodes[static_cast<size_t>(dst)].inlets[static_cast<size_t>(inlet)].wires; }
    void level(int node, Level l, float db = -100.0f, int outlet = 0)
    {
        auto& o = g.nodes[static_cast<size_t>(node)].outlets[static_cast<size_t>(outlet)];
        o.level = l;
        o.levelDb = db;
    }
};

bool hasLiveEnd(Result const& r, Graph const& g, std::string const& id)
{
    return std::any_of(r.liveEnds.begin(), r.liveEnds.end(), [&](int n) { return g.nodes[static_cast<size_t>(n)].id == id; });
}

int hopOf(Result const& r, Graph const& g, std::string const& id)
{
    for (auto const& row : r.rows)
        if (g.nodes[static_cast<size_t>(row.node)].id == id) return row.hop;
    return -99;
}

std::string stopId(Result const& r, Graph const& g)
{
    return r.stop.node >= 0 ? g.nodes[static_cast<size_t>(r.stop.node)].id : std::string();
}

} // namespace

int main()
{
    // L2 (session 2026-11-08): noise -> lop~ (unplugged) and [*~ 0.25] -> dac~ with no input; the dust branch dies
    // at its gain [*~] (the artist's slider at 0) one hop further up. The walk ends at the nearest stop: sig_8's open
    // inlet 0. The dust gain is a row with its own facts, never the stop; lop~ is a live end (always listed).
    {
        Builder b;
        int const dac = b.add("out_dac", 2, 0, 0, true);
        int const sig8 = b.add("sig_8", 1, 1, 1);         // [*~ 0.25]: inlet 1 is a control inlet
        int const noise = b.add("l2_noise", 1, 0, 1);
        int const lop = b.add("lp_lop", 1, 1, 1);
        int const dustSrc = b.add("dust_src", 1, 0, 1);
        int const dustG = b.add("dust_g", 2, 0, 1);       // [*~] without argument: two signal inlets
        int const dustOut = b.add("dust_out", 1, 1, 1);
        b.wire(sig8, dac, 0);
        b.wire(noise, lop, 0);
        b.wire(dustSrc, dustG, 0);
        b.control(dustG, 1); // the hsl at 0
        b.wire(dustG, dustOut, 0);
        b.wire(dustOut, dac, 1);
        b.level(sig8, Level::Silent);
        b.level(noise, Level::Audible, -6.0f);
        b.level(dustSrc, Level::Audible, -8.0f);
        b.level(dustG, Level::Silent);
        b.level(dustOut, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.hasStop);
        CHECK(r.stop.kind == StopKind::OpenInlet);
        CHECK(stopId(r, b.g) == "sig_8");
        CHECK(r.stop.hop == 1 && r.stop.inlet == 0);
        CHECK(r.liveEnds.size() == 1 && hasLiveEnd(r, b.g, "lp_lop"));
        CHECK(hopOf(r, b.g, "sig_8") == 1);
        CHECK(hopOf(r, b.g, "dust_out") == 1);
        CHECK(hopOf(r, b.g, "dust_g") == 2);
        CHECK(hopOf(r, b.g, "dust_src") == 3);
        CHECK(hopOf(r, b.g, "lp_lop") == 0); // the live end's own row (walk "liveEnd")
        bool liveRow = false;
        for (auto const& row : r.rows)
            if (b.g.nodes[static_cast<size_t>(row.node)].id == "lp_lop") liveRow = row.liveEnd;
        CHECK(liveRow);
        CHECK(r.refocus.empty());
        CHECK(r.rowsTruncated == 0);
        CHECK(std::string(kindName(r.stop.kind)) == "open_inlet");
        auto const open = openSignalInlets(b.g.nodes[static_cast<size_t>(sig8)]);
        CHECK(open.size() == 1 && open[0] == 0);
        CHECK(openSignalInlets(b.g.nodes[static_cast<size_t>(dustG)]).empty()); // inlet 1 has the hsl's wire
        CHECK(inputAudible(b.g, dustG) && !inputAudible(b.g, sig8));
    }

    // A VCA at 0: osc~ -> [*~] (inlet 1 fed by a silent envelope) -> dac~: the signal dies at the VCA.
    {
        Builder b;
        int const dac = b.add("dac", 2, 0, 0, true);
        int const osc = b.add("osc", 1, 1, 1);
        int const env = b.add("env", 0, 1, 1);
        int const vca = b.add("vca", 2, 0, 1);
        b.wire(osc, vca, 0);
        b.wire(env, vca, 1);
        b.wire(vca, dac, 0);
        b.wire(vca, dac, 1);
        b.level(osc, Level::Audible, -3.0f);
        b.level(env, Level::Silent);
        b.level(vca, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere);
        CHECK(stopId(r, b.g) == "vca" && r.stop.hop == 1 && r.stop.inlet == -1);
        CHECK(r.liveEnds.empty());
        CHECK(std::string(kindName(r.stop.kind)) == "dies_here");
        // The osc~'s inlet 0 is a signal inlet without a wire, but the osc sounds: no stop there.
        CHECK(hopOf(r, b.g, "osc") == 2 && hopOf(r, b.g, "env") == 2);
    }

    // A dc_only output counts as dead too: [+~ 0.5] fed by a silent node above an audible one.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const src = b.add("src", 0, 0, 1);
        int const dc = b.add("dc", 1, 1, 1);
        b.wire(src, dc, 0);
        b.wire(dc, dac, 0);
        b.level(src, Level::Audible, -10.0f);
        b.level(dc, Level::DcOnly);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "dc");
    }

    // Sound at the output: none, at the output itself (hop 0).
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const osc = b.add("osc", 1, 0, 1);
        b.wire(osc, dac, 0);
        b.level(osc, Level::Audible, -12.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::None && stopId(r, b.g) == "dac" && r.stop.hop == 0);
        CHECK(std::string(kindName(r.stop.kind)) == "none");
    }

    // Server review C: `none` is honest per branch. A bass sounds into dac~; a lead dies at hop 2 into the same dac~.
    // The output hears the bass, but the lead's stop is still reported (never hidden behind a sounding voice).
    {
        Builder b;
        int const dac = b.add("dac", 2, 0, 0, true);
        int const bass = b.add("bass", 1, 0, 1);
        int const leadOsc = b.add("lead_osc", 1, 0, 1);
        int const leadVca = b.add("lead_vca", 1, 1, 1);
        int const leadOut = b.add("lead_out", 1, 1, 1);
        b.wire(bass, dac, 0);
        b.wire(leadOsc, leadVca, 0);
        b.wire(leadVca, leadOut, 0);
        b.wire(leadOut, dac, 1);
        b.level(bass, Level::Audible, -9.0f);
        b.level(leadOsc, Level::Audible, -6.0f);
        b.level(leadVca, Level::Silent);
        b.level(leadOut, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "lead_vca" && r.stop.hop == 2);
        // Every branch sounds: none.
        b.level(leadVca, Level::Audible, -8.0f);
        b.level(leadOut, Level::Audible, -8.0f);
        auto const all = walk(b.g, { dac });
        CHECK(all.stop.kind == StopKind::None && stopId(all, b.g) == "dac");
    }

    // Two live ends, both listed (whatever the walk found); the output has no wire at all: the output's own inlet.
    {
        Builder b;
        int const dac = b.add("dac", 2, 0, 0, true);
        int const a = b.add("a_src", 0, 0, 1);
        int const fa = b.add("a_filt", 1, 1, 1);
        int const c = b.add("c_src", 0, 0, 1);
        int const fc = b.add("c_filt", 1, 0, 2); // two signal outlets, neither wired
        b.wire(a, fa, 0);
        b.wire(c, fc, 0);
        b.level(a, Level::Audible, -6.0f);
        b.level(c, Level::Audible, -9.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.liveEnds.size() == 2 && hasLiveEnd(r, b.g, "a_filt") && hasLiveEnd(r, b.g, "c_filt"));
        CHECK(r.stop.kind == StopKind::OpenInlet && stopId(r, b.g) == "dac" && r.stop.hop == 0 && r.stop.inlet == 0);
        (void)dac;
    }

    // A node with a wired signal outlet is never a live end, even when that wire leads nowhere useful; a sink with no
    // outlets is never one either.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const a = b.add("a", 0, 0, 1);
        int const t = b.add("throw", 1, 0, 0); // throw~: no outlets
        b.wire(a, t, 0);
        b.level(a, Level::Audible, -6.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.liveEnds.empty());
    }

    // A feedback loop: the walk terminates (visited set) and still finds the stop.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const sum = b.add("sum", 2, 0, 1);
        int const del = b.add("del", 1, 0, 1);
        int const vca = b.add("vca", 2, 0, 1);
        int const osc = b.add("osc", 1, 0, 1);
        b.wire(osc, vca, 0);
        b.wire(vca, sum, 0);
        b.wire(sum, del, 0);
        b.wire(del, sum, 1);
        b.wire(sum, dac, 0);
        b.level(osc, Level::Audible, -6.0f);
        b.level(vca, Level::Silent);
        b.level(sum, Level::Silent);
        b.level(del, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "vca");
        CHECK(hopOf(r, b.g, "sum") == 1 && hopOf(r, b.g, "del") == 2 && hopOf(r, b.g, "vca") == 2);
        int sumRows = 0;
        for (auto const& row : r.rows)
            if (row.node == sum) ++sumRows;
        CHECK(sumRows == 1);
    }

    // A 40-node chain: rows are capped at 30 (rowsTruncated = the rest); the stop is found past the cap all the same.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int prev = dac;
        std::vector<int> chain;
        for (int i = 0; i < 40; ++i) {
            int const n = b.add("n" + std::to_string(i), 1, 0, 1);
            b.wire(n, prev, 0);
            chain.push_back(n);
            prev = n;
        }
        for (int i = 0; i < 40; ++i) b.level(chain[static_cast<size_t>(i)], i >= 35 ? Level::Audible : Level::Silent, i >= 35 ? -6.0f : -100.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.rows.size() == 30);
        CHECK(r.rowsTruncated == 10);
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "n34" && r.stop.hop == 35);
        for (size_t k = 1; k < r.rows.size(); ++k) CHECK(r.rows[k - 1].hop <= r.rows[k].hop); // ordered by hop
    }

    // An untapped stretch: the outlets between the output and the sound were not tapped. Nothing is decided below
    // them: unread, at the nearest untapped node, and refocus names the ids to tap.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const a = b.add("a", 1, 0, 1);
        int const bb = b.add("b", 1, 0, 1);
        int const c = b.add("c", 1, 0, 1);
        int const osc = b.add("osc", 1, 0, 1);
        b.wire(osc, c, 0);
        b.wire(c, bb, 0);
        b.wire(bb, a, 0);
        b.wire(a, dac, 0);
        b.level(a, Level::Silent);
        b.level(osc, Level::Audible, -6.0f);
        // b and c: Level::Unread (not tapped)
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::Unread);
        CHECK(stopId(r, b.g) == "b" && r.stop.hop == 2);
        CHECK(r.refocus.size() == 2);
        CHECK(r.refocus.size() == 2 && b.g.nodes[static_cast<size_t>(r.refocus[0])].id == "b" && b.g.nodes[static_cast<size_t>(r.refocus[1])].id == "c");
        CHECK(std::string(kindName(r.stop.kind)) == "unread");
    }

    // A measured stop nearer than the untapped stretch wins (it is a fact on its own).
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const a = b.add("a", 1, 0, 1);
        int const u = b.add("u", 1, 0, 1);
        b.wire(u, a, 0);
        b.wire(a, dac, 0);
        b.level(a, Level::Silent);
        b.level(u, Level::Audible, -6.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "a");
        CHECK(r.refocus.empty());
    }

    // Same hop: a dying node is named before an open inlet (both are facts; the order is fixed, not weighted).
    {
        Builder b;
        int const dac = b.add("dac", 2, 0, 0, true);
        int const open = b.add("open", 1, 1, 1);
        int const vca = b.add("vca", 1, 1, 1);
        int const osc = b.add("osc", 0, 0, 1);
        b.wire(open, dac, 0);
        b.wire(vca, dac, 1);
        b.wire(osc, vca, 0);
        b.level(open, Level::Silent);
        b.level(vca, Level::Silent);
        b.level(osc, Level::Audible, -6.0f);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::DiesHere && stopId(r, b.g) == "vca");
    }

    // The walk ends at a silent source (no signal inlet, nothing wired into it): silent_source.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const vca = b.add("vca", 1, 1, 1);
        int const src = b.add("src", 0, 1, 1); // [sig~ 0]: a control inlet only
        b.wire(src, vca, 0);
        b.wire(vca, dac, 0);
        b.level(src, Level::Silent);
        b.level(vca, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::SilentSource && stopId(r, b.g) == "src" && r.stop.hop == 2);
        CHECK(std::string(kindName(r.stop.kind)) == "silent_source");
    }

    // Review M4: a bus. Two throw~ voices (bus outlets) into one catch~ -> *~ -> dac~; voice 1 dies at its VCA, voice 2's
    // throw~ has nothing plugged in. The walk crosses the bus: the nearest stop is voice 2's open throw~ inlet (hop 3),
    // voice 1's VCA is a row one hop further. A catch~ whose throws are elsewhere is a bus_end, not a silent source.
    {
        Builder b;
        int const dac = b.add("dac", 1, 0, 0, true);
        int const gain = b.add("gain", 1, 1, 1);
        int const cat = b.add("catch", 0, 1, 1);
        int const t1 = b.add("throw1", 1, 0, 1);
        int const t2 = b.add("throw2", 1, 0, 1);
        b.g.nodes[static_cast<size_t>(t1)].outlets[0].bus = true;
        b.g.nodes[static_cast<size_t>(t2)].outlets[0].bus = true;
        int const vca1 = b.add("vca1", 1, 1, 1);
        int const osc1 = b.add("osc1", 1, 0, 1);
        b.wire(osc1, vca1, 0);
        b.wire(vca1, t1, 0);
        b.wire(t1, cat, 0);
        b.wire(t2, cat, 0);
        b.wire(cat, gain, 0);
        b.wire(gain, dac, 0);
        b.level(osc1, Level::Audible, -6.0f);
        b.level(vca1, Level::Silent);
        b.level(t1, Level::Silent);
        b.level(t2, Level::Silent);
        b.level(cat, Level::Silent);
        b.level(gain, Level::Silent);
        auto const r = walk(b.g, { dac });
        CHECK(r.stop.kind == StopKind::OpenInlet && stopId(r, b.g) == "throw2" && r.stop.hop == 3);
        CHECK(hopOf(r, b.g, "catch") == 2 && hopOf(r, b.g, "vca1") == 4);
        CHECK(r.liveEnds.empty());

        Builder e;
        int const dac2 = e.add("dac", 1, 0, 0, true);
        int const c2 = e.add("catch", 0, 1, 1);
        e.g.nodes[static_cast<size_t>(c2)].busEnd = true;
        e.wire(c2, dac2, 0);
        e.level(c2, Level::Silent);
        auto const r2 = walk(e.g, { dac2 });
        CHECK(r2.stop.kind == StopKind::BusEnd && stopId(r2, e.g) == "catch");
        CHECK(std::string(kindName(r2.stop.kind)) == "bus_end");
    }

    // No output on the canvas: no stop (null), live ends still listed.
    {
        Builder b;
        int const a = b.add("a", 0, 0, 1);
        int const f = b.add("f", 1, 0, 1);
        b.wire(a, f, 0);
        b.level(a, Level::Audible, -6.0f);
        auto const r = walk(b.g, {});
        CHECK(!r.hasStop);
        CHECK(r.liveEnds.size() == 1 && hasLiveEnd(r, b.g, "f"));
    }

    // Two outputs: the hop is the distance to the NEAREST output; bad indices are ignored.
    {
        Builder b;
        int const d1 = b.add("d1", 1, 0, 0, true);
        int const d2 = b.add("d2", 1, 0, 0, true);
        int const x = b.add("x", 1, 0, 1);
        int const y = b.add("y", 1, 0, 1);
        b.wire(y, x, 0);
        b.wire(x, d1, 0);
        b.wire(y, d2, 0);
        b.level(x, Level::Silent);
        b.level(y, Level::Silent);
        b.g.edges.push_back({ 99, 0, 0, 0 }); // garbage: never walked
        auto const r = walk(b.g, { d1, d2, -1, 77 });
        CHECK(hopOf(r, b.g, "y") == 1 && hopOf(r, b.g, "x") == 1);
        CHECK(r.outputs.size() == 2);
    }

    std::printf(failures ? "stop_walk_test: %d FAILED\n" : "stop_walk_test: OK\n", failures);
    return failures ? 1 : 0;
}
