/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
// The hidden voice engine of the ai.* widgets. See McpVoiceEngine.h.
#include <juce_events/juce_events.h>

#include <cstring>
#include <map>
#include <mutex>

#include "PluginProcessor.h"
#include "Utility/Config.h"
#include "Mcp/McpPdFile.h"
#include "Mcp/McpVoiceEngine.h"

namespace mcp::voice {

namespace {

constexpr int lookIntervalMs = 5000;
constexpr juce::int64 settleMs = 2000;       // a file younger than this may still be being copied: not opened yet
constexpr juce::uint32 reopenGapMs = 10000;  // an engine that was loaded and died is not reopened more often than this

// One engine per processor (a plugin host may hold several).
struct Engine {
    PluginProcessor* processor = nullptr;
    pd::Patch::Ptr patch;             // keeps the Pd root alive; never put into processor.patches
    juce::int64 loadedStamp = 0;      // the file this canvas was made from
    juce::int64 failedStamp = 0;      // a file that did not open (or a swap that came out with broken boxes): not retried until it changes
    juce::uint32 loadedAtMs = 0;      // juce::Time::getMillisecondCounter() of the last successful open
};

// The file's modification time (ms) mixed with its size; 0 = the file is not there (one stat: it may vanish between looks).
juce::int64 stampOf(juce::File const& file)
{
    auto const time = file.getLastModificationTime().toMilliseconds();
    if (time <= 0 || !file.existsAsFile())
        return 0;
    return time ^ (file.getSize() << 24);
}

bool settled(juce::File const& file)
{
    return juce::Time::getCurrentTime().toMilliseconds() - file.getLastModificationTime().toMilliseconds() >= settleMs;
}

bool alive(Engine const& e)
{
    return e.patch != nullptr && e.patch->getRawPointer() != nullptr;
}

// Boxes Pd could not create (red boxes: text_class, T_OBJECT), subpatches included.
int brokenBoxes(t_canvas* cnv)
{
    int n = 0;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
        if (pd_class(&g->g_pd) == canvas_class)
            n += brokenBoxes(reinterpret_cast<t_canvas*>(g));
        else if (pd_class(&g->g_pd) == text_class && reinterpret_cast<t_text*>(g)->te_type == T_OBJECT)
            ++n;
    }
    return n;
}

// Opens the file into a fresh canvas. The old canvas goes only once the new one is there (under the Pd lock: the audio
// thread may be running), and a swap that comes out with broken boxes is thrown away: a worse engine never replaces a
// working one. A FIRST open with broken boxes is kept (some sound beats none) and said loudly.
bool openInto(Engine& e, juce::File const& file, juce::int64 const stamp)
{
    auto& processor = *e.processor;
    bool const swap = alive(e);
    processor.lockAudioThread();
    pd::Patch::Ptr patch = processor.openPatch(file); // no editor tab, not in `patches`: only Pd knows this root
    bool const opened = patch != nullptr && patch->getRawPointer() != nullptr;
    int const broken = opened ? brokenBoxes(patch->getRawPointer()) : 0;
    bool const keepNew = opened && !(swap && broken > 0);
    if (keepNew)
        e.patch = patch; // the old root is freed here (~Patch closes it and its object implementations)
    patch = nullptr;     // otherwise the new root is closed again and the old one keeps running
    processor.unlockAudioThread();
    juce::String const name(engineFileName);
    if (!opened) {
        e.failedStamp = stamp;
        processor.logError(name + ": the voice engine could not be opened (" + file.getFullPathName() + ")" + (swap ? "; the old one keeps running" : ""));
        return swap;
    }
    if (!keepNew) {
        e.failedStamp = stamp;
        processor.logError(name + ": the changed file has " + juce::String(broken) + " box(es) Pd could not create; the old voice engine keeps running");
        return true;
    }
    e.loadedStamp = stamp;
    e.failedStamp = 0;
    e.loadedAtMs = juce::Time::getMillisecondCounter();
    if (broken > 0)
        processor.logError(name + ": " + juce::String(broken) + " box(es) could not be created: the voice engine is incomplete");
    processor.logMessage(name + (swap ? ": voice engine reloaded (hidden, no tab)" : ": voice engine loaded (hidden, no tab): ai.* widgets sound by themselves"));
    return true;
}

// One look at the file: opens it when the engine is not alive, swaps it when it changed. A file touched in the last 2 s is
// left alone (it may be mid-copy). True when an engine is alive afterwards.
bool refresh(Engine& e)
{
    auto const file = engineFile();
    bool const isAlive = alive(e);
    auto const stamp = stampOf(file);
    if (stamp == 0 || !settled(file))
        return isAlive; // gone, or touched a moment ago: what runs keeps running
    if (isAlive) {
        if (stamp == e.loadedStamp || stamp == e.failedStamp)
            return true;
    } else {
        if (stamp == e.failedStamp)
            return false; // this very file did not open
        if (e.loadedStamp != 0 && juce::Time::getMillisecondCounter() - e.loadedAtMs < reopenGapMs)
            return false; // it was loaded and died a moment ago: not in a loop
    }
    return openInto(e, file, stamp);
}

struct Registry final : private juce::Timer {
    std::recursive_mutex lock;
    std::map<pd::Instance const*, Engine> engines;

    void timerCallback() override
    {
        std::lock_guard<std::recursive_mutex> const guard(lock);
        for (auto& entry : engines)
            refresh(entry.second);
    }
    void startLooking() { if (!isTimerRunning()) startTimer(lookIntervalMs); }
    void stopLooking() { stopTimer(); }
};

// Never destroyed: a static destructor at exit would free a Pd root after its instance is gone.
Registry& registry()
{
    static auto* instance = new Registry();
    return *instance;
}

pd::Instance const* keyOf(PluginProcessor const& p)
{
    return static_cast<pd::Instance const*>(&p);
}

} // namespace

juce::File engineFile()
{
    return ProjectInfo::appDataDir.getChildFile("Abstractions").getChildFile(engineFileName);
}

bool engineEnabled()
{
    auto const value = juce::SystemStats::getEnvironmentVariable("PLUGDATA_NO_VOICE_ENGINE", {}).trim();
    return value.isEmpty() || value == "0";
}

bool isEngineRoot(_glist const* canvas)
{
    if (!canvas || canvas->gl_owner || !canvas->gl_name || !canvas->gl_name->s_name)
        return false;
    if (std::strcmp(canvas->gl_name->s_name, engineFileName) != 0)
        return false;
    auto* dir = canvas_getdir(const_cast<t_canvas*>(canvas));
    if (!dir || !dir->s_name)
        return false;
    return mcp::fileInPdDir(juce::String::fromUTF8(dir->s_name), juce::String::fromUTF8(canvas->gl_name->s_name)) == engineFile();
}

bool openEngine(PluginProcessor& processor)
{
    if (!engineEnabled())
        return false;
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> const guard(r.lock);
    auto& e = r.engines[keyOf(processor)];
    e.processor = &processor;
    r.startLooking();
    return refresh(e);
}

bool lookNow(PluginProcessor& processor)
{
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> const guard(r.lock);
    auto const it = r.engines.find(keyOf(processor));
    return it != r.engines.end() && refresh(it->second);
}

bool engineLoaded(PluginProcessor const& processor)
{
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> const guard(r.lock);
    auto const it = r.engines.find(keyOf(processor));
    return it != r.engines.end() && alive(it->second);
}

_glist* engineRoot(PluginProcessor const& processor)
{
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> const guard(r.lock);
    auto const it = r.engines.find(keyOf(processor));
    return it != r.engines.end() && it->second.patch != nullptr ? it->second.patch->getRawPointer() : nullptr;
}

void closeEngine(PluginProcessor& processor)
{
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> const guard(r.lock);
    auto const it = r.engines.find(keyOf(processor));
    if (it == r.engines.end())
        return;
    processor.setThis();
    it->second.patch = nullptr; // ~Patch closes the root (the audio device is already stopped when a processor shuts down)
    r.engines.erase(it);
    if (r.engines.empty())
        r.stopLooking();
}

} // namespace mcp::voice
