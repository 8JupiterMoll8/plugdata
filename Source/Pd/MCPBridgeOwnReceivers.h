/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The bridge's own native receivers: CLASS_PD objects bound to a Pd name, on no canvas, that the bridge made for itself. They are
// bridge probes, not listeners of a patch: the armed control window's (`mcp_ctrl_receive`, MCPBridgeControlWindow.h), the `ai-ask`
// receiver (`mcp_ai_ask`, MCPBridgeWidgetAsk.h), the live widget history's state receivers (`mcp_ai_state`, MCPBridgeWidgetState.h), the
// `ai-event` receiver (`mcp_ai_event`, MCPBridgeWidgetEvent.h) and the collector of one /pd/widget_call (`mcp_widget_call`,
// MCPBridgeWidgetCall.h: bound only inside that call, under the Pd lock).
// The verbs that ask who listens on a name (/pd/send_checked, /pd/bound) leave them out: a send is not refused because of one ("a
// binder that is not a box"), and /pd/bound reports one as `bridge_probe` without a walk of the patches for it. They are told apart by
// their class pointer alone (`*binder`): nothing of the binder is read.
//
// Included by MCPBridgeCmdControl.cpp and MCPBridgeCmdBound.cpp.

#pragma once

#include "MCPBridgeControlWindow.h"
#include "MCPBridgeWidgetAsk.h"
#include "MCPBridgeWidgetCall.h"

inline bool mcpIsBridgeOwnReceiverClass(t_class const* cls)
{
    return cls
        && ((mcpCtrlReceiverClass && cls == mcpCtrlReceiverClass)
            || (mcpAskReceiverClass && cls == mcpAskReceiverClass)
            || (mcpStateReceiverClass && cls == mcpStateReceiverClass)
            || (mcpEventReceiverClass && cls == mcpEventReceiverClass)
            || (mcpCallCollectorClass && cls == mcpCallCollectorClass));
}
