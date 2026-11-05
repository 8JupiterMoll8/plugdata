/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Alphi drop 1: what Alphi says (the say board, Source/Mcp/McpAlphiSay.h). Step W stores the say and asks for a
// repaint; F3 adds the strip and the say drawing to this file. The server's services/alphi-bubble.ts is the only
// sender of /pd/alphi_say. Display only: this file never touches the patch, the Pd lock or flow mode, and never
// names the flow switch or the click on Alphi (scripts/test-bridge-safety.sh).

#include "../Pd/MCPBridgeCmdCommon.h"
#include "Mcp/McpAlphiSay.h"

namespace {

std::optional<mcp::AlphiSay> alphiSayOfJson(juce::String const& json)
{
    auto const parsed = juce::JSON::parse(json);
    auto* o = parsed.getDynamicObject();
    if (!o) return std::nullopt;
    auto const str = [o](char const* key) -> std::optional<std::string> {
        auto const v = o->getProperty(key);
        if (!v.isString()) return std::nullopt;
        return v.toString().toStdString();
    };
    auto const ttl = o->getProperty("ttlMs");
    bool const ttlIsNumber = ttl.isInt() || ttl.isInt64() || ttl.isDouble();
    auto const text = str("text"), kind = str("kind"), state = str("state"), turn = str("turn");
    if (!text || !kind || !state || !turn || !ttlIsNumber) return std::nullopt;
    return mcp::alphiSayFrom(*text, *kind, *state, *turn, static_cast<double>(ttl));
}

} // namespace

    // /pd/alphi_say <json {text, kind: plain|busy|fail, state: idle|building|measuring|failed, turn, ttlMs}>
    // No reply (replyMode none): a say that breaks the contract is dropped whole.
void MCPBridge::pdAlphiSay(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    if (msg.size() < 1 || !processor) return;
    auto const say = alphiSayOfJson(getArgString(msg[0]));
    if (!say) {
        DBG("alphi_say: dropped a say that breaks the contract");
        return;
    }
    if (processor->getMcp()->session.alphiSay.apply(*say, mcp::alphiSayNowMs()))
        postMessage([proc = processor]() { proc->getMcp()->gui().repaintAll(); });
}
