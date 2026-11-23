#include "Config.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Standalone/PlugDataWindow.h"

#if PLUGDATA_FX
char const* ProjectInfo::projectName = "plugdata-fx";
#elif PLUGDATA_MIDI
char const* ProjectInfo::projectName = "plugdata-midi";
#else
char const* ProjectInfo::projectName = "plugdata";
#endif

#if PLUGDATA_STANDALONE
bool ProjectInfo::isStandalone = true;
#else
bool ProjectInfo::isStandalone = false;
#endif

#if PLUGDATA_FX
bool ProjectInfo::isFx = true;
#else
bool ProjectInfo::isFx = false;
#endif

juce::AudioDeviceManager* ProjectInfo::getDeviceManager()
{
#if PLUGDATA_STANDALONE
    if (auto* standalone = getStandalonePluginHolder())
        return &standalone->deviceManager;

    return nullptr;
#else
    return nullptr;
#endif
}

PlugDataWindow* ProjectInfo::createNewWindow(PluginEditor* editor)
{
#if PLUGDATA_STANDALONE
    return new PlugDataWindow(reinterpret_cast<AudioProcessorEditor*>(editor));
#else
    ignoreUnused(editor);
    return nullptr;
#endif
}

void ProjectInfo::closeWindow(PlugDataWindow* window)
{
#if PLUGDATA_STANDALONE
    delete window;
#else
    ignoreUnused(window);
#endif
}

bool ProjectInfo::canUseSemiTransparentWindows()
{
#if JUCE_IOS
    return true;
#endif
#if !JUCE_MAC || PLUGDATA_STANDALONE
    // MCP-CARRIED(x11-roundtrips): on X11 this asks the X server (XGetSelectionOwner + XGetVisualInfo, synchronous round trips)
    // and PluginEditor::paint calls it on EVERY frame (perf 2026-11-23: ~10 % of the message thread waiting on X). The answer
    // (is a compositor running) practically never changes: re-ask at most every 2 s. Message thread only (paint, look and feel).
    static bool cachedAnswer = false;
    static uint32 askedAtMs = 0;
    auto const nowMs = Time::getMillisecondCounter();
    if (askedAtMs == 0 || nowMs - askedAtMs > 2000) {
        cachedAnswer = Desktop::canUseSemiTransparentWindows();
        askedAtMs = nowMs == 0 ? 1 : nowMs;
    }
    return cachedAnswer;
#else
    return Desktop::canUseSemiTransparentWindows();
#endif
}

bool ProjectInfo::isMidiEffect() noexcept
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

StandalonePluginHolder* ProjectInfo::getStandalonePluginHolder()
{
#if PLUGDATA_STANDALONE
    return StandalonePluginHolder::getInstance();
#else
    return nullptr;
#endif
}
