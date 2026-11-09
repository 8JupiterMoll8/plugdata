/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P11.4 V19: /pd/bound <corrId> <name...> asks Pd who listens on each name ("is anything listening" without a model
// of receiver classes in TS).
//
// Under the Pd lock, per name the symbol's binding is read first; a name nothing binds is answered without a walk.
// Then one walk over every open root canvas, recursively into subpatches and abstractions and into clone instances
// (clone_get_n / clone_get_instance), looks for exactly those binders and stops when all are found (review M3; a
// budget of objects and time stops it too: `truncated`, binders not reached are `unresolved`). The binding: a single binder is s_thing; several binders are Pd's bindlist, detected by
// its class NAME ("bindlist", the class is static in m_pd.c) and read through McpBindlistView, whose layout copies
// m_pd.c's t_bindlist (Tests/mcp/bridge_handler_tests.cpp runBoundCases pins it with three [r] on one name: a Pd
// update that changes the struct fails there, loudly). A binder found on a canvas is a receiver (counted); a canvas's
// own pd-<name> binding, the bridge's control probes and any binder that is not an object on a canvas are `other`.
// The symbol is looked up without creating it (a name nothing ever used stays out of Pd's symbol table).
// Reply: /pd/bound/reply/<corrId> {"names":[{name, bound, count, receivers:[{canvasKey, path, index, class, kind,
// tempId?}], other:[{kind, class}]}], walkObjects (objects visited), walkUs, truncated?}; at most kMaxBoundNames names
// per call. kind of a receiver: control, signal, table (arrays, tables) or delay (delwrite~).

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeControlWindow.h" // mcpCtrlReceiverClass: the bridge's own control probes
#include "MCPBridgeBound.h" // bindersOf, findSymbolNoCreate (truth slice 3: shared with the stop facts and send_checked)

#include <chrono>
#include <cstring>

namespace {

constexpr int kMaxBoundNames = 64;

using mcp::bound::bindersOf;
using mcp::bound::findSymbolNoCreate;

struct Where {
    t_canvas* owner = nullptr;
    int index = 0;
    juce::String path;
};

juce::String containerLabel(t_gobj* g)
{
    juce::String abstraction;
    if (pd::getAbstractionFileName(g, abstraction) && abstraction.isNotEmpty())
        return "[" + abstraction + "]";
    auto* c = reinterpret_cast<t_canvas*>(g);
    juce::String const name = c->gl_name && c->gl_name->s_name ? juce::String::fromUTF8(c->gl_name->s_name) : juce::String();
    return name.isNotEmpty() ? "[pd " + name + "]" : juce::String("[pd]");
}

// Review M3: the walk looks only for the binders the asked names have, stops as soon as all are found, and stops at a
// budget (objects visited, milliseconds); a stop at the budget is `truncated` and the binders not reached are
// `unresolved`. The budget's object count can be lowered for a test (PLUGDATA_MCP_BOUND_BUDGET_OBJECTS).
struct Walk {
    std::unordered_set<t_pd const*> const& wanted;
    std::unordered_map<t_pd const*, Where> found;
    int visited = 0;
    int maxObjects = 200000;
    double maxMs = 25.0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    bool truncated = false;

    bool done() const { return found.size() == wanted.size() || truncated; }

    void canvas(t_canvas* c, juce::String const& path, int depth)
    {
        if (!c || depth > 64)
            return;
        int index = 0;
        for (t_gobj* g = c->gl_list; g && !done(); g = g->g_next, ++index) {
            if (++visited > maxObjects
                || ((visited & 255) == 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > maxMs)) {
                truncated = true;
                return;
            }
            if (wanted.count(&g->g_pd) != 0)
                found[&g->g_pd] = Where { c, index, path };
            t_class* cl = pd_class(&g->g_pd);
            if (cl == canvas_class) {
                canvas(reinterpret_cast<t_canvas*>(g), path + "/" + containerLabel(g), depth + 1);
            } else if (cl == clone_class) {
                int const n = clone_get_n(g);
                for (int i = 0; i < n && !done(); ++i)
                    if (auto* inst = mcpCloneInstance(g, i))
                        canvas(reinterpret_cast<t_canvas*>(inst), path + "/[clone]#" + juce::String(i), depth + 1);
            }
        }
    }
};

// What a name is to its receiver: a delay line, a table/array, a signal bus, or a control name.
juce::String receiverKind(char const* cls)
{
    juce::String const c = juce::String::fromUTF8(cls ? cls : "");
    if (c == "delwrite~" || c == "sigdelwrite~")
        return "delay";
    if (c == "array" || c == "garray" || c == "array define" || c == "text define" || c == "table")
        return "table";
    if (c.endsWithChar('~'))
        return "signal";
    return "control";
}

} // namespace

void MCPBridge::pdBound(const juce::String& action, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1)
        return;
    auto const correlationId = getArgString(msg[0]);
    juce::StringArray names;
    for (int i = 1; i < msg.size() && names.size() < kMaxBoundNames; ++i) {
        auto const n = getArgString(msg[i]);
        if (n.isNotEmpty() && !names.contains(n))
            names.add(n);
    }
    bool const truncated = msg.size() - 1 > kMaxBoundNames;

    juce::Array<juce::var> list;
    int walkObjects = 0;
    double walkUs = 0.0;
    bool truncatedWalk = false;
    {
        McpHost::PdLockScope lock(*processor);
        auto const t0 = std::chrono::steady_clock::now();
        // 1. The binders of each name (no walk yet); a name with none is answered without one.
        std::vector<std::vector<t_pd*>> binders;
        std::unordered_set<t_pd const*> wanted;
        for (auto const& name : names) {
            binders.push_back(bindersOf(findSymbolNoCreate(name.toRawUTF8())));
            for (auto* b : binders.back())
                if (*b != canvas_class && !(mcpCtrlReceiverClass && *b == mcpCtrlReceiverClass))
                    wanted.insert(b);
        }
        // 2. One walk for exactly those binders, oldest root first, early exit, budget.
        Walk walk { wanted };
        if (char const* env = std::getenv("PLUGDATA_MCP_BOUND_BUDGET_OBJECTS"); env && std::atoi(env) > 0)
            walk.maxObjects = std::atoi(env);
        if (!wanted.empty()) {
            std::vector<t_canvas*> roots; // pd_canvaslist is newest first
            for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
                roots.push_back(c);
            for (auto it = roots.rbegin(); it != roots.rend() && !walk.done(); ++it)
                walk.canvas(*it, juce::String(processor->getMcp()->identity.keyOf(*it)), 0); // P11.6 F10: the wire key
        }
        walkObjects = walk.visited;
        truncatedWalk = walk.truncated;

        auto& identity = processor->getMcp()->identity;
        for (size_t n = 0; n < names.size(); ++n) {
            juce::Array<juce::var> receivers, other;
            for (t_pd* binder : binders[n]) {
                t_class* cl = *binder;
                auto const found = walk.found.find(binder);
                if (found != walk.found.end() && cl != canvas_class) {
                    auto* r = new juce::DynamicObject();
                    // P11.6 F10: the identity wire key (unique: "twin.pd#2" for a second tab of that name).
                    r->setProperty("canvasKey", juce::String(identity.keyOf(found->second.owner)));
                    r->setProperty("path", found->second.path);
                    r->setProperty("index", found->second.index);
                    r->setProperty("class", juce::String::fromUTF8(class_getname(cl)));
                    r->setProperty("kind", receiverKind(class_getname(cl)));
                    auto const id = identity.idOf(reinterpret_cast<t_gobj*>(binder));
                    if (!id.empty())
                        r->setProperty("tempId", juce::String(id));
                    receivers.add(juce::var(r));
                    continue;
                }
                auto* o = new juce::DynamicObject();
                if (cl == canvas_class)
                    o->setProperty("kind", "canvas");
                else if (mcpCtrlReceiverClass && cl == mcpCtrlReceiverClass)
                    o->setProperty("kind", "bridge_probe");
                else if (walk.truncated)
                    o->setProperty("kind", "unresolved"); // the budget stopped the walk before it was reached
                else
                    o->setProperty("kind", juce::String::fromUTF8(class_getname(cl)));
                o->setProperty("class", juce::String::fromUTF8(class_getname(cl)));
                other.add(juce::var(o));
            }
            auto* e = new juce::DynamicObject();
            e->setProperty("name", names[static_cast<int>(n)]);
            e->setProperty("bound", !receivers.isEmpty());
            e->setProperty("count", receivers.size());
            e->setProperty("receivers", receivers);
            e->setProperty("other", other);
            list.add(juce::var(e));
        }
        walkUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("names", list);
    root->setProperty("walkObjects", walkObjects);
    root->setProperty("walkUs", walkUs);
    if (truncated || truncatedWalk)
        root->setProperty("truncated", true); // too many names, or the walk stopped at its budget
    sendReply("/pd/bound/reply/" + correlationId, juce::JSON::toString(juce::var(root), true));
}
