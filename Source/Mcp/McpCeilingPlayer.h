/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "Mcp/McpOutputCeiling.h"

namespace mcp {

// P9 round 1: an AudioSourcePlayer whose device callback ends with the output ceiling's clamp (no breaker), while
// the ceiling is active. MCPBridge::playVoiceTake plays the artist's takes through it on its own device callback,
// outside processBlock. Residue: the device sums this with processBlock's output, and that sum can still exceed
// +-1.0 (each part is clamped, the sum is not).
class CeilingSourcePlayer final : public juce::AudioSourcePlayer {
public:
    explicit CeilingSourcePlayer(OutputCeiling const& c)
        : ceiling(c)
    {
    }

    void audioDeviceIOCallbackWithContext(float const* const* inputChannelData, int totalNumInputChannels,
        float* const* outputChannelData, int totalNumOutputChannels, int numSamples,
        juce::AudioIODeviceCallbackContext const& context) override
    {
        juce::AudioSourcePlayer::audioDeviceIOCallbackWithContext(inputChannelData, totalNumInputChannels,
            outputChannelData, totalNumOutputChannels, numSamples, context);
        if (ceiling.isActive())
            OutputCeiling::clampBuffer(outputChannelData, totalNumOutputChannels, numSamples);
    }

private:
    OutputCeiling const& ceiling;
};

} // namespace mcp
