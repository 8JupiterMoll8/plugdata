/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The env-gated frame log (PLUGDATA_NVG_FRAME_LOG=1): one line per second on stderr, so a laggy session can say whether
// it records MORE frames or MORE EXPENSIVE ones. JUCE-free (Tests/mcp/frame_pacer_test.cpp).
//
//   [nvg-frame] inv=<invalidations> rec=<frames recorded> skipped=<ticks that found the last frame pending>
//               damage=<share of the window the recorded frames redrew, %> recMs=<mean recordFrame ms>
//               renderFrames=<frames the render thread replayed> swaps=<presents / swapBuffers the render thread did>
//               inRender=<invalidations asked from inside recordFrame>
//   (swaps above renderFrames are empty presents: the render thread swapped with nothing new to show; inRender above 0
//   means something still asks for a repaint while the frame is being painted, and recordFrame drops that damage)
//
// The counters come in as running totals (the pacer's stats, the render thread's atomic); the window keeps what it
// saw at the last line and prints the difference. Message thread only, except that renderFrames is read from the
// atomic the render thread bumps.

#include "McpFramePacer.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace mcp {

class FrameLogWindow {
public:
    // An invalidation that arrived while recordFrame was painting (the damage is cleared when the frame is recorded).
    void addInvalidateDuringRecord() { ++duringRecord; }

    // One recorded frame: the pixels it redrew (the damage), the pixels of the window, and what recordFrame cost.
    void addRecord(double damagePixels, double boundsPixels, double recordMs)
    {
        damageSum += damagePixels;
        boundsSum += boundsPixels;
        recordMsSum += recordMs;
        ++recordsSeen;
    }

    // The line for the window that ends now, then the next window starts from these totals.
    std::string takeLine(FramePacer::Stats const& totals, std::uint64_t renderFramesTotal, std::uint64_t swapsTotal)
    {
        auto const inv = totals.invalidates - last.invalidates;
        auto const rec = totals.records - last.records;
        auto const skipped = totals.skippedPending - last.skippedPending;
        auto const frames = renderFramesTotal - lastRenderFrames;
        auto const swaps = swapsTotal - lastSwaps;
        double const share = boundsSum > 0.0 ? 100.0 * damageSum / boundsSum : 0.0;
        double const meanMs = recordsSeen > 0 ? recordMsSum / static_cast<double>(recordsSeen) : 0.0;

        char buffer[256];
        std::snprintf(buffer, sizeof buffer,
            "[nvg-frame] inv=%llu rec=%llu skipped=%llu damage=%.0f%% recMs=%.2f renderFrames=%llu swaps=%llu inRender=%llu",
            static_cast<unsigned long long>(inv), static_cast<unsigned long long>(rec),
            static_cast<unsigned long long>(skipped), share, meanMs, static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(swaps), static_cast<unsigned long long>(duringRecord));

        last = totals;
        lastRenderFrames = renderFramesTotal;
        lastSwaps = swapsTotal;
        damageSum = boundsSum = recordMsSum = 0.0;
        recordsSeen = 0;
        duringRecord = 0;
        return buffer;
    }

private:
    FramePacer::Stats last;
    std::uint64_t lastRenderFrames = 0;
    std::uint64_t lastSwaps = 0;
    double damageSum = 0.0;
    double boundsSum = 0.0;
    double recordMsSum = 0.0;
    std::uint64_t recordsSeen = 0;
    std::uint64_t duringRecord = 0;
};

} // namespace mcp
