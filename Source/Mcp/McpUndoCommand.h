/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Undo log entries (Plan 4 P4.2 S6): a typed command, not a JUCE OSC message. The log holds an address and its
// typed values; the conversion to an OSC message happens once, at replay (McpHost::undoMcpTransaction and redo).
// Arguments are int32, float32 and string: the only kinds the bridge's own handlers put in a forward or inverse.
#pragma once

#include <juce_osc/juce_osc.h>
#include <utility>
#include <vector>

namespace mcp {

struct UndoArg {
    char type = 's'; // 'i' int32, 'f' float32, 's' string
    juce::int32 i = 0;
    float f = 0.0f;
    juce::String s;
};

struct UndoCommand {
    juce::String address;
    std::vector<UndoArg> args;
};

inline UndoCommand undoCommandFrom(const juce::OSCMessage& msg)
{
    UndoCommand cmd;
    cmd.address = msg.getAddressPattern().toString();
    for (int k = 0; k < msg.size(); ++k) {
        auto const& a = msg[k];
        UndoArg u;
        if (a.isInt32()) {
            u.type = 'i';
            u.i = a.getInt32();
        } else if (a.isFloat32()) {
            u.type = 'f';
            u.f = a.getFloat32();
        } else {
            u.type = 's';
            u.s = a.isString() ? a.getString() : juce::String();
        }
        cmd.args.push_back(std::move(u));
    }
    return cmd;
}

inline juce::OSCMessage oscMessageFrom(const UndoCommand& cmd)
{
    juce::OSCMessage msg { juce::OSCAddressPattern(cmd.address) };
    for (auto const& u : cmd.args) {
        if (u.type == 'i')
            msg.addInt32(u.i);
        else if (u.type == 'f')
            msg.addFloat32(u.f);
        else
            msg.addString(u.s);
    }
    return msg;
}

} // namespace mcp
