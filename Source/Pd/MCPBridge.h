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
#include "Utility/Config.h"
#include "Pd/Instance.h"

class PluginProcessor;

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

private:
    void handlePdDomain(const juce::String& action, const juce::OSCMessage& msg);
    void handleParamDomain(const juce::String& paramName, const juce::OSCMessage& msg);
    void handleTriggerDomain(const juce::String& triggerAction, const juce::OSCMessage& msg);
    void handleTelemetryDomain(const juce::String& telAction, const juce::OSCMessage& msg);
    void handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg);
    void handleBridgeDomain(const juce::String& bridgeAction, const juce::OSCMessage& msg);
    void handleMorphDomain(const juce::String& morphAction, const juce::OSCMessage& msg);

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

    std::vector<MorphJob> morphJobs;
    juce::CriticalSection morphLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MCPBridge)
};
