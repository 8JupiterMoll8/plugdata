/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: index-based create/connect/disconnect/delete/move, and move_batch_id (the undo/redo replay verb). Plan 3 P3.3: handlers moved unchanged out of MCPBridge::handlePdDomain; S9 removed the other id-addressed forms (batch_atomic covers them).

#include "MCPBridgeCmdCommon.h"

// Build the textual object representation PlugData's GUI expects, applying
// the same kind mapping the retired mcp_create_batch_id used.
static juce::String buildObjectText(const juce::String& kind, const juce::StringArray& tokens)
{
    juce::StringArray t = tokens;
    if (kind == "msg") {
        if (!t.isEmpty() && t[0] == "msg") t.remove(0);
        return "msg " + t.joinIntoString(" ");
    }
    if (kind == "text" || kind == "comment") {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        return "comment " + t.joinIntoString(" ");
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        // Match the batch_atomic substitution: emit [nbx]. "floatbox" here maps
        // back to the crashy floatatom via Patch::createObject (and "numbox" is
        // not a Pd class at all).
        return "nbx " + t.joinIntoString(" ");
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "symbolatom " + t.joinIntoString(" ");
    }
    // IEM GUI short-form guard (see McpIemArgs.h).
    t = mcp::expandIemGuiShortForm(t);
    return t.joinIntoString(" ");
}

void MCPBridge::pdCreate(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 5 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto kind = getArgString(msg[1]);
        float x = getArgFloat(msg[2]);
        float y = getArgFloat(msg[3]);

        juce::StringArray tokens;
        for (int i = 4; i < msg.size(); ++i) {
            tokens.add(getArgString(msg[i]));
        }

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
            patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens));
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });
    }
    return;
}

void MCPBridge::pdCreateBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        int count = static_cast<int>(getArgFloat(msg[2]));
        int cursor = 3;
        int created = 0;

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
            for (int o = 0; o < count && cursor < msg.size(); o++) {
                auto kind = getArgString(msg[cursor++]);
                float x = getArgFloat(msg[cursor++]);
                float y = getArgFloat(msg[cursor++]);
                int nargs = static_cast<int>(getArgFloat(msg[cursor++]));

                juce::StringArray tokens;
                for (int a = 0; a < nargs && cursor < msg.size(); a++) {
                    tokens.add(getArgString(msg[cursor++]));
                }

                if (patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens))) {
                    created++;
                }
            }
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });

        sendReply("/pd/create_batch/reply/" + correlationId, static_cast<float>(created));
    }
    return;
}

void MCPBridge::pdConnectBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = action == "connect_batch" ? getArgString(msg[1]) : "0";
        int startIdx = action == "connect_batch" ? 3 : 1;
        int count = action == "connect_batch" ? static_cast<int>(getArgFloat(msg[2])) : 1;
        int done = 0;

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            for (int c = 0; c < count && startIdx + 3 < msg.size(); c++) {
                int srcIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                int srcOut = static_cast<int>(getArgFloat(msg[startIdx++]));
                int destIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                int destIn = static_cast<int>(getArgFloat(msg[startIdx++]));

                t_atom cArgs[4];
                SETFLOAT(&cArgs[0], static_cast<float>(srcIdx));
                SETFLOAT(&cArgs[1], static_cast<float>(srcOut));
                SETFLOAT(&cArgs[2], static_cast<float>(destIdx));
                SETFLOAT(&cArgs[3], static_cast<float>(destIn));
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("connect"), 4, cArgs);
                done++;
            }
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });

        if (action == "connect_batch") {
            sendReply("/pd/connect_batch/reply/" + correlationId, static_cast<float>(done));
        }
    }
    return;
}

void MCPBridge::pdDisconnect(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 5 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int srcIdx = static_cast<int>(getArgFloat(msg[1]));
        int srcOut = static_cast<int>(getArgFloat(msg[2]));
        int destIdx = static_cast<int>(getArgFloat(msg[3]));
        int destIn = static_cast<int>(getArgFloat(msg[4]));

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
            t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
            if (src && dest) {
                pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                canvas_dirty(cnv, 1);
            }
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });
    }
    return;
}

void MCPBridge::pdDisconnectBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        int count = static_cast<int>(getArgFloat(msg[2]));
        int cursor = 3;
        int done = 0;

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            for (int c = 0; c < count && cursor + 3 < msg.size(); c++) {
                int srcIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                int srcOut = static_cast<int>(getArgFloat(msg[cursor++]));
                int destIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                int destIn = static_cast<int>(getArgFloat(msg[cursor++]));

                t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
                t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
                if (src && dest) {
                    pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                    done++;
                }
            }
            if (done > 0) canvas_dirty(cnv, 1);
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });

        sendReply("/pd/disconnect_batch/reply/" + correlationId, static_cast<float>(done));
    }
    return;
}

void MCPBridge::pdDelete(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int index = static_cast<int>(getArgFloat(msg[1]));

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            t_gobj* obj = glistObjectAt(cnv, index);
            if (obj) {
                SmallArray<t_gobj*> toDelete;
                toDelete.add(obj);
                mcp::PdOps::removeObjects(cnv, toDelete);
                canvas_dirty(cnv, 1);
            }
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });
    }
    return;
}

void MCPBridge::pdDeleteBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        std::vector<int> indices;
        for (int i = 2; i < msg.size(); ++i) {
            indices.push_back(static_cast<int>(getArgFloat(msg[i])));
        }
        std::sort(indices.rbegin(), indices.rend());

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (cnv) {
            SmallArray<t_gobj*> toDelete;
            for (int idx : indices) {
                t_gobj* obj = glistObjectAt(cnv, idx);
                if (obj) toDelete.add(obj);
            }
            if (toDelete.size() > 0) {
                mcp::PdOps::removeObjects(cnv, toDelete);
                canvas_dirty(cnv, 1);
            }
        }
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->getMcp()->synchroniseCanvases(); });

        sendReply("/pd/delete_batch/reply/" + correlationId, static_cast<float>(indices.size()));
    }
    return;
}

void MCPBridge::pdMoveBatchId(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 3 && processor) {
        SmallArray<pd::Atom> atoms;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
        atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

        for (int i = 2; i < msg.size(); ++i) {
            if (msg[i].isFloat32() || msg[i].isInt32()) {
                atoms.add(pd::Atom(getArgFloat(msg[i])));
            } else {
                atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
            }
        }

        if (action == "move_batch_id") {
            processor->receiveSysMessage("mcp_move_batch_id", atoms);
        } else {
            processor->receiveSysMessage("mcp_move_batch", atoms);
        }
    }
    return;
}
