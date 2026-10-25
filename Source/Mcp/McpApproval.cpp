/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12c: the bridge side of the canvas approval chips. The server's core/approval.ts is the only sender of
// /pd/approval_show and /pd/approval_clear; the chips are drawn and answered in CanvasMcpApproval.cpp, whose click /
// Enter / Esc is the only caller of sendApprovalAnswer. No read verb returns a nonce.

#include "../Pd/MCPBridgeCmdCommon.h"

namespace {
std::int64_t approvalNowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }
}

    // /pd/approval_show <corrId> <json {nonce, canvas, reason, targets[], ttlMs}>
    // Reply: /pd/approval_show/reply/<corrId> 1 shown | 0 refused (no nonce, bad JSON)
void MCPBridge::pdApprovalShow(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (msg.size() < 2 || !processor) {
        sendReply("/pd/approval_show/reply/" + correlationId, 0.0f);
        return;
    }
    auto const parsed = juce::JSON::parse(getArgString(msg[1]));
    auto* o = parsed.getDynamicObject();
    bool shown = false;
    if (o) {
        std::vector<std::string> targets;
        if (auto* arr = o->getProperty("targets").getArray())
            for (auto const& t : *arr)
                targets.push_back(t.toString().toStdString());
        auto const ttl = o->hasProperty("ttlMs") ? static_cast<std::int64_t>(static_cast<double>(o->getProperty("ttlMs"))) : 60000;
        shown = processor->getMcp()->session.approvals.show(
            o->getProperty("nonce").toString().toStdString(),
            o->getProperty("canvas").toString().toStdString(),
            o->getProperty("reason").toString().toStdString(),
            targets, ttl, approvalNowMs());
    }
    if (shown)
        postMessage([proc = processor]() { proc->getMcp()->gui().repaintAll(); });
    sendReply("/pd/approval_show/reply/" + correlationId, shown ? 1.0f : 0.0f);
}

    // /pd/approval_clear <corrId> <nonce>
    // Reply: /pd/approval_clear/reply/<corrId> 1 removed | 0 no such chip
void MCPBridge::pdApprovalClear(const juce::String& action, const juce::OSCMessage& msg)
{
    juce::ignoreUnused(action);
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (msg.size() < 2 || !processor) {
        sendReply("/pd/approval_clear/reply/" + correlationId, 0.0f);
        return;
    }
    bool const removed = processor->getMcp()->session.approvals.clear(getArgString(msg[1]).toStdString());
    if (removed)
        postMessage([proc = processor]() { proc->getMcp()->gui().repaintAll(); });
    sendReply("/pd/approval_clear/reply/" + correlationId, removed ? 1.0f : 0.0f);
}

void MCPBridge::sendApprovalAnswer(const juce::String& nonce, bool yes)
{
    if (!canReply() || nonce.isEmpty()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/approval/answer") };
    msg.addArgument(nonce);
    msg.addArgument(juce::String(yes ? "yes" : "no"));
    sender.send(msg);
}
