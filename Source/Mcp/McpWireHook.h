/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// MCP-HOOK(wireActivity): the first line of pd::MessageDispatcher::enqueueMessage. Pd calls it for every message that
// crosses a connection (type 1, target = the t_outconnect) while plugdata's debugging/activity flag is on; this hands
// it to the /pd/wire_activity window (Source/Pd/MCPBridgeWireActivity.h). With no window armed it costs one relaxed
// atomic load; it never blocks, allocates or dereferences `target`.
#pragma once

#include <m_pd.h>

#include "Pd/MCPBridgeWireActivity.h"

namespace mcp {

inline void wireMessage(int type, void* target, t_symbol* selector, int argc, t_atom* argv) noexcept
{
    if (type != 1) return;
    auto& window = wire::table();
    if (!window.armed()) return;
    auto kind = wire::ValueKind::None;
    float f = 0.0f;
    void const* symbol = nullptr;
    if (argc > 0 && argv) {
        if (argv[0].a_type == A_FLOAT) {
            kind = wire::ValueKind::Float;
            f = argv[0].a_w.w_float;
        } else if (argv[0].a_type == A_SYMBOL) {
            kind = wire::ValueKind::Symbol;
            symbol = argv[0].a_w.w_symbol;
        } else {
            kind = wire::ValueKind::Other;
        }
    }
    window.record(target, selector, kind, f, symbol);
}

} // namespace mcp
