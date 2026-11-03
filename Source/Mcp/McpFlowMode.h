/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// P12a A3: the flow-mode switch ("act first, undo after" for the AI's own objects). The state lives here, in C++,
// for the whole PlugData run, shared by every MCP server talking to it. JUCE-free (Tests/mcp/flow_provenance_test.cpp).
//
// - Off at every start: in memory only, never saved (a restart, or a new bridge, builds it off).
// - ON only by the artist's own click on Alphi, the robot face top left of the canvas (P12a.1, owner decision
//   2026-11-03; McpAlphi.h): McpHost::alphiClicked applies it directly, called only by the canvas click
//   (CanvasMcpLayer::handleAlphiClick). No verb, HTTP field, HUD text, note or patch text turns it on.
//   scripts/test-bridge-safety.sh fails when artistTurnOn is called from anywhere else.
// - OFF from anywhere (/pd/flow_mode_off, the pill words "flow mode off", the next click on Alphi): the safe direction.
// - The "flow:" nonce prefix of the old flow chip (P12a A3, removed in P12a.1) stays reserved: /pd/approval_show
//   refuses it, so no server chip can ever pass for a flow switch.

#include <atomic>
#include <cstdint>
#include <string>

namespace mcp {

class FlowMode {
public:
    bool isOn() const { return state.load(std::memory_order_acquire) != 0; }
    // Wall-clock ms when it went on (0 = off).
    std::int64_t sinceMs() const { return state.load(std::memory_order_acquire); }

    // The artist clicked Alphi while flow was off. Only caller: McpHost::alphiClicked (checked by a grep gate).
    // Review L2: on/off and the start time are ONE atomic word (0 = off, else the start), so this compare-and-swap
    // cannot interleave with off(): already on keeps its first start.
    void artistTurnOn(std::int64_t nowMs)
    {
        std::int64_t expected = 0;
        state.compare_exchange_strong(expected, nowMs > 0 ? nowMs : 1, std::memory_order_acq_rel);
    }

    // Any door may switch it off. True when it was on.
    bool off() { return state.exchange(0, std::memory_order_acq_rel) != 0; }

private:
    std::atomic<std::int64_t> state { 0 };
};

inline constexpr char kFlowNoncePrefix[] = "flow:";

inline bool isFlowNonce(std::string const& nonce)
{
    return nonce.rfind(kFlowNoncePrefix, 0) == 0;
}

} // namespace mcp
