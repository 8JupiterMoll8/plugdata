// Unit tests for the identity service core (Plan 3 P3.5 S1). The core owns the tempId maps, the serials and the
// identity version that used to be three McpSession fields; S1 moves them behind it without changing behaviour,
// so these tests pin the behaviour the call sites relied on. Fake object handles stand in for t_gobj*.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpIdentityCore.h"
#include "../../Source/Pd/MCPIdentity.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct Obj {
    int n;
};
using Core = mcp::identity::IdentityCore<Obj*>;

int main()
{
    Obj a { 1 }, b { 2 }, c { 3 };

    // ── bind: id -> object in the canvas map, a serial for the object, one version bump ──
    {
        Core core;
        CHECK(core.version() == 0);
        core.bind("song.pd", "osc1", &a);
        CHECK(core.version() == 1);
        CHECK(core.hasSerial(&a));
        auto const* m = core.findCanvas("song.pd");
        CHECK(m && m->size() == 1 && m->at("osc1") == &a);
        CHECK(core.idOf(&a) == "osc1");
        CHECK(core.idOf(&b).empty());
        CHECK(core.idOf(nullptr).empty());
    }

    // ── a rebind of the same id overwrites today (theft refusal comes in S4) ──
    {
        Core core;
        core.bind("song.pd", "osc1", &a);
        core.bind("song.pd", "osc1", &b);
        CHECK(core.findCanvas("song.pd")->at("osc1") == &b);
        CHECK(core.version() == 2);
    }

    // ── findCanvas never creates; canvasMap does ──
    {
        Core core;
        CHECK(core.findCanvas("x") == nullptr);
        core.canvasMap("x");
        CHECK(core.findCanvas("x") != nullptr && core.findCanvas("x")->empty());
        Core const& constCore = core;
        CHECK(constCore.findCanvas("x") != nullptr);
    }

    // ── serials: assign, ensure keeps an existing one, erase ──
    {
        Core core;
        CHECK(!core.hasSerial(&a));
        core.ensureSerial(&a);
        CHECK(core.hasSerial(&a));
        auto const first = core.serialOf(&a);
        core.ensureSerial(&a);
        CHECK(core.serialOf(&a) == first);
        core.assignSerial(&a);
        CHECK(core.serialOf(&a) != first);
        core.eraseSerial(&a);
        CHECK(!core.hasSerial(&a));
        CHECK(core.serialOf(&a) == 0);
    }

    // ── eraseCanvas drops the ids and their serials, leaves other canvases ──
    {
        Core core;
        core.bind("a.pd", "x", &a);
        core.bind("b.pd", "y", &b);
        CHECK(core.eraseCanvas("a.pd"));
        CHECK(!core.eraseCanvas("a.pd"));
        CHECK(core.findCanvas("a.pd") == nullptr);
        CHECK(!core.hasSerial(&a));
        CHECK(core.hasSerial(&b));
        CHECK(core.idOf(&b) == "y");
    }

    // ── moveCanvas: save-as moves the map to the new key; same key or a missing key does nothing ──
    {
        Core core;
        core.bind("old.pd", "x", &a);
        core.moveCanvas("old.pd", "new.pd");
        CHECK(core.findCanvas("old.pd") == nullptr);
        CHECK(core.findCanvas("new.pd") && core.findCanvas("new.pd")->at("x") == &a);
        core.moveCanvas("new.pd", "new.pd");
        CHECK(core.findCanvas("new.pd") && core.findCanvas("new.pd")->size() == 1);
        core.moveCanvas("missing.pd", "other.pd");
        CHECK(core.findCanvas("other.pd") == nullptr);
    }

    // ── idOf scans every canvas (the reverse lookup of getStableId) ──
    {
        Core core;
        core.bind("a.pd", "x", &a);
        core.bind("pd-sub", "z", &c);
        CHECK(core.idOf(&c) == "z");
    }

    // ── the rename helper works on a canvas map handed out by the core ──
    {
        Core core;
        core.bind("a.pd", "x", &a);
        auto r = mcp::identity::renameStableId(core.canvasMap("a.pd"), std::string("x"), std::string("carrier"));
        CHECK(r == mcp::identity::RenameResult::Applied);
        CHECK(core.idOf(&a) == "carrier");
    }

    // ── bumpVersion alone ──
    {
        Core core;
        core.bumpVersion();
        core.bumpVersion();
        CHECK(core.version() == 2);
    }

    if (failures == 0)
        std::printf("identity_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
