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
#include <mutex>
#include <string>

namespace mcp {

class FlowMode {
public:
    bool isOn() const { return state.load(std::memory_order_acquire) != 0; }
    // Wall-clock ms when it went on (0 = off).
    std::int64_t sinceMs() const { return state.load(std::memory_order_acquire); }

    // The artist clicked ✓ on the flow chip. Only caller: McpHost::answerApprovalChip (checked by a grep gate).
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

// Review M2: who may show the flow offer how often. The verb is reachable by any client, so an offer is refused within
// kMinIntervalMs of the last one and for kCooldownAfterDeclineMs after the artist said no (Esc / ✗). The host also
// refuses it while any other chip is on screen.
class FlowOfferGate {
public:
    static constexpr std::int64_t kMinIntervalMs = 10000;
    static constexpr std::int64_t kCooldownAfterDeclineMs = 30000;
    enum class Refusal { None, TooSoon, Cooldown };
    static char const* name(Refusal r)
    {
        switch (r) {
        case Refusal::None: return "none";
        case Refusal::TooSoon: return "too_soon";
        case Refusal::Cooldown: return "cooldown";
        }
        return "none";
    }
    Refusal check(std::int64_t nowMs) const
    {
        std::lock_guard<std::mutex> const l(lock);
        if (declinedAt != 0 && nowMs < declinedAt + kCooldownAfterDeclineMs) return Refusal::Cooldown;
        if (offeredAt != 0 && nowMs < offeredAt + kMinIntervalMs) return Refusal::TooSoon;
        return Refusal::None;
    }
    void offered(std::int64_t nowMs) { std::lock_guard<std::mutex> const l(lock); offeredAt = nowMs > 0 ? nowMs : 1; }
    void declined(std::int64_t nowMs) { std::lock_guard<std::mutex> const l(lock); declinedAt = nowMs > 0 ? nowMs : 1; }

private:
    mutable std::mutex lock;
    std::int64_t offeredAt = 0;
    std::int64_t declinedAt = 0;
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
