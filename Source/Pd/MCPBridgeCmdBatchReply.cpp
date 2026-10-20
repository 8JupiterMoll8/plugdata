/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stage 4 (P3.4 S9): the reply, on the command worker.

#include "MCPBridgeCmdBatch.h"

juce::OSCMessage mcp::BatchJob::buildReply()
{
    // Build reply with counts and inline identity mappings
    juce::OSCMessage reply { juce::OSCAddressPattern("/pd/batch_atomic/reply/" + correlationId) };
    reply.addArgument(static_cast<int32>(created));
    reply.addArgument(static_cast<int32>(connected));
    reply.addArgument(static_cast<int32>(edited));
    reply.addArgument(static_cast<int32>(deleted));
    reply.addArgument(static_cast<int32>(disconnected));
    // PHASE 0 reconcile counts (appended — backward compatible)
    reply.addArgument(static_cast<int32>(reconcileEvicted));
    reply.addArgument(static_cast<int32>(reconcileAdopted));

    // Append inline mappings — built from data collected in lambda
    for (size_t i = 0; i < createdIds.size() && i < mappingIndices.size(); i++) {
        if (mappingIndices[i] >= 0) {
            reply.addArgument(juce::String(createdIds[i]));
            reply.addArgument(static_cast<int32>(mappingIndices[i]));
        }
    }

    // Append PHASE 0 detected connections as a compact JSON string.
    // Format: [{"s":"srcId","so":0,"d":"destId","di":0}, ...]
    // ALWAYS appended (empty "[]" when no wires) — the TS parser relies
    // on this marker to locate the failure-fact section that follows.
    {
        juce::String dcJson = "[";
        for (size_t i = 0; i < detectedConnections.size(); i++) {
            auto& dc = detectedConnections[i];
            if (i > 0) dcJson += ",";
            dcJson += "{\"s\":\"" + jsonEscape(juce::String(dc.srcId))
                   + "\",\"so\":" + juce::String(dc.srcOut)
                   + ",\"d\":\"" + jsonEscape(juce::String(dc.destId))
                   + "\",\"di\":" + juce::String(dc.destIn) + "}";
        }
        dcJson += "]";
        reply.addArgument(dcJson);
    }

    // Phase A (PRD diagnostic layer): create/connect failure facts —
    // appended VERY LAST, after the detected-connections JSON. Old TS
    // clients stop parsing at the JSON and ignore these; new TS clients
    // read them as named failures (PRD diagnostic layer §2.1).
    reply.addArgument(static_cast<int32>(createFailures.size()));
    for (auto& cf : createFailures) {
        reply.addArgument(juce::String(cf.tempId));
        reply.addArgument(juce::String(cf.type));
        reply.addArgument(juce::String(cf.reason));
    }
    reply.addArgument(static_cast<int32>(connectFailures.size()));
    for (auto& cf : connectFailures) {
        reply.addArgument(juce::String(cf.srcId) + juce::String("->") + juce::String(cf.destId));
        reply.addArgument(juce::String(cf.reason));
    }

    // Inline conditional X-ray: if any created object has a signal type (~),
    // compute diagnose facts in the audio thread lambda and append to reply.
    bool hasSignalCreate = false;
    for (auto& pc : pendingCreates) {
        if (pc.objType.contains("~")) { hasSignalCreate = true; break; }
    }
    juce::String diagJson;
    if (hasSignalCreate && cnv) {
        sys_lock();
        diagJson = MCPBridge::computeDiagnoseFacts(processor, cnv, canvasName);
        sys_unlock();
    }
    reply.addArgument(diagJson);

    // Bug #11 tail fact (appended LAST, after diagnose JSON): 1 when the
    // requested canvas symbol did not resolve and the mutation was
    // refused. Old TS clients ignore trailing args; new clients gate on it.
    reply.addArgument(static_cast<int32>(canvasNotFound ? 1 : 0));
    if (canvasNotFound)
        reply.addArgument(canvasName); // the name that failed, for the receipt

    // Undo-safety tail fact (appended LAST): 1 when this batch flushed the canvas's Pd undo queue. Since P4.4 that
    // happens only on a canvas that cannot register undo (a closed GOP module); elsewhere the batch is one step of
    // the canvas's one history. Clients surface it as _v2meta.undoReset (undo cannot step back past this batch).
    reply.addArgument(static_cast<int32>(undoReset ? 1 : 0));

    // Layout-sanitize tail fact (appended LAST, PRD_CONTEXT_LAYOUT_GUARD
    // P2): number of objects the inline deoverlap moved (0 = clean).
    reply.addArgument(static_cast<int32>(layoutSanitizedMoved));
    // Occlusion-fix tail fact (P3.1): objects nudged off wire paths.
    reply.addArgument(static_cast<int32>(occlusionsFixed));
    // Fallback tail fact: 1 if the guard fell back to compose.
    reply.addArgument(static_cast<int32>(fallbackComposed ? 1 : 0));

    // Guard 2 tail fact (appended LAST, old clients ignore): control->signal
    // advisories — [count, (wire, reason)*]. Not failures; the wire connected.
    reply.addArgument(static_cast<int32>(connectAdvisories.size()));
    for (auto& ca : connectAdvisories) {
        reply.addArgument(juce::String(ca.srcId) + juce::String("->") + juce::String(ca.destId));
        reply.addArgument(juce::String(ca.reason));
    }

    // Edit-failure tail fact (appended LAST, old clients ignore): edits whose
    // RECREATE failed (class couldn't be created → Pd leaves an error box).
    // Format: [count, (tempId, type, reason)*]. These are counted OUT of
    // `edited`, so the header count is truthfully short (no false success).
    reply.addArgument(static_cast<int32>(editFailures.size()));
    for (auto& ef : editFailures) {
        reply.addArgument(juce::String(ef.tempId));
        reply.addArgument(juce::String(ef.type));
        reply.addArgument(juce::String(ef.reason));
    }

    // Control-kind advisory tail fact (appended LAST, old clients ignore):
    // [count, (tempId, reason)*]. Not failures — the object was created.
    reply.addArgument(static_cast<int32>(createAdvisories.size()));
    for (auto& ca : createAdvisories) {
        reply.addArgument(juce::String(ca.tempId));
        reply.addArgument(juce::String(ca.reason));
    }
    return reply;
}
