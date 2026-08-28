/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_osc/juce_osc.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <readerwriterqueue.h>
#include <array>
#include "Utility/Config.h"
#include "Pd/Instance.h"

class PluginProcessor;
class MCPBridge;

struct ProbeResult {
    uint32_t probeId = 0;
    float rms = 0.0f;
    float peak = 0.0f;
    int blockSize = 0;
};

static constexpr int PROBE_RING_SIZE = 1024;
static constexpr int MAX_PROBES = 16;

struct ActiveProbe {
    std::atomic<bool> active { false };
    std::atomic<t_outconnect*> outconnect { nullptr };
    uint32_t probeId = 0;
    juce::String correlationId;
    juce::String canvasName;
    juce::String tempId;
    int outletIndex = 0;
    int durationMs = 500;
    juce::int64 startTimeMs = 0;

    moodycamel::ReaderWriterQueue<ProbeResult> resultQueue { 256 };

    std::array<float, PROBE_RING_SIZE> ringBuffer {};
    std::atomic<int> ringWritePos { 0 };

    float accRms = 0.0f;
    float accPeak = 0.0f;
    int accBlocks = 0;
};

class ProbeManager {
public:
    explicit ProbeManager(MCPBridge* owner);

    void audioTick();
    int startProbe(t_outconnect* oc, const juce::String& canvasName, const juce::String& tempId, int outletIndex, const juce::String& correlationId, int durationMs);
    void stopProbe(uint32_t probeId, const juce::String& correlationId = {});
    void stopAllProbes(const juce::String& correlationId = {});
    void collectResults();

    int getActiveProbeCount() const;
    bool isDebugEnabledByUs() const { return debugWasEnabledByUs; }
    void setDebugEnabledByUs(bool v) { debugWasEnabledByUs = v; }

    ActiveProbe probes[MAX_PROBES];

private:
    MCPBridge* bridge = nullptr;
    std::atomic<uint32_t> nextProbeId { 1 };
    bool debugWasEnabledByUs = false;
};

class MCPBridge final : public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
                      , private juce::Timer
{
public:
    explicit MCPBridge(PluginProcessor* processor, int listenPort = 9000, int sendPort = 19010);
    ~MCPBridge() override;

    bool start();
    void stop();
    bool isConnected() const;

    // Status for the GUI / console. Returns a short human-readable state:
    // "connected" | "listening" | "stopped" | "error: <detail>".
    juce::String getStatus() const;
    int getListenPort() const { return listenPort; }
    int getSendPort() const { return sendPort; }

    // Mark server activity (called on any inbound OSC message) so the
    // bridge can distinguish "listening" from "server connected".
    void noteServerActivity();

    // OSC Inbound Callbacks
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void oscBundleReceived(const juce::OSCBundle& bundle) override;

    // Audio thread hook for zero-dropout passive signal metering
    void audioTick();
    ProbeManager& getProbeManager() { return probeManager; }

    // Native Telemetry Dispatch (Port 19010)
    void sendSelectionTelemetry(const juce::String& selector, const SmallArray<pd::Atom>& list);
    void sendConsoleLog(const juce::String& message, bool isError);
    void sendPrompt(const juce::String& promptText);
    void sendReply(const juce::String& addressPattern, const juce::Array<juce::var>& args);
    void sendReply(const juce::String& addressPattern, float val);
    void sendReply(const juce::String& addressPattern, const juce::String& str);
    void sendRawReply(const juce::String& addressPattern);

    // Canvas & Table Helpers
    static juce::String normalizeCanvas(const juce::String& name);

    friend class ProbeManager;

private:
    void handlePdDomain(const juce::String& action, const juce::OSCMessage& msg);
    void handleParamDomain(const juce::String& paramName, const juce::OSCMessage& msg);
    void handleTriggerDomain(const juce::String& triggerAction, const juce::OSCMessage& msg);
    void handleTelemetryDomain(const juce::String& telAction, const juce::OSCMessage& msg);
    void handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg);
    void handleBridgeDomain(const juce::String& bridgeAction, const juce::OSCMessage& msg);
    void handleMorphDomain(const juce::String& morphAction, const juce::OSCMessage& msg);
    void handleMeterDomain(const juce::String& meterAction, const juce::OSCMessage& msg);

    t_outconnect* resolveProbeTarget(const juce::String& canvasName, const juce::String& targetId, int outletIndex, juce::String& errorOut);
    bool activateProbing();

    void timerCallback() override;

    struct MorphParam {
        juce::String name;
        float from = 0.0f;
        float to = 0.0f;
    };

    struct MorphJob {
        juce::String correlationId;
        int totalSteps = 20;
        int currentStep = 0;
        int intervalMs = 50;
        std::vector<MorphParam> params;
    };

    PluginProcessor* processor = nullptr;
    int listenPort = 9000;
    int sendPort = 19010;

    juce::OSCReceiver receiver;
    juce::OSCSender sender;
    std::atomic<bool> active { false };

    std::atomic<juce::int64> lastServerActivity { 0 };
    mutable bool reportedConnected = false;
    mutable bool reportedLost = false;
    juce::String statusMessage;
    // Unique per-process boot token. The MCP server compares it across
    // capability handshakes to detect a PlugData restart and clear its stale
    // identity cache (tempId -> index mappings from the previous session).
    juce::String bootToken;

    std::vector<MorphJob> morphJobs;
    juce::CriticalSection morphLock;

    ProbeManager probeManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MCPBridge)
};

