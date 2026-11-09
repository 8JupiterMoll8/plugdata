/*
 // The MCP audio hooks (Plan 3 P3.2 S5). PluginProcessor calls each one at the exact place
 // where its block used to be (before/after performDSP, the output copy, gain/limiter);
 // the bodies moved verbatim. Realtime behaviour is unchanged: P3.4 reworks it.
 //
 // CONTRACT (P9 Part B): the `audio` hook audioBlockFinished WRITES the final output. It runs after the master
 // gain and PlugData's limiter, and its first step is the output ceiling + breaker (Mcp/McpOutputCeiling.h): in
 // the standalone app, while the owner setting `mcp_bridge_ai_ceiling` is on, every output sample is clamped to
 // +-1.0 (NaN/inf -> 0) and sustained clipped or square-like full-scale sound is cut 20 dB. Everything after it (the
 // armed meters) reads what leaves
 // the app. What runs BEFORE it and so sees the unclamped signal: the recorder (PluginProcessor, "Record before
 // limiter"), the toolbar meter and writeRecorderTap. MCPBridge::playVoiceTake's own device callback (outside
 // processBlock) gets the clamp alone (Mcp/McpCeilingPlayer.h).
 */
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Mcp/McpAudit.h"
#include "Mcp/McpRtGuard.h"
#include "Pd/MCPBridgeHandoff.h"

// prepareToPlay, after the Pd/JUCE setup.
void McpHost::prepare(double const sampleRate, int const samplesPerBlock)
{
    ignoreUnused(samplesPerBlock);
    // P9 round 3: NOT a reset of the output ceiling's loudness here: a patch can repeat prepareToPlay
    // (`; pd oversample 1/0`) and would keep the breaker from tripping or release it under a full-scale square. The
    // ceiling clears its loudness on an output channel-count change and recomputes its coefficient on a new rate.
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
    mcp::rt::Scope const rtScope; // P3.4 S6: realtime guard (counts only in a -DMCP_RT_GUARD=ON build)
    session.mcpAudioBlocks.fetch_add(1, std::memory_order_relaxed); // #83 audio-thread liveness
    session.mcpLastBlockMs.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    // Review L3: a new arm epoch: this thread empties the window's accumulators before it adds anything to them.
    if (uint32_t const epoch = session.mcpArmEpoch.load(std::memory_order_acquire); epoch != session.mcpArmEpochSeen) {
        session.mcpArmEpochSeen = epoch;
        session.mcpArmPeak.store(0.0f, std::memory_order_relaxed);
        session.mcpArmPreCeilingPeak.store(0.0f, std::memory_order_relaxed);
        session.mcpArmedTailPeak.store(0.0f, std::memory_order_relaxed);
        session.mcpArmPreLimiterPeak.store(0.0f, std::memory_order_relaxed);
        session.mcpArmPreLimiterSumSq.store(0.0, std::memory_order_relaxed);
        session.mcpArmPreLimiterCount.store(0, std::memory_order_relaxed);
        session.mcpArmFinalSumSq.store(0.0, std::memory_order_relaxed);
        session.mcpArmFinalCount.store(0, std::memory_order_relaxed);
        session.mcpArmLimiterReductionDbMax.store(0.0f, std::memory_order_relaxed);
        session.mcpArmPdOutPeak.store(0.0f, std::memory_order_relaxed);
        session.resetArmAc(); // V1 AC taps
    }
    // P3.4 S11 test-only: a forced stall, as a very heavy DSP block would hold the Pd lock (never set otherwise).
    // The value turns -1 once the lock is held (the verb answers then) and -2 when the stall ends.
    if (int const stallMs = session.mcpDebugStallMs.load(std::memory_order_relaxed); stallMs > 0) {
        mcp::rt::Pause const pause;
        processor.audioLock.enter();
        session.mcpDebugStallMs.store(-1, std::memory_order_relaxed);
        juce::Thread::sleep(stallMs);
        processor.audioLock.exit();
        session.mcpDebugStallMs.store(-2, std::memory_order_relaxed);
    }
    mcp::audit::markAudioThread(); // P3.4 S0: no-op unless PLUGDATA_MCP_AUDIT=1
}

// processBlock, after the MIDI input history is filled and before the toolbar sees it.
void McpHost::midiTick(MidiBuffer& midiInputHistory, AudioBuffer<float>& buffer, double const sampleRate)
{
    mcp::rt::Scope const rtScope; // P3.4 S6: realtime guard (counts only in a -DMCP_RT_GUARD=ON build)
    // P11 V2: the PRE-LIMITER tap of an armed window. This hook runs after the master gain and before PlugData's
    // limiter, on the output channels (as the other armed taps).
    session.mcpBlockPreLimiterPeak = -1.0f;
    if (session.mcpArmActive.load(std::memory_order_relaxed)) {
        int const outputs = std::min(buffer.getNumChannels(), processor.getTotalNumOutputChannels());
        float peak = 0.0f;
        double sumSq = 0.0;
        for (int ch = 0; ch < outputs; ++ch) {
            auto const* d = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                if (!std::isfinite(d[i])) continue;
                peak = std::max(peak, std::abs(d[i]));
                sumSq += static_cast<double>(d[i]) * d[i];
            }
        }
        session.mcpBlockPreLimiterPeak = peak;
        if (peak > session.mcpArmPreLimiterPeak.load(std::memory_order_relaxed))
            session.mcpArmPreLimiterPeak.store(peak, std::memory_order_relaxed);
        session.mcpArmPreLimiterSumSq.store(session.mcpArmPreLimiterSumSq.load(std::memory_order_relaxed) + sumSq, std::memory_order_relaxed);
        session.mcpArmPreLimiterCount.fetch_add(static_cast<uint64_t>(outputs) * static_cast<uint64_t>(buffer.getNumSamples()), std::memory_order_relaxed);
    }
    // MIDI melodic-intent capture & playback tap
    if (mcpBridge && (mcpBridge->isMidiCapturing() || mcpBridge->isMidiPlaying()))
        mcpBridge->midiInputTick(midiInputHistory, buffer.getNumSamples(), &buffer, sampleRate);
}

// End of processBlock, on the FINAL output (after gain and limiter).
void McpHost::audioBlockFinished(AudioBuffer<float>& buffer)
{
    mcp::rt::Scope const rtScope; // P3.4 S6: realtime guard (counts only in a -DMCP_RT_GUARD=ON build)

    // P9 Part B: the output ceiling first, on the output channels only (L3), so every meter below reads exactly what
    // leaves the app. While a measure window is armed, its peak BEFORE the ceiling is kept too (finite samples).
    int const outputs = std::min(buffer.getNumChannels(), processor.getTotalNumOutputChannels());
    if (session.mcpArmActive.load(std::memory_order_relaxed)) {
        float pre = 0.0f;
        for (int ch = 0; ch < outputs; ++ch) {
            auto const* d = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (std::isfinite(d[i])) pre = std::max(pre, std::abs(d[i]));
        }
        float prev = session.mcpArmPreCeilingPeak.load(std::memory_order_relaxed);
        while (pre > prev && !session.mcpArmPreCeilingPeak.compare_exchange_weak(prev, pre, std::memory_order_relaxed)) {}
        // P11 V2: the limiter's gain reduction in this block, from the two measured peaks around it (0 when it is off).
        float const preLimiter = session.mcpBlockPreLimiterPeak;
        if (processor.enableLimiter.load() && preLimiter > 1.0e-6f && pre > 1.0e-9f && preLimiter > pre) {
            float const reductionDb = 20.0f * std::log10(preLimiter / pre);
            if (reductionDb > session.mcpArmLimiterReductionDbMax.load(std::memory_order_relaxed))
                session.mcpArmLimiterReductionDbMax.store(reductionDb, std::memory_order_relaxed);
        }
    }
    session.outputCeiling.process(buffer.getArrayOfWritePointers(), outputs, buffer.getNumSamples(),
        processor.getSampleRate(), juce::Time::getMillisecondCounter());

    // Armed peak-hold (trigger verification): track the loudest sample of the FINAL
    // output (post-gain/limiter) while armed. Placed at the common end of
    // processBlock so it runs for BOTH constant and variable block sizes. The window
    // opens BEFORE the trigger (arm) and is read AFTER it, so a one-shot can never
    // fall outside it. No-op when not armed.
    if (session.mcpArmActive.load(std::memory_order_relaxed)) {
        session.mcpArmedTailRuns.fetch_add(1, std::memory_order_relaxed); // #83 diagnostics
        float p = 0.0f;
        double sumSq = 0.0; // P11 V2: the final tap's mean square
        for (int ch = 0; ch < outputs; ++ch) { // round 1 (review L2): what leaves the app, not input-only channels
            auto* d = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                float const a = std::abs(d[i]);
                if (a > p) p = a;
                sumSq += static_cast<double>(d[i]) * d[i];
            }
        }
        session.mcpArmFinalSumSq.store(session.mcpArmFinalSumSq.load(std::memory_order_relaxed) + sumSq, std::memory_order_relaxed);
        session.mcpArmFinalCount.fetch_add(static_cast<uint64_t>(outputs) * static_cast<uint64_t>(buffer.getNumSamples()), std::memory_order_relaxed);
        float prevTail = session.mcpArmedTailPeak.load(std::memory_order_relaxed);
        while (p > prevTail && !session.mcpArmedTailPeak.compare_exchange_weak(prevTail, p, std::memory_order_relaxed)) {}
        float prev = session.mcpArmPeak.load(std::memory_order_relaxed);
        while (p > prev && !session.mcpArmPeak.compare_exchange_weak(prev, p, std::memory_order_relaxed)) {}
    }

    // V1 AC taps: the FINAL output through each channel's DC blocker (reads the buffer, never writes it). The blockers
    // run on every block so a window opens settled (a fresh blocker would prime mid-tone); the sums count while armed.
    {
        double const rate = processor.getSampleRate();
        if (rate != session.mcpAcRate) {
            session.mcpAcRate = rate;
            float const coeff = mcp::ac::coefficientFor(rate);
            for (auto& blocker : session.mcpAcBlockers)
                blocker.reset(coeff);
        }
        bool const armed = session.mcpArmActive.load(std::memory_order_relaxed);
        // truth slice 2 (M2) / delta review 1-3: acquire pairs with the bridge's release at arm_spectral (the epoch bump
        // comes before it); a new epoch empties the stats here, on their only writer; busy covers the AC adds too, so
        // read_spectral waits for them
        bool const specArmed = session.mcpSpecArmed.load(std::memory_order_acquire);
        if (specArmed) {
            session.mcpSpecBusy.store(true, std::memory_order_release);
            if (uint32_t const epoch = session.mcpSpecEpoch.load(std::memory_order_acquire); epoch != session.mcpSpecEpochSeen) {
                session.mcpSpecEpochSeen = epoch;
                session.resetSpecAc();
            }
        }
        int const acChannels = std::min(outputs, McpSession::kAcChannels);
        for (int ch = 0; ch < acChannels; ++ch) {
            mcp::ac::AcStats channel;
            mcp::ac::accumulate(session.mcpAcBlockers[static_cast<size_t>(ch)], channel, buffer.getReadPointer(ch), buffer.getNumSamples());
            if (armed)
                session.mcpArmFinalAc[static_cast<size_t>(ch)].add(channel); // each channel on its own (review F3)
            if (specArmed)
                session.mcpSpecFinalAc[static_cast<size_t>(ch)].add(channel); // fixed-size atomics: no allocation
        }
        if (specArmed && !session.mcpSpecArmed.load(std::memory_order_relaxed))
            session.mcpSpecBusy.store(false, std::memory_order_release); // a read closed the window meanwhile
    }

    // Armed spectral capture (Phase 5): append the FINAL output into the mono
    // buffer while armed, so a one-shot fired between arm and read lands inside
    // the analysis window. Same common end-of-processBlock placement as the
    // peak-hold (runs for constant AND variable block sizes). `mcpSpecBusy`
    // brackets the write so the reader never copies a half-written tail.
    if (session.mcpSpecArmed.load(std::memory_order_acquire)) {
        session.mcpSpecBusy.store(true, std::memory_order_release);
        int pos = session.mcpSpecWritePos.load(std::memory_order_relaxed);
        int const stop = std::min(session.mcpSpecLimit.load(std::memory_order_relaxed),
                                  session.mcpSpecCapacity.load(std::memory_order_relaxed));
        if (!session.mcpSpecBuffer.empty() && pos < stop) {
            int const numSamples = buffer.getNumSamples();
            // truth slice 2 (M2): the OUTPUT channels only (the JUCE buffer also holds the input channels)
            int const numCh = std::min(buffer.getNumChannels(), outputs);
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
    mcp::rt::Scope const rtScope; // P3.4 S6: realtime guard (counts only in a -DMCP_RT_GUARD=ON build)
    if (mcpBridge) {
        mcpBridge->audioTick();
        mcpBridge->voiceInputTick(audioVectorIn, pdBlockSize);
    }

    // Monitor tap (solo/mute) — per Pd sub-block, before it lands in the
    // JUCE buffer, so it also goes through the master gain/limiter.
    applyMonitorTap(audioVectorOut, pdBlockSize, numChannels);

    // P11 V2: Pd's own output of an armed window, before the master gain (output channels; the vector is one run of
    // pdBlockSize samples per channel).
    if (session.mcpArmActive.load(std::memory_order_relaxed) && audioVectorOut) {
        int const outputs = std::min(numChannels, processor.getTotalNumOutputChannels());
        float peak = 0.0f;
        for (int i = 0; i < outputs * pdBlockSize; ++i)
            if (std::isfinite(audioVectorOut[i]))
                peak = std::max(peak, std::abs(audioVectorOut[i]));
        if (peak > session.mcpArmPdOutPeak.load(std::memory_order_relaxed))
            session.mcpArmPdOutPeak.store(peak, std::memory_order_relaxed);
    }
}

float McpHost::masterGainFromSlider(float const slider)
{
    // The mapping of PluginProcessor::processBlock (the toolbar volume: 0.8 is unity).
    if (slider <= 0.8f)
        return std::pow(juce::jmap(slider, 0.0f, 0.8f, 0.0f, 1.0f), 2.5f);
    return juce::jmap(slider, 0.8f, 1.0f, 1.0f, 2.0f);
}

float McpHost::masterGain() const
{
    return processor.volume ? masterGainFromSlider(processor.volume->load()) : 1.0f;
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
    if (session.mcpShuttingDown.load(std::memory_order_acquire))
        return; // P3.4 S8: after shutdown nothing may touch the patches
    job = [work = mcp::v2::captureWork(), inner = std::move(job)] { // P4.1 S3: the job replies for the request that
        mcp::v2::WorkScope const scope(work);                         // queued it (none: for none), done after it
        inner();
    };
    session.mcpPdQueue.enqueue(std::move(job));
    // One posted drain at a time; a job queued after the drain has started schedules the next one.
    if (!session.mcpPdDrainPosted.exchange(true, std::memory_order_acq_rel)) {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) {
            mm->callAsync([weak = juce::WeakReference<McpHost>(this)] { // P3.4 S8: the host may be gone
                auto* self = weak.get();
                if (self == nullptr)
                    return;
                self->session.mcpPdDrainPosted.store(false, std::memory_order_release);
                self->drainPdQueue();
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
