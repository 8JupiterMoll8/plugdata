/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: system (ping, DSP, clear, undo/redo) (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::pdPing(const juce::String& action, const juce::OSCMessage& msg)
{
    sendRawReply("/pd/pong");
    return;
}

void MCPBridge::pdLimiter(const juce::String& action, const juce::OSCMessage& msg)
{
    // P5.1 sink floor S1: /pd/limiter <corrId> -> /pd/limiter/reply/<corrId> {"enabled":bool}. Read-only: the
    // AI never switches the limiter. The LIVE state (an AtomicValue, safe off the message thread), not
    // settings.json: `limit` from a patch does not persist.
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!processor) {
        sendReply("/pd/limiter/error/" + correlationId, juce::String("no processor"));
        return;
    }
    bool const enabled = processor->getEnableLimiter();
    sendReply("/pd/limiter/reply/" + correlationId, juce::String(enabled ? "{\"enabled\":true}" : "{\"enabled\":false}"));
}

void MCPBridge::pdClearConsole(const juce::String& action, const juce::OSCMessage& msg)
{
    if (processor) {
        postMessage([proc = processor] {
            proc->getConsoleMessages().clear();
            proc->getMcp()->gui().clearConsole();
        });
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        sendReply("/pd/clear_console/reply/" + correlationId, 1.0f);
    }
    return;
}

void MCPBridge::pdMcpReloadLua(const juce::String& action, const juce::OSCMessage& msg)
{
    auto correlationId = msg.size() >= 2 ? getArgString(msg[1]) : (msg.size() > 0 ? getArgString(msg[0]) : "0");
    sendReply("/pd/mcp_reload_lua/reply/" + correlationId, 1.0f);
    return;
}

void MCPBridge::pdDsp(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() > 0) {
        float val = getArgFloat(msg[0]);
        if (processor) {
            if (val > 0.5f) processor->startDSP();
            else processor->releaseDSP();
        }
    }
    return;
}

void MCPBridge::pdUpdateDsp(const juce::String& action, const juce::OSCMessage& msg)
{
    // Full DSP graph recompile (stop+start). Needed after bulk
    // reconstruct operations (load/undo/redo) which create/connect
    // objects while the graph was compiled against an empty canvas.
    sys_lock();
    canvas_update_dsp();
    sys_unlock();
    return;
}

void MCPBridge::pdVis(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        float vis = getArgFloat(msg[1]);
        sys_lock();
        t_canvas* cnv = reinterpret_cast<t_canvas*>(pd_findbyclass(gensym(canvasName.toRawUTF8()), canvas_class));
        if (cnv) {
            t_atom a;
            SETFLOAT(&a, vis);
            pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("vis"), 1, &a);
        }
        sys_unlock();
    }
    return;
}

void MCPBridge::pdClear(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

        // Clear on the JUCE message thread. Deleting GUI/abstraction/GOP
        // objects off the message thread crashes (same class as the load
        // bug). clearCanvas() deselects the editor + glist_delete.
        postMessage(
            [proc = processor, canvasName, correlationId, bridge = this]() {
                sys_lock();
                t_canvas* cnv = proc->getMcp()->getCanvasBySymbol(canvasName);
                if (cnv) {
                    mcp::PdOps::clearCanvas(cnv);
                }
                sys_unlock();

                // Clearing freed every object while Pd's index-based undo
                // entries still point at them — drop the stale queue.
                resetCanvasUndo(proc, canvasName);

                mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
                atoms.add(juce::String(canvasName));
                atoms.add(juce::String("0"));
                proc->getMcp()->callSys("mcp_clear_ids", atoms);

                proc->getMcp()->synchroniseCanvases();

                bridge->sendRawReply("/pd/cleared");
                if (correlationId != "0") {
                    juce::Array<juce::var> clearReplyArgs;
                    clearReplyArgs.add(1.0);
                    clearReplyArgs.add(1.0);
                    bridge->sendReply("/pd/clear/reply/" + correlationId, clearReplyArgs);
                }
            });
    }
    return;
}

void MCPBridge::pdClearUndo(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/clear_undo <canvasName>
    // Routes through receiveSysMessage -> McpHost::sysClearUndo, which runs
    // here (command worker) and takes the Pd lock canvas_undo_free needs.
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(juce::String(canvasName));
        processor->getMcp()->callSys("mcp_clear_undo", atoms);
        sendRawReply("/pd/clear_undo/reply");
    }
    return;
}

void MCPBridge::pdClearSelection(const juce::String& action, const juce::OSCMessage& msg)
{
    if (processor) {
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        processor->getMcp()->callSys("mcp_clear_selection", atoms);
    }
    return;
}

// /pd/undo and /pd/redo (Plan 4 P4.4): one step of the canvas's own undo queue, the same one Ctrl+Z walks (AI and
// manual edits in time order). Reply 1 = a step was taken, 0 = nothing to take (or the canvas is gone): never a
// claimed success on a no-op.
static void pdUndoStep(MCPBridge& bridge, PluginProcessor* processor, juce::OSCMessage const& msg, bool redo)
{
    auto const canvasName = MCPBridge::normalizeCanvas(getArgString(msg[0]));
    auto const correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
    auto const replyAddr = juce::String(redo ? "/pd/redo/reply/" : "/pd/undo/reply/") + correlationId;
    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) {
        bridge.sendReply(replyAddr, 0.0f);
        return;
    }
    bridge.postMessage([proc = processor, cnv, replyAddr, redo, b = &bridge]() {
        if (mcpCanvasGone(cnv)) { b->sendReply(replyAddr, 0.0f); return; } // P3.4 S4: closed since the handler ran
        // An open canvas view runs plugdata's own undo (its tail: sync, selection, sidebar), which steps through
        // the same McpHost::undoStep; without a view the step runs directly.
        auto& gui = proc->getMcp()->gui();
        auto const viaView = redo ? gui.redo(cnv) : gui.undo(cnv);
        bool stepped = viaView == McpGuiHost::UndoResult::Done;
        if (viaView == McpGuiHost::UndoResult::NoView) {
            stepped = proc->getMcp()->undoStep(cnv, redo);
            if (stepped) proc->getMcp()->synchroniseCanvases();
        }
        b->sendReply(replyAddr, stepped ? 1.0f : 0.0f);
    });
}

void MCPBridge::pdUndo(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) pdUndoStep(*this, processor, msg, false);
}

void MCPBridge::pdRedo(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) pdUndoStep(*this, processor, msg, true);
}

// P3.4 S11 (test-only, registered only with PLUGDATA_MCP_AUDIT=1): the next audio block holds the Pd lock and
// sleeps <ms> (clamped to 10 s), so the stall tests can check that waits time out cleanly.
void MCPBridge::pdDebugStallAudio(const juce::String&, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;
    int const ms = juce::jlimit(1, 10000, static_cast<int>(getArgFloat(msg[0])));
    auto const correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
    auto& stall = processor->getMcp()->session.mcpDebugStallMs;
    stall.store(ms, std::memory_order_relaxed);
    // Answer once the audio thread holds the Pd lock (-1), so a request sent after the reply meets the stall;
    // 0 means it never started within 2 s (audio not running).
    bool started = false;
    for (int i = 0; i < 2000 && !(started = stall.load(std::memory_order_relaxed) < 0); ++i)
        juce::Thread::sleep(1);
    if (!started) stall.store(0, std::memory_order_relaxed);
    sendReply("/pd/debug_stall_audio/reply/" + correlationId, started ? static_cast<float>(ms) : 0.0f);
}
