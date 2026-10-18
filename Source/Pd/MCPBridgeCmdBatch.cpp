/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge command: batch_atomic (Plan 3 P3.3): the handler moved unchanged out of MCPBridge::handlePdDomain.
// P3.4 S9: split into stages (MCPBridgeCmdBatch.h); this file runs them in order and owns the
// retry cache, the wait for the Pd context and the message-thread stages.

#include "MCPBridgeCmdBatch.h"

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

void mcp::BatchJob::runLayoutGuard(MCPBridge& bridge)
{
    // PRD_CONTEXT_LAYOUT_GUARD (P2/P3/P3.1): layout guard on the JUCE
    // message thread. Deoverlap + occlusion rounds (fast, Pd coords),
    // then a GUARANTEED fallback: if true-rect collisions remain,
    // compose the canvas by role (proven collision-free).
    if (cnv && autoLayout) {
        auto runLayout = [&]() {
            if (mcpCanvasGone(cnv)) return; // P3.4 S4: closed between the batch and the guard
            const int guardPad = layoutPad + 15;
            for (int round = 0; round < 8; round++) {
                sys_lock();
                int c = MCPBridge::sanitizeLayout(processor, cnv, guardPad, layoutSnap);
                int o = autoFixOcclusions ? MCPBridge::fixOcclusions(processor, cnv, guardPad, layoutSnap) : 0;
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
            bridge.postMessage([&runLayout, layoutHandoff]() {
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
            bridge.postMessage([p = processor] {
                if (p) p->getMcp()->synchroniseCanvases();
            });
        }
    }
}

void mcp::BatchJob::frameCreated(MCPBridge& bridge)
{
    // Auto-frame newly-created objects: if a CREATED object landed outside
    // the current viewport, PAN it into view — never change the zoom.
    // (The old zoomToFitAll() fit the whole canvas and yanked the artist's
    // zoom from 100% to 66% on every offscreen build.) Only counts objects
    // created in THIS batch, so panning around never triggers it; a
    // creation inside the view never moves the camera.
    if (cnv && created > 0 && !canvasNotFound) {
        auto frameIfCreateOffscreen = [p = processor, cnv = cnv, ptrs = createdPtrs]() {
            if (mcpCanvasGone(cnv)) return; // P3.4 S4 (frameCreatedObjects checks the objects: L18)
            p->getMcp()->gui().frameCreatedObjects(cnv, ptrs);
        };
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
            frameIfCreateOffscreen();
        } else {
            bridge.postMessage(frameIfCreateOffscreen);
        }
    }
}

void mcp::BatchJob::resetUndo()
{
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
    if (created > 0 || connected > 0 || edited > 0 || deleted > 0 || disconnected > 0 || layoutSanitizedMoved > 0 || occlusionsFixed > 0) {
        resetCanvasUndo(processor, canvasName);
        undoReset = true;
    }
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
    //        that must match it) is in mcp-server docs/protocol/v1-wire-schema.json; the reply writer is MCPBridgeCmdBatchReply.cpp.
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

        mcp::BatchJob job { processor, canvasName, correlationId };
        job.parse(msg);
        if (job.pdLockBusy) {
            juce::OSCMessage errReply { juce::OSCAddressPattern("/pd/batch_atomic/error/" + correlationId) };
            errReply.addArgument(juce::String("rejected: the Pd lock was held elsewhere for 2000ms; nothing was applied"));
            sender.send(errReply);
            return;
        }

        // =========================================================================
        // EXECUTE IN THE PD CONTEXT (message thread, sys_lock; P3.4 S3)
        // =========================================================================
        // Plan 3 P3.4 S3: the mutation runs in the Pd context = the message thread under
        // sys_lock, where plugdata's own GUI edits Pd (the old note here said GUI edits
        // went through the audio-thread queue; they do not). See McpAudio.cpp.

        // Report F3: the lambda below captures this frame by reference. The
        // handoff guarantees we never return while it can still touch it.
        auto handoff = std::make_shared<mcp::OneShotHandoff>();

        processor->getMcp()->runPd([&job, handoff]() {
            if (!handoff->begin())
                return; // the caller gave up: this frame may be gone, touch nothing
            mcp::OneShotHandoff::FinishGuard finishGuard(*handoff);
            job.runPdJob();
        }); // finishGuard signals the caller

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

        job.runLayoutGuard(*this);
        job.frameCreated(*this);
        job.resetUndo();
        job.recordUndo(msg);
        auto reply = job.buildReply();

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
