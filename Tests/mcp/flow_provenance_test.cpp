// Unit tests for P12a A1-A3 (flow mode, fork side), JUCE-free:
//  - A1 provenance: the identity core's per-lifetime origin (ai | unknown), set only on a live serial, never carried by
//    a name, reset when a new object takes a freed address or a retype starts a new lifetime, stamped with the boot id;
//  - A2 the ownership predicate (McpOwnership.h): which object counts as the AI's own, and why not;
//  - A3 the flow-mode switch (McpFlowMode.h): off at start, on only through the artist's chip, off from anywhere;
//    the flow chip's nonce prefix is reserved.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpIdentityCore.h"
#include "../../Source/Mcp/McpOwnership.h"
#include "../../Source/Mcp/McpFlowMode.h"
#include "../../Source/Mcp/McpApprovalState.h"

#include <cstdio>
#include <functional>
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
    bool alive = true;
    int generation = 0;
};
struct Cnv {
    std::string base;
    bool alive = true;
};
using Core = mcp::identity::IdentityCore<Obj*, Cnv*>;
using Origin = Core::Origin;

static void hook(Core& core)
{
    Core::Hooks hooks;
    hooks.baseKey = [](Cnv* c) { return c->base; };
    hooks.liveness = [](Cnv* c) { return std::function<bool()>([c] { return c->alive; }); };
    hooks.objectLive = [](Cnv*, Obj* o) { return o->alive; };
    hooks.objectGuard = [](Obj* o) {
        int const gen = o->generation;
        return std::function<bool()>([o, gen] { return o->alive && o->generation == gen; });
    };
    core.setHooks(hooks);
    core.setBootId(42);
}

static void provenance()
{
    std::printf("A1 provenance (identity core)\n");
    // Default: unknown. A bound (hand-made, minted, registered) object is unknown until the batch create marks it.
    {
        Cnv c { "song.pd" };
        Obj a { 1 }, b { 2 };
        Core core;
        hook(core);
        CHECK(core.originOf(&a) == Origin::Unknown); // no serial at all
        core.bind(&c, "osc1", &a);
        CHECK(core.originOf(&a) == Origin::Unknown);
        core.mint(&c, &b, "osc~");
        CHECK(core.originOf(&b) == Origin::Unknown);
        CHECK(core.setOriginAi(&a));
        CHECK(core.originOf(&a) == Origin::Ai);
        CHECK(core.originOf(&b) == Origin::Unknown);
        // Never without a live serial.
        Obj loose { 3 };
        CHECK(!core.setOriginAi(&loose));
        CHECK(core.originOf(&loose) == Origin::Unknown);
        CHECK(!core.setOriginAi(nullptr));
    }
    // A name never carries it: a rename keeps it on the object; a second id (register_id under an AI-looking name)
    // on an artist object gives nothing; binding the AI's id to another object is refused (theft) or gives nothing.
    {
        Cnv c { "song.pd" };
        Obj ai { 1 }, artist { 2 };
        Core core;
        hook(core);
        core.bind(&c, "osc_ai", &ai);
        core.setOriginAi(&ai);
        core.mint(&c, &artist, "osc~");
        CHECK(core.rename(&c, "osc_ai", "renamed") == mcp::identity::RenameResult::Applied);
        CHECK(core.originOf(&ai) == Origin::Ai);
        CHECK(core.bind(&c, "osc_ai", &artist) == Core::BindResult::Bound); // the name is free again: an artist box takes it
        CHECK(core.originOf(&artist) == Origin::Unknown);
        CHECK(core.bind(&c, "renamed", &artist) == Core::BindResult::Refused);
        CHECK(core.originOf(&artist) == Origin::Unknown);
        // rename an artist id onto a name that looks like the AI's: still unknown
        CHECK(core.rename(&c, "osc_ai", "osc_ai_2") == mcp::identity::RenameResult::Applied);
        CHECK(core.originOf(&artist) == Origin::Unknown);
    }
    // A freed AI box whose address a new hand-made box reuses: unknown (a new lifetime gets a new serial).
    {
        Cnv c { "song.pd" };
        Obj slot { 1 };
        Core core;
        hook(core);
        core.bind(&c, "osc1", &slot);
        core.setOriginAi(&slot);
        slot.generation++; // freed and reallocated at the same address
        CHECK(core.originOf(&slot) == Origin::Unknown);
        core.mint(&c, &slot, "osc~");
        CHECK(core.originOf(&slot) == Origin::Unknown);
        // and the batch delete path erases the serial: the same holds after eraseSerial
        core.setOriginAi(&slot);
        core.eraseSerial(&slot);
        core.mint(&c, &slot, "osc~");
        CHECK(core.originOf(&slot) == Origin::Unknown);
    }
    // GUI retype (moveId): the id moves to the new box, the new lifetime is unknown; also when Pd reuses the address.
    {
        Cnv c { "song.pd" };
        Obj from { 1 }, to { 2 };
        Core core;
        hook(core);
        core.bind(&c, "osc1", &from);
        core.setOriginAi(&from);
        from.alive = false;
        CHECK(core.moveId(&from, &to) == "osc1");
        CHECK(core.originOf(&to) == Origin::Unknown);
        Obj same { 3 };
        core.bind(&c, "osc2", &same);
        core.setOriginAi(&same);
        same.generation++; // recreated at the same address
        CHECK(core.moveId(&same, &same) == "osc2");
        CHECK(core.originOf(&same) == Origin::Unknown);
    }
    // assignSerial on an existing (dead) entry resets the origin.
    {
        Obj a { 1 };
        Core core;
        hook(core);
        core.ensureSerial(&a);
        core.setOriginAi(&a);
        core.assignSerial(&a);
        CHECK(core.originOf(&a) == Origin::Unknown);
    }
    // clearOrigin (encapsulate, undo rebind) and the boot id: another boot's mark is not this session's.
    {
        Cnv c { "song.pd" };
        Obj a { 1 };
        Core core;
        hook(core);
        core.bind(&c, "x", &a);
        core.setOriginAi(&a);
        core.clearOrigin(&a);
        CHECK(core.originOf(&a) == Origin::Unknown);
        core.setOriginAi(&a);
        core.setBootId(43);
        CHECK(core.originOf(&a) == Origin::Unknown);
        core.setBootId(42);
        CHECK(core.originOf(&a) == Origin::Ai);
        core.setBootId(0); // no boot id: nothing is the AI's
        CHECK(!core.setOriginAi(&a));
        CHECK(core.originOf(&a) == Origin::Unknown);
    }
}

static void ownership()
{
    std::printf("A2 ownership predicate\n");
    using namespace mcp::ownership;
    // The data holders: a superset of the TS isTableClass (table, array define; mutation-budget.ts).
    CHECK(isDataHolder("canvas", "table tbl1 100"));
    CHECK(isDataHolder("array define", "array define -k buf 100"));
    CHECK(isDataHolder("garray", ""));
    CHECK(isDataHolder("text define", "text define -k notes"));
    CHECK(isDataHolder("qlist", "qlist"));
    CHECK(isDataHolder("textfile", "textfile"));
    CHECK(isDataHolder("coll", "coll seq"));
    CHECK(isDataHolder("cyclone/coll", "cyclone/coll seq"));
    CHECK(isDataHolder("buffer~", "buffer~ smp 1000"));
    CHECK(isDataHolder("cyclone/buffer~", "cyclone/buffer~ smp"));
    CHECK(isDataHolder("table", "table t"));
    CHECK(isDataHolder("else/buffer", "else/buffer b"));
    CHECK(isDataHolder("list store", "list store 1 2 3"));
    CHECK(!isDataHolder("osc~", "osc~ 440"));
    CHECK(!isDataHolder("array get", "array get buf"));
    CHECK(!isDataHolder("tabread~", "tabread~ buf"));
    CHECK(!isDataHolder("text", "text get notes")); // a comment or [text get] holds nothing

    Facts f;
    CHECK(decide(f) == Why::NotFound);
    f.found = true;
    CHECK(decide(f) == Why::OriginUnknown);
    f.originAi = true;
    CHECK(decide(f) == Why::AiOwned);
    CHECK(std::string(whyName(Why::AiOwned)) == "ai_own");
    {
        auto g = f;
        g.ambiguousTab = true;
        CHECK(decide(g) == Why::AmbiguousTab);
    }
    {
        auto g = f;
        g.container = true;
        CHECK(decide(g) == Why::Container);
    }
    {
        auto g = f;
        g.dataHolder = true;
        CHECK(decide(g) == Why::DataHolder);
    }
    {
        auto g = f;
        g.nonAiInWire = true;
        CHECK(decide(g) == Why::ArtistInWire);
    }
    // Everything at once still never owns.
    Facts all { true, true, true, true, true, true };
    CHECK(decide(all) != Why::AiOwned);
    for (auto w : { Why::NotFound, Why::OriginUnknown, Why::AmbiguousTab, Why::Container, Why::DataHolder, Why::ArtistInWire, Why::AiOwned })
        CHECK(std::string(whyName(w)).size() > 0);
}

static void flowMode()
{
    std::printf("A3 flow-mode switch\n");
    {
        mcp::FlowMode f; // a new run: off
        CHECK(!f.isOn());
        CHECK(f.sinceMs() == 0);
        CHECK(!f.off()); // off when off: nothing changed
        f.artistTurnOn(1000);
        CHECK(f.isOn() && f.sinceMs() == 1000);
        f.artistTurnOn(2000); // already on: keeps its start
        CHECK(f.sinceMs() == 1000);
        CHECK(f.off());
        CHECK(!f.isOn() && f.sinceMs() == 0);
    }
    {
        mcp::FlowMode restarted; // a restart builds a new state: off, whatever the last run had
        CHECK(!restarted.isOn());
    }
    // The flow chip's nonce prefix is reserved; a server nonce never has it.
    CHECK(mcp::isFlowNonce(mcp::makeFlowNonce(0x1234abcdULL, 0x55ULL)));
    CHECK(mcp::makeFlowNonce(1, 2) != mcp::makeFlowNonce(1, 3));
    CHECK(!mcp::isFlowNonce("n_a"));
    CHECK(!mcp::isFlowNonce(""));
    // A chip shown by the server (show() from JSON) is never a flow offer; replacing a flow chip by nonce makes it a
    // plain chip (the safe direction).
    {
        mcp::ApprovalState s;
        mcp::ApprovalChip offer;
        offer.nonce = mcp::makeFlowNonce(7, 8);
        offer.flowOffer = true;
        CHECK(s.show(offer, 60000, 1000));
        CHECK(s.visible(1000).size() == 1 && s.visible(1000)[0].flowOffer);
        CHECK(s.show(offer.nonce, "pd-main", "x", {}, 60000, 1000));
        CHECK(s.visible(1000).size() == 1 && !s.visible(1000)[0].flowOffer);
    }
}

int main()
{
    provenance();
    ownership();
    flowMode();
    if (failures == 0)
        std::printf("flow_provenance_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
