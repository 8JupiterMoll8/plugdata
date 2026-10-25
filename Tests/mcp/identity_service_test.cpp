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
    bool alive = true;    // in its canvas (objectLive)
    int generation = 0;   // bumped when "freed and reallocated at the same address"
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
    // S5: a per-object weak reference: valid while the object of THIS generation lives.
    hooks.objectGuard = [](Obj* o) {
        int const gen = o->generation;
        return std::function<bool()>([o, gen] { return o->alive && o->generation == gen; });
    };
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
        CHECK(core.resolve(&song, "osc1") == nullptr);
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
        CHECK(core.recordCount() == 1);
        CHECK(core.idOf(&a).empty());
        CHECK(core.idOf(&b) == "b");
        a.generation++; // a is freed: prune collects its serial
        core.prune();
        CHECK(!core.hasSerial(&a));
        CHECK(core.hasSerial(&b));
        a.generation = 0;
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
        CHECK(core.idOf(&c).empty());
        CHECK(core.idOf(&a) == "a" && core.idOf(&b) == "b");
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

    // ── S5: an id never resolves to a different object that later sits at the same address ──
    {
        Cnv song { "song.pd" };
        Obj o { 1 };
        Core core;
        hook(core);
        core.bind(&song, "osc1", &o);
        CHECK(core.resolve(&song, "osc1") == &o);
        o.generation++; // freed, and a new object allocated at the same address (still "in the canvas")
        CHECK(core.resolve(&song, "osc1") == nullptr);
        CHECK(core.findCanvas(&song)->count("osc1") == 0); // the stale entry is dropped
        // The new object gets its own id; the old id stays gone.
        auto const id = core.mint(&song, &o, "osc_sig");
        CHECK(id != "osc1");
        CHECK(core.resolve(&song, id) == &o);
        CHECK(core.resolve(&song, "osc1") == nullptr);
        CHECK(core.resolve(&song, "nope") == nullptr);
    }

    // ── S5: moveId (the retype hook): the id follows the object Pd recreated ──
    {
        Cnv song { "song.pd" };
        Obj before { 1 }, after { 2 };
        Core core;
        hook(core);
        core.bind(&song, "osc1", &before);
        before.alive = false; // retype frees the old box
        CHECK(core.moveId(&before, &after) == "osc1");
        CHECK(core.resolve(&song, "osc1") == &after);
        CHECK(core.moveId(&before, &after).empty()); // nothing bound to `before` any more
        // Same address (the allocator handed the freed memory to the new box): the id still follows.
        Obj same { 3 };
        core.bind(&song, "dac1", &same);
        same.generation++;
        CHECK(core.moveId(&same, &same) == "dac1");
        CHECK(core.resolve(&song, "dac1") == &same);
        // A retype that recreated nothing (same text): the old binding is still valid, the id stays put.
        Obj kept { 4 }, unrelated { 5 };
        core.bind(&song, "lop1", &kept);
        CHECK(core.moveId(&kept, &unrelated).empty());
        CHECK(core.resolve(&song, "lop1") == &kept);
    }

    // ── S5: prune also drops stale entries of live canvases (and bumps the version when it does) ──
    {
        Cnv song { "song.pd" };
        Obj p1 { 1 }, p2 { 2 };
        Core core;
        hook(core);
        core.bind(&song, "a", &p1);
        core.bind(&song, "b", &p2);
        auto const v = core.version();
        core.prune();
        CHECK(core.version() == v); // nothing stale: no bump
        p1.generation++;
        core.prune();
        CHECK(core.findCanvas(&song)->count("a") == 0);
        CHECK(core.findCanvas(&song)->count("b") == 1);
        CHECK(core.version() == v + 1);
        CHECK(core.valid(&song, "b"));
        CHECK(!core.valid(&song, "a"));
    }

    // ── S5 (L9): tombstones: an abstraction reload recreates the box at the same index; the id is rebound ──
    {
        Cnv song { "song.pd" };
        Obj absOld { 1 }, absNew { 2 };
        Core core;
        hook(core);
        core.bind(&song, "voice1", &absOld);
        core.addTombstone(&song, "voice1", 3, "voice.pd");
        CHECK(core.tombstones(&song).size() == 1);
        absOld.alive = false; // the reload freed it
        // the adapter finds the object at index 3 with the same tag and rebinds:
        auto stones = core.tombstones(&song);
        CHECK(stones[0].id == "voice1" && stones[0].index == 3 && stones[0].tag == "voice.pd");
        CHECK(core.bind(&song, "voice1", &absNew) == Core::BindResult::Bound);
        core.dropTombstone(&song, "voice1");
        CHECK(core.tombstones(&song).empty());
        CHECK(core.resolve(&song, "voice1") == &absNew);
        // A tombstone for an id that resolved again is dropped by the adapter; a dead canvas drops them all.
        core.addTombstone(&song, "x", 0, "t");
        song.alive = false;
        CHECK(core.tombstones(&song).empty());
    }

    // ── S6: per-canvas versions; every change bumps its canvas and the global counter; stamps catch GUI edits ──
    {
        Cnv x { "x.pd" }, y { "y.pd" };
        Obj o1 { 1 }, o2 { 2 };
        Core core;
        hook(core);
        CHECK(core.canvasVersion(&x) == 0);
        core.bind(&x, "a", &o1);
        CHECK(core.canvasVersion(&x) == 1);
        CHECK(core.canvasVersion(&y) == 0);
        core.mint(&y, &o2, "osc_sig");
        CHECK(core.canvasVersion(&y) == 1);
        CHECK(core.version() == 2);
        core.bumpVersion(&x);
        CHECK(core.canvasVersion(&x) == 2 && core.version() == 3);
        // Stamps: the first observation sets the baseline; a different stamp is a change (GUI edit) and bumps.
        CHECK(!core.observeStamp(&x, 111));
        CHECK(core.canvasVersion(&x) == 2);
        CHECK(!core.observeStamp(&x, 111));
        CHECK(core.observeStamp(&x, 222));
        CHECK(core.canvasVersion(&x) == 3 && core.version() == 4);
        // P11.5 V23: the content stamp (wires, x/y, text atoms) has its own per-canvas counter and a global one; it
        // never bumps the identity version. The first observation is the baseline.
        auto const idBefore = core.version();
        CHECK(core.contentVersion(&x) == 0);
        CHECK(!core.observeContent(&x, 7));
        CHECK(!core.observeContent(&x, 7));
        CHECK(core.observeContent(&x, 8));
        CHECK(core.contentVersion(&x) == 1 && core.contentVersionGlobal() == 1);
        CHECK(core.observeContent(&y, 5) == false && core.observeContent(&y, 6));
        CHECK(core.contentVersion(&y) == 1 && core.contentVersionGlobal() == 2);
        CHECK(core.version() == idBefore && core.canvasVersion(&x) == 3);
        // prune bumps the canvas whose entry went
        o1.generation++;
        core.prune();
        CHECK(core.canvasVersion(&x) == 4);
        CHECK(core.canvasVersion(&y) == 1);
    }

    // ── review H2: a second bind on a live object keeps its serial, so its other id stays valid; idOf skips invalid ──
    {
        Cnv song { "song.pd" };
        Obj o { 1 };
        Core core;
        hook(core);
        auto const minted = core.mint(&song, &o, "osc_sig");
        CHECK(core.bind(&song, "lead", &o) == Core::BindResult::Bound);
        CHECK(core.resolve(&song, minted) == &o);
        CHECK(core.resolve(&song, "lead") == &o);
        o.generation++; // freed, a new object at the same address, before any prune
        CHECK(core.idOf(&o).empty());
    }

    // ── review H1: moveId moves only the ids of the lifetime that just ended, never a live box's ──
    {
        Cnv song { "song.pd" };
        Obj o { 1 }, other { 2 };
        Core core;
        hook(core);
        core.bind(&song, "lead", &o);
        CHECK(core.moveId(&o, &other).empty()); // o still lives: nothing moves
        CHECK(core.resolve(&song, "lead") == &o);
        core.bind(&song, "x", &other);
        o.alive = false;
        CHECK(core.moveId(&o, &other).empty()); // `to` already has a valid id: refuse
        CHECK(core.resolve(&song, "x") == &other);
    }

    // ── review H3: eraseId and rename keep the bound serials in step; a dead target counts as free ──
    {
        Cnv song { "song.pd" };
        Obj dead { 1 }, fresh { 2 }, keep { 3 };
        Core core;
        hook(core);
        core.bind(&song, "lfo", &dead);
        core.eraseId(&song, "lfo");
        auto const m = core.mint(&song, &fresh, "osc_sig");
        CHECK(core.rename(&song, m, "lfo") == mcp::identity::RenameResult::Applied);
        CHECK(core.resolve(&song, "lfo") == &fresh);
        core.bind(&song, "gone", &dead);
        dead.alive = false;
        core.bind(&song, "k", &keep);
        CHECK(core.rename(&song, "k", "gone") == mcp::identity::RenameResult::Applied); // dead holder: free
        CHECK(core.resolve(&song, "gone") == &keep);
        Obj live2 { 4 };
        core.bind(&song, "taken", &live2);
        CHECK(core.rename(&song, "gone", "taken") == mcp::identity::RenameResult::Taken);
        CHECK(core.rename(&song, "nope", "x") == mcp::identity::RenameResult::Missing);
        CHECK(core.rename(&song, "taken", "taken") == mcp::identity::RenameResult::NoOp);
    }

    // ── review M1/M2: tombstones age; a fresh one survives a "still valid" look, an old one is dropped ──
    {
        Cnv song { "song.pd" };
        Core core;
        hook(core);
        core.addTombstone(&song, "v", 0, "t", 1000);
        CHECK(core.tombstones(&song).size() == 1);
        core.expireTombstones(1000 + 29000);
        CHECK(core.tombstones(&song).size() == 1);
        core.expireTombstones(1000 + 31000);
        CHECK(core.tombstones(&song).empty());
    }

    // ── review M3: refreshKey follows a canvas renamed behind the identity layer ──
    {
        Cnv song { "old.pd" };
        Core core;
        hook(core);
        core.canvasMap(&song);
        song.base = "new.pd";
        core.refreshKey(&song);
        CHECK(core.keyOf(&song) == "new.pd");
        CHECK(core.canvasByKey("new.pd") == &song);
    }

    if (failures == 0)
        std::printf("identity_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
