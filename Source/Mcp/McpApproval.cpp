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
        mcp::ApprovalChip chip;
        chip.nonce = o->getProperty("nonce").toString().toStdString();
        // P12a A3: the "flow:" prefix belongs to the flow-mode offer, made in C++ only; a server never shows one.
        if (mcp::isFlowNonce(chip.nonce))
            chip.nonce.clear();
        chip.canvas = o->getProperty("canvas").toString().toStdString();
        chip.reason = o->getProperty("reason").toString().toStdString();
        // P12c review 5: line 1 is the server's fixed words; without a title the chip says only that something waits
        // (never the reason, which can carry model-chosen text).
        // Round 3 item 4: line 1 is fixed words only; a canvas name (model-chosen) goes on line 2.
        chip.title = o->hasProperty("title") ? o->getProperty("title").toString().toStdString()
                                             : std::string("CONFIRM a change");
        chip.detail = o->hasProperty("title") ? o->getProperty("detail").toString().toStdString()
                                              : mcp::approvalCanvasLabel(chip.canvas) + " \xc2\xb7 " + chip.reason;
        chip.targets = targets;
        // The canvas the question is about (the outline is drawn only there). hardware / snapshots are no canvas.
        if (chip.canvas != "hardware" && chip.canvas != "snapshots")
            chip.canvasId = reinterpret_cast<std::uintptr_t>(processor->getMcp()->getCanvasBySymbol(normalizeCanvas(juce::String(chip.canvas))));
        shown = processor->getMcp()->session.approvals.show(std::move(chip), ttl, approvalNowMs());
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
