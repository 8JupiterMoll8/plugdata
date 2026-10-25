/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Identity service core (Plan 3 P3.5). Owns everything the bridge knows about tempIds: per live canvas a record
// with its wire key and its map id -> object, a serial per bound object, and the identity version TS polls.
// JUCE-free and templated on the object and canvas handles, so Tests/mcp/identity_service_test.cpp runs with
// plain g++ on fake handles; the plugdata side is mcp::IdentityService (McpIdentity.h), which installs the hooks.
//
// S1: the three McpSession fields moved here unchanged. S2: every access but version() calls the access check
// (the audit build reports an access without the Pd lock). S3 (#62): records are keyed by the live canvas, not
// by a name. A record's wire key is the adapter's base key ("song.pd", "pd-voice"); a second live canvas with
// the same base key gets "#2" (the lowest free number), kept for the canvas's life. A record whose liveness
// check fails (the canvas was freed, even if a new canvas now sits at the same address) is dropped on sight
// with its serials, so it never lends its ids.

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../Pd/MCPIdentity.h" // RenameResult

namespace mcp::identity {

template <typename Obj, typename Canvas>
class IdentityCore {
public:
    using IdMap = std::unordered_map<std::string, Obj>;

    struct Hooks {
        // The canvas's key before any "#n" ("song.pd", "pd-voice", "pd-anon").
        std::function<std::string(Canvas)> baseKey;
        // Called once when a record is made: the returned check answers whether THAT canvas still lives.
        std::function<std::function<bool()>(Canvas)> liveness;
        // S4: whether `obj` is still a live object of canvas `c` (the theft check). Unset = always live.
        std::function<bool(Canvas, Obj)> objectLive;
        // S5: called when an object gets a serial: the returned check answers whether THAT object still lives
        // (a weak reference: false once it is freed, even if a new object later gets the same address).
        // Unset = always live.
        std::function<std::function<bool()>(Obj)> objectGuard;
    };

    struct Tombstone {
        std::string id;
        int index = -1;
        std::string tag; // the adapter's match key (an abstraction's file name)
        uint64_t bornMs = 0;
    };

    // Milliseconds on a steady clock (tombstone ages).
    static uint64_t nowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void setHooks(Hooks h) { hooks = std::move(h); }

    // The id map of a live canvas; makes its record (and wire key) when missing.
    IdMap& canvasMap(Canvas c)
    {
        check("canvasMap");
        return ensure(c).ids;
    }

    // The id map of a live canvas, or nullptr; never makes a record.
    IdMap* findCanvas(Canvas c)
    {
        check("findCanvas");
        auto* r = live(c);
        return r ? &r->ids : nullptr;
    }
    IdMap const* findCanvas(Canvas c) const
    {
        check("findCanvas");
        auto it = records.find(c);
        return it != records.end() && it->second.isAlive() ? &it->second.ids : nullptr;
    }

    // The wire key of a live canvas (makes its record when missing).
    std::string keyOf(Canvas c)
    {
        check("keyOf");
        return ensure(c).key;
    }

    // The live canvas whose wire key is `name`, or its "pd-" spelling ("pd-song.pd" for a root "song.pd",
    // "voice" for "pd-voice"); a null handle when none. Only canvases that already have a record are found:
    // the adapter makes records for every open canvas before it resolves a name.
    Canvas canvasByKey(std::string const& name) const
    {
        check("canvasByKey");
        if (name.empty())
            return Canvas {};
        Canvas found {};
        for (auto const& [c, r] : records) {
            if (!r.isAlive())
                continue;
            if (r.key == name)
                return c;
            if (!found && ("pd-" + r.key == name || r.key == "pd-" + name))
                found = c;
        }
        return found;
    }

    // Drops a canvas's ids; the record and its key stay. True when the canvas had a record.
    // No version bump.
    bool clearCanvas(Canvas c)
    {
        check("clearCanvas");
        auto* r = live(c);
        if (!r)
            return false;
        r->ids.clear();
        r->boundSerial.clear();
        return true;
    }

    // Save-as renamed the canvas: recompute its key (a new "#n" if another live canvas holds the new name).
    void rekey(Canvas c)
    {
        check("rekey");
        auto* r = live(c);
        if (!r)
            return;
        r->key.clear();
        r->base = baseOf(c);
        r->key = freeKey(r->base);
    }

    // Review M3: a canvas renamed behind the identity layer (GUI Save-As, a subpatch renamed by hand) gets a new
    // key when the adapter next walks it.
    void refreshKey(Canvas c)
    {
        check("refreshKey");
        auto* r = live(c);
        if (r && baseOf(c) != r->base)
            rekey(c);
    }

    // Drops every record whose canvas is gone and (S5) every entry of a live canvas whose object is gone (one
    // version bump when any entry went), then the serials of freed objects.
    void prune()
    {
        check("prune");
        bool dropped = false;
        for (auto it = records.begin(); it != records.end();) {
            if (!it->second.isAlive()) {
                it = records.erase(it);
                continue;
            }
            auto& r = it->second;
            bool droppedHere = false;
            for (auto e = r.ids.begin(); e != r.ids.end();) {
                if (entryValid(r, e->first, e->second)) {
                    ++e;
                    continue;
                }
                r.boundSerial.erase(e->first);
                e = r.ids.erase(e);
                droppedHere = true;
            }
            if (droppedHere)
                ++r.version;
            dropped = dropped || droppedHere;
            ++it;
        }
        // S5: serials are collected only here, by their own weak reference: dropping a record or an entry must
        // not erase a serial by address (the address may already belong to a new object bound elsewhere).
        if (hooks.objectGuard)
            for (auto s = serials.begin(); s != serials.end();)
                s = s->second.isAlive() ? std::next(s) : serials.erase(s);
        if (dropped)
            bumpVersion();
    }

    // S5: whether `id` names a live binding on canvas `c` (no side effects).
    bool valid(Canvas c, std::string const& id) const
    {
        check("valid");
        auto it = records.find(c);
        if (it == records.end() || !it->second.isAlive())
            return false;
        auto e = it->second.ids.find(id);
        return e != it->second.ids.end() && entryValid(it->second, id, e->second);
    }

    size_t recordCount() const { return records.size(); }

    // The id bound to `obj` on any live canvas, or "".
    std::string idOf(Obj obj) const
    {
        check("idOf");
        if (!obj)
            return {};
        for (auto const& [c, r] : records) {
            if (!r.isAlive())
                continue;
            for (auto const& [id, bound] : r.ids)
                if (bound == obj && entryValid(r, id, obj)) // review H2: a stale entry at a reused address is no id
                    return id;
        }
        return {};
    }

    enum class BindResult {
        Bound,  // the id now names obj (new serial, version bumped)
        Same,   // it already did: nothing changed
        Refused // #38 (S4): the id names another live object, which keeps it
    };

    // Binds `id` to `obj` on canvas `c`. An id held by a dead object is taken over; one held by another live
    // object is refused (theft would orphan that object and misroute every later op on the id).
    BindResult bind(Canvas c, std::string const& id, Obj obj)
    {
        check("bind");
        auto& r = ensure(c);
        auto it = r.ids.find(id);
        if (it != r.ids.end()) {
            if (it->second == obj && entryValid(r, id, obj))
                return BindResult::Same;
            if (it->second != obj && objectAlive(c, it->second) && entryValid(r, id, it->second))
                return BindResult::Refused;
        }
        r.ids[id] = obj;
        ensureSerial(obj); // review H2: a live object keeps its serial, so its other ids stay valid
        r.boundSerial[id] = serials[obj].serial;
        bump(r);
        return BindResult::Bound;
    }

    // Review H3: drop one id (and its bound serial). True when it was there.
    bool eraseId(Canvas c, std::string const& id)
    {
        check("eraseId");
        auto* r = live(c);
        if (!r)
            return false;
        r->boundSerial.erase(id);
        return r->ids.erase(id) > 0;
    }

    // Review H3: rename `from` to `to` on canvas c, keeping the bound serial with the id. A target held by a dead
    // object counts as free; one held by a live object is Taken (C5).
    RenameResult rename(Canvas c, std::string const& from, std::string const& to)
    {
        check("rename");
        auto* r = live(c);
        if (!r)
            return RenameResult::Missing;
        auto it = r->ids.find(from);
        if (it == r->ids.end())
            return RenameResult::Missing;
        if (from == to)
            return RenameResult::NoOp;
        auto target = r->ids.find(to);
        if (target != r->ids.end() && objectAlive(c, target->second) && entryValid(*r, to, target->second))
            return RenameResult::Taken;
        Obj const obj = it->second;
        auto bs = r->boundSerial.find(from);
        bool const hadSerial = bs != r->boundSerial.end();
        uint64_t const serial = hadSerial ? bs->second : 0;
        r->ids.erase(it);
        r->boundSerial.erase(from);
        r->ids[to] = obj;
        if (hadSerial)
            r->boundSerial[to] = serial;
        else
            r->boundSerial.erase(to);
        bump(*r);
        return RenameResult::Applied;
    }

    // S4: the only minter, for objects nobody named (hand-made, or adopted on load): "<prefix>_<n>" (owner
    // decision O1). n counts per canvas and is never reused; it starts above the highest number any id of the
    // canvas already ends in, and skips taken names. An object that already has an id here keeps it.
    std::string mint(Canvas c, Obj obj, std::string const& prefix)
    {
        check("mint");
        auto& r = ensure(c);
        for (auto const& [id, bound] : r.ids)
            if (bound == obj && entryValid(r, id, obj))
                return id;
        if (r.nextMint == 0) {
            uint64_t highest = 0;
            for (auto const& entry : r.ids)
                highest = std::max(highest, trailingNumber(entry.first));
            r.nextMint = highest + 1;
        }
        uint64_t n = r.nextMint;
        std::string id;
        do
            id = prefix + "_" + std::to_string(n++);
        while (r.ids.count(id));
        r.nextMint = n;
        r.ids[id] = obj;
        ensureSerial(obj);
        r.boundSerial[id] = serials[obj].serial;
        bump(r);
        return id;
    }

    // S5: the object `id` names on canvas `c` while that binding is valid: the object it was bound to still
    // lives (not merely something at the same address). A stale entry is dropped and answers nullptr.
    Obj resolve(Canvas c, std::string const& id)
    {
        check("resolve");
        auto* r = live(c);
        if (!r)
            return Obj {};
        auto it = r->ids.find(id);
        if (it == r->ids.end())
            return Obj {};
        if (entryValid(*r, id, it->second))
            return it->second;
        r->boundSerial.erase(id);
        r->ids.erase(it);
        return Obj {};
    }

    // S5: the retype hook. Pd recreated `from` as `to` (maybe at the same address): the id bound to `from`
    // follows, on whichever canvas it lives. Answers that id, or "" when `from` had none.
    std::string moveId(Obj from, Obj to)
    {
        check("moveId");
        // Review H1: decide once. A `from` whose own lifetime still lives was not recreated (the same text, or a
        // subpatch rename in place): nothing moves.
        auto s = serials.find(from);
        if (s != serials.end() && s->second.isAlive())
            return {};
        uint64_t const ended = s != serials.end() ? s->second.serial : 0;
        if (!idOf(to).empty())
            return {}; // the new box already has an id: never stack a second one on it
        for (auto& [c, r] : records) {
            if (!r.isAlive())
                continue;
            for (auto& [id, bound] : r.ids) {
                if (bound != from)
                    continue;
                auto b = r.boundSerial.find(id);
                if (b != r.boundSerial.end() && b->second != ended)
                    continue; // bound in another lifetime of this address: not the box that was just retyped
                bound = to;
                if (from != to)
                    serials.erase(from); // dead: its lifetime ended
                assignSerial(to);
                r.boundSerial[id] = serials[to].serial;
                bump(r);
                return id;
            }
        }
        return {};
    }

    // S5 (L9): an abstraction reload frees and recreates its instances at the same index. Before a reload the
    // adapter leaves a tombstone per id bound to an instance; afterwards it rebinds the id to the object at that
    // index with the same tag and drops the tombstone. A dead canvas has none.
    void addTombstone(Canvas c, std::string const& id, int index, std::string const& tag, uint64_t now = nowMs())
    {
        check("addTombstone");
        auto& stones = ensure(c).tombstones;
        for (auto& s : stones)
            if (s.id == id) {
                s = { id, index, tag, now };
                return;
            }
        if (stones.size() < 4096)
            stones.push_back({ id, index, tag, now });
    }
    // Review M2: a tombstone that never found its box (the instance was deleted before the reload) goes after 30 s.
    void expireTombstones(uint64_t now = nowMs())
    {
        check("expireTombstones");
        for (auto& [c, r] : records)
            r.tombstones.erase(std::remove_if(r.tombstones.begin(), r.tombstones.end(),
                                   [now](Tombstone const& s) { return now > s.bornMs + 30000; }),
                r.tombstones.end());
    }
    bool anyTombstones() const
    {
        check("anyTombstones");
        for (auto const& entry : records)
            if (!entry.second.tombstones.empty())
                return true;
        return false;
    }
    std::vector<Tombstone> tombstones(Canvas c)
    {
        check("tombstones");
        auto* r = live(c);
        return r ? r->tombstones : std::vector<Tombstone> {};
    }
    void dropTombstone(Canvas c, std::string const& id)
    {
        check("dropTombstone");
        if (auto* r = live(c))
            r->tombstones.erase(std::remove_if(r->tombstones.begin(), r->tombstones.end(),
                                    [&](Tombstone const& s) { return s.id == id; }),
                r->tombstones.end());
    }

    void assignSerial(Obj obj)
    {
        check("assignSerial");
        auto& s = serials[obj];
        s.serial = nextSerial++;
        s.alive = hooks.objectGuard ? hooks.objectGuard(obj) : std::function<bool()>();
    }
    // A serial for `obj` unless it has one from its own lifetime (a dead one belongs to a freed predecessor
    // at the same address).
    void ensureSerial(Obj obj)
    {
        check("ensureSerial");
        auto it = serials.find(obj);
        if (it == serials.end() || !it->second.isAlive())
            assignSerial(obj);
    }
    void eraseSerial(Obj obj)
    {
        check("eraseSerial");
        serials.erase(obj);
    }
    bool hasSerial(Obj obj) const
    {
        check("hasSerial");
        return serials.find(obj) != serials.end();
    }
    uint64_t serialOf(Obj obj) const
    {
        check("serialOf");
        auto it = serials.find(obj);
        return it == serials.end() ? 0 : it->second.serial;
    }

    // Monotonic, bumped on every identity change; TS polls it to skip snapshot fetches. Atomic: readable without
    // the Pd lock (the only access that is).
    uint64_t version() const { return identityVersion.load(std::memory_order_relaxed); }
    void bumpVersion() { identityVersion.fetch_add(1, std::memory_order_relaxed); }

    // S6: per-canvas versions. A change of canvas c bumps c's version and the global one.
    void bumpVersion(Canvas c)
    {
        check("bumpVersion");
        bump(ensure(c));
    }
    uint64_t canvasVersion(Canvas c)
    {
        check("canvasVersion");
        auto* r = live(c);
        return r ? r->version : 0;
    }
    // S6: GUI edits. The adapter computes a structure stamp of the canvas (object count + a hash of its objects)
    // whenever it looks; a stamp different from the last one seen means the canvas changed behind the identity
    // layer's back: bump. The first observation only sets the baseline. True when it bumped.
    bool observeStamp(Canvas c, uint64_t stamp)
    {
        check("observeStamp");
        auto& r = ensure(c);
        if (!r.stamped) {
            r.stamped = true;
            r.stamp = stamp;
            return false;
        }
        if (r.stamp == stamp)
            return false;
        r.stamp = stamp;
        bump(r);
        return true;
    }

    // P11.5 V23: the content stamp (wires, x/y and text atoms on top of the structure) has its own counters, per canvas
    // and global; it never bumps the identity version (a wire or a move changes no identity). The first observation
    // only sets the baseline. True when it bumped.
    bool observeContent(Canvas c, uint64_t stamp)
    {
        check("observeContent");
        auto& r = ensure(c);
        if (!r.contentStamped) {
            r.contentStamped = true;
            r.contentStamp = stamp;
            return false;
        }
        if (r.contentStamp == stamp)
            return false;
        r.contentStamp = stamp;
        ++r.contentVersion;
        contentGlobal.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t contentVersion(Canvas c)
    {
        check("contentVersion");
        auto* r = live(c);
        return r ? r->contentVersion : 0;
    }
    uint64_t contentVersionGlobal() const { return contentGlobal.load(std::memory_order_relaxed); }

    // P3.5 S2: called on every access but version(); the audit build installs a check that reports an access
    // without the Pd lock. nullptr (the default) = no check.
    using AccessCheck = void (*)(char const* where);
    void setAccessCheck(AccessCheck c) { accessCheck = c; }

private:
    struct Record {
        std::string key;
        std::string base; // the adapter's key before "#n", to notice a rename
        IdMap ids;
        std::function<bool()> alive;
        uint64_t nextMint = 0; // 0 = not seeded yet (see mint)
        std::unordered_map<std::string, uint64_t> boundSerial; // S5: id -> its object's serial at bind time
        std::vector<Tombstone> tombstones;                     // S5 (L9)
        uint64_t version = 0;                                  // S6: bumped by every change of this canvas
        uint64_t stamp = 0;                                    // S6: last structure stamp seen (see observeStamp)
        bool stamped = false;
        uint64_t contentStamp = 0;                             // P11.5 V23 (see observeContent)
        bool contentStamped = false;
        uint64_t contentVersion = 0;
        bool isAlive() const { return !alive || alive(); }
    };

    struct SerialInfo {
        uint64_t serial = 0;
        std::function<bool()> alive; // S5: this object's weak reference
        bool isAlive() const { return !alive || alive(); }
    };

    // S5: the binding id -> obj is valid while obj (of the lifetime it was bound in) lives. Entries written
    // without bind/mint have no bound serial: then only the object's own guard counts.
    bool entryValid(Record const& r, std::string const& id, Obj obj) const
    {
        auto s = serials.find(obj);
        if (s == serials.end())
            return !hooks.objectGuard; // never given a serial: unknown, live only without guards (tests)
        if (!s->second.isAlive())
            return false;
        auto b = r.boundSerial.find(id);
        return b == r.boundSerial.end() || b->second == s->second.serial;
    }

    bool objectAlive(Canvas c, Obj obj) const { return !hooks.objectLive || hooks.objectLive(c, obj); }

    // "osc_sig_12" -> 12; 0 when the id does not end in "_<digits>".
    static uint64_t trailingNumber(std::string const& id)
    {
        auto const us = id.rfind('_');
        if (us == std::string::npos || us + 1 >= id.size() || id.size() - us > 19)
            return 0;
        uint64_t n = 0;
        for (size_t i = us + 1; i < id.size(); ++i) {
            if (id[i] < '0' || id[i] > '9')
                return 0;
            n = n * 10 + static_cast<uint64_t>(id[i] - '0');
        }
        return n;
    }

    void bump(Record& r)
    {
        ++r.version;
        bumpVersion();
    }

    void check(char const* where) const
    {
        if (accessCheck)
            accessCheck(where);
    }

    std::string baseOf(Canvas c) const { return hooks.baseKey ? hooks.baseKey(c) : std::string(); }

    // The base key, or base + "#n" with the lowest n no other live record holds.
    std::string freeKey(std::string const& base) const
    {
        std::unordered_set<std::string> taken;
        for (auto const& [c, r] : records)
            if (r.isAlive())
                taken.insert(r.key);
        if (!taken.count(base))
            return base;
        for (int n = 2;; ++n) {
            auto candidate = base + "#" + std::to_string(n);
            if (!taken.count(candidate))
                return candidate;
        }
    }

    Record* live(Canvas c)
    {
        auto it = records.find(c);
        if (it == records.end())
            return nullptr;
        if (it->second.isAlive())
            return &it->second;
        records.erase(it); // dead (maybe a new canvas at the same address): never lend its ids
        return nullptr;
    }

    Record& ensure(Canvas c)
    {
        if (auto* r = live(c))
            return *r;
        Record r;
        r.base = baseOf(c);
        r.key = freeKey(r.base);
        if (hooks.liveness)
            r.alive = hooks.liveness(c);
        return records.emplace(c, std::move(r)).first->second;
    }

    AccessCheck accessCheck = nullptr;
    Hooks hooks;
    std::unordered_map<Canvas, Record> records;
    std::unordered_map<Obj, SerialInfo> serials;
    uint64_t nextSerial = 1;
    std::atomic<uint64_t> identityVersion { 0 };
    std::atomic<uint64_t> contentGlobal { 0 }; // P11.5 V23
};

} // namespace mcp::identity
