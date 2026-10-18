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
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    };
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

    // Drops a canvas's ids and their serials; the record and its key stay. True when the canvas had a record.
    // No version bump.
    bool clearCanvas(Canvas c)
    {
        check("clearCanvas");
        auto* r = live(c);
        if (!r)
            return false;
        for (auto const& entry : r->ids)
            serials.erase(entry.second);
        r->ids.clear();
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
        r->key = freeKey(baseOf(c));
    }

    // Drops every record whose canvas is gone, with its serials.
    void prune()
    {
        check("prune");
        for (auto it = records.begin(); it != records.end();) {
            if (it->second.isAlive()) {
                ++it;
                continue;
            }
            dropSerials(it->second);
            it = records.erase(it);
        }
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
                if (bound == obj)
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
            if (it->second == obj)
                return BindResult::Same;
            if (objectAlive(c, it->second))
                return BindResult::Refused;
            serials.erase(it->second);
        }
        r.ids[id] = obj;
        assignSerial(obj);
        bumpVersion();
        return BindResult::Bound;
    }

    // S4: the only minter, for objects nobody named (hand-made, or adopted on load): "<prefix>_<n>" (owner
    // decision O1). n counts per canvas and is never reused; it starts above the highest number any id of the
    // canvas already ends in, and skips taken names. An object that already has an id here keeps it.
    std::string mint(Canvas c, Obj obj, std::string const& prefix)
    {
        check("mint");
        auto& r = ensure(c);
        for (auto const& [id, bound] : r.ids)
            if (bound == obj)
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
        bumpVersion();
        return id;
    }

    void assignSerial(Obj obj)
    {
        check("assignSerial");
        serials[obj] = nextSerial++;
    }
    void ensureSerial(Obj obj)
    {
        check("ensureSerial");
        if (serials.find(obj) == serials.end())
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
        return it == serials.end() ? 0 : it->second;
    }

    // Monotonic, bumped on every identity change; TS polls it to skip snapshot fetches. Atomic: readable without
    // the Pd lock (the only access that is).
    uint64_t version() const { return identityVersion.load(std::memory_order_relaxed); }
    void bumpVersion() { identityVersion.fetch_add(1, std::memory_order_relaxed); }

    // P3.5 S2: called on every access but version(); the audit build installs a check that reports an access
    // without the Pd lock. nullptr (the default) = no check.
    using AccessCheck = void (*)(char const* where);
    void setAccessCheck(AccessCheck c) { accessCheck = c; }

private:
    struct Record {
        std::string key;
        IdMap ids;
        std::function<bool()> alive;
        uint64_t nextMint = 0; // 0 = not seeded yet (see mint)
        bool isAlive() const { return !alive || alive(); }
    };

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

    void dropSerials(Record const& r)
    {
        for (auto const& entry : r.ids)
            serials.erase(entry.second);
    }

    Record* live(Canvas c)
    {
        auto it = records.find(c);
        if (it == records.end())
            return nullptr;
        if (it->second.isAlive())
            return &it->second;
        dropSerials(it->second); // dead (maybe a new canvas at the same address): never lend its ids
        records.erase(it);
        return nullptr;
    }

    Record& ensure(Canvas c)
    {
        if (auto* r = live(c))
            return *r;
        Record r;
        r.key = freeKey(baseOf(c));
        if (hooks.liveness)
            r.alive = hooks.liveness(c);
        return records.emplace(c, std::move(r)).first->second;
    }

    AccessCheck accessCheck = nullptr;
    Hooks hooks;
    std::unordered_map<Canvas, Record> records;
    std::unordered_map<Obj, uint64_t> serials;
    uint64_t nextSerial = 1;
    std::atomic<uint64_t> identityVersion { 0 };
};

} // namespace mcp::identity
