/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stages (Plan 3 P3.4 S9): one BatchJob per request carries the parsed
// request and the outcome through the stages, each in its own context:
//   parse (command worker)            MCPBridgeCmdBatchParse.cpp
//   runPdJob (Pd context, sys_lock)   MCPBridgeCmdBatchPd.cpp + MCPBridgeCmdBatchConnect.cpp
//   runLayoutGuard, frameCreated (message thread), resetUndo   MCPBridgeCmdBatch.cpp
//   recordUndo, buildReply (command worker)                    MCPBridgeCmdBatchReply.cpp
// The fields are the old pdBatchAtomic locals, unchanged.

#pragma once

#include "MCPBridgeCmdCommon.h"

namespace mcp {

struct BatchJob {
    BatchJob(PluginProcessor* p, juce::String canvas, juce::String corrId)
        : processor(p), canvasName(std::move(canvas)), correlationId(std::move(corrId)) {}

    PluginProcessor* processor;
    juce::String canvasName;
    juce::String correlationId;

    int deleteCount = 0, disconnectCount = 0, editCount = 0, createCount = 0, connectCount = 0;

    int deleted = 0, disconnected = 0, edited = 0, created = 0, connected = 0;
    int reconcileEvicted = 0, reconcileAdopted = 0;
    int layoutSanitizedMoved = 0;
    int occlusionsFixed = 0;
    bool fallbackComposed = false;
    // Phase A (PRD diagnostic layer): create/connect failure facts —
    // named failures instead of silent skips (see PRD §2.1).
    struct CreateFailure { std::string tempId; std::string type; std::string reason; };
    struct ConnectFailure { std::string srcId; std::string destId; std::string reason; };
    struct EditFailure { std::string tempId; std::string type; std::string reason; };
    std::vector<CreateFailure> createFailures;
    std::vector<ConnectFailure> connectFailures;
    std::vector<EditFailure> editFailures;
    // Guard 2: control->signal connections are legal (Pd sets the inlet
    // scalar) but step per DSP block. Advisory only — never rejected.
    std::vector<ConnectFailure> connectAdvisories;
    // Control-kind advisories (PRD preflight): a GUI control can render
    // wrong (inverted geometry → reads as "no slider") or carry an invalid
    // range. Advisory only — the create succeeds; the artist still gets a
    // working object. Format: {tempId, reason}. Emitted as a tail fact.
    struct CreateAdvisory { juce::String tempId; juce::String reason; };
    std::vector<CreateAdvisory> createAdvisories;
    std::vector<std::string> createdIds;
    std::vector<t_gobj*> createdPtrs;
    std::vector<int32> mappingIndices;

    struct PreDelete { juce::String objectId; };
    std::vector<PreDelete> preDeletes;
    struct PreDisconnect { juce::String srcId; int srcOut; juce::String destId; int destIn; };
    std::vector<PreDisconnect> preDisconnects;
    struct PreEdit { juce::String objectId; juce::String newText; };
    std::vector<PreEdit> preEdits;
    struct PendingCreate {
        juce::String tempId;
        juce::String objType;
        juce::String kind;
        int x;
        int y;
        float initValue;
        bool seedInit;
    };
    std::vector<PendingCreate> pendingCreates;
    juce::String pastaBuffer;
    struct CrossConn { juce::String srcId; int srcOut; juce::String destId; int destIn; };
    std::vector<CrossConn> allConns;

    // Optional trailing flags (PRD_CONTEXT_LAYOUT_GUARD P2/P3/P3.1):
    // autoLayout (default ON), pad, grid snap, fixOcclusions (default ON).
    // Old clients omit them.
    bool autoLayout = true;
    int layoutPad = 5, layoutSnap = 10;
    bool autoFixOcclusions = true;

    // Storage for PHASE 0 reconcile results (filled inside lambda, read outside)
    struct DetectedConn { std::string srcId; int srcOut; std::string destId; int destIn; };
    std::vector<DetectedConn> detectedConnections;

    // ── Undo capture (Stage 2 coverage: edits + deletes) ──────────────
    // Filled inside the audio lambda BEFORE the mutation, consumed by the
    // inverse-message builder below. Message-delta only — never pointers.
    struct DeletedSnapshot {
        juce::String tempId, kind, type;
        juce::StringArray args;
        int x = 0, y = 0;
    };
    std::vector<DeletedSnapshot> deletedSnapshots;
    struct UndoWire { juce::String srcId; int srcOut; juce::String destId; int destIn; };
    std::vector<UndoWire> deletedWires;
    struct EditUndo { juce::String objectId, type; juce::StringArray args; };
    std::vector<EditUndo> editUndoEntries;

    t_canvas* cnv = nullptr;
    // Bug #11 (fault gauntlet 2026-09-09): strict canvas resolution.
    // Unknown names must NOT silently fall back to the focused/root
    // canvas — name the failure instead of mutating the wrong patch.
    bool canvasNotFound = false;
    bool undoReset = false;
    bool pdLockBusy = false; // P3.4 S11: parse could not take the Pd lock within 2 s (nothing applied)

    void parse(const juce::OSCMessage& msg);
    void runPdJob();
    void reconcile(const std::string& canonStr);
    void disconnectPhase();
    void deletePhase(const std::string& canonStr);
    void editPhase(const std::string& canonStr);
    void createPhase(const std::string& canonStr);
    void connectPhase();
    void scanConnections(const std::string& canonStr);
    void runLayoutGuard(MCPBridge& bridge);
    void frameCreated(MCPBridge& bridge);
    void resetUndo();
    void recordUndo(const juce::OSCMessage& msg);
    juce::OSCMessage buildReply();
};

} // namespace mcp
