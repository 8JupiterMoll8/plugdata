/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: parameter, trigger, telemetry and chat (Plan 3 P3.3): the domain functions moved unchanged out of MCPBridge.cpp.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::handleParamDomain(const juce::String& /*paramName*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /param <name> <value...> — the receiver name is the FIRST
    // argument, never the address path.
    auto paramName = getArgString(msg[0]);
    if (paramName.isEmpty() || paramName == "nil") return;

    std::vector<t_atom> shadow; // P4: record last value per bus for /pd/param_get
    if (msg.size() == 2) {
        auto const& v = msg[1];
        if (v.isFloat32() || v.isInt32()) {
            t_atom a; SETFLOAT(&a, getArgFloat(v)); shadow.push_back(a);
            processor->sendFloat(paramName.toRawUTF8(), getArgFloat(v));
        } else {
            auto s = getArgString(v);
            if (s.startsWith("s:")) s = s.substring(2);
            t_atom a; SETSYMBOL(&a, gensym(s.toRawUTF8())); shadow.push_back(a);
            processor->sendSymbol(paramName.toRawUTF8(), s.toRawUTF8());
        }
    } else if (msg.size() > 2) {
        SmallArray<pd::Atom> atoms;
        for (int i = 1; i < msg.size(); ++i) {
            auto const& arg = msg[i];
            if (arg.isFloat32() || arg.isInt32()) {
                t_atom a; SETFLOAT(&a, getArgFloat(arg)); shadow.push_back(a);
                atoms.add(pd::Atom(getArgFloat(arg)));
            } else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                t_atom a; SETSYMBOL(&a, gensym(s.toRawUTF8())); shadow.push_back(a);
                atoms.add(pd::Atom(processor->generateSymbol(s)));
            }
        }
        processor->sendList(paramName.toRawUTF8(), atoms);
    }

    {
        std::lock_guard<std::mutex> lk(s_paramShadowMutex);
        s_paramShadow[paramName.toStdString()] = shadow;
    }
}

void MCPBridge::handleTriggerDomain(const juce::String& triggerAction, const juce::OSCMessage& msg)
{
    if (!processor) return;

    if (triggerAction == "note" && msg.size() >= 3) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int pitch = static_cast<int>(getArgFloat(msg[1]));
        int vel = static_cast<int>(getArgFloat(msg[2]));
        processor->sendNoteOn(ch, pitch, vel);
        return;
    }

    if ((triggerAction == "cc" || triggerAction == "controlchange") && msg.size() >= 3) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int cc = static_cast<int>(getArgFloat(msg[1]));
        int val = static_cast<int>(getArgFloat(msg[2]));
        processor->sendControlChange(ch, cc, val);
        return;
    }

    if ((triggerAction == "bend" || triggerAction == "pitchbend") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendPitchBend(ch, val);
        return;
    }

    if ((triggerAction == "aftertouch" || triggerAction == "touch") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendAfterTouch(ch, val);
        return;
    }

    if ((triggerAction == "program" || triggerAction == "programchange") && msg.size() >= 2) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int val = static_cast<int>(getArgFloat(msg[1]));
        processor->sendProgramChange(ch, val);
        return;
    }

    // Lua-parity: /trigger <name> — the receiver name is the FIRST argument.
    auto bangName = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    if (bangName.isEmpty() || bangName == "nil") return;
    processor->sendBang(bangName.toRawUTF8());
}

void MCPBridge::handleTelemetryDomain(const juce::String& /*telAction*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /telemetry <action> [args...] — the action is the FIRST argument.
    auto action = getArgString(msg[0]);

    if (action == "dsp" && msg.size() >= 2) {
        float val = getArgFloat(msg[1]);
        if (val > 0.5f) processor->startDSP();
        else processor->releaseDSP();
    } else if (action == "delete" || action == "delete_on") {
        // [telemetry, delete, index]           → canvas defaults to main
        // [telemetry, delete, canvas, index]   → explicit canvas
        int idx = -1;
        juce::String canvasName = "pd-main";
        if (msg.size() == 2) {
            idx = static_cast<int>(getArgFloat(msg[1]));
        } else if (msg.size() >= 3) {
            canvasName = normalizeCanvas(getArgString(msg[1]));
            idx = static_cast<int>(getArgFloat(msg[2]));
        }
        if (idx >= 0) {
            sys_lock();
            t_canvas* canvas = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (canvas) {
                t_gobj* obj = glistObjectAt(canvas, idx);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    mcp::PdOps::removeObjects(canvas, toDelete);
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();
        }
    }
}

void MCPBridge::handleChatDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // /chat/msg <role> <text>
    if (msg.size() >= 2) {
        auto role = getArgString(msg[0]);
        auto text = getArgString(msg[1]);
        postChatMessage(role, text);
    }
}
