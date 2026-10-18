/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Identity service core (Plan 3 P3.5). Owns everything the bridge knows about tempIds: per canvas key the map
// id -> object, a serial per bound object, and the identity version TS polls. JUCE-free and templated on the
// object handle, so Tests/mcp/identity_service_test.cpp runs with plain g++ on fake handles; the plugdata side
// is mcp::IdentityService (McpIdentity.h).
//
// S1: the three McpSession fields moved here unchanged; still keyed by canvas name, callers keep their eviction
// and minting rules. Later steps add canvas records, validation and the lock rule (see docs/plans/p3.5-steps.md).

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace mcp::identity {

template <typename Obj>
class IdentityCore {
public:
    using IdMap = std::unordered_map<std::string, Obj>;

    // The id map of one canvas key, created empty when missing.
    IdMap& canvasMap(std::string const& key)
    {
        check("canvasMap");
        return canvases[key];
    }

    // The id map of one canvas key, or nullptr; never creates one.
    IdMap* findCanvas(std::string const& key)
    {
        check("findCanvas");
        auto it = canvases.find(key);
        return it == canvases.end() ? nullptr : &it->second;
    }
    IdMap const* findCanvas(std::string const& key) const
    {
        check("findCanvas");
        auto it = canvases.find(key);
        return it == canvases.end() ? nullptr : &it->second;
    }

    // Drops a canvas key with its ids and their serials. True when the key existed. No version bump.
    bool eraseCanvas(std::string const& key)
    {
        check("eraseCanvas");
        auto it = canvases.find(key);
        if (it == canvases.end())
            return false;
        for (auto const& entry : it->second)
            serials.erase(entry.second);
        canvases.erase(it);
        return true;
    }

    // Save-as: the ids follow the file to its new key (replacing whatever that key held).
    void moveCanvas(std::string const& from, std::string const& to)
    {
        check("moveCanvas");
        if (from == to)
            return;
        auto it = canvases.find(from);
        if (it == canvases.end())
            return;
        auto moved = std::move(it->second);
        canvases.erase(it);
        canvases[to] = std::move(moved);
    }

    // The id bound to `obj` on any canvas, or "" (the first match in map order, as the old scans did).
    std::string idOf(Obj obj) const
    {
        check("idOf");
        if (!obj)
            return {};
        for (auto const& [key, ids] : canvases)
            for (auto const& [id, bound] : ids)
                if (bound == obj)
                    return id;
        return {};
    }

    // Binds `id` to `obj` on `key` (overwriting), gives the object a new serial, bumps the version.
    void bind(std::string const& key, std::string const& id, Obj obj)
    {
        check("bind");
        canvases[key][id] = obj;
        assignSerial(obj);
        bumpVersion();
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
    void check(char const* where) const
    {
        if (accessCheck)
            accessCheck(where);
    }

    AccessCheck accessCheck = nullptr;
    std::unordered_map<std::string, IdMap> canvases;
    std::unordered_map<Obj, uint64_t> serials;
    uint64_t nextSerial = 1;
    std::atomic<uint64_t> identityVersion { 0 };
};

} // namespace mcp::identity
