/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// MCP-HOOK(wireActivity): the first line of pd::MessageDispatcher::enqueueMessage. Pd calls it for every message that
// crosses a connection (type 1, target = the t_outconnect) while forwarding is on (plugdata's debugging/activity flag,
// or the bridge's own wire flag while a window is open); this hands it to the /pd/wire_activity window of THAT
// instance (`instance` = the pd::Instance, the key of its McpSession table, MCPBridgeWireActivity.h). With no window
// open anywhere it costs one relaxed atomic load; it never blocks, allocates or dereferences `target`.
#pragma once

#include <m_pd.h>

#include "Pd/MCPBridgeWireActivity.h"

namespace mcp {

inline void wireMessage(void* instance, int type, void* target, t_symbol* selector, int argc, t_atom* argv) noexcept
{
    if (type != 1 || wire::windows().open.load(std::memory_order_relaxed) == 0) return;
    auto* table = wire::tableFor(instance);
    if (!table || !table->armed()) return;
    auto& window = *table;
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
