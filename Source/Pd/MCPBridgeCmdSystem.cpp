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
                // Drop MCP transactions too — their inverses reference the
                // objects we just freed.
                if (cnv) proc->getMcp()->clearMcpTransactions(cnv);

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

void MCPBridge::pdUndo(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty())) cnv = pd_this->pd_canvaslist;

        if (cnv) {
            postMessage([proc = processor, cnv, correlationId, bridge = this]() {
                if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/undo/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
                if (proc->getMcp()->hasMcpTransaction(cnv)) {
                    proc->getMcp()->undoMcpTransaction(cnv);
                    proc->getMcp()->synchroniseCanvases();
                    bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    return;
                }
                auto const viewUndo = proc->getMcp()->gui().undo(cnv);
                if (viewUndo == McpGuiHost::UndoResult::Done) {
                    bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                } else if (viewUndo == McpGuiHost::UndoResult::NoView) {
                    pd::Interface::undo(cnv);
                    proc->getMcp()->synchroniseCanvases();
                    bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                } else {
                    // Canvas present but the undo stack is EMPTY (e.g. after a
                    // GOP/refactor/encapsulate flush) — report the truth so the
                    // server stops claiming "undo succeeded" on a no-op.
                    bridge->sendReply("/pd/undo/reply/" + correlationId, 0.0f);
                }
            });
        } else {
            sendReply("/pd/undo/reply/" + correlationId, 0.0f);
        }
    }
    return;
}

void MCPBridge::pdRedo(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty())) cnv = pd_this->pd_canvaslist;

        if (cnv) {
            postMessage([proc = processor, cnv, correlationId, bridge = this]() {
                if (mcpCanvasGone(cnv)) { bridge->sendReply("/pd/redo/reply/" + correlationId, 0.0f); return; } // P3.4 S4: closed since the handler ran
                if (proc->getMcp()->hasMcpRedoTransaction(cnv)) {
                    proc->getMcp()->redoMcpTransaction(cnv);
                    proc->getMcp()->synchroniseCanvases();
                    bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                } else if (proc->getMcp()->gui().redo(cnv)) {
                    bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                } else {
                    pd::Interface::redo(cnv);
                    proc->getMcp()->synchroniseCanvases();
                    bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                }
            });
        } else {
            sendReply("/pd/redo/reply/" + correlationId, 0.0f);
        }
    }
    return;
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
