// Unit tests for the identity service core (Plan 3 P3.5). The core owns the tempId maps, the serials and the
// identity version (S1: moved out of McpSession unchanged). S3 keys them by the live canvas: one record per
// canvas with a wire key ("song.pd", "pd-voice", a "#n" suffix for duplicates, kept for the canvas's life) and a
// liveness check, so a closed canvas, even one whose address is reused, never lends its ids to another.
// Fake handles stand in for t_gobj* and t_canvas*.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpIdentityCore.h"
#include "../../Source/Pd/MCPIdentity.h"

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
};
struct Cnv {
    std::string base; // what the adapter's key function answers
    bool alive = true;
};
using Core = mcp::identity::IdentityCore<Obj*, Cnv*>;

static void hook(Core& core)
{
    Core::Hooks hooks;
    hooks.baseKey = [](Cnv* c) { return c->base; };
    hooks.liveness = [](Cnv* c) { return std::function<bool()>([c] { return c->alive; }); };
    hooks.objectLive = [](Cnv*, Obj* o) { return o->alive; };
    core.setHooks(hooks);
}

int main()
{
    Obj a { 1 }, b { 2 }, c { 3 };

    // ── bind: id -> object in the canvas's record, a serial for the object, one version bump ──
    {
        Cnv song { "song.pd" };
        Core core;
        hook(core);
        CHECK(core.version() == 0);
        core.bind(&song, "osc1", &a);
        CHECK(core.version() == 1);
        CHECK(core.hasSerial(&a));
        auto const* m = core.findCanvas(&song);
        CHECK(m && m->size() == 1 && m->at("osc1") == &a);
        CHECK(core.idOf(&a) == "osc1");
        CHECK(core.idOf(&b).empty());
        CHECK(core.idOf(nullptr).empty());
        CHECK(core.keyOf(&song) == "song.pd");
    }

    // ── #38 (S4): binding an id that names another LIVE object is refused; a dead holder is replaced ──
    {
        Cnv song { "song.pd" };
        Core core;
        hook(core);
        CHECK(core.bind(&song, "osc1", &a) == Core::BindResult::Bound);
        CHECK(core.bind(&song, "osc1", &a) == Core::BindResult::Same);
        CHECK(core.bind(&song, "osc1", &b) == Core::BindResult::Refused);
        CHECK(core.findCanvas(&song)->at("osc1") == &a);
        CHECK(core.version() == 1); // neither the repeat nor the refusal bumps
        Obj dead { 9, false };
        Core core2;
        hook(core2);
        core2.bind(&song, "x", &dead);
        CHECK(core2.bind(&song, "x", &b) == Core::BindResult::Bound);
        CHECK(core2.findCanvas(&song)->at("x") == &b);
        CHECK(!core2.hasSerial(&dead));
    }

    // ── one minter (S4, owner decision O1): <prefix>_<n>, n counts per canvas, never reused, skips taken names,
    //    starts above the highest number already in use ──
    {
        Cnv song { "song.pd" }, other { "other.pd" };
        Obj d { 4 }, e { 5 }, f { 6 };
        Core core;
        hook(core);
        CHECK(core.mint(&song, &a, "osc_sig") == "osc_sig_1");
        CHECK(core.mint(&song, &b, "hsl") == "hsl_2");
        CHECK(core.idOf(&b) == "hsl_2");
        CHECK(core.mint(&song, &b, "hsl") == "hsl_2"); // already named: its id, no new mint
        CHECK(core.mint(&other, &c, "osc_sig") == "osc_sig_1"); // another canvas counts on its own
        core.findCanvas(&song)->erase("osc_sig_1");             // deleted: the number is not reused
        CHECK(core.mint(&song, &d, "osc_sig") == "osc_sig_3");
        core.bind(&song, "dac_4", &e);                          // a name that would collide is skipped
        CHECK(core.mint(&song, &f, "dac") == "dac_5");
        Cnv loaded { "loaded.pd" };
        Obj g { 7 }, h { 8 };
        core.bind(&loaded, "osc_sig_12", &g); // ids restored from a saved patch: the counter starts above them
        CHECK(core.mint(&loaded, &h, "osc_sig") == "osc_sig_13");
        CHECK(core.version() > 0);
    }

    // ── findCanvas never creates; canvasMap does ──
    {
        Cnv x { "x.pd" };
        Core core;
        hook(core);
        CHECK(core.findCanvas(&x) == nullptr);
        core.canvasMap(&x);
        CHECK(core.findCanvas(&x) != nullptr && core.findCanvas(&x)->empty());
        Core const& constCore = core;
        CHECK(constCore.findCanvas(&x) != nullptr);
    }

    // ── #62: two canvases with the same name get two records; the second is "#2", for its whole life ──
    {
        Cnv v1 { "pd-voice" }, v2 { "pd-voice" }, v3 { "pd-voice" };
        Core core;
        hook(core);
        core.bind(&v1, "x", &a);
        core.bind(&v2, "x", &b); // same id on the other canvas: no clash, no eviction
        CHECK(core.keyOf(&v1) == "pd-voice");
        CHECK(core.keyOf(&v2) == "pd-voice#2");
        CHECK(core.findCanvas(&v1)->at("x") == &a);
        CHECK(core.findCanvas(&v2)->at("x") == &b);
        CHECK(core.canvasByKey("pd-voice") == &v1);
        CHECK(core.canvasByKey("pd-voice#2") == &v2);
        // The first one closes: the second keeps "#2" (no renumbering under the AI's feet) ...
        v1.alive = false;
        CHECK(core.keyOf(&v2) == "pd-voice#2");
        CHECK(core.canvasByKey("pd-voice") == nullptr);
        CHECK(core.canvasByKey("pd-voice#2") == &v2);
        // ... and a new one takes the lowest free number.
        CHECK(core.keyOf(&v3) == "pd-voice");
    }

    // ── a dead record never lends its ids: not to idOf, not to a new canvas at the same address ──
    {
        Cnv song { "song.pd" };
        Core core;
        hook(core);
        core.bind(&song, "osc1", &a);
        song.alive = false;
        CHECK(core.findCanvas(&song) == nullptr);
        CHECK(core.idOf(&a).empty());
        song.alive = true; // "another canvas" allocated at the same address
        CHECK(core.findCanvas(&song) == nullptr);
        CHECK(core.canvasMap(&song).empty());
        CHECK(!core.hasSerial(&a));
    }

    // ── prune drops dead records with their serials ──
    {
        Cnv x { "x.pd" }, y { "y.pd" };
        Core core;
        hook(core);
        core.bind(&x, "a", &a);
        core.bind(&y, "b", &b);
        x.alive = false;
        core.prune();
        CHECK(!core.hasSerial(&a));
        CHECK(core.hasSerial(&b));
        CHECK(core.recordCount() == 1);
    }

    // ── canvasByKey: exact key, or the "pd-" spelling of a root / a bare subpatch name ──
    {
        Cnv root { "song.pd" }, sub { "pd-voice" };
        Core core;
        hook(core);
        core.canvasMap(&root);
        core.canvasMap(&sub);
        CHECK(core.canvasByKey("song.pd") == &root);
        CHECK(core.canvasByKey("pd-song.pd") == &root);
        CHECK(core.canvasByKey("pd-voice") == &sub);
        CHECK(core.canvasByKey("voice") == &sub);
        CHECK(core.canvasByKey("nope") == nullptr);
        CHECK(core.canvasByKey("") == nullptr);
    }

    // ── clearCanvas: drops the ids and their serials, keeps the record and its key ──
    {
        Cnv x { "pd-voice" }, x2 { "pd-voice" }, y { "y.pd" };
        Core core;
        hook(core);
        core.bind(&x, "a", &a);
        core.bind(&x2, "c", &c);
        core.bind(&y, "b", &b);
        CHECK(core.clearCanvas(&x2));
        CHECK(core.findCanvas(&x2) && core.findCanvas(&x2)->empty());
        CHECK(core.keyOf(&x2) == "pd-voice#2");
        CHECK(!core.hasSerial(&c));
        CHECK(core.hasSerial(&a) && core.hasSerial(&b));
        Cnv never { "z.pd" };
        CHECK(!core.clearCanvas(&never));
    }

    // ── rekey: save-as renames a root; the record and its ids stay ──
    {
        Cnv song { "old.pd" };
        Core core;
        hook(core);
        core.bind(&song, "x", &a);
        song.base = "new.pd";
        core.rekey(&song);
        CHECK(core.keyOf(&song) == "new.pd");
        CHECK(core.canvasByKey("new.pd") == &song);
        CHECK(core.canvasByKey("old.pd") == nullptr);
        CHECK(core.findCanvas(&song)->at("x") == &a);
    }

    // ── serials: assign, ensure keeps an existing one, erase ──
    {
        Core core;
        hook(core);
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

    // ── idOf scans every live record (the reverse lookup of getStableId) ──
    {
        Cnv x { "a.pd" }, s { "pd-sub" };
        Core core;
        hook(core);
        core.bind(&x, "x", &a);
        core.bind(&s, "z", &c);
        CHECK(core.idOf(&c) == "z");
    }

    // ── the rename helper works on a canvas map handed out by the core ──
    {
        Cnv x { "a.pd" };
        Core core;
        hook(core);
        core.bind(&x, "x", &a);
        auto r = mcp::identity::renameStableId(core.canvasMap(&x), std::string("x"), std::string("carrier"));
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

    // ── the access check (S2: the audit build reports an access without the Pd lock) sees every access but the
    //    atomic version read ──
    {
        static int calls = 0;
        static std::string last;
        static bool sawBind = false;
        Cnv x { "a.pd" };
        Core core;
        hook(core);
        core.setAccessCheck([](char const* where) { ++calls; last = where; sawBind = sawBind || last == "bind"; });
        core.bind(&x, "x", &a);
        CHECK(calls >= 1 && sawBind);
        int const before = calls;
        (void)core.version();
        CHECK(calls == before);
        (void)core.idOf(&a);
        CHECK(last == "idOf");
        (void)core.findCanvas(&x);
        CHECK(last == "findCanvas");
        core.canvasMap(&x);
        CHECK(last == "canvasMap");
        core.clearCanvas(&x);
        CHECK(last == "clearCanvas");
        (void)core.canvasByKey("a.pd");
        CHECK(last == "canvasByKey");
        core.setAccessCheck(nullptr);
        int const after = calls;
        core.bind(&x, "y", &b);
        CHECK(calls == after);
    }

    // ── without hooks every canvas is alive and keys are empty (no crash) ──
    {
        Core bare;
        Cnv x { "a.pd" };
        bare.bind(&x, "x", &a);
        CHECK(bare.idOf(&a) == "x");
    }

    if (failures == 0)
        std::printf("identity_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
