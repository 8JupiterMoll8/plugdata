/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge command: batch_atomic (Plan 3 P3.3): the handler moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

struct BatchDedupEntry {
  uint64_t ts = 0;
  juce::OSCMessage reply;
  BatchDedupEntry() = delete;
  BatchDedupEntry(uint64_t t, juce::OSCMessage r) : ts(t), reply(std::move(r)) {}
  BatchDedupEntry(const BatchDedupEntry&) = default;
  BatchDedupEntry(BatchDedupEntry&&) = default;
  BatchDedupEntry& operator=(const BatchDedupEntry&) = default;
  BatchDedupEntry& operator=(BatchDedupEntry&&) = default;
};

// Escape semicolons for Pd paste buffer format
static juce::String escapePdText(const juce::String& text)
{
    return text.replace(";", " \\;");
}

// Escape commas AND semicolons inside OBJECT box creation args so
// binbuf_text doesn't split them into separate messages. binbuf_text
// treats `,` as a message separator, so an unescaped `[expr pow($f1, 2)]`
// was created as `expr pow($f1` + a stray `2)` message. Backslash escapes
// are the Pd paste-buffer convention: `\,` → literal comma in the atom.
static juce::String escapePdObjArgs(const juce::String& text)
{
    juce::String out = text;
    out = out.replace("\\", "\\\\");
    out = out.replace(",", "\\,");
    out = out.replace(";", "\\;");
    return out;
}

// Convert MCP kind + tokens to Pd paste-format line
static juce::String formatAsPdLine(const juce::String& kind,
                                    const juce::StringArray& tokens,
                                    int x, int y)
{
    juce::StringArray t = tokens;

    if (kind == "msg" || kind == "message" || (!t.isEmpty() && t[0] == "msg")) {
        if (!t.isEmpty() && (t[0] == "msg" || t[0] == "message")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X msg " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "text" || kind == "comment" || (!t.isEmpty() && (t[0] == "text" || t[0] == "comment"))) {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X text " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        // The TS sends type:"" for kind floatatom/symbolatom, so t may START with
        // an empty token. Strip it (like the msg/text branches) or the paste
        // binbuf gets a stray empty atom that shifts the IEM arg count and the
        // object fails to create ("couldn't create — red box rolled back").
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        // PlugData's FloatAtomObject has a GUI-component lifecycle bug that
        // crashes the renderer when a [floatatom] is created/destroyed via the
        // MCP mutation engine (reproduced by the GUI stress gauntlet). [numbox]
        // is the equivalent numeric display/input with a stable component and
        // identical gatom argument layout, so substitute it. NOTE: the Pd class
        // name is "nbx" (g_numbox.c class_new(gensym("nbx"))) — "numbox" is only
        // an MCP-side alias and does NOT resolve in the paste/object-maker, which
        // is why '#X obj … numbox …' failed to create.
        t.insert(0, "nbx");
        return "#X obj " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X symbolatom " + juce::String(x) + " " + juce::String(y)
               + " " + t.joinIntoString(" ").trim() + ";";
    }

    // IEM GUI short-form guard: `hsl 128 15 0 1000 0` and friends must be
    // expanded to Pd's full 17-arg form or the range/init is silently dropped.
    t = mcp::expandIemGuiShortForm(t);

    if (!t.isEmpty() && t[0] == "+") t.set(0, "\\+");
    return "#X obj " + juce::String(x) + " " + juce::String(y)
           + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
}

// True-rect collision check (message thread) — same measurement as the
// /pd/collisions X-ray, so the guard and the verifier agree.
static bool mcpHasCollisions(PluginProcessor* processor, t_canvas* cnv, int pad)
{
    if (!processor || !cnv) return false;
    auto gui = mcpLiveBounds(processor, cnv);
    std::vector<t_gobj*> objs;
    struct R { int x, y, w, h; };
    std::vector<R> rects;
    sys_lock(); // P3.4 S3: reads gl_list and Pd geometry (this ran unlocked on the message thread)
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { if (mcpIsDecorativeBackground(y)) continue; objs.push_back(y); }
    rects.resize(objs.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        int x = 0, y = 0, w = 0, h = 0;
        mcpGetTrueObjectBounds(cnv, objs[i], gui.get(), &x, &y, &w, &h);
        rects[i] = { x, y, w, h };
    }
    sys_unlock();
    for (size_t i = 0; i < rects.size(); ++i)
        for (size_t j = i + 1; j < rects.size(); ++j) {
            auto& a = rects[i]; auto& b = rects[j];
            if (a.x < b.x + b.w + pad && a.x + a.w + pad > b.x
             && a.y < b.y + b.h + pad && a.y + a.h + pad > b.y) return true;
        }
    for (auto const& rg : processor->getMcp()->session.mcpRegions) {
        if (rg.title.isEmpty()) continue;
        int rx = static_cast<int>(rg.x), ry = static_cast<int>(rg.y);
        int rw = static_cast<int>(rg.w), rh = 26;
        for (size_t i = 0; i < rects.size(); ++i) {
            auto& a = rects[i];
            if (a.x < rx + rw + pad && a.x + a.w + pad > rx
             && a.y < ry + rh + pad && a.y + a.h + pad > ry) return true;
        }
    }
    return false;
}

void MCPBridge::pdBatchAtomic(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/batch_atomic <canvas> <corrId> <deleteCount> <disconnectCount> <editCount> <createCount> <connectCount> [data...]
    // Performs delete + disconnect + edit + create + connect in ONE sys_lock with exactly ONE DSP recompile.
    // Protocol:
    //   Deletes: [tempId] * deleteCount
    //   Disconnects: [srcId, srcOut, destId, destIn] * disconnectCount
    //   Edits: [tempId, newType, nargs, arg1..argN] * editCount
    //   Creates: [tempId, x, y, kind, type, nargs, arg1..argN] * createCount
    //   Connects: [srcId, srcOut, destId, destIn] * connectCount
    // Reply: /pd/batch_atomic/reply/<corrId> <created> <connected> <edited> <deleted> <disconnected>
    //        <reconcileEvicted> <reconcileAdopted> [(tempId, index)* mappings] <detectedConnectionsJson "[...]">
    //        then failure/advisory tail facts, each appended in a fixed order. The full layout (and the TS decoder
    //        that must match it) is in mcp-server docs/protocol/v1-wire-schema.json; the reply writer is below.
    // NOTE: the counts are NOT in request order (the request is delete, disconnect, edit, create, connect).
    if (msg.size() >= 5 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        // R3a — corrId dedup (PRD preflight): retry with same corrId must
        // not double-apply. LRU + TTL cache of full replies.
        static std::unordered_map<std::string, BatchDedupEntry> s_batchDedupCache;
        static std::deque<std::string> s_batchDedupOrder;
        static constexpr int BATCH_DEDUP_MAX = 128;
        static constexpr uint64_t BATCH_DEDUP_TTL_MS = 30000;
        if (!processor->getMcp()->session.isExecutingMcpUndoRedo) {
            std::string corrKey = correlationId.toStdString();
            uint64_t nowMs = juce::Time::currentTimeMillis();
            auto it = s_batchDedupCache.find(corrKey);
            if (it != s_batchDedupCache.end() && (nowMs - it->second.ts) < BATCH_DEDUP_TTL_MS) {
                sender.send(it->second.reply);
                return;
            }
            // Prune expired entries to bound memory.
            for (auto eit = s_batchDedupCache.begin(); eit != s_batchDedupCache.end();) {
                if ((nowMs - eit->second.ts) >= BATCH_DEDUP_TTL_MS) {
                    auto oit = std::find(s_batchDedupOrder.begin(), s_batchDedupOrder.end(), eit->first);
                    if (oit != s_batchDedupOrder.end()) s_batchDedupOrder.erase(oit);
                    eit = s_batchDedupCache.erase(eit);
                } else ++eit;
            }
        }
        
        int deleteCount = 0, disconnectCount = 0, editCount = 0, createCount = 0, connectCount = 0;
        int cursor = 2;

        if (msg.size() >= 7) {
            // 5-phase extended atomic protocol
            deleteCount = static_cast<int>(getArgFloat(msg[cursor++]));
            disconnectCount = static_cast<int>(getArgFloat(msg[cursor++]));
            editCount = static_cast<int>(getArgFloat(msg[cursor++]));
            createCount = static_cast<int>(getArgFloat(msg[cursor++]));
            connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
        } else {
            // Legacy 3-phase protocol fallback (creates, connects, edits)
            createCount = static_cast<int>(getArgFloat(msg[cursor++]));
            connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
            editCount = static_cast<int>(getArgFloat(msg[cursor++]));
        }

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

        // =========================================================================
        // PHASE 0: PRE-PARSE outside audio thread — zero contention
        // =========================================================================
        struct PreDelete { juce::String objectId; };
        std::vector<PreDelete> preDeletes;
        for (int d = 0; d < deleteCount && cursor < msg.size(); d++)
            preDeletes.push_back({ getArgString(msg[cursor++]) });

        struct PreDisconnect { juce::String srcId; int srcOut; juce::String destId; int destIn; };
        std::vector<PreDisconnect> preDisconnects;
        for (int dc = 0; dc < disconnectCount && cursor + 3 < msg.size(); dc++) {
            auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
            auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
            preDisconnects.push_back({ s, so, d2, di });
        }

        struct PreEdit { juce::String objectId; juce::String newText; };
        std::vector<PreEdit> preEdits;
        for (int e = 0; e < editCount && cursor < msg.size(); e++) {
            auto oid = getArgString(msg[cursor++]);
            auto nt = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
            int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
            juce::StringArray tk;
            if (nt != "msg" && nt.isNotEmpty()) tk.add(nt);
            for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));
            preEdits.push_back({ oid, tk.joinIntoString(" ") });
        }

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
        for (int o = 0; o < createCount && cursor < msg.size(); o++) {
            auto oid = getArgString(msg[cursor++]);
            float px = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
            float py = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
            auto kind = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
            auto ot = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
            int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
            juce::StringArray tk;
            if (ot.isNotEmpty()) tk.add(ot);
            for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));

            // tempId theft guard (authoritative — the bridge OWNS the id map).
            // A create whose tempId already resolves to a LIVE object would
            // silently overwrite mcpStableObjectMap (below), orphaning the
            // original as gui_* and misrouting every later op on that id.
            // Refuse it here; skip the paste + registration entirely.
            if (ot.isNotEmpty() && !oid.isEmpty()
                && processor->getMcp()->resolveStableId(canvasName, oid) != nullptr) {
                CreateFailure cf;
                cf.tempId = oid.toStdString();
                cf.type = ot.toStdString();
                // On a RETRY the model re-sends the whole batch, including
                // objects its previous call already created — "pick a new
                // id" then sends it renaming things it already owns. The
                // object it wants IS there; say that.
                cf.reason = "tempId already exists on the canvas - it was NOT re-created. To USE it, wire or set it directly (connect[]/set[]); to REPLACE it, rename or delete it first";
                createFailures.push_back(cf);
                continue;
            }

            // ── Control-kind preflight (PRD): detect the two SILENT GUI
            // failures straight from the creation args — inverted geometry
            // (an hsl built 15x120 reads as "no slider") and an invalid
            // range. The object still gets created; this only advises.
            // Arg positions match Source/analysis patch-analyzer
            // parseGuiProperties: tk[0]=class, tk[1]=width, tk[2]=height,
            // tk[3]=min, tk[4]=max (for hsl/vsl/nbx).
            {
                const juce::String cls = ot.toLowerCase();
                if (cls == "hsl" || cls == "vsl" || cls == "nbx") {
                    auto num = [&tk](int i, double& out) -> bool {
                        if (i < 0 || i >= tk.size()) return false;
                        if (tk[i].isEmpty()) return false;
                        out = tk[i].getDoubleValue();
                        return true;
                    };
                    double w = 0, h = 0, lo = 0, hi = 0;
                    if ((cls == "hsl" || cls == "vsl")
                        && num(1, w) && num(2, h) && w > 0 && h > 0) {
                        const bool inverted = (cls == "hsl") ? (h >= w) : (w >= h);
                        if (inverted) {
                            createAdvisories.push_back({ oid,
                                cls + " " + juce::String(w) + "x" + juce::String(h)
                                + (cls == "hsl"
                                    ? " — width/height look inverted: hsl wants width>height (wide+short), e.g. [128,15]"
                                    : " — width/height look inverted: vsl wants height>width (narrow+tall), e.g. [15,128]") });
                        }
                    }
                    if (num(3, lo) && num(4, hi) && lo > hi) {
                        createAdvisories.push_back({ oid,
                            cls + " min " + juce::String(lo) + " > max " + juce::String(hi)
                            + " — invalid range: swap min/max" });
                    }
                }
                if (cls == "knob") {
                    // ELSE knob args: knob <size> <min> <max> [exp] [load] …
                    // With fewer than 3 args Pd reads the missing min/max as 0,
                    // giving a 0→0 range = a knob that controls NOTHING (the
                    // artist turns it and hears no change).
                    if (tk.size() < 4) {
                        createAdvisories.push_back({ oid,
                            "knob '" + oid + "' has only a size — min/max default to 0 (range 0→0, controls NOTHING). Use knob <size> <min> <max> [exp] [load], e.g. knob 40 20 20000 1 220" });
                    }
                }
            }

            // line~ in this fork takes NO creation arguments (line_tilde_new(void)),
            // so `[line~ 0.057]` is silently created at 0. Capture a numeric init
            // arg here and re-seed it onto the object right after paste, so
            // `[r x] -> [line~ 440] -> [osc~]` actually starts at 440 instead of 0.
            bool seedInit = false;
            float initValue = 0.0f;
            if ((ot == "line~" || ot == "vline~") && na >= 1 && tk.size() >= 2) {
                juce::String firstArg = tk[1];
                if (firstArg.isNotEmpty()) {
                    auto charptr = firstArg.getCharPointer();
                    auto ptr = charptr;
                    juce::CharacterFunctions::readDoubleValue(ptr);
                    if (ptr - charptr == firstArg.getNumBytesAsUTF8()) {
                        initValue = firstArg.getFloatValue();
                        seedInit = true;
                    }
                }
            }

            pastaBuffer += formatAsPdLine(kind, tk, static_cast<int>(px), static_cast<int>(py)) + "\n";
            pendingCreates.push_back({ oid, ot, kind, static_cast<int>(px), static_cast<int>(py), initValue, seedInit });
        }

        // ALL connections go through obj_connect after paste (no #X connect in buffer)
        struct CrossConn { juce::String srcId; int srcOut; juce::String destId; int destIn; };
        std::vector<CrossConn> allConns;
        for (int c = 0; c < connectCount && cursor + 3 < msg.size(); c++) {
            auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
            auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
            allConns.push_back({ s, so, d2, di });
        }

        // Optional trailing flags (PRD_CONTEXT_LAYOUT_GUARD P2/P3/P3.1):
        // autoLayout (default ON), pad, grid snap, fixOcclusions (default ON).
        // Old clients omit them.
        bool autoLayout = true;
        int layoutPad = 5, layoutSnap = 10;
        bool autoFixOcclusions = true;
        if (cursor < msg.size()) autoLayout = getArgFloat(msg[cursor++]) > 0.5f;
        if (cursor < msg.size()) layoutPad = static_cast<int>(getArgFloat(msg[cursor++]));
        if (cursor < msg.size()) layoutSnap = static_cast<int>(getArgFloat(msg[cursor++]));
        if (cursor < msg.size()) autoFixOcclusions = getArgFloat(msg[cursor++]) > 0.5f;

        if (processor->getMcp()->session.isExecutingMcpUndoRedo) {
            autoLayout = false;
            autoFixOcclusions = false;
        }


        // =========================================================================
        // EXECUTE IN THE PD CONTEXT (message thread, sys_lock; P3.4 S3)
        // =========================================================================

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
        // Plan 3 P3.4 S3: the mutation runs in the Pd context = the message thread under
        // sys_lock, where plugdata's own GUI edits Pd (the old note here said GUI edits
        // went through the audio-thread queue; they do not). See McpAudio.cpp.

        extern int dsp_update_deferred;  // defined in m_obj.c
        // Report F3: the lambda below captures this frame by reference. The
        // handoff guarantees we never return while it can still touch it.
        auto handoff = std::make_shared<mcp::OneShotHandoff>();
        t_canvas* cnv = nullptr;
        // Bug #11 (fault gauntlet 2026-09-09): strict canvas resolution.
        // Unknown names must NOT silently fall back to the focused/root
        // canvas — name the failure instead of mutating the wrong patch.
        bool canvasNotFound = false;

        processor->getMcp()->runPd([&, handoff]() {
            if (!handoff->begin())
                return; // the caller gave up: this frame may be gone, touch nothing
            mcp::OneShotHandoff::FinishGuard finishGuard(*handoff);
            auto tLambdaStart = std::chrono::high_resolution_clock::now();
            cnv = processor->getMcp()->getCanvasBySymbolStrict(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                cnv = pd_this->pd_canvaslist;
            if (!cnv) {
                canvasNotFound = true;
                post("batch_atomic: canvas '%s' not found — mutation refused", canvasName.toRawUTF8());
            }

            if (cnv) {
                dsp_update_deferred = 1;
                auto canonKey = MCPBridge::canonicalCanvasKey(cnv);
                auto canonStr = canonKey.toStdString();

                // PHASE 0: PRE-FLIGHT AUTO-RECONCILE
                // Single walk of gl_list to sync identity map with live canvas.
                // Detects manual GUI edits (user created/deleted objects) and
                // fixes the map BEFORE any mutation — zero extra OSC round-trips,
                // zero audio dropout (runs inside the same audio-thread lambda).
                {
                    auto canvasStr = canonStr;
                    auto& canvasMap = processor->getMcp()->session.mcpStableObjectMap[canvasStr];

                    // Step A: Build set of all live pointers from gl_list (O(n))
                    std::unordered_set<t_gobj*> livePointers;
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                        livePointers.insert(y);

                    // Step B: Evict stale entries — user deleted objects via GUI
                    std::vector<std::string> toEvict;
                    for (auto& [tempId, ptr] : canvasMap) {
                        if (livePointers.find(ptr) == livePointers.end()) {
                            toEvict.push_back(tempId);
                            processor->getMcp()->session.mcpStableSerialMap.erase(ptr);
                        }
                    }
                    for (auto& id : toEvict) {
                        canvasMap.erase(id);
                        processor->getMcp()->session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                        reconcileEvicted++;
                    }

                    // Step C: Adopt untracked objects — user created via GUI
                    // Build reverse set of tracked pointers for O(1) lookup
                    std::unordered_set<t_gobj*> trackedPointers;
                    for (auto& [_, ptr] : canvasMap)
                        trackedPointers.insert(ptr);

                    // Build set of tempIds being edited in this transaction
                    // so we can re-assign them instead of creating gui_* names.
                    std::unordered_set<std::string> editedIds;
                    for (auto& pe : preEdits)
                        editedIds.insert(pe.objectId.toStdString());

                    // Also build ordered list of evicted tempIds for re-use
                    // (Step B evicted them because their pointer changed after edit)
                    std::vector<std::string> evictedEditIds;
                    for (auto& id : toEvict) {
                        if (editedIds.count(id))
                            evictedEditIds.push_back(id);
                    }
                    size_t evictedEditIdx = 0;

                    int adoptIdx = 0;
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                        if (trackedPointers.find(y) == trackedPointers.end()) {
                            // Untracked object — auto-assign a tempId
                            // Read class name for a meaningful prefix
                            // If this untracked object corresponds to an edited
                            // tempId (pointer changed after renameObject), reuse
                            // the original tempId instead of assigning gui_*.
                            // This keeps the identity map stable across edits.
                            std::string autoId;
                            if (evictedEditIdx < evictedEditIds.size()) {
                                autoId = evictedEditIds[evictedEditIdx++];
                            } else {
                                juce::String className = juce::String::fromUTF8(
                                    class_getname(pd_class(&y->g_pd)));
                                className = className.replace("~", "_t");
                                autoId = ("gui_" + className + "_"
                                    + juce::String(adoptIdx++)).toStdString();
                                while (canvasMap.count(autoId))
                                    autoId = ("gui_" + className + "_"
                                        + juce::String(adoptIdx++)).toStdString();
                            }
                            canvasMap[autoId] = y;
                            processor->getMcp()->session.mcpStableSerialMap[y] = processor->getMcp()->session.mcpSerialCounter++;
                            processor->getMcp()->session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                            reconcileAdopted++;
                        }
                    }

                     // Step D: Connection scan — MOVED to post-PHASE-5 (bug #5,
                     // fault gauntlet 2026-09-09). Scanning inside PHASE 0 made
                     // detectedConnections lag one transaction: the receipt
                     // showed PRE-mutation wires; wires created by this very
                     // batch only appeared one call later. Scan now runs AFTER
                     // connect/edit/delete so the receipt is always current.
                     // (linetraverser walks every wire on the canvas in O(wires);
                     // both endpoints have tempIds — new creates are registered
                     // in mcpStableObjectMap by PHASE 4 before PHASE 5 wiring.)
                     // Build reverse ptr→tempId map (O(m)) — kept for the
                     // post-mutation Step D scan below (bug #5 move).
                }

                // PHASE 1: DISCONNECT — remove wires BEFORE objects are deleted
                // so endpoints and stable IDs remain valid for clean unlinking.
                for (auto& pdc : preDisconnects) {
                    t_gobj* sg = processor->getMcp()->resolveStableId(canvasName, pdc.srcId);
                    t_gobj* dg = processor->getMcp()->resolveStableId(canvasName, pdc.destId);
                    if (sg && dg) {
                        t_object* so = pd::Interface::checkObject(sg);
                        t_object* d_o = pd::Interface::checkObject(dg);
                        // Only count wires that ACTUALLY existed. obj_disconnect
                        // is a silent no-op for a missing wire, but we counted
                        // the attempt ("1 disconnect(s)" while removing nothing).
                        if (so && d_o && pd::Interface::hasConnection(cnv, so, pdc.srcOut, d_o, pdc.destIn)) {
                            obj_disconnect(so, pdc.srcOut, d_o, pdc.destIn);
                            disconnected++;
                        }
                    }
                }

                // PHASE 2: DELETE — audio-thread-safe object removal (zero undo/GUI
                // overhead; editor reconciles via trailing synchroniseCanvases).
                // Capture each doomed object's creation text + bounds + wires
                // FIRST so the inverse can recreate it (message-delta, no pointers).
                {
                    auto& delMap = processor->getMcp()->session.mcpStableObjectMap[canonStr];
                    std::unordered_map<t_gobj*, std::string> ptrToTempId;
                    for (auto& [tid, ptr] : delMap)
                        if (ptr) ptrToTempId[ptr] = tid;

                    std::unordered_set<t_gobj*> deleteSet;
                    for (auto& pd : preDeletes) {
                        t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pd.objectId);
                        if (!obj) continue;
                        deleteSet.insert(obj);

                        DeletedSnapshot ds;
                        ds.tempId = pd.objectId;
                        juce::String cls = juce::String::fromUTF8(class_getname(pd_class(&obj->g_pd)));
                        ds.kind = "obj";
                        if (cls == "text" || cls == "comment") ds.kind = "text";
                        else if (cls == "msg" || cls == "message") ds.kind = "msg";
                        if (auto* tobj = pd::Interface::checkObject(obj)) {
                            char* tb = nullptr; int tsz = 0;
                            mcpGetObjectText(tobj, &tb, &tsz);
                            juce::String full = (tb && tsz) ? juce::String::fromUTF8(tb, tsz) : juce::String();
                            if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));
                            juce::StringArray toks; toks.addTokens(full, " ", "");
                            if (ds.kind == "obj") {
                                ds.type = toks.size() > 0 ? toks[0] : full;
                                for (int i = 1; i < toks.size(); ++i) ds.args.add(toks[i]);
                            } else {
                                ds.type = cls;   // "text"/"msg" — formatAsPdLine strips it
                                ds.args = toks;
                            }
                        }
                        int bx = 0, by = 0, bw = 0, bh = 0;
                        pd::Interface::getObjectBounds(cnv, obj, &bx, &by, &bw, &bh);
                        ds.x = bx; ds.y = by;
                        deletedSnapshots.push_back(ds);
                    }

                    // Wires touching any doomed object (captured while tempIds live).
                    if (!deleteSet.empty()) {
                        t_linetraverser lt;
                        linetraverser_start(&lt, cnv);
                        t_outconnect* ltoc = nullptr;
                        while ((ltoc = linetraverser_next_nosize(&lt))) {
                            t_gobj* sg = &lt.tr_ob->ob_g;
                            t_gobj* dg = &lt.tr_ob2->ob_g;
                            if (!deleteSet.count(sg) && !deleteSet.count(dg)) continue;
                            auto sIt = ptrToTempId.find(sg);
                            auto dIt = ptrToTempId.find(dg);
                            if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
                            deletedWires.push_back({ juce::String(sIt->second),
                                static_cast<int>(lt.tr_outno),
                                juce::String(dIt->second),
                                static_cast<int>(lt.tr_inno) });
                        }
                    }
                }

                SmallArray<t_gobj*> toDelete;
                for (auto& pd : preDeletes) {
                    t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pd.objectId);
                    if (obj) {
                        processor->getMcp()->session.mcpStableObjectMap[canonStr].erase(pd.objectId.toStdString());
                        processor->getMcp()->session.mcpStableSerialMap.erase(obj);
                        processor->getMcp()->session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                        toDelete.add(obj);
                        deleted++;
                    }
                }
                if (toDelete.size() > 0) {
                    mcp::PdOps::removeObjectsAudioThread(cnv, toDelete);
                }

                // PHASE 3: EDIT
                for (auto& pe : preEdits) {
                    t_gobj* obj = processor->getMcp()->resolveStableId(canvasName, pe.objectId);
                    if (obj) {
                        t_object* o = pd::Interface::checkObject(obj);
                        if (o) {
                            // Capture old creation text so undo can edit it back.
                            {
                                char* tb = nullptr; int tsz = 0;
                                mcpGetObjectText(o, &tb, &tsz);
                                juce::String oldT = (tb && tsz) ? juce::String::fromUTF8(tb, tsz) : juce::String();
                                if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));
                                juce::StringArray toks; toks.addTokens(oldT, " ", "");
                                EditUndo eu;
                                eu.objectId = pe.objectId;
                                eu.type = toks.size() > 0 ? toks[0] : oldT;
                                for (int i = 1; i < toks.size(); ++i) eu.args.add(toks[i]);
                                editUndoEntries.push_back(eu);
                            }
                            mcp::PdOps::renameObject(cnv, obj, pe.newText.toRawUTF8(), pe.newText.length());
                            t_gobj* newObj = nullptr; bool still = false;
                            for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                                if (y == obj) { still = true; break; }
                            newObj = still ? obj : pd::Interface::getNewest(cnv);
                            if (newObj) {
                                processor->getMcp()->session.mcpStableObjectMap[canonStr][pe.objectId.toStdString()] = newObj;
                                if (newObj != obj) processor->getMcp()->session.mcpStableSerialMap.erase(obj);
                                processor->getMcp()->session.mcpStableSerialMap[newObj] = processor->getMcp()->session.mcpSerialCounter++;
                                processor->getMcp()->session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                // C++ truth: when a class can't be instantiated Pd
                                // substitutes text_class (g_text.c canvas_objtext,
                                // "... couldn't create") and leaves an ERROR BOX — the
                                // object still exists, so counting it as `edited` was a
                                // FALSE SUCCESS (a bogus retype reported ok:true). Detect
                                // the substitution and report an edit failure instead.
                                juce::String wantCls = pe.newText.upToFirstOccurrenceOf(" ", false, false).trim().toLowerCase();
                                t_object* no = pd::Interface::checkObject(newObj);
                                juce::String gotCls = no
                                    ? juce::String::fromUTF8(class_getname(pd_class(&no->ob_pd))).toLowerCase()
                                    : juce::String();
                                if (!no || (gotCls == "text" && wantCls != "text" && wantCls != "comment")) {
                                    editFailures.push_back({ pe.objectId.toStdString(),
                                        pe.newText.toStdString(),
                                        "class could not be created — object left as an error box (check the external/library)" });
                                } else {
                                    edited++;
                                }
                            }
                        }
                    }
                }

                // PHASE 4: CREATE via pasteDirect
                int preCreateCount = 0;
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next) preCreateCount++;

                if (createCount > 0 && pastaBuffer.isNotEmpty()) {
                    pasteDirect(cnv, pastaBuffer.toRawUTF8());

                    std::vector<t_gobj*> allObjects;
                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                        allObjects.push_back(g);
                    int newCount = static_cast<int>(allObjects.size()) - preCreateCount;

                    struct NewObjInfo {
                        t_gobj* ptr = nullptr;
                        int glIndex = -1;
                        int x = 0;
                        int y = 0;
                        int type = -1;
                        juce::String firstWord;
                        juce::String className;
                        bool isRedBox = false;
                    };

                    std::vector<NewObjInfo> newInfos;
                    for (int i = 0; i < newCount; ++i) {
                        t_gobj* g = allObjects[preCreateCount + i];
                        NewObjInfo info;
                        info.ptr = g;
                        info.glIndex = preCreateCount + i;
                        t_object* ob = pd::Interface::checkObject(g);
                        if (ob) {
                            info.x = ob->te_xpix;
                            info.y = ob->te_ypix;
                            info.type = ob->te_type;
                            t_class* cl = pd_class(&g->g_pd);
                            if (cl) {
                                const char* cName = class_getname(cl);
                                if (cName) info.className = juce::String::fromUTF8(cName);
                            }
                            info.isRedBox = (ob->te_type == T_OBJECT && cl == text_class);
                            if (ob->te_binbuf) {
                                char* tb = nullptr; int tsz = 0;
                                binbuf_gettext(ob->te_binbuf, &tb, &tsz);
                                if (tb && tsz > 0) {
                                    juce::String full = juce::String::fromUTF8(tb, tsz).trim();
                                    info.firstWord = full.upToFirstOccurrenceOf(" ", false, false);
                                    freebytes(tb, tsz);
                                }
                            }
                        }
                        newInfos.push_back(info);
                    }

                    auto computeMatchScore = [](const PendingCreate& pc, const NewObjInfo& no) -> int {
                        int score = 0;
                        // Exact coordinate match is a strong structural anchor
                        if (pc.x == no.x && pc.y == no.y) score += 20;

                        // Red-box match: Pd creates text_class dummy, but te_binbuf preserves original type
                        if (no.isRedBox && no.firstWord == pc.objType) {
                            score += 25;
                            return score;
                        }

                        // Class name / binbuf first word match
                        if (no.firstWord.isNotEmpty() && no.firstWord == pc.objType) {
                            score += 20;
                        } else if (no.className.isNotEmpty() && no.className == pc.objType) {
                            score += 20;
                        }

                        // Kind match
                        if (pc.kind == "msg" && no.type == T_MESSAGE) score += 10;
                        else if (pc.kind == "text" && no.type == T_TEXT) score += 10;
                        else if ((pc.kind == "floatatom" || pc.kind == "symbolatom") &&
                                 (no.type == T_ATOM || no.className.containsIgnoreCase("atom"))) score += 10;
                        else if ((pc.kind == "obj" || pc.kind.isEmpty()) && no.type == T_OBJECT) score += 5;

                        return score;
                    };

                    // Monotonic alignment of created gobjs to pendingCreates.
                    // Since pasteDirect appends strictly in buffer order, newInfos
                    // is guaranteed to be a monotonic subsequence of pendingCreates.
                    int pCur = 0;
                    const int totalPending = static_cast<int>(pendingCreates.size());

                    // Red-box ghosts (failed creates) collected for rollback
                    // AFTER alignment, so the mapping scan reflects the final
                    // gl_list. A failed create must be a pure no-op (parity with
                    // the GOP-encapsulate path).
                    SmallArray<t_gobj*> redBoxes;

                    for (int o = 0; o < newCount; ++o) {
                        const auto& no = newInfos[o];
                        int bestP = -1;
                        int bestScore = 0;

                        int remainingObjs = newCount - 1 - o;
                        int maxSearchP = totalPending - remainingObjs;

                        for (int p = pCur; p < maxSearchP; ++p) {
                            int score = computeMatchScore(pendingCreates[p], no);
                            if (score > bestScore) {
                                bestScore = score;
                                bestP = p;
                            }
                        }

                        if (bestP < 0) {
                            bestP = pCur;
                        }

                        // All pendingCreates between pCur and bestP produced NO gobj on canvas
                        for (int p = pCur; p < bestP; ++p) {
                            createFailures.push_back({
                                pendingCreates[p].tempId.toStdString(),
                                pendingCreates[p].objType.toStdString(),
                                "couldn't create"
                            });
                        }

                        auto& pc = pendingCreates[bestP];

                        // Check if this object is a dummy red box
                        if (no.isRedBox) {
                            // Roll the red box back so a failed create leaves
                            // NOTHING behind (parity with the GOP-encapsulate
                            // path). The ghost is still named in createFailures.
                            createFailures.push_back({
                                pc.tempId.toStdString(),
                                pc.objType.toStdString(),
                                "couldn't create — red box rolled back (nothing left on canvas)"
                            });
                            redBoxes.add(no.ptr);
                        } else {
                            // Map the stable tempId to the live gobj pointer
                            processor->getMcp()->session.mcpStableObjectMap[canonStr][pc.tempId.toStdString()] = no.ptr;
                            processor->getMcp()->session.mcpStableSerialMap[no.ptr] = processor->getMcp()->session.mcpSerialCounter++;
                            processor->getMcp()->session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

                            // Auto-seed line~/vline~ init value (fork ignores creation args)
                            if (pc.seedInit) {
                                t_object* so = pd::Interface::checkObject(no.ptr);
                                if (so) {
                                    t_atom sa;
                                    SETFLOAT(&sa, pc.initValue);
                                    pd_typedmess(reinterpret_cast<t_pd*>(so), gensym("float"), 1, &sa);
                                }
                            }

                            createdIds.push_back(pc.tempId.toStdString());
                            createdPtrs.push_back(no.ptr);
                            mappingIndices.push_back(static_cast<int32>(no.glIndex));
                            created++;
                        }

                        pCur = bestP + 1;
                    }

                    // Any remaining pendingCreates after all newInfos were matched
                    // had no corresponding live object
                    for (int p = pCur; p < totalPending; ++p) {
                        createFailures.push_back({
                            pendingCreates[p].tempId.toStdString(),
                            pendingCreates[p].objType.toStdString(),
                            "couldn't create"
                        });
                    }

                    // Roll back red-box ghosts now that alignment is done.
                    // Audio-thread-safe removal (they were created moments ago in
                    // this same lambda; GUI has not synced them yet). Rebuild the
                    // index mappings against the post-delete gl_list so the reply's
                    // tempId→index pairs stay correct.
                    if (redBoxes.size() > 0) {
                        mcp::PdOps::removeObjectsAudioThread(cnv, redBoxes);
                        for (size_t mi = 0; mi < mappingIndices.size() && mi < createdPtrs.size(); ++mi) {
                            if (!createdPtrs[mi]) continue;
                            int idx = 0, found = -1;
                            for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                                if (y == createdPtrs[mi]) { found = idx; break; }
                            }
                            if (found >= 0) mappingIndices[mi] = found;
                        }
                    }
                }

                // PHASE 5: CONNECT via obj_connect
                // R2 — live signal adjacency for cycle detection (PRD preflight).
                // Built once before the loop: signal edges only. Bounded: skip
                // if graph too large (2000 objs / 4000 wires) — X-ray still reports.
                std::unordered_map<t_gobj*, std::vector<t_gobj*>> liveSigAdj;
                std::unordered_map<t_gobj*, std::vector<t_gobj*>> tentativeSigAdj;
                bool cycleGuardActive = false;
                {
                    int liveObjCount = 0;
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) liveObjCount++;
                    if (liveObjCount <= 2000) {
                        t_linetraverser ltCount;
                        linetraverser_start(&ltCount, cnv);
                        int wireCount = 0;
                        while (linetraverser_next_nosize(&ltCount)) wireCount++;
                        if (wireCount <= 4000) {
                            cycleGuardActive = true;
                            t_linetraverser lt;
                            linetraverser_start(&lt, cnv);
                            t_outconnect* ocTmp = nullptr;
                            while ((ocTmp = linetraverser_next_nosize(&lt))) {
                                t_gobj* sgTmp = &lt.tr_ob->ob_g;
                                t_gobj* dgTmp = &lt.tr_ob2->ob_g;
                                t_object* soTmp = pd::Interface::checkObject(sgTmp);
                                if (soTmp && obj_issignaloutlet(soTmp, lt.tr_outno)) {
                                    liveSigAdj[sgTmp].push_back(dgTmp);
                                }
                            }
                            tentativeSigAdj = liveSigAdj;
                        }
                    }
                }
                for (auto& cc : allConns) {
                    t_gobj* sg = processor->getMcp()->resolveStableId(canvasName, cc.srcId);
                    t_gobj* dg = processor->getMcp()->resolveStableId(canvasName, cc.destId);
                    if (!sg || !dg) {
                        // Phase A (PRD diagnostic layer): name the silent
                        // failure instead of skipping silently.
                        std::string reason;
                        if (!sg && !dg) reason = "src and dest not found";
                        else if (!sg)   reason = "src not found";
                        else            reason = "dest not found";
                        connectFailures.push_back({
                            cc.srcId.toStdString(), cc.destId.toStdString(), reason });
                        continue;
                    }
                    t_object* so = pd::Interface::checkObject(sg);
                    t_object* d_o = pd::Interface::checkObject(dg);
                    if (so && d_o) {
                        // Phase B (fault-injection gauntlet 2026-09-09):
                        // name self-connections, duplicate wires, and
                        // out-of-range ports instead of counting them as
                        // fresh successes.
                        if (sg == dg) {
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "self-connection (same object src==dest) — forbidden, breaks DSP scheduling" });
                            continue;
                        }
                        if (cc.srcOut < 0 || cc.srcOut >= obj_noutlets(so)) {
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "src outlet " + std::to_string(cc.srcOut) + " out of range (object has "
                                    + std::to_string(obj_noutlets(so)) + ")" });
                            continue;
                        }
                        if (cc.destIn < 0 || cc.destIn >= obj_ninlets(d_o)) {
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "dest inlet " + std::to_string(cc.destIn) + " out of range (object has "
                                    + std::to_string(obj_ninlets(d_o)) + ")" });
                            continue;
                        }
                        // Duplicate-wire check: Pd silently accepts re-connecting
                        // an existing wire; count it as a no-op, not a fresh success.
                        // Walk the source outlet's connection list (Pd traverse API).
                        bool alreadyWired = false;
                        {
                            t_outlet* srcOutlet = nullptr;
                            t_outconnect* oc = obj_starttraverseoutlet(so, &srcOutlet, cc.srcOut);
                            while (oc) {
                                t_object* destObj = nullptr;
                                t_inlet* destInl = nullptr;
                                int which = -1;
                                oc = obj_nexttraverseoutlet(oc, &destObj, &destInl, &which);
                                if (destObj == d_o && which == cc.destIn) {
                                    alreadyWired = true;
                                    break;
                                }
                            }
                        }
                        if (alreadyWired) {
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "duplicate wire (already connected) — no-op, not counted" });
                            continue;
                        }
                        // R1 — Rate guard (PRD preflight, 2026-09-09): signal outlet
                        // -> control inlet. Same rule as Interface::canConnect
                        // (return !obj_issignaloutlet || obj_issignalinlet). The
                        // GUI enforces it; batch_atomic previously bypassed it
                        // via raw obj_connect. Now REJECT with stable prefix.
                        {
                            bool srcSig  = obj_issignaloutlet(so,  cc.srcOut) != 0;
                            bool destSig = obj_issignalinlet(d_o, cc.destIn)  != 0;
                            if (srcSig && !destSig) {
                                // Class-aware hint. The classic trap: [*~ 0.5] has a
                                // FLOAT inlet 1, so an envelope signal into it is
                                // correctly rejected — while a BARE [*~] has a SIGNAL
                                // inlet 1 and IS the canonical VCA. The old wording
                                // ("rewire via [*~]") left the model flailing, since
                                // the wire already pointed at [*~].
                                // ASCII only: non-ASCII in a reply string is mangled to
                                // mojibake on the C++ -> OSC -> TS path (see line 667).
                                juce::String hint = " - Pd drops the audio; use [snapshot~] to read it as control";
                                {
                                    const char* dcls = class_getname(pd_class(&d_o->ob_pd));
                                    juce::String dclsStr(dcls ? dcls : "");
                                    if (dclsStr == "*~" || dclsStr == "+~" || dclsStr == "-~" || dclsStr == "/~") {
                                        char* otext = nullptr; int olen = 0;
                                        mcpGetObjectText(d_o, &otext, &olen);
                                        juce::String objTxt = (otext && olen > 0)
                                            ? juce::String::fromUTF8(otext, olen) : dclsStr;
                                        if (otext && olen > 0) freebytes(otext, olen);
                                        hint = " - [" + objTxt.trim()
                                             + "] has an ARGUMENT, so its inlet 1 is a FLOAT (control) inlet and a signal there is dropped. "
                                               "For an envelope VCA use a BARE [*~] (no args): its inlet 1 IS a signal inlet. "
                                               "Keep the level in a separate [*~ gain].";
                                    }
                                }
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "rate: signal outlet " + std::to_string(cc.srcOut)
                                      + " -> control inlet " + std::to_string(cc.destIn)
                                      + hint.toStdString() });
                                continue;
                            }
                            // Guard 2: control outlet -> signal inlet. Pd accepts
                            // this (sets the inlet's scalar, updated once per block)
                            // so we do NOT reject — and it is NOT a required bridge.
                            // Wording matches the TS rate-advisory (canonicalFix):
                            // "legal in Pd (no bridge needed)".
                            if (!srcSig && destSig) {
                                connectAdvisories.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "control outlet " + std::to_string(cc.srcOut)
                                      + " -> signal inlet " + std::to_string(cc.destIn)
                                      + " — legal in Pd (the float sets the signal inlet; no bridge needed). "
                                        "Optional: [sig~] only if you want audio-rate smoothing" });
                            }
                        }
                        // R2 — would this wire close a zero-delay signal loop?
                        // Only signal-outlet wires can close signal cycles; control
                        // edges are not tracked. Tentative graph grows in batch order
                        // so the FIRST loop-closing wire is rejected.
                        if (cycleGuardActive && obj_issignaloutlet(so, cc.srcOut)) {
                            std::unordered_set<t_gobj*> visited;
                            std::vector<t_gobj*> stack;
                            stack.push_back(dg);
                            visited.insert(dg);
                            bool closesCycle = false;
                            while (!stack.empty() && !closesCycle) {
                                t_gobj* cur = stack.back(); stack.pop_back();
                                if (cur == sg) { closesCycle = true; break; }
                                auto it = tentativeSigAdj.find(cur);
                                if (it == tentativeSigAdj.end()) continue;
                                for (t_gobj* nxt : it->second) {
                                    if (visited.insert(nxt).second) stack.push_back(nxt);
                                }
                            }
                            if (closesCycle) {
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "cycle: would close zero-delay signal loop — break it with [delwrite~]/[delread~] or [send~]/[receive~]" });
                                continue;
                            }
                            tentativeSigAdj[sg].push_back(dg);
                        }
                        if (obj_connect(so, cc.srcOut, d_o, cc.destIn))
                            connected++;
                        else
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "obj_connect failed" });
                    } else {
                        connectFailures.push_back({
                            cc.srcId.toStdString(), cc.destId.toStdString(),
                            "not a patchable object" });
                    }
                }

                // Step D (moved, bug #5): post-mutation connection scan — runs
                // AFTER PHASE 5 connect so the receipt reflects the wires this
                // batch just created. PHASE 0 adoption already minted tempIds
                // for GUI objects; PHASE 4 registered new creates; deletes
                // were erased — every live wire endpoint resolves.
                {
                    auto canvasStr = canonStr;
                    auto& canvasMap = processor->getMcp()->session.mcpStableObjectMap[canvasStr];
                    std::unordered_map<t_gobj*, std::string> ptrToTempId;
                    for (auto& [tid, ptr] : canvasMap)
                        if (ptr) ptrToTempId[ptr] = tid;

                    t_linetraverser lt;
                    linetraverser_start(&lt, cnv);
                    t_outconnect* ltOc = nullptr;
                    while ((ltOc = linetraverser_next_nosize(&lt))) {
                        t_gobj* sg = &lt.tr_ob->ob_g;
                        t_gobj* dg = &lt.tr_ob2->ob_g;
                        auto sIt = ptrToTempId.find(sg);
                        auto dIt = ptrToTempId.find(dg);
                        if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
                        detectedConnections.push_back({
                            sIt->second,
                            static_cast<int>(lt.tr_outno),
                            dIt->second,
                            static_cast<int>(lt.tr_inno)
                        });
                    }
                }

                canvas_dirty(cnv, 1);
                dsp_update_deferred = 0;

                auto t0 = std::chrono::high_resolution_clock::now();
                canvas_update_dsp();
                auto t1 = std::chrono::high_resolution_clock::now();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                post("batch_atomic: canvas_update_dsp took %lld us", (long long)us);
            }

            // Fire loadbangs
            for (auto* g : createdPtrs) {
                if (g) {
                    if (pd_class(&g->g_pd) == canvas_class)
                        canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                    else if (zgetfn(&g->g_pd, gensym("loadbang")))
                        vmess(&g->g_pd, gensym("loadbang"), "f", LB_LOAD);
                }
            }

            // (layout guard + fallback run on the message thread below.)

            auto tLambdaEnd = std::chrono::high_resolution_clock::now();
            auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(tLambdaEnd - tLambdaStart).count();
            post("batch_atomic: TOTAL lambda took %lld us", (long long)totalUs);
        }); // end enqueueFunctionAsync lambda (finishGuard signals the caller)

        // Wait for the Pd context to run it (max 2000ms)
        auto tWaitStart = std::chrono::high_resolution_clock::now();
        // Returns early only if the lambda never started; once it runs, we
        // wait for it to finish however long that takes.
        bool const waitOk = handoff->waitOrAbandon(std::chrono::milliseconds(2000))
            == mcp::OneShotHandoff::Result::Finished;
        if (waitOk && handoff->wasLate())
            post("batch_atomic: finished after the 2000ms budget (the Pd context was busy)");
        auto tWaitEnd = std::chrono::high_resolution_clock::now();
        auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(tWaitEnd - tWaitStart).count();
        post("batch_atomic: done.wait() took %lld us (%.1f ms)", (long long)waitUs, waitUs / 1000.0f);
        // R3b + Report F3: !waitOk now means the lambda was abandoned before
        // it began, so NOTHING was applied (a started batch is always waited
        // for). Say so plainly; the TS side classifies this as "rejected".
        if (!waitOk) {
            juce::OSCMessage errReply { juce::OSCAddressPattern("/pd/batch_atomic/error/" + correlationId) };
            errReply.addArgument(juce::String("rejected: the Pd context did not start the batch within 2000ms; nothing was applied"));
            sender.send(errReply);
            return;
        }

        // PRD_CONTEXT_LAYOUT_GUARD (P2/P3/P3.1): layout guard on the JUCE
        // message thread. Deoverlap + occlusion rounds (fast, Pd coords),
        // then a GUARANTEED fallback: if true-rect collisions remain,
        // compose the canvas by role (proven collision-free).
        if (cnv && autoLayout) {
            auto runLayout = [&]() {
                const int guardPad = layoutPad + 15;
                for (int round = 0; round < 8; round++) {
                    sys_lock();
                    int c = sanitizeLayout(processor, cnv, guardPad, layoutSnap);
                    int o = autoFixOcclusions ? fixOcclusions(processor, cnv, guardPad, layoutSnap) : 0;
                    sys_unlock();
                    layoutSanitizedMoved += c;
                    occlusionsFixed += o;
                    if (c == 0 && o == 0) break;
                }
                if (mcpHasCollisions(processor, cnv, 5)) {
                    // Artist & Agent mandate: Do NOT obliterate creative layout with composeLayout!
                    // Preserve modular columns and emit warning instead of smashing canvas to y=40.
                    fallbackComposed = true;
                }
                processor->getMcp()->synchroniseCanvases();
            };

            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                runLayout();
            } else {
                // Report F3: runLayout captures this frame by reference.
                auto layoutHandoff = std::make_shared<mcp::OneShotHandoff>();
                juce::MessageManager::callAsync([&runLayout, layoutHandoff]() {
                    if (!layoutHandoff->begin())
                        return; // abandoned: the frame may be gone
                    mcp::OneShotHandoff::FinishGuard finishGuard(*layoutHandoff);
                    runLayout();
                });
                if (layoutHandoff->waitOrAbandon(std::chrono::milliseconds(6000))
                    == mcp::OneShotHandoff::Result::Abandoned)
                    post("batch_atomic: layout guard skipped, message thread busy for 6000ms");
            }
        } else {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                if (processor) processor->getMcp()->synchroniseCanvases();
            } else {
                // Decoupled UI viewport sync - non-blocking async idle dispatch
                juce::MessageManager::callAsync([p = processor] {
                    if (p) p->getMcp()->synchroniseCanvases();
                });
            }
        }

        // Auto-frame newly-created objects: if a CREATED object landed outside
        // the current viewport, PAN it into view — never change the zoom.
        // (The old zoomToFitAll() fit the whole canvas and yanked the artist's
        // zoom from 100% to 66% on every offscreen build.) Only counts objects
        // created in THIS batch, so panning around never triggers it; a
        // creation inside the view never moves the camera.
        if (cnv && created > 0 && !canvasNotFound) {
            auto frameIfCreateOffscreen = [p = processor, cnv, ptrs = createdPtrs]() {
                p->getMcp()->gui().frameCreatedObjects(cnv, ptrs);
            };
            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                frameIfCreateOffscreen();
            } else {
                juce::MessageManager::callAsync(frameIfCreateOffscreen);
            }
        }

        // MCP mutations never register correct Pd undo actions, yet they
        // still reorder/free objects that pre-existing index-based undo
        // entries point at (delete shifts indices; encapsulate/GOP/refactor
        // and clear/load replace objects outright). Any such entry left on
        // the queue makes the NEXT undo/redo dereference freed memory — the
        // recurring "unsupported undo command <garbage int>" segfault.
        // Flushing only on deletes was NOT enough: a create/edit/move after
        // a delete re-populates the queue with entries that a later
        // object-freeing op invalidates. The only provably-safe state for
        // the queue after ANY MCP mutation is EMPTY.
        bool undoReset = false;
        if (created > 0 || connected > 0 || edited > 0 || deleted > 0 || disconnected > 0 || layoutSanitizedMoved > 0 || occlusionsFixed > 0) {
            resetCanvasUndo(processor, canvasName);
            undoReset = true;
        }

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
            diagJson = computeDiagnoseFacts(processor, cnv, canvasName);
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

        sender.send(reply);
        // R3a — store reply in dedup cache (LRU + TTL, for retry idempotency).
        if (!processor->getMcp()->session.isExecutingMcpUndoRedo) {
            uint64_t nowMs = juce::Time::currentTimeMillis();
            std::string corrKey = correlationId.toStdString();
            s_batchDedupCache.insert_or_assign(corrKey, BatchDedupEntry{nowMs, reply});
            s_batchDedupOrder.push_back(corrKey);
            while ((int)s_batchDedupOrder.size() > BATCH_DEDUP_MAX) {
                std::string oldest = s_batchDedupOrder.front();
                s_batchDedupOrder.pop_front();
                s_batchDedupCache.erase(oldest);
            }
        }
    }
    return;
}
