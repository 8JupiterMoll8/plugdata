/*
 // The MCP audio hooks (Plan 3 P3.2 S5). PluginProcessor calls each one at the exact place
 // where its block used to be (before/after performDSP, the output copy, gain/limiter);
 // the bodies moved verbatim. Realtime behaviour is unchanged: P3.4 reworks it.
 */
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"

// prepareToPlay, after the Pd/JUCE setup.
void McpHost::prepare(double const sampleRate, int const samplesPerBlock)
{
    ignoreUnused(samplesPerBlock);
    // Armed spectral capture buffer (Phase 5): up to 2s of mono final output.
    {
        int const cap = static_cast<int>(std::ceil(sampleRate * 2.0));
        if (cap > 0) {
            session.mcpSpecBuffer.assign(static_cast<size_t>(cap), 0.0f);
            session.mcpSpecCapacity.store(cap, std::memory_order_relaxed);
            session.mcpSpecWritePos.store(0, std::memory_order_relaxed);
            session.mcpSpecLimit.store(0, std::memory_order_relaxed);
            session.mcpSpecArmed.store(false, std::memory_order_relaxed);
        }
    }
}

// First line of processBlock: counts blocks, the proof that the audio thread runs (#83).
void McpHost::audioBlockStarted()
{
    session.mcpAudioBlocks.fetch_add(1, std::memory_order_relaxed); // #83 audio-thread liveness
}

// processBlock, after the MIDI input history is filled and before the toolbar sees it.
void McpHost::midiTick(MidiBuffer& midiInputHistory, AudioBuffer<float>& buffer, double const sampleRate)
{
    // MIDI melodic-intent capture & playback tap
    if (mcpBridge && (mcpBridge->isMidiCapturing() || mcpBridge->isMidiPlaying()))
        mcpBridge->midiInputTick(midiInputHistory, buffer.getNumSamples(), &buffer, sampleRate);
}

// End of processBlock, on the FINAL output (after gain and limiter).
void McpHost::audioBlockFinished(AudioBuffer<float>& buffer)
{
    // Armed peak-hold (trigger verification): track the loudest sample of the FINAL
    // output (post-gain/limiter) while armed. Placed at the common end of
    // processBlock so it runs for BOTH constant and variable block sizes. The window
    // opens BEFORE the trigger (arm) and is read AFTER it, so a one-shot can never
    // fall outside it. No-op when not armed.
    if (session.mcpArmActive.load(std::memory_order_relaxed)) {
        session.mcpArmedTailRuns.fetch_add(1, std::memory_order_relaxed); // #83 diagnostics
        float p = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            auto* d = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                float const a = std::abs(d[i]);
                if (a > p) p = a;
            }
        }
        float prevTail = session.mcpArmedTailPeak.load(std::memory_order_relaxed);
        while (p > prevTail && !session.mcpArmedTailPeak.compare_exchange_weak(prevTail, p, std::memory_order_relaxed)) {}
        float prev = session.mcpArmPeak.load(std::memory_order_relaxed);
        while (p > prev && !session.mcpArmPeak.compare_exchange_weak(prev, p, std::memory_order_relaxed)) {}
    }

    // Armed spectral capture (Phase 5): append the FINAL output into the mono
    // buffer while armed, so a one-shot fired between arm and read lands inside
    // the analysis window. Same common end-of-processBlock placement as the
    // peak-hold (runs for constant AND variable block sizes). `mcpSpecBusy`
    // brackets the write so the reader never copies a half-written tail.
    if (session.mcpSpecArmed.load(std::memory_order_relaxed)) {
        session.mcpSpecBusy.store(true, std::memory_order_release);
        int pos = session.mcpSpecWritePos.load(std::memory_order_relaxed);
        int const stop = std::min(session.mcpSpecLimit.load(std::memory_order_relaxed),
                                  session.mcpSpecCapacity.load(std::memory_order_relaxed));
        if (!session.mcpSpecBuffer.empty() && pos < stop) {
            int const numSamples = buffer.getNumSamples();
            int const numCh = buffer.getNumChannels();
            auto* dst = session.mcpSpecBuffer.data();
            if (numCh > 0) {
                for (int i = 0; i < numSamples && pos < stop; ++i) {
                    float s = 0.0f;
                    for (int ch = 0; ch < numCh; ++ch) s += buffer.getReadPointer(ch)[i];
                    dst[pos++] = s / static_cast<float>(numCh);
                }
            }
            session.mcpSpecWritePos.store(pos, std::memory_order_release);
            if (pos >= stop) session.mcpSpecArmed.store(false, std::memory_order_relaxed);
        }
        session.mcpSpecBusy.store(false, std::memory_order_release);
    }
}

// Every Pd sub-block, right after performDSP and before the output copy.
void McpHost::pdBlock(float* audioVectorIn, float* audioVectorOut, int const pdBlockSize, int const numChannels)
{
    if (mcpBridge) {
        mcpBridge->audioTick();
        mcpBridge->voiceInputTick(audioVectorIn, pdBlockSize);
    }

    // Monitor tap (solo/mute) — per Pd sub-block, before it lands in the
    // JUCE buffer, so it also goes through the master gain/limiter.
    applyMonitorTap(audioVectorOut, pdBlockSize, numChannels);
}
