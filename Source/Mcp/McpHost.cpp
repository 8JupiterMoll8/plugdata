/*
 // The MCP host (Plan 3 P3.2 S4). The functions below were PluginProcessor members;
 // moved verbatim, with `session.` / `processor.` prefixes. See Source/Mcp/McpHost.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Utility/SettingsFile.h"
#include "Canvas.h"
#include "PluginEditor.h"
#include "Utility/Config.h"
#include "Mcp/McpAudit.h"
#include "Mcp/McpRtGuard.h"

extern "C" int ugen_getsortno(void); // d_ugen.c (P3.4 S5)

McpHost::McpHost(PluginProcessor& p)
    : processor(p)
    , guiHost(createMcpGuiHost(p))
{
    sysTable(); // a bad selector table fails here, at startup
    if (mcp::audit::enabled())
        identity.setAccessCheck(&mcp::audit::checkIdentityAccess); // P3.5 S2: report access without the Pd lock
    // P3.5 S3: identity records are per live canvas. The liveness check is a pd::WeakReference: plugdata's Pd
    // clears it when the canvas is freed, so a record never outlives its canvas (nor passes to a new canvas at
    // the same address).
    mcp::IdentityService::Hooks hooks;
    hooks.baseKey = [](t_canvas* c) { return identityBaseKey(c).toStdString(); };
    hooks.liveness = [this](t_canvas* c) {
        pd::WeakReference ref(c, &processor);
        return std::function<bool()>([ref] { return ref.isValid(); });
    };
    // S4: the theft check. An id held by an object that is no longer in its canvas may be taken over.
    hooks.objectLive = [](t_canvas* c, t_gobj* g) {
        for (t_gobj* y = c ? c->gl_list : nullptr; y; y = y->g_next)
            if (y == g) return true;
        return false;
    };
    // S5: a weak reference per bound object, so an id never resolves to another object at a reused address.
    hooks.objectGuard = [this](t_gobj* g) {
        pd::WeakReference ref(g, &processor);
        return std::function<bool()>([ref] { return ref.isValid(); });
    };
    identity.setHooks(std::move(hooks));
}

// P3.5 S4: the prefix of a minted id (owner decision O1: type + number): an abstraction's file name, "pd" for
// a subpatch, "table" for an array, else the class; sanitized ("osc~" -> "osc_sig", "plate.rev.m~" -> "plate_rev").
juce::String McpHost::mintPrefix(t_gobj* g)
{
    if (!g)
        return "obj";
    t_class* cl = pd_class(&g->g_pd);
    char const* clName = class_getname(cl);
    juce::String type = clName ? juce::String::fromUTF8(clName) : juce::String("obj");
    if (cl == canvas_class) {
        juce::String abstractName;
        pd::getAbstractionFileName(g, abstractName);
        type = abstractName.isNotEmpty() ? abstractName : juce::String("pd");
    } else if (cl == garray_class) {
        type = "table";
    }
    return pd::sanitizeClassNameForTempId(type);
}

// P3.5 S3: a canvas's wire key before any "#n": a root is its file name, a subcanvas "pd-<name>", an unnamed
// canvas "pd-anon" (the old canonicalCanvasKey used the pointer there, which survived address reuse).
juce::String McpHost::identityBaseKey(t_canvas* cnv)
{
    if (!cnv)
        return {};
    juce::String const name = cnv->gl_name && cnv->gl_name->s_name ? juce::String::fromUTF8(cnv->gl_name->s_name) : juce::String();
    if (cnv->gl_owner != nullptr) {
        if (name.startsWith("pd-")) return name;
        if (name.isNotEmpty()) return "pd-" + name;
        return "pd-anon";
    }
    if (name.isNotEmpty() && name != "main" && name != "pd-main")
        return name;
    return "pd-anon";
}

void McpHost::registerOpenCanvases()
{
    PdLockScope lock(processor);
    if (!pd_this)
        return;
    identity.prune();
    // S6: one walk also takes each canvas's structure stamp (object count + FNV-1a of the object pointers): a
    // GUI create/delete/retype changes it, and IdentityCore::observeStamp bumps that canvas's version, so
    // analyze_patch(sync) sees a manual edit on its next identity_version (S0: 0.04-0.13 us per 37 objects).
    bool edited = false;
    std::function<void(t_canvas*)> walk = [&](t_canvas* c) {
        identity.canvasMap(c);
        uint64_t stamp = 1469598103934665603ull;
        uint64_t count = 0;
        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++count) {
            stamp = (stamp ^ reinterpret_cast<uintptr_t>(g)) * 1099511628211ull;
            if (pd_class(&g->g_pd) == canvas_class)
                walk(reinterpret_cast<t_canvas*>(g));
        }
        edited = identity.observeStamp(c, stamp ^ (count << 40)) || edited;
    };
    std::vector<t_canvas*> roots; // pd_canvaslist is newest first: walk it oldest first
    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next)
        roots.push_back(c);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        walk(*it);
    if (edited)
        identity.prune(); // drop the entries of objects deleted by hand
    rebindTombstones();
}

// P3.5 S5 (L9): before an abstraction reload (a save of that abstraction, or encapsulate_to_file) every id bound to
// one of its instances leaves a tombstone {id, index, file}; the reload recreates the instance at the same index
// (glist_doreload restores it, P3.5 S0 read 2).
void McpHost::tombstoneAbstractionInstances(juce::String const& fileName)
{
    PdLockScope lock(processor);
    if (!pd_this || fileName.isEmpty())
        return;
    std::function<void(t_canvas*)> walk = [&](t_canvas* c) {
        int index = 0;
        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
            if (pd_class(&g->g_pd) != canvas_class)
                continue;
            juce::String absName;
            pd::getAbstractionFileName(g, absName);
            if (absName == fileName) {
                auto const id = identity.idOf(g);
                if (!id.empty())
                    identity.addTombstone(c, id, index, absName.toStdString());
            }
            walk(reinterpret_cast<t_canvas*>(g));
        }
    };
    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next)
        walk(c);
}

// ... and afterwards each such id is bound to the object at that index if it is an instance of the same file.
void McpHost::rebindTombstones()
{
    PdLockScope lock(processor);
    if (!identity.anyTombstones())
        return;
    std::function<void(t_canvas*)> walk = [&](t_canvas* c) {
        for (auto const& stone : identity.tombstones(c)) {
            if (identity.valid(c, stone.id)) {
                identity.dropTombstone(c, stone.id); // never died: nothing to do
                continue;
            }
            int index = 0;
            for (t_gobj* g = c->gl_list; g; g = g->g_next, ++index) {
                if (index != stone.index)
                    continue;
                juce::String absName;
                if (pd_class(&g->g_pd) == canvas_class)
                    pd::getAbstractionFileName(g, absName);
                if (absName.toStdString() == stone.tag && identity.idOf(g).empty()) {
                    identity.bind(c, stone.id, g);
                    identity.dropTombstone(c, stone.id);
                }
                break;
            }
        }
        for (t_gobj* g = c->gl_list; g; g = g->g_next)
            if (pd_class(&g->g_pd) == canvas_class)
                walk(reinterpret_cast<t_canvas*>(g));
    };
    for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
        walk(c);
}

McpHost::PdLockScope::PdLockScope(PluginProcessor& p)
{
    p.setThis();
    sys_lock();
}

McpHost::PdLockScope::~PdLockScope()
{
    sys_unlock();
}

McpHost::~McpHost()
{
    shutdown();
}

void McpHost::postMessage(std::function<void()> fn)
{
    juce::MessageManager::callAsync([weak = juce::WeakReference<McpHost>(this), fn = std::move(fn)] {
        if (weak.get() != nullptr)
            fn();
    });
}

void McpHost::shutdown()
{
    if (session.mcpShuttingDown.exchange(true))
        return;
    focusTimer.stopTimer();
    if (mcpBridge)
        mcpBridge->shutdown();
    std::function<void()> job;
    while (session.mcpPdQueue.try_dequeue(job))
        job = nullptr; // never run: the patches they would touch are about to go
}

// Audio-device xruns since the device opened; -1 when unknown (no device, a
// plugin build, or a backend that does not count them). P3.4 S0: lets live
// gates prove "zero dropouts" by measurement. Reads the device manager only.
int McpHost::xrunCount() const
{
    if (auto* dm = ProjectInfo::getDeviceManager())
        if (auto* dev = dm->getCurrentAudioDevice())
            return dev->getXRunCount();
    return -1;
}

void McpHost::createBridge()
{
    auto const mcpEnabled = processor.settingsFile->getProperty<bool>("mcp_bridge_enabled");
    auto const mcpListen = processor.settingsFile->getProperty<int>("mcp_bridge_listen_port");
    auto const mcpSend = processor.settingsFile->getProperty<int>("mcp_bridge_send_port");
    // P3.4 S0: with PLUGDATA_MCP_AUDIT=1, time every Pd-lock hold (no-op otherwise).
    processor.setThis();
    mcp::audit::installLock(&processor.audioLock);
    mcpBridge = std::make_unique<MCPBridge>(&processor, mcpListen, mcpSend);
    focusTimer.startTimer(50); // P3.4 S3: publishes the focused root for getCanvasBySymbol("main")
    if (!mcpEnabled) {
        mcpBridge->stop();
    }
}

juce::String McpHost::getMcpBridgeStatus() const
{
    return mcpBridge ? mcpBridge->getStatus() : juce::String("unavailable");
}

// PluginProcessor::settingsChanged, first: the mcp_bridge_* settings are the host's.
bool McpHost::settingsChanged(String const& name, var const& value)
{
    if (name.startsWith("mcp_bridge_")) {
        mcpSettingsChanged(name, value);
        return true;
    }
    return false;
}

void McpHost::mcpSettingsChanged(String const& name, var const& value)
{
    if (!mcpBridge) return;

    if (name == "mcp_bridge_enabled") {
        if (static_cast<bool>(value)) {
            mcpBridge->start();
        } else {
            mcpBridge->stop();
        }
    } else if (name == "mcp_bridge_listen_port" || name == "mcp_bridge_send_port") {
        // Recreate the bridge socket with the new ports. Applies immediately;
        // the MCP server must be configured to match the listen port.
        auto const enabled = processor.settingsFile->getProperty<bool>("mcp_bridge_enabled");
        auto const listen = processor.settingsFile->getProperty<int>("mcp_bridge_listen_port");
        auto const send = processor.settingsFile->getProperty<int>("mcp_bridge_send_port");
        // P3.4 S8: the same object on new ports. Rebuilding it here destroyed the bridge while the audio
        // thread could be inside its audioTick or midiInputTick.
        mcpBridge->rebind(listen, send);
        if (enabled && !mcpBridge->isConnected()) mcpBridge->start();
        if (!enabled) mcpBridge->stop();
        processor.logMessage("MCP bridge ports updated (listen " + String(listen) + ", send " + String(send) + ")");
    }
}

// ── Monitor tap: solo / mute (NON-DESTRUCTIVE) ──────────────────────────────
// Runs per Pd SUB-BLOCK right after performDSP, so it works for any JUCE block
// size (Pd runs pdBlockSize sub-blocks inside a larger buffer) and the soloed
// signal goes through the master gain/limiter exactly like it does in the mix.
// The patch is never touched; the tap EXPIRES and a dead signal turns it off.
void McpHost::applyMonitorTap(float* out, int numSamples, int numChannels)
{
    if (session.mcpTapMode.load(std::memory_order_relaxed) == McpSession::mcpTapOff) return;
    auto const nowMs = juce::Time::getMillisecondCounter();
    if (nowMs > session.mcpTapUntilMs.load(std::memory_order_relaxed)) {
        session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        session.mcpTapSig.store(nullptr, std::memory_order_relaxed);
        return;
    }
    // P3.4 S5: touch the signal only under the Pd lock and only while the DSP chain it was resolved in is
    // still the current one (a rebuild frees signals; the timer then resolves the tap again). A busy lock or
    // an old chain leaves this block untouched rather than wait or read freed memory.
    juce::ScopedTryLock const pdLock(processor.audioLock);
    if (!pdLock.isLocked() || pd_getdspstate() == 0
        || session.mcpTapEpoch.load(std::memory_order_relaxed) != ugen_getsortno())
        return;
    // Re-read s_vec EVERY sub-block: Pd swaps outlet buffers per DSP tick, so a
    // captured vector goes stale after one block.
    auto* sig = static_cast<t_signal*>(session.mcpTapSig.load(std::memory_order_acquire));
    float* vec = (sig != nullptr && sig->s_vec != nullptr) ? sig->s_vec : nullptr;
    int const n = (vec != nullptr) ? sig->s_n : 0;
    if (vec == nullptr || n <= 0) {
        session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        return;
    }
    int const mode = session.mcpTapMode.load(std::memory_order_relaxed);
    float const g = session.mcpTapGain.load(std::memory_order_relaxed);
    int const copy = juce::jmin(n, numSamples);
    for (int ch = 0; ch < numChannels; ++ch) {
        float* d = out + static_cast<size_t>(ch) * static_cast<size_t>(numSamples);
        if (mode == McpSession::mcpTapSolo) {
            for (int i = 0; i < copy; ++i) d[i] = vec[i] * g;
            for (int i = copy; i < numSamples; ++i) d[i] = 0.0f;
        } else {
            // mute: subtract the node's contribution at unity — exact on the
            // pre-limiter path (downstream of the target, linear mixing).
            for (int i = 0; i < copy; ++i) d[i] -= vec[i];
        }
    }
}

bool McpHost::mcpOpenRecorder(juce::String const& path)
{
    juce::File const destFile(path);
    auto outStream = std::unique_ptr<juce::FileOutputStream>(destFile.createOutputStream());
    if (!outStream || outStream->failedToOpen()) {
        post("MCP record: could not create output stream for %s", path.toRawUTF8());
        return false;
    }
    // FileOutputStream appends to an existing file; a take must start empty.
    outStream->setPosition(0);
    outStream->truncate();

    double sampleRate = processor.getSampleRate();
    if (sampleRate <= 0)
        sampleRate = 44100.0;
    // Cap at stereo so the file plays everywhere; the audible mix is on 0-1.
    int const numCh = jlimit(1, 2, processor.getTotalNumOutputChannels());

    // 16-bit PCM = maximum player compatibility.
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(outStream.get(), sampleRate, static_cast<unsigned int>(numCh), 16, {}, 0));
    if (!writer) {
        post("MCP record: createWriterFor failed (sr=%.0f ch=%d)", sampleRate, numCh);
        return false;
    }
    outStream.release(); // the writer owns the stream now

    if (!session.mcpRecorderThread.isThreadRunning())
        session.mcpRecorderThread.startThread();
    // ~3 s of FIFO at 44.1 kHz between the audio thread and the disk.
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), session.mcpRecorderThread, 1 << 17);

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> previous;
    {
        const ScopedLock sl(session.mcpRecorderLock);
        previous = std::move(session.mcpThreadedWriter);
        session.mcpThreadedWriter = std::move(threaded);
        session.mcpRecorderPath = path;
    }
    previous.reset(); // flush any stale take outside the lock
    post("MCP record: writer created sr=%.0f ch=%d path=%s", sampleRate, numCh, path.toRawUTF8());
    return true;
}

void McpHost::mcpCloseRecorder()
{
    session.mcpRecording.store(false, std::memory_order_release);
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> finished;
    {
        const ScopedLock sl(session.mcpRecorderLock);
        finished = std::move(session.mcpThreadedWriter);
    }
    finished.reset(); // flushes the FIFO and closes the file, off the audio thread
}

// MCP zero-dropout recorder tap, called at the end of processConstant (the full
// constant-path output block) and processVariable (after output is ready). Taps the
// final output buffer directly, no canvas objects needed.
void McpHost::writeRecorderTap(dsp::AudioBlock<float> const& buffer)
{
    mcp::rt::Scope const rtScope; // P3.4 S6
    if (!session.mcpRecording.load(std::memory_order_acquire))
        return;

    const ScopedTryLock sl(session.mcpRecorderLock);
    if (!sl.isLocked() || !session.mcpThreadedWriter)
        return;

    // Audio thread: no allocation, no file I/O. ThreadedWriter::write only
    // copies into its FIFO; a full FIFO drops this block instead of blocking.
    int const available = static_cast<int>(buffer.getNumChannels());
    if (available <= 0)
        return;
    float const* channels[2] = { buffer.getChannelPointer(0), buffer.getChannelPointer(available > 1 ? 1 : 0) };
    session.mcpThreadedWriter->write(channels, static_cast<int>(buffer.getNumSamples()));
}

// "main" must ALWAYS resolve to the true top-level patch. The editor's current
// canvas can be a subpatch OR an abstraction tab the artist has navigated into —
// automating against that would silently target the voice's internals instead of
// the patch (phantom "receive->canvas connection failed", census listing internals
// as top-level, silent recalls). Walk gl_owner all the way up (past abstractions
// too, unlike canvas_getrootfor which stops at them) so MCP operations always land
// on the actual root patch. Message thread only (it reads the editors).
void McpHost::refreshFocusedRoot()
{
    t_canvas* root = nullptr;
    for (auto* editor : processor.getEditors()) {
        if (editor) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                auto* glist = reinterpret_cast<t_glist*>(cnv->patch.getRawPointer());
                while (glist && glist->gl_owner) glist = glist->gl_owner;
                if (glist) {
                    root = reinterpret_cast<t_canvas*>(glist);
                    break;
                }
            }
        }
    }
    // P3.5 S3: no editor (the plugin's window is closed, or a headless host): "main" is the most recently opened
    // patch the processor holds, never the first canvas (which with no patch open is Pd's float-array template).
    if (!root) {
        for (int i = static_cast<int>(processor.patches.size()) - 1; i >= 0 && !root; --i) {
            if (auto* glist = reinterpret_cast<t_glist*>(processor.patches[i]->getRawPointer())) {
                while (glist->gl_owner) glist = glist->gl_owner;
                root = reinterpret_cast<t_canvas*>(glist);
            }
        }
    }
    session.mcpFocusedRoot.store(root, std::memory_order_release);
}

t_canvas* McpHost::getCanvasBySymbol(const String& canvas_symbol)
{
    // "main" is the true root of the focused editor's current canvas, published by the message thread
    // (refreshFocusedRoot), used only while it is still an open root patch. P3.5 S3: with no patch open, "main"
    // is nothing (it used to be the first canvas: Pd's float-array template).
    if (canvas_symbol == "pd-main" || canvas_symbol == "main" || canvas_symbol.isEmpty()) {
        PdLockScope lock(processor);
        registerOpenCanvases();
        if (auto* root = session.mcpFocusedRoot.load(std::memory_order_acquire)) {
            for (t_canvas* c = pd_this ? pd_this->pd_canvaslist : nullptr; c; c = c->gl_next)
                if (c == root) return root;
        }
        return nullptr;
    }

    // P3.5 S3 (#62): every other name is a wire key of the identity records ("song.pd", "pd-voice", "pd-voice#2",
    // or the "pd-" spelling of either). Unknown names answer nullptr: no fallback to the first canvas, no
    // per-request Pd symbol.
    PdLockScope lock(processor);
    registerOpenCanvases();
    return identity.canvasByKey(canvas_symbol.toStdString());
}

t_canvas* McpHost::getCanvasBySymbolStrict(const String& canvas_symbol)
{
    return getCanvasBySymbol(canvas_symbol); // P3.5 S3: every lookup is strict now
}

void McpHost::synchroniseCanvases()
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) {
        if (!mm->isThisTheMessageThread()) {
            postMessage([this] { synchroniseCanvases(); });
            return;
        }
    } else {
        return;
    }

    for (auto* editor : processor.getEditors()) {
        if (!editor) continue;
        SmallArray<Component::SafePointer<Canvas>> canvases;
        for (auto* canvas : editor->getCanvases()) {
            if (canvas) canvases.add(canvas);
        }
        for (auto& cnv : canvases) {
            if (cnv.getComponent()) {
                cnv->synchronise();
            }
        }
    }
}

juce::String McpHost::getStableId(t_gobj* g) const
{
    PdLockScope lock(processor);
    return juce::String(identity.idOf(g));
}

juce::String McpHost::getOrAdoptStableId(t_canvas* canvas, t_gobj* g)
{
    if (!g) return {};
    PdLockScope lock(processor);
    auto const existing = getStableId(g);
    if (existing.isNotEmpty()) return existing;
    if (!canvas) return {};

    // P3.5 S4: one minter (IdentityCore::mint), only for an object of this canvas.
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next)
        if (y == g)
            return juce::String(identity.mint(canvas, g, mintPrefix(g).toStdString()));
    return {};
}

t_gobj* McpHost::resolveStableId(const String& canvasName, const String& objectId)
{
    PdLockScope lock(processor);
    // Identity is keyed by the canvas's CANONICAL key. "main" is a
    // focus-dependent ALIAS, never a stored key — resolve the canvas first and
    // derive its canonical key so both sides of the map agree.
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    if (!canvas) return nullptr; // P3.5 S3: no canvas, no id
    auto idStr = objectId.toStdString();
    
    // P3.5 S5: IdentityCore::resolve checks the binding itself (the object it was bound to still lives: its weak
    // reference, not just an address; this closes Report F3, address reuse) and drops a stale entry. The
    // gl_list walk keeps the old guarantee that the object is still in THIS canvas.
    if (t_gobj* bound = identity.resolve(canvas, idStr)) {
        for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next)
            if (y_obj == bound)
                return bound;
        if (auto* ids = identity.findCanvas(canvas)) // moved out of this canvas: forget it here
            ids->erase(idStr);
    }

    // Index-based fallback: "0", "1", "obj_0", "obj_1", etc.
    if (objectId.startsWith("obj_") || objectId.containsOnly("0123456789")) {
        int idx = objectId.startsWith("obj_") ? objectId.substring(4).getIntValue() : objectId.getIntValue();
        if (canvas) {
            int cur = 0;
            for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next, cur++) {
                if (cur == idx) return y_obj;
            }
        }
    }

    return nullptr;
}

void McpHost::reconcileIdentity(const String& canvasName)
{
    PdLockScope lock(processor);
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    if (!canvas)
        return;

    auto& canvasMap = identity.canvasMap(canvas);

    // Step A: live pointer set from gl_list (the ground truth).
    std::unordered_set<t_gobj*> livePointers;
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next)
        livePointers.insert(y);

    // Step B: evict entries whose gobj no longer exists on the canvas.
    for (auto it = canvasMap.begin(); it != canvasMap.end();) {
        if (livePointers.find(it->second) == livePointers.end()) {
            it = canvasMap.erase(it); // P3.5 S5: serial kept (the address may be reused by a bound object)
            identity.bumpVersion(canvas);
        } else {
            ++it;
        }
    }

    // Step C: adopt objects the identity map doesn't track yet (GUI-created).
    std::unordered_set<t_gobj*> trackedPointers;
    for (auto& [_, ptr] : canvasMap)
        trackedPointers.insert(ptr);

    for (t_gobj* y = canvas->gl_list; y; y = y->g_next) {
        if (trackedPointers.find(y) == trackedPointers.end())
            identity.mint(canvas, y, mintPrefix(y).toStdString()); // P3.5 S4: one minter
    }
}

void McpHost::sendMCPReply(const String& replyAddr, mcp::SysList const& values)
{
    if (!mcpBridge)
        return;
    juce::Array<juce::var> args;
    for (int i = 0; i < values.size(); ++i) {
        if (values[i].isNumber)
            args.add(static_cast<double>(values[i].number));
        else
            args.add(values[i].text);
    }
    mcpBridge->sendReply(replyAddr, args);
}

bool McpHost::hasMcpTransaction(t_canvas* cnv) const
{
    juce::ScopedLock sl(session.mcpUndoLock);
    auto it = session.mcpUndoStack.find(cnv);
    return it != session.mcpUndoStack.end() && !it->second.empty();
}

bool McpHost::hasMcpRedoTransaction(t_canvas* cnv) const
{
    juce::ScopedLock sl(session.mcpUndoLock);
    auto it = session.mcpRedoStack.find(cnv);
    return it != session.mcpRedoStack.end() && !it->second.empty();
}

void McpHost::pushMcpTransaction(t_canvas* cnv, const juce::String& canvasName, const juce::OSCMessage& forward, const juce::OSCMessage& inverse, const juce::String& replayAction)
{
    {
        juce::ScopedLock sl(session.mcpUndoLock);
        session.mcpUndoStack[cnv].push_back({ canvasName, forward, inverse, replayAction });
        if (session.mcpUndoStack[cnv].size() > 50) {
            session.mcpUndoStack[cnv].erase(session.mcpUndoStack[cnv].begin());
        }
        session.mcpRedoStack[cnv].clear();
    }
    postMessage([this] {
        for (auto* editor : processor.getEditors()) {
            editor->triggerAsyncUpdate();
        }
    });
}

void McpHost::undoMcpTransaction(t_canvas* cnv)
{
    // McpTransaction has no default ctor (OSCMessage needs an address), so hold
    // the popped transaction by pointer.
    std::unique_ptr<McpTransaction> tx;
    {
        juce::ScopedLock sl(session.mcpUndoLock);
        auto it = session.mcpUndoStack.find(cnv);
        if (it == session.mcpUndoStack.end() || it->second.empty()) return;

        tx = std::make_unique<McpTransaction>(it->second.back());
        it->second.pop_back();
        session.mcpRedoStack[cnv].push_back(*tx);
    }

    // Execute OUTSIDE the lock — the nested batch_atomic must not re-enter the
    // stack map while we hold mcpUndoLock.
    if (mcpBridge && tx) {
        session.isExecutingMcpUndoRedo = true;
        mcpBridge->replayPd(tx->replayAction, tx->inverse);
        session.isExecutingMcpUndoRedo = false;
    }
    postMessage([this] {
        for (auto* editor : processor.getEditors()) {
            editor->triggerAsyncUpdate();
        }
    });
}

void McpHost::redoMcpTransaction(t_canvas* cnv)
{
    std::unique_ptr<McpTransaction> tx;
    {
        juce::ScopedLock sl(session.mcpUndoLock);
        auto it = session.mcpRedoStack.find(cnv);
        if (it == session.mcpRedoStack.end() || it->second.empty()) return;

        tx = std::make_unique<McpTransaction>(it->second.back());
        it->second.pop_back();
        session.mcpUndoStack[cnv].push_back(*tx);
    }

    if (mcpBridge && tx) {
        session.isExecutingMcpUndoRedo = true;
        mcpBridge->replayPd(tx->replayAction, tx->forward);
        session.isExecutingMcpUndoRedo = false;
    }
    postMessage([this] {
        for (auto* editor : processor.getEditors()) {
            editor->triggerAsyncUpdate();
        }
    });
}

void McpHost::clearMcpTransactions(t_canvas* cnv)
{
    {
        juce::ScopedLock sl(session.mcpUndoLock);
        session.mcpUndoStack.erase(cnv);
        session.mcpRedoStack.erase(cnv);
    }
    postMessage([this] {
        for (auto* editor : processor.getEditors()) {
            editor->triggerAsyncUpdate();
        }
    });
}
