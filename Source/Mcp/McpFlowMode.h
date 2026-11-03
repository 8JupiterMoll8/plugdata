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
// - ON only by the artist's own click: the ✓ of the flow chip (the P12c chip, its 400 ms on-screen rule), which the
//   chip handler (McpHost::answerApprovalChip) applies directly; no answer travels to a server first. No verb, HTTP
//   field, HUD text, note or patch text turns it on. scripts/test-bridge-safety.sh fails when artistTurnOn is called
//   from anywhere else.
// - OFF from anywhere (/pd/flow_mode_off, the FLOW badge click, Esc on the chip leaves it off): the safe direction.
// - The flow chip's nonce starts with "flow:", made here; /pd/approval_show refuses that prefix, so a server cannot
//   show a chip that would count as the flow chip.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mcp {

class FlowMode {
public:
    bool isOn() const { return on.load(std::memory_order_acquire); }
    // Wall-clock ms when it went on (0 = off).
    std::int64_t sinceMs() const { return isOn() ? since.load(std::memory_order_acquire) : 0; }

    // The artist clicked ✓ on the flow chip. Only caller: McpHost::answerApprovalChip (checked by a grep gate).
    void artistTurnOn(std::int64_t nowMs)
    {
        if (isOn())
            return; // already on: the first start time stays
        since.store(nowMs, std::memory_order_release);
        on.store(true, std::memory_order_release);
    }

    // Any door may switch it off. True when it was on.
    bool off()
    {
        bool const was = on.exchange(false, std::memory_order_acq_rel);
        since.store(0, std::memory_order_release);
        return was;
    }

private:
    std::atomic<bool> on { false };
    std::atomic<std::int64_t> since { 0 };
};

inline constexpr char kFlowNoncePrefix[] = "flow:";

inline bool isFlowNonce(std::string const& nonce)
{
    return nonce.rfind(kFlowNoncePrefix, 0) == 0;
}

// A fresh flow-chip nonce from two random words (the caller's RNG).
inline std::string makeFlowNonce(std::uint64_t a, std::uint64_t b)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%016llx%016llx", kFlowNoncePrefix, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

} // namespace mcp
