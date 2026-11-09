/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Truth slice 3 (F3): the trace's stopFacts block. See MCPBridgeStopFacts.h.

#include "MCPBridgeStopFacts.h"
#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeInletValues.h"
#include "MCPBridgeBound.h"
#include "MCPBridgeBroken.h"
#include "MCPBridgePdAccessors.h"

#include <cmath>

namespace mcp::stops {
namespace {

bool isOutputClass(juce::String const& c) { return c == "dac~" || c == "out~"; }

char const* levelName(Level l)
{
    switch (l) {
    case Level::Audible: return "audible";
    case Level::Silent: return "silent";
    case Level::DcOnly: return "dc_only";
    case Level::Unmeasured: return "unmeasured";
    case Level::Unread: return "unread";
    }
    return "unread";
}

juce::var dbVar(bool has, float db)
{
    if (!has || !std::isfinite(db)) return {};
    return std::round(static_cast<double>(std::max(db, -100.0f)) * 10.0) / 10.0;
}

// The verdict and level of one outlet as a JSON pair.
void putLevel(juce::DynamicObject* o, Outlet const& out)
{
    o->setProperty("verdict", juce::String(levelName(out.level)));
    bool const has = out.level == Level::Audible || out.level == Level::Silent;
    o->setProperty("levelDb", dbVar(has, out.levelDb));
}

// The node's output: the combined verdict (MCPBridgeStopWalk.h outLevel) and the level of the loudest read outlet.
juce::var outJson(Node const& n)
{
    auto* o = new juce::DynamicObject();
    Level const level = outLevel(n);
    o->setProperty("verdict", juce::String(levelName(level)));
    bool has = false;
    float db = -100.0f;
    for (auto const& out : n.outlets)
        if (out.signal && (out.level == Level::Audible || out.level == Level::Silent)) {
            db = has ? std::max(db, out.levelDb) : out.levelDb;
            has = true;
        }
    o->setProperty("levelDb", dbVar(has && level != Level::DcOnly, db));
    return juce::var(o);
}

struct ControlEdge {
    int src = 0;
    int srcOutlet = 0;
    int dst = 0;
    int dstInlet = 0;
    t_outconnect* oc = nullptr; // its record (pure-data/0005): the last message the wire carried
};

// The wires of the canvas, as the builder and the row words need them. One pass under the Pd lock.
struct Wiring {
    std::vector<ControlEdge> control;
};

bool isGuiFeeder(t_object* o)
{
    double v = 0.0;
    return o && mcpGuiValue(o, v);
}

// The name a bus object is created with (its first creation argument), or null. A $0 name is realised in the
// object's canvas (delta review 4: [throw~ $0-mix] -> [catch~ $0-mix]).
t_symbol* busName(t_canvas* cnv, t_gobj* g)
{
    t_object* o = pd_checkobject(&g->g_pd);
    if (!o || !o->te_binbuf || binbuf_getnatom(o->te_binbuf) < 2) return nullptr;
    t_atom const* a = binbuf_getvec(o->te_binbuf);
    if (a[1].a_type == A_SYMBOL) return a[1].a_w.w_symbol;
    if (a[1].a_type == A_DOLLSYM && cnv) return canvas_realizedollar(cnv, a[1].a_w.w_symbol);
    return nullptr;
}

bool bindsTo(t_symbol* name, t_gobj* g)
{
    for (auto* b : mcp::bound::bindersOf(name))
        if (b == &g->g_pd) return true;
    return false;
}

// Review M4: the walk crosses Pd's signal buses. throw~ N -> catch~ N (the catch~ binds N) and send~ N -> receive~ N
// (the send~ binds N), checked against the binding, on this canvas. A sender gets one virtual `bus` outlet whose level
// is its input's (it passes the signal on); a receiver whose senders are not on this canvas is a bus end.
void addBusEdges(Graph& g, t_canvas* cnv, CanvasObjects const& objects)
{
    int const n = static_cast<int>(g.nodes.size());
    auto isClass = [&](int i, std::initializer_list<char const*> names) {
        for (auto const* c : names)
            if (objects.classNames[static_cast<size_t>(i)] == c) return true;
        return false;
    };
    std::unordered_map<int, int> busOutlet; // sender node -> its bus outlet index
    for (int r = 0; r < n; ++r) {
        bool const isCatch = isClass(r, { "catch~" });
        bool const isReceive = isClass(r, { "receive~", "r~" });
        if (!isCatch && !isReceive) continue;
        t_symbol* name = busName(cnv, objects.objs[static_cast<size_t>(r)]);
        bool found = false;
        for (int sIdx = 0; name && sIdx < n; ++sIdx) {
            bool const sender = isCatch ? isClass(sIdx, { "throw~" }) : isClass(sIdx, { "send~", "s~" });
            if (!sender || busName(cnv, objects.objs[static_cast<size_t>(sIdx)]) != name) continue;
            // catch~ binds the name (a throw~ finds it there); send~ binds it (a receive~ finds it there).
            if (!bindsTo(name, objects.objs[static_cast<size_t>(isCatch ? r : sIdx)])) continue;
            auto& node = g.nodes[static_cast<size_t>(sIdx)];
            auto const at = busOutlet.find(sIdx);
            int k = 0;
            if (at == busOutlet.end()) {
                k = static_cast<int>(node.outlets.size());
                Outlet o;
                o.signal = true;
                o.bus = true;
                node.outlets.push_back(o);
                busOutlet[sIdx] = k;
            } else {
                k = at->second;
            }
            ++node.outlets[static_cast<size_t>(k)].wires;
            g.edges.push_back({ sIdx, k, r, 0 });
            found = true;
        }
        g.nodes[static_cast<size_t>(r)].busEnd = !found;
    }
    // A sender's bus level is its input: audible if any input is, unread if any is not read, else dc_only / silent.
    for (auto const& [sIdx, k] : busOutlet) {
        bool audible = false, unread = false, dc = false;
        float db = -100.0f;
        for (auto const& e : g.edges) {
            if (e.dst != sIdx) continue;
            auto const& src = g.nodes[static_cast<size_t>(e.src)].outlets[static_cast<size_t>(e.srcOutlet)];
            if (src.bus) continue;
            audible = audible || src.level == Level::Audible;
            unread = unread || src.level == Level::Unread || src.level == Level::Unmeasured;
            dc = dc || src.level == Level::DcOnly;
            if (src.level == Level::Audible || src.level == Level::Silent) db = std::max(db, src.levelDb);
        }
        auto& o = g.nodes[static_cast<size_t>(sIdx)].outlets[static_cast<size_t>(k)];
        o.level = audible ? Level::Audible : unread ? Level::Unread : dc ? Level::DcOnly : Level::Silent;
        o.levelDb = db;
    }
}

} // namespace

WalkSummary walkCanvas(t_canvas* cnv, CanvasObjects const& objects,
    std::unordered_map<std::string, mcp::verdict::Verdict> const& verdicts)
{
    WalkSummary out;
    auto& g = out.graph;
    int const numObjs = static_cast<int>(objects.objs.size());
    std::unordered_map<t_gobj*, int> indexOf;
    g.nodes.resize(static_cast<size_t>(numObjs));
    std::vector<int> outputs;
    for (int i = 0; i < numObjs; ++i) {
        t_gobj* gobj = objects.objs[static_cast<size_t>(i)];
        indexOf[gobj] = i;
        auto& node = g.nodes[static_cast<size_t>(i)];
        node.id = objects.names[static_cast<size_t>(i)].toStdString();
        node.output = isOutputClass(objects.classNames[static_cast<size_t>(i)]);
        if (node.output) outputs.push_back(i);
        t_object* o = pd_checkobject(&gobj->g_pd);
        if (!o) continue;
        int const nin = obj_ninlets(o);
        int const nout = obj_noutlets(o);
        for (int k = 0; k < nin; ++k)
            node.inlets.push_back({ obj_issignalinlet(o, k) != 0, 0 });
        for (int k = 0; k < nout; ++k)
            node.outlets.push_back({ obj_issignaloutlet(o, k) != 0, 0, Level::Unread, -100.0f });
    }

    t_linetraverser lt;
    linetraverser_start(&lt, cnv);
    while (linetraverser_next_nosize(&lt)) {
        auto const si = indexOf.find(&lt.tr_ob->ob_g);
        auto const di = indexOf.find(&lt.tr_ob2->ob_g);
        if (si == indexOf.end() || di == indexOf.end()) continue;
        auto& src = g.nodes[static_cast<size_t>(si->second)];
        auto& dst = g.nodes[static_cast<size_t>(di->second)];
        if (lt.tr_outno >= 0 && static_cast<size_t>(lt.tr_outno) < src.outlets.size()) ++src.outlets[static_cast<size_t>(lt.tr_outno)].wires;
        if (lt.tr_inno >= 0 && static_cast<size_t>(lt.tr_inno) < dst.inlets.size()) ++dst.inlets[static_cast<size_t>(lt.tr_inno)].wires;
        if (obj_issignaloutlet(lt.tr_ob, lt.tr_outno))
            g.edges.push_back({ si->second, lt.tr_outno, di->second, lt.tr_inno });
    }

    for (auto& node : g.nodes)
        for (size_t k = 0; k < node.outlets.size(); ++k) {
            auto& o = node.outlets[k];
            if (!o.signal || o.wires == 0) continue;
            auto const hit = verdicts.find(node.id + ":" + std::to_string(k));
            if (hit == verdicts.end()) continue; // not tapped: unread
            o.level = levelOf(hit->second);
            o.levelDb = hit->second.hasLevel ? hit->second.levelDb : -100.0f;
        }

    addBusEdges(g, cnv, objects);
    // Delta review 1: summing nodes ([+~], a catch~ with several throws, any signal inlet with several wires): a dead
    // voice's path may end there although the sum sounds from its other voices.
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        std::unordered_map<int, int> perInlet;
        for (auto const& e : g.edges)
            if (e.dst == static_cast<int>(i)) ++perInlet[e.dstInlet];
        bool many = false;
        for (auto const& [inlet, count] : perInlet) many = many || count > 1;
        g.nodes[i].sums = many || objects.classNames[i] == "+~";
    }
    out.result = walk(g, outputs, kRowCap);
    return out;
}

juce::String stopFactsJson(PluginProcessor* processor, t_canvas* cnv, CanvasObjects const& objects, WalkSummary const& w)
{
    auto const& g = w.graph;
    auto const& r = w.result;
    auto* host = processor->getMcp();
    auto idOf = [&](int n) { return juce::String(g.nodes[static_cast<size_t>(n)].id); };

    // The control wires into the rows (for ctl[].from) in one more pass.
    std::unordered_set<int> rowNodes;
    for (auto const& row : r.rows) rowNodes.insert(row.node);
    std::unordered_map<t_gobj*, int> indexOf;
    for (size_t i = 0; i < objects.objs.size(); ++i) indexOf[objects.objs[i]] = static_cast<int>(i);
    Wiring wiring;
    std::unordered_set<t_object*> wanted;
    for (int n : rowNodes)
        if (t_object* o = pd_checkobject(&objects.objs[static_cast<size_t>(n)]->g_pd)) wanted.insert(o);
    {
        t_linetraverser lt;
        linetraverser_start(&lt, cnv);
        while (t_outconnect* oc = linetraverser_next_nosize(&lt)) {
            if (obj_issignaloutlet(lt.tr_ob, lt.tr_outno)) continue;
            auto const di = indexOf.find(&lt.tr_ob2->ob_g);
            auto const si = indexOf.find(&lt.tr_ob->ob_g);
            if (di == indexOf.end() || si == indexOf.end() || !rowNodes.count(di->second)) continue;
            wiring.control.push_back({ si->second, lt.tr_outno, di->second, lt.tr_inno, oc });
        }
    }
    auto const inletWires = mcp::inlets::wiresInto(cnv, wanted);
    // The last message per inlet (slice 3 F2): `by` of an inlet's value.
    auto const lastRecords = mcp::inlets::lastInto(cnv, wanted, [&](t_object* o) {
        auto const it = indexOf.find(&o->ob_g);
        return it == indexOf.end() ? juce::String() : idOf(it->second);
    });
    auto byOf = [&](unsigned int seq) { auto const by = host->seqBy(seq); return by ? juce::var(*by) : juce::var(); };
    bool const canvasOn = mcp_canvas_switchedon_chain(cnv) != 0;

    // handMoved of a GUI feeder: true = no wire in, no receive name, and a value other than the AI's last; false = the
    // AI's last value; null = cannot tell (no AI value recorded, or a wire / a receive name could have moved it).
    auto handMoved = [&](int src) -> juce::var {
        t_gobj* gobj = objects.objs[static_cast<size_t>(src)];
        t_object* o = pd_checkobject(&gobj->g_pd);
        double now = 0.0;
        if (!o || !mcpGuiValue(o, now)) return {};
        auto const ai = host->aiGuiValue(gobj);
        if (!ai) return {};
        if (std::abs(now - *ai) <= 1e-6 * std::max(1.0, std::abs(*ai))) return false;
        for (auto const& in : g.nodes[static_cast<size_t>(src)].inlets)
            if (in.wires > 0) return {};
        if (!mcp::bound::namesBoundTo(o, cnv).empty()) return {};
        return true;
    };

    juce::Array<juce::var> rows;
    for (auto const& row : r.rows) {
        int const n = row.node;
        auto const& node = g.nodes[static_cast<size_t>(n)];
        t_gobj* gobj = objects.objs[static_cast<size_t>(n)];
        t_object* o = pd_checkobject(&gobj->g_pd);
        auto* ro = new juce::DynamicObject();
        ro->setProperty("tempId", idOf(n));
        ro->setProperty("class", objects.classNames[static_cast<size_t>(n)]);
        ro->setProperty("hop", row.hop);
        if (row.liveEnd) ro->setProperty("liveEnd", true);
        ro->setProperty("made", juce::String(host->aiIntact(gobj) ? "ai" : "other"));
        ro->setProperty("out", outJson(node));
        int outWires = 0;
        for (auto const& out : node.outlets)
            if (out.signal && !out.bus) outWires += out.wires;
        ro->setProperty("outWires", outWires);
        juce::Array<juce::var> open;
        for (int k : openSignalInlets(node)) open.add(k);
        ro->setProperty("openSignalInlets", open);

        juce::Array<juce::var> sig;
        for (size_t k = 0; k < node.inlets.size(); ++k) {
            if (!node.inlets[k].signal) continue;
            juce::Array<juce::var> from;
            for (auto const& e : g.edges) {
                if (e.dst != n || static_cast<size_t>(e.dstInlet) != k) continue;
                if (g.nodes[static_cast<size_t>(e.src)].outlets[static_cast<size_t>(e.srcOutlet)].bus) continue; // in `bus`
                auto* f = new juce::DynamicObject();
                f->setProperty("id", idOf(e.src) + ":" + juce::String(e.srcOutlet));
                putLevel(f, g.nodes[static_cast<size_t>(e.src)].outlets[static_cast<size_t>(e.srcOutlet)]);
                from.add(juce::var(f));
            }
            auto* s = new juce::DynamicObject();
            s->setProperty("inlet", static_cast<int>(k));
            s->setProperty("from", from);
            sig.add(juce::var(s));
        }
        ro->setProperty("sig", sig);
        // Review M4: what this bus receiver gets from its senders (throw~ -> catch~, send~ -> receive~).
        juce::Array<juce::var> bus;
        for (auto const& e : g.edges) {
            auto const& src = g.nodes[static_cast<size_t>(e.src)].outlets[static_cast<size_t>(e.srcOutlet)];
            if (e.dst != n || !src.bus) continue;
            auto* f = new juce::DynamicObject();
            f->setProperty("id", idOf(e.src) + ":bus");
            putLevel(f, src);
            bus.add(juce::var(f));
        }
        if (!bus.isEmpty() || node.busEnd) ro->setProperty("bus", bus);

        juce::Array<juce::var> ctl;
        if (o) {
            auto const wit = inletWires.find(o);
            auto const inlets = mcp::inlets::objectInlets(o, wit == inletWires.end() ? std::unordered_map<int, mcp::inlets::Wires> {} : wit->second);
            if (auto const* list = inlets.getArray())
                for (auto const& in : *list) {
                    int const k = static_cast<int>(in["index"]);
                    int const controlWires = static_cast<int>(in["controlWires"]);
                    int const signalWires = static_cast<int>(in["signalWires"]);
                    bool const holds = !in["value"].isVoid();
                    if (controlWires == 0 && !(holds && signalWires == 0)) continue;
                    auto* c = new juce::DynamicObject();
                    c->setProperty("inlet", k);
                    c->setProperty("value", in["value"]);
                    c->setProperty("source", holds ? in["source"] : juce::var()); // void when the inlet keeps no copy
                    juce::var by;
                    if (auto const lo = lastRecords.find(o); lo != lastRecords.end())
                        if (auto const li = lo->second.find(k); li != lo->second.end() && li->second.has) by = byOf(li->second.seq);
                    c->setProperty("by", by);
                    juce::Array<juce::var> from;
                    for (auto const& e : wiring.control) {
                        if (e.dst != n || e.dstInlet != k) continue;
                        auto* f = new juce::DynamicObject();
                        f->setProperty("id", idOf(e.src) + ":" + juce::String(e.srcOutlet));
                        t_object* so = pd_checkobject(&objects.objs[static_cast<size_t>(e.src)]->g_pd);
                        mcp::inlets::Last rec;
                        rec.has = mcp_outconnect_last(e.oc, &rec.sel, &rec.argc, rec.atoms, &rec.seq) != 0;
                        double v = 0.0;
                        juce::var value; // a GUI's own value; else the first atom the wire last carried
                        if (so && mcpGuiValue(so, v)) value = v;
                        else if (auto const n = mcp::inlets::lastNumber(rec)) value = *n;
                        f->setProperty("value", value);
                        f->setProperty("by", rec.has ? byOf(rec.seq) : juce::var());
                        f->setProperty("handMoved", isGuiFeeder(so) ? handMoved(e.src) : juce::var());
                        from.add(juce::var(f));
                    }
                    c->setProperty("from", from);
                    ctl.add(juce::var(c));
                }
        }
        ro->setProperty("ctl", ctl);

        juce::Array<juce::var> names;
        for (auto const& name : mcp::bound::namesBoundTo(o, cnv)) names.add(name);
        ro->setProperty("names", names);
        ro->setProperty("redBox", mcp::broken::isRedBox(gobj));
        bool on = canvasOn;
        if (pd_class(&gobj->g_pd) == canvas_class) on = mcp_canvas_switchedon_chain(reinterpret_cast<t_canvas*>(gobj)) != 0;
        ro->setProperty("switchedOff", !on);
        rows.add(juce::var(ro));
    }

    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> outs;
    for (int o : r.outputs) outs.add(idOf(o));
    root->setProperty("outputs", outs);
    if (r.hasStop && r.stop.node >= 0) {
        auto* s = new juce::DynamicObject();
        s->setProperty("tempId", idOf(r.stop.node));
        s->setProperty("kind", juce::String(kindName(r.stop.kind)));
        s->setProperty("hop", r.stop.hop);
        if (r.stop.kind == StopKind::OpenInlet) s->setProperty("inlet", r.stop.inlet);
        root->setProperty("nearestStop", juce::var(s));
    } else {
        root->setProperty("nearestStop", juce::var());
    }
    root->setProperty("rows", rows);
    juce::Array<juce::var> live;
    for (int n : r.liveEnds) live.add(idOf(n));
    root->setProperty("liveEnds", live);
    root->setProperty("rowsTruncated", r.rowsTruncated);
    juce::Array<juce::var> refocus;
    for (int n : r.refocus) refocus.add(idOf(n));
    root->setProperty("refocus", refocus);
    return juce::JSON::toString(juce::var(root), true);
}

} // namespace mcp::stops
