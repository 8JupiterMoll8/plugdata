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
#include "Mcp/McpAudit.h"
#include "Pd/MCPBridgeHandoff.h"

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
    mcp::audit::markAudioThread(); // P3.4 S0: no-op unless PLUGDATA_MCP_AUDIT=1
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

// ── The Pd context (Plan 3 P3.4 S2/S3) ──────────────────────────────────────
// The bridge's Pd jobs run on the JUCE MESSAGE THREAD under sys_lock: that is where plugdata itself edits
// Pd (Canvas::removeSelection -> Patch::removeObjects; WeakReference::get() holds sys_lock), and the GUI's
// own reads assume nothing else changes the object list behind them (Canvas::performSynchronise checks
// isValid() unlocked, then builds the view object). S2 drained this queue on the audio thread; a batch
// delete there landed inside a performSynchronise and segfaulted in Object::setType (S3 gates). For audio
// it makes no difference: a job under the Pd lock stalls the audio callback for the same time either way.

void McpHost::enqueuePd(std::function<void()> job)
{
    session.mcpPdQueue.enqueue(std::move(job));
    // One posted drain at a time; a job queued after the drain has started schedules the next one.
    if (!session.mcpPdDrainPosted.exchange(true, std::memory_order_acq_rel)) {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) {
            mm->callAsync([this] {
                session.mcpPdDrainPosted.store(false, std::memory_order_release);
                drainPdQueue();
            });
        } else {
            session.mcpPdDrainPosted.store(false, std::memory_order_release);
        }
    }
}

void McpHost::runPd(std::function<void()> job)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm && mm->isThisTheMessageThread()) {
        processor.setThis();
        sys_lock();
        job();
        sys_unlock();
        return;
    }
    enqueuePd(std::move(job));
}

bool McpHost::pdWait(std::function<void()> const& job, int const timeoutMs)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm && mm->isThisTheMessageThread()) { // already the Pd context: run inline (waiting would deadlock)
        processor.setThis();
        sys_lock();
        job();
        sys_unlock();
        return true;
    }
    if (mcp::audit::holdsPdLock())
        mcp::audit::reportWaitUnderLock("pdWait");
    auto handoff = std::make_shared<mcp::OneShotHandoff>();
    enqueuePd([handoff, &job] {
        if (!handoff->begin())
            return; // the caller gave up: `job` (and its captures) may be gone
        mcp::OneShotHandoff::FinishGuard finishGuard(*handoff);
        job();
    });
    return handoff->waitOrAbandon(std::chrono::milliseconds(timeoutMs)) == mcp::OneShotHandoff::Result::Finished;
}

// Jobs run per drain at most; the rest go to the next posted drain, so a flood never starves the GUI.
static constexpr int kMaxPdJobsPerDrain = 256;

void McpHost::drainPdQueue()
{
    if (session.mcpPdQueue.size_approx() == 0)
        return;
    processor.setThis();
    sys_lock();
    std::function<void()> job;
    int n = 0;
    for (; n < kMaxPdJobsPerDrain && session.mcpPdQueue.try_dequeue(job); ++n) {
        job();
        job = nullptr; // release the captures under the lock, before the next job
    }
    sys_unlock();
    if (n == kMaxPdJobsPerDrain && session.mcpPdQueue.size_approx() > 0)
        enqueuePd([] {}); // posts the next drain
}
