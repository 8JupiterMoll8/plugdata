/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Command table for the MCP bridge (Plan 3 P3.3): one row per OSC verb.
// JUCE-free on purpose, so Tests/mcp/registry_test.cpp builds with g++ alone.
//
// Keys are "<domain>/<verb>". A verb-agnostic domain (param, chat: the handler
// ignores the address verb) holds one handler under the bare "<domain>" key.
// Registering the same key twice throws, so a duplicate row is a startup
// failure and cannot hide as a second `if (action == ...)` branch again.

#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// Where the verb of a message comes from. `Path` reads /domain/<verb> only;
// `PathOrArg0` falls back to the first argument for the flat form
// (/array ["stats", ...]); `Arg0` always reads the first argument (telemetry).
enum class VerbSource { Path, PathOrArg0, Arg0 };

// The verb of one inbound message. `pathVerb` and `arg0` are empty when the
// address has no second part / the message has no (string-like) first argument.
inline std::string resolveVerb(VerbSource source, std::string const& pathVerb, std::string const& arg0)
{
    switch (source) {
    case VerbSource::Path:
        return pathVerb;
    case VerbSource::Arg0:
        return arg0;
    case VerbSource::PathOrArg0:
        break;
    }
    return pathVerb.empty() ? arg0 : pathVerb;
}

template<typename Handler>
class CommandTable {
public:
    // Declares a domain and where its verb comes from. Once per domain.
    void setSource(std::string const& domain, VerbSource source)
    {
        if (!sources.emplace(domain, source).second)
            throw std::logic_error("duplicate verb source for domain: " + domain);
    }

    // One verb row; an alias is a second row with the same handler.
    void add(std::string const& domain, std::string const& verb, Handler handler)
    {
        requireDomain(domain);
        if (verb.empty())
            throw std::logic_error("empty verb for domain: " + domain);
        if (domainHandlers.count(domain))
            throw std::logic_error("verb row in verb-agnostic domain: " + domain + "/" + verb);
        if (!verbs.emplace(domain + "/" + verb, handler).second)
            throw std::logic_error("duplicate verb: " + domain + "/" + verb);
    }

    // A verb-agnostic domain: the handler takes every verb, including none.
    void addDomain(std::string const& domain, Handler handler)
    {
        requireDomain(domain);
        for (auto const& entry : verbs)
            if (entry.first.compare(0, domain.size() + 1, domain + "/") == 0)
                throw std::logic_error("domain handler beside verb rows: " + domain);
        if (!domainHandlers.emplace(domain, handler).second)
            throw std::logic_error("duplicate domain handler: " + domain);
    }

    // The exact row, else the domain handler, else nullptr (unknown verb).
    Handler const* find(std::string const& domain, std::string const& verb) const
    {
        auto const row = verbs.find(domain + "/" + verb);
        if (row != verbs.end())
            return &row->second;
        auto const whole = domainHandlers.find(domain);
        return whole != domainHandlers.end() ? &whole->second : nullptr;
    }

    // False for a domain that has not been declared (or has no rows yet).
    bool hasDomain(std::string const& domain) const { return sources.count(domain) != 0; }

    VerbSource sourceOf(std::string const& domain) const
    {
        auto const found = sources.find(domain);
        return found != sources.end() ? found->second : VerbSource::PathOrArg0;
    }

    // "<domain>/<verb>" for every verb row, sorted; domain handlers are not listed.
    std::vector<std::string> keys() const
    {
        std::vector<std::string> out;
        out.reserve(verbs.size());
        for (auto const& entry : verbs)
            out.push_back(entry.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    // Domains that hold one verb-agnostic handler, sorted.
    std::vector<std::string> domainKeys() const
    {
        std::vector<std::string> out;
        for (auto const& entry : domainHandlers)
            out.push_back(entry.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    size_t size() const { return verbs.size() + domainHandlers.size(); }

private:
    void requireDomain(std::string const& domain) const
    {
        if (!hasDomain(domain))
            throw std::logic_error("row for an undeclared domain: " + domain);
    }

    std::unordered_map<std::string, VerbSource> sources;
    std::unordered_map<std::string, Handler> verbs;
    std::unordered_map<std::string, Handler> domainHandlers;
};

} // namespace mcp
