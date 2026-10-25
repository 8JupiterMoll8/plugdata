/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P11.4 V19: /pd/bound <corrId> <name...> asks Pd who listens on each name ("is anything listening" without a model
// of receiver classes in TS).
//
// One walk per call, under the Pd lock: every object of every open root canvas, recursively into subpatches and
// abstractions, and into clone instances (clone_get_n / clone_get_instance), is indexed by its t_pd address. Then
// per name the symbol's binding is read: a single binder is s_thing; several binders are Pd's bindlist, detected by
// its class NAME ("bindlist", the class is static in m_pd.c) and read through McpBindlistView, whose layout copies
// m_pd.c's t_bindlist (Tests/mcp/bridge_handler_tests.cpp runBoundCases pins it with three [r] on one name: a Pd
// update that changes the struct fails there, loudly). A binder found on a canvas is a receiver (counted); a canvas's
// own pd-<name> binding, the bridge's control probes and any binder that is not an object on a canvas are `other`.
// The symbol is looked up without creating it (a name nothing ever used stays out of Pd's symbol table).
// Reply: /pd/bound/reply/<corrId> {"names":[{name, bound, count, receivers:[{canvasKey, path, index, class,
// tempId?}], other:[{kind}]}], walkObjects, walkUs, truncated?}; at most kMaxBoundNames names per call.

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeControlWindow.h" // mcpCtrlReceiverClass: the bridge's own control probes

#include <chrono>
#include <cstring>

namespace {

constexpr int kMaxBoundNames = 64;

// The layout of t_bindlist in Libraries/pure-data/src/m_pd.c (static there). Pinned by runBoundCases.
struct McpBindlistView {
    t_pd b_pd;
    t_pd** b_vec;
    int b_n;
};

// gensym's lookup (m_class.c dogensym: djb2 over the bytes, SYMTABHASHSIZE buckets) without the insert.
t_symbol* findSymbolNoCreate(char const* s)
{
    if (!s || !pd_this || !pd_this->pd_symhash)
        return nullptr;
    unsigned int hash = 5381;
    for (char const* p = s; *p; ++p)
        hash = ((hash << 5) + hash) + static_cast<unsigned int>(*p); // as dogensym: a char added (sign included)
    for (t_symbol* sym = pd_this->pd_symhash[hash & (SYMTABHASHSIZE - 1)]; sym; sym = sym->s_next)
        if (std::strcmp(sym->s_name, s) == 0)
            return sym;
    return nullptr;
}

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

void indexCanvas(t_canvas* c, juce::String const& path, std::unordered_map<t_pd const*, Where>& where, int depth)
{
    if (!c || depth > 64)
        return;
    int index = 0;
    for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
        where[&g->g_pd] = Where { c, index, path };
        t_class* cl = pd_class(&g->g_pd);
        if (cl == canvas_class) {
            indexCanvas(reinterpret_cast<t_canvas*>(g), path + "/" + containerLabel(g), where, depth + 1);
        } else if (cl == clone_class) {
            int const n = clone_get_n(g);
            for (int i = 0; i < n; ++i)
                if (auto* inst = clone_get_instance(g, i))
                    indexCanvas(reinterpret_cast<t_canvas*>(inst), path + "/[clone]#" + juce::String(i), where, depth + 1);
        }
    }
}

std::vector<t_pd*> bindersOf(t_symbol* sym)
{
    std::vector<t_pd*> out;
    if (!sym || !sym->s_thing)
        return out;
    t_pd* thing = sym->s_thing;
    char const* cls = class_getname(*thing);
    if (cls && std::strcmp(cls, "bindlist") == 0) {
        auto const* list = reinterpret_cast<McpBindlistView const*>(thing);
        if (list->b_vec && list->b_n > 0 && list->b_n < 1000000)
            for (int i = 0; i < list->b_n; ++i)
                if (list->b_vec[i])
                    out.push_back(list->b_vec[i]);
        return out;
    }
    out.push_back(thing);
    return out;
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
    {
        McpHost::PdLockScope lock(*processor);
        auto const t0 = std::chrono::steady_clock::now();
        std::unordered_map<t_pd const*, Where> where;
        where.reserve(4096);
        std::vector<t_canvas*> roots; // pd_canvaslist is newest first: report oldest first
        for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
            roots.push_back(c);
        for (auto it = roots.rbegin(); it != roots.rend(); ++it)
            indexCanvas(*it, canonicalCanvasKey(*it), where, 0);
        walkObjects = static_cast<int>(where.size());

        auto& identity = processor->getMcp()->identity;
        for (auto const& name : names) {
            juce::Array<juce::var> receivers, other;
            for (t_pd* binder : bindersOf(findSymbolNoCreate(name.toRawUTF8()))) {
                t_class* cl = *binder;
                auto const found = where.find(binder);
                if (found != where.end() && cl != canvas_class) {
                    auto* r = new juce::DynamicObject();
                    r->setProperty("canvasKey", canonicalCanvasKey(found->second.owner));
                    r->setProperty("path", found->second.path);
                    r->setProperty("index", found->second.index);
                    r->setProperty("class", juce::String::fromUTF8(class_getname(cl)));
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
                else
                    o->setProperty("kind", juce::String::fromUTF8(class_getname(cl)));
                other.add(juce::var(o));
            }
            auto* e = new juce::DynamicObject();
            e->setProperty("name", name);
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
    if (truncated)
        root->setProperty("truncated", true);
    sendReply("/pd/bound/reply/" + correlationId, juce::JSON::toString(juce::var(root), true));
}
