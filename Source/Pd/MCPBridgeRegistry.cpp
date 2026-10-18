/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Builds the bridge's command table from MCPBridgeVerbs.inc and dispatches
// inbound OSC messages through it (Plan 3 P3.3).

#include "MCPBridge.h"
#include "MCPBridgeInternal.h"
#include "Mcp/McpAudit.h"

MCPBridge::VerbTable const& MCPBridge::commandTable()
{
    static VerbTable const table = [] {
        VerbTable t;
#define MCP_SOURCE(domain, source) t.setSource(#domain, mcp::VerbSource::source);
#define MCP_VERB(domain, verb, handler) t.add(#domain, verb, &MCPBridge::handler);
#define MCP_ALIAS(domain, verb, handler) t.add(#domain, verb, &MCPBridge::handler);
#define MCP_DOMAIN(domain, handler) t.addDomain(#domain, &MCPBridge::handler);
#include "MCPBridgeVerbs.inc"
        return t;
    }();
    return table;
}

// True when the table served the message. `pathVerb` is the second part of the
// address ("" when there is none); where the verb really comes from is the
// domain's MCP_SOURCE row.
bool MCPBridge::dispatchRegistered(const juce::String& domain, const juce::String& pathVerb, const juce::OSCMessage& msg)
{
    auto const& table = commandTable();
    auto const domainKey = domain.toStdString();
    auto const source = table.sourceOf(domainKey);
    auto const arg0 = (source != mcp::VerbSource::Path && msg.size() > 0) ? getArgString(msg[0]) : juce::String();
    auto const verb = mcp::resolveVerb(source, pathVerb.toStdString(), arg0.toStdString());

    auto const* handler = table.find(domainKey, verb);
    if (handler == nullptr)
        return false;

    mcp::audit::commandBegin(msg.getAddressPattern().toString().toRawUTF8());
    (this->**handler)(juce::String::fromUTF8(verb.c_str()), msg);
    mcp::audit::commandEnd();
    return true;
}

// The caller has found no row for the message. Where the arguments are the verb source,
// the first one is read the same way dispatchRegistered reads it.
void MCPBridge::replyUnknownVerb(const juce::String& domain, const juce::String& pathVerb, const juce::OSCMessage& msg)
{
    auto const& table = commandTable();
    auto const domainKey = domain.toStdString();
    auto const arg0 = (table.sourceOf(domainKey) != mcp::VerbSource::Path && msg.size() > 0) ? getArgString(msg[0]) : juce::String();

    auto const unknown = mcp::unknownVerbOf(table, domainKey, pathVerb.toStdString(), arg0.toStdString());
    if (!unknown)
        return;

    if (!active.load())
        return;
    juce::OSCMessage reply { juce::OSCAddressPattern("/bridge/unknown_verb") };
    reply.addArgument(msg.getAddressPattern().toString());
    reply.addArgument(juce::String::fromUTF8(unknown->domain.c_str()));
    reply.addArgument(juce::String::fromUTF8(unknown->verb.c_str()));
    sender.send(reply);
}

void MCPBridge::replayPd(const juce::String& action, const juce::OSCMessage& msg)
{
    dispatchRegistered("pd", action, msg);
}
