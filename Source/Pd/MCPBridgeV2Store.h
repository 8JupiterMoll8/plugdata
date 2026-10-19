/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The v2 idempotency store (Plan 4 P4.1 S4; docs/plans/p4.1-steps.md "Idempotency store"). JUCE-free, unit-tested
// by Tests/mcp/v2_store_test.cpp. Not thread-safe: the router calls it under its own mutex.
//
// A key (`<clientId>:<n>`, chosen by the client, the same across its reconnects) names one request. Its entry is
// in flight until the request's final, then done and holding that final, so:
//   - the same key while in flight: the asker joins as a waiter and gets the final when it comes;
//   - the same key once done: the stored final, re-sent under the asker's id with `replayed: true`;
//   - the same key with other content (fingerprint): a conflict;
//   - a request refused before it ran is abandoned (forgotten), so a resend runs.
// Bounds: kStoreEntries entries, kStoreTtlMs since a DONE entry was completed, kStoreMaxBytes of stored finals; the
// oldest done entries go first. An in-flight entry never expires or is evicted: the router ends every one (its final,
// a refusal before it ran, a dropped post, or the bridge stopping), and expiring one while its work runs would let a
// resend run twice (S4 review HIGH). At most kMaxWaiters requests wait on one key. One store per boot: a PlugData restart forgets every key (the new boot token in
// welcome tells the client).
#pragma once

#include "MCPBridgeV2Codec.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::v2 {

// A request's final, without the id it was sent under.
struct StoredFinal {
    std::string outcome;
    std::string address; // "" for a synthesized final (no handler reply behind it)
    std::vector<Arg> args;
    std::optional<std::pair<ErrorCode, std::string>> error;

    std::size_t bytes() const
    {
        std::size_t n = 64 + outcome.size() + address.size() + (error ? error->second.size() : 0);
        for (auto const& a : args)
            n += 16 + a.s.size();
        return n;
    }
};

inline std::string encodeFinal(std::string_view id, StoredFinal const& f, bool replayed)
{
    std::string out = std::string(R"({"t":"rep","id":)") + json::quote(id) + R"(,"final":true,"outcome":)" + json::quote(f.outcome);
    if (!f.address.empty())
        out += ",\"address\":" + json::quote(f.address) + ",\"args\":" + encodeArgs(f.args);
    if (f.error)
        out += ",\"error\":" + encodeError(f.error->first, f.error->second);
    if (replayed)
        out += ",\"replayed\":true";
    return out + "}";
}

// FNV-1a over the verb and the typed args: the same key with other content is a different request.
inline std::uint64_t fingerprintOf(std::string_view verb, std::vector<Arg> const& args)
{
    std::uint64_t h = 1469598103934665603ull;
    auto const mix = [&](std::string_view s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= 0xff; // separator
        h *= 1099511628211ull;
    };
    mix(verb);
    mix(encodeArgs(args));
    return h;
}

struct Waiter {
    int session = 0;
    std::string id;
};

inline constexpr std::size_t kMaxWaiters = 8; // per key: more askers are told "in flight" without joining
using FinalPtr = std::shared_ptr<StoredFinal const>; // shared: a stored final is never copied under the router lock

class Store {
public:
    struct Limits {
        std::size_t entries = v2schema::kStoreEntries;
        std::int64_t ttlMs = v2schema::kStoreTtlMs;
        std::size_t maxBytes = v2schema::kStoreMaxBytes;
    };
    enum class State { New, InFlight, Done, Conflict };
    struct Lookup {
        State state = State::New;
        FinalPtr final; // Done
    };

    Store() = default;
    explicit Store(Limits l)
        : limits(l)
    {
    }

    // First sight of `key` makes it in flight (New); otherwise what it is.
    Lookup begin(std::string const& key, std::uint64_t fingerprint, std::int64_t now)
    {
        expire(now);
        auto const it = entries.find(key);
        if (it == entries.end()) {
            entries.emplace(key, Entry { fingerprint, now, false, {}, {} });
            evict();
            return { State::New, nullptr };
        }
        if (it->second.fingerprint != fingerprint)
            return { State::Conflict, nullptr };
        if (!it->second.done)
            return { State::InFlight, nullptr };
        return { State::Done, it->second.final };
    }

    // What `begin` would answer, without creating anything (the caller may still refuse the request).
    Lookup peek(std::string const& key, std::uint64_t fingerprint, std::int64_t now)
    {
        expire(now);
        auto const it = entries.find(key);
        if (it == entries.end())
            return { State::New, nullptr };
        if (it->second.fingerprint != fingerprint)
            return { State::Conflict, nullptr };
        if (!it->second.done)
            return { State::InFlight, nullptr };
        return { State::Done, it->second.final };
    }

    // False when the key is not in flight or already has kMaxWaiters askers; the same (session, id) joins once.
    bool join(std::string const& key, Waiter w)
    {
        auto const it = entries.find(key);
        if (it == entries.end() || it->second.done)
            return false;
        auto& ws = it->second.waiters;
        for (auto const& x : ws)
            if (x.session == w.session && x.id == w.id)
                return true;
        if (ws.size() >= kMaxWaiters)
            return false;
        ws.push_back(std::move(w));
        return true;
    }

    // The request's final: stored; the waiters to send it to are returned (and forgotten).
    std::vector<Waiter> complete(std::string const& key, FinalPtr final, std::int64_t now)
    {
        auto const it = entries.find(key);
        if (it == entries.end() || it->second.done)
            return {};
        auto waiters = std::move(it->second.waiters);
        it->second.waiters.clear();
        it->second.done = true;
        it->second.changedAt = now;
        storedBytes += final->bytes();
        it->second.final = std::move(final);
        doneOrder.push_back({ key, now });
        evict();
        return waiters;
    }

    // The request was refused before it ran: forget the key (a resend may run); its waiters are returned.
    std::vector<Waiter> abandon(std::string const& key)
    {
        auto const it = entries.find(key);
        if (it == entries.end() || it->second.done)
            return {};
        auto waiters = std::move(it->second.waiters);
        entries.erase(it);
        return waiters;
    }

    Lookup status(std::string const& key, std::int64_t now)
    {
        expire(now);
        auto const it = entries.find(key);
        if (it == entries.end())
            return { State::New, nullptr };
        if (!it->second.done)
            return { State::InFlight, nullptr };
        return { State::Done, it->second.final };
    }

    std::size_t size() const { return entries.size(); }
    std::size_t bytes() const { return storedBytes; }

private:
    struct Entry {
        std::uint64_t fingerprint = 0;
        std::int64_t changedAt = 0;
        bool done = false;
        FinalPtr final;
        std::vector<Waiter> waiters;
    };

    void erase(std::unordered_map<std::string, Entry>::iterator it)
    {
        if (it->second.final)
            storedBytes -= std::min(storedBytes, it->second.final->bytes());
        entries.erase(it);
    }

    void expire(std::int64_t now)
    {
        // Done entries in completion order; in-flight entries never expire (see the header).
        while (!doneOrder.empty()) {
            auto const& [key, at] = doneOrder.front();
            if (now - at <= limits.ttlMs)
                break;
            if (auto const it = current(key, at); it != entries.end())
                erase(it);
            doneOrder.pop_front();
        }
    }

    void evict()
    {
        while ((entries.size() > limits.entries || storedBytes > limits.maxBytes) && !doneOrder.empty()) {
            auto const [key, at] = doneOrder.front();
            doneOrder.pop_front();
            if (auto const it = current(key, at); it != entries.end())
                erase(it);
        }
    }

    // The done entry this order slot was made for (a key may have expired and been used again since).
    std::unordered_map<std::string, Entry>::iterator current(std::string const& key, std::int64_t at)
    {
        auto const it = entries.find(key);
        return it != entries.end() && it->second.done && it->second.changedAt == at ? it : entries.end();
    }

    Limits limits;
    std::unordered_map<std::string, Entry> entries;
    std::deque<std::pair<std::string, std::int64_t>> doneOrder; // (key, completed at), oldest first; may be stale
    std::size_t storedBytes = 0;
};

} // namespace mcp::v2
