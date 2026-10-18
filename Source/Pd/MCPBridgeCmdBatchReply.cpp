/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stages 4-5 (P3.4 S9): the undo record and the reply, on the command worker.

#include "MCPBridgeCmdBatch.h"

void mcp::BatchJob::recordUndo(const juce::OSCMessage& msg)
{
    // Stage 2: Register with Unified C++ Transaction Engine for GUI Ctrl+Z / Undo
    if (processor && cnv && !processor->getMcp()->session.isExecutingMcpUndoRedo) {
        if (created > 0 || connected > 0 || disconnected > 0 || edited > 0 || deleted > 0) {
            juce::OSCMessage invMsg { juce::OSCAddressPattern("/pd/batch_atomic") };
            invMsg.addArgument(canvasName);
            invMsg.addArgument(juce::String("inv_" + correlationId));

            // Header counts: deletes, disconnects, edits, creates, connects
            invMsg.addArgument(static_cast<float>(pendingCreates.size()));               // deleteCount
            invMsg.addArgument(static_cast<float>(allConns.size()));                     // disconnectCount
            invMsg.addArgument(static_cast<float>(editUndoEntries.size()));              // editCount
            invMsg.addArgument(static_cast<float>(deletedSnapshots.size()));             // createCount
            invMsg.addArgument(static_cast<float>(preDisconnects.size() + deletedWires.size())); // connectCount

            // 1. DELETES: delete all objects created in pendingCreates
            for (auto const& pc : pendingCreates) {
                invMsg.addArgument(pc.tempId);
            }

            // 2. DISCONNECTS: disconnect all connections made in allConns
            for (auto const& c : allConns) {
                invMsg.addArgument(c.srcId);
                invMsg.addArgument(static_cast<float>(c.srcOut));
                invMsg.addArgument(c.destId);
                invMsg.addArgument(static_cast<float>(c.destIn));
            }

            // 3. EDITS: edit each object back to its pre-mutation text
            for (auto const& eu : editUndoEntries) {
                invMsg.addArgument(eu.objectId);
                invMsg.addArgument(eu.type);
                invMsg.addArgument(static_cast<float>(eu.args.size()));
                for (auto const& a : eu.args) invMsg.addArgument(a);
            }

            // 4. CREATES: recreate every deleted object (tempId, x, y, kind, type, args...)
            for (auto const& ds : deletedSnapshots) {
                invMsg.addArgument(ds.tempId);
                invMsg.addArgument(static_cast<float>(ds.x));
                invMsg.addArgument(static_cast<float>(ds.y));
                invMsg.addArgument(ds.kind);
                invMsg.addArgument(ds.type);
                invMsg.addArgument(static_cast<float>(ds.args.size()));
                for (auto const& a : ds.args) invMsg.addArgument(a);
            }

            // 5. CONNECTS: reconnect wires cut in the forward (pre-disconnect
            //    + every wire that touched a deleted object)
            for (auto const& pdc : preDisconnects) {
                invMsg.addArgument(pdc.srcId);
                invMsg.addArgument(static_cast<float>(pdc.srcOut));
                invMsg.addArgument(pdc.destId);
                invMsg.addArgument(static_cast<float>(pdc.destIn));
            }
            for (auto const& w : deletedWires) {
                invMsg.addArgument(w.srcId);
                invMsg.addArgument(static_cast<float>(w.srcOut));
                invMsg.addArgument(w.destId);
                invMsg.addArgument(static_cast<float>(w.destIn));
            }

            // Layout flags: no autolayout on undo
            invMsg.addArgument(0.0f);
            invMsg.addArgument(static_cast<float>(layoutPad));
            invMsg.addArgument(static_cast<float>(layoutSnap));
            invMsg.addArgument(0.0f);

            processor->getMcp()->pushMcpTransaction(cnv, canvasName, msg, invMsg);
        }
    }
}

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

    // Undo-safety tail fact (appended LAST): 1 when this batch deleted
    // objects and therefore flushed the Pd undo queue (MCP deletes never
    // register undo actions, so the queue must be emptied to stay
    // crash-safe). Old TS clients ignore the extra trailing atom; new
    // clients surface it as _v2meta.undoReset so the AI knows native
    // undo cannot step back past this mutation.
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
