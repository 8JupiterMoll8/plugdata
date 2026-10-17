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

McpHost::McpHost(PluginProcessor& p)
    : processor(p)
{
}

McpHost::~McpHost() = default;

void McpHost::createBridge()
{
    auto const mcpEnabled = processor.settingsFile->getProperty<bool>("mcp_bridge_enabled");
    auto const mcpListen = processor.settingsFile->getProperty<int>("mcp_bridge_listen_port");
    auto const mcpSend = processor.settingsFile->getProperty<int>("mcp_bridge_send_port");
    mcpBridge = std::make_unique<MCPBridge>(&processor, mcpListen, mcpSend);
    if (!mcpEnabled) {
        mcpBridge->stop();
    }
}

juce::String McpHost::getMcpBridgeStatus() const
{
    return mcpBridge ? mcpBridge->getStatus() : juce::String("unavailable");
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
        mcpBridge->stop();
        mcpBridge.reset(new MCPBridge(&processor, listen, send));
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
    // Re-read s_vec EVERY sub-block: Pd swaps outlet buffers per DSP tick, so a
    // captured vector goes stale after one block.
    auto* sig = static_cast<t_signal*>(session.mcpTapSig.load(std::memory_order_relaxed));
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

void McpHost::writeRecorderTap(dsp::AudioBlock<float> const& buffer)
{
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

t_canvas* McpHost::getCanvasBySymbol(const String& canvas_symbol)
{
    if (canvas_symbol == "pd-main" || canvas_symbol == "main" || canvas_symbol.isEmpty()) {
        // "main" must ALWAYS resolve to the true top-level patch. The editor's
        // current canvas can be a subpatch OR an abstraction tab the artist has
        // navigated into — automating against that would silently target the
        // voice's internals instead of the patch (phantom "receive->canvas
        // connection failed", census listing internals as top-level, silent
        // recalls). Walk gl_owner all the way up (past abstractions too, unlike
        // canvas_getrootfor which stops at them) so MCP operations always land
        // on the actual root patch.
        for (auto* editor : processor.getEditors()) {
            if (editor) {
                if (auto* cnv = editor->getCurrentCanvas()) {
                    auto* glist = reinterpret_cast<t_glist*>(cnv->patch.getRawPointer());
                    while (glist && glist->gl_owner) glist = glist->gl_owner;
                    if (glist) return reinterpret_cast<t_canvas*>(glist);
                }
            }
        }
    }

    t_canvas* canvas = (t_canvas*)pd_findbyclass(gensym(canvas_symbol.toRawUTF8()), canvas_class);
    if (!canvas && !canvas_symbol.startsWith("pd-")) {
        canvas = (t_canvas*)pd_findbyclass(gensym(String("pd-" + canvas_symbol).toRawUTF8()), canvas_class);
    }
    if (!canvas) {
        canvas = pd_this->pd_canvaslist;
    }
    return canvas;
}

t_canvas* McpHost::getCanvasBySymbolStrict(const String& canvas_symbol)
{
    // 'main' / 'pd-main' / empty resolve exactly like the legacy path (true
    // top-level patch of the focused editor) — those aliases stay valid.
    if (canvas_symbol == "pd-main" || canvas_symbol == "main" || canvas_symbol.isEmpty()) {
        return getCanvasBySymbol(canvas_symbol);
    }
    t_canvas* canvas = (t_canvas*)pd_findbyclass(gensym(canvas_symbol.toRawUTF8()), canvas_class);
    if (!canvas && !canvas_symbol.startsWith("pd-")) {
        canvas = (t_canvas*)pd_findbyclass(gensym(String("pd-" + canvas_symbol).toRawUTF8()), canvas_class);
    }
    if (!canvas && pd_this) {
        for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
            if (c->gl_name) {
                juce::String glName(c->gl_name->s_name);
                if (glName == canvas_symbol || ("pd-" + glName) == canvas_symbol || glName == ("pd-" + canvas_symbol)) {
                    canvas = c;
                    break;
                }
            }
        }
    }
    // NO fallback: unknown names return nullptr so the caller can name the
    // failure instead of mutating the focused/root canvas by accident.
    return canvas;
}

void McpHost::synchroniseCanvases()
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) {
        if (!mm->isThisTheMessageThread()) {
            mm->callAsync([this] { synchroniseCanvases(); });
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
    if (!g) return {};
    for (auto const& [canvasKey, canvasMap] : session.mcpStableObjectMap) {
        for (auto const& [id, ptr] : canvasMap) {
            if (ptr == g) return juce::String(id);
        }
    }
    return {};
}

juce::String McpHost::getOrAdoptStableId(t_canvas* canvas, t_gobj* g)
{
    if (!g) return {};
    auto const existing = getStableId(g);
    if (existing.isNotEmpty()) return existing;
    if (!canvas) return {};

    juce::String canonKey = MCPBridge::canonicalCanvasKey(canvas);
    auto canonStr = canonKey.toStdString();

    int objectIndex = 0;
    for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next, ++objectIndex) {
        if (y_obj == g) {
            t_class* cl = pd_class(&y_obj->g_pd);
            const char* clName = class_getname(cl);
            juce::String className = clName ? juce::String::fromUTF8(clName) : juce::String("unknown");
            juce::String type = className;
            juce::String abstractName;
            if (cl == canvas_class) {
                type = "pd";
                pd::getAbstractionFileName(y_obj, abstractName);
            } else if (cl == garray_class) {
                type = "table";
            }
            juce::String classSrc = abstractName.isNotEmpty() ? abstractName : type;
            juce::String canvasKeySymbol = canonKey;
            if (canvasKeySymbol.startsWith("pd-"))
                canvasKeySymbol = canvasKeySymbol.substring(3);
            juce::String canvasKey;
            for (auto ch : canvasKeySymbol)
                canvasKey << (juce::CharacterFunctions::isLetterOrDigit(ch) || ch == '_'
                                   ? juce::String::charToString(ch)
                                   : juce::String("_"));
            juce::String minted = pd::sanitizeClassNameForTempId(classSrc)
                                      + "_" + canvasKey + "_" + juce::String(objectIndex);

            auto& canvasIdMap = session.mcpStableObjectMap[canonStr];
            canvasIdMap[minted.toStdString()] = y_obj;
            if (session.mcpStableSerialMap.find(y_obj) == session.mcpStableSerialMap.end()) {
                session.mcpStableSerialMap[y_obj] = session.mcpSerialCounter++;
            }
            session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
            return minted;
        }
    }
    return {};
}

t_gobj* McpHost::resolveStableId(const String& canvasName, const String& objectId)
{
    // Identity is keyed by the canvas's CANONICAL key. "main" is a
    // focus-dependent ALIAS, never a stored key — resolve the canvas first and
    // derive its canonical key so both sides of the map agree.
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    auto canvasStr = (canvas ? MCPBridge::canonicalCanvasKey(canvas) : canvasName).toStdString();
    auto idStr = objectId.toStdString();
    
    auto& canvasMap = session.mcpStableObjectMap[canvasStr];
    auto it = canvasMap.find(idStr);
    if (it != canvasMap.end()) {
        if (canvas) {
            // KNOWN GAP (Report F3): this cannot detect a freed object whose
            // address Pd's allocator recycled for a new object on the same
            // canvas. The old "serial" check compared mcpStableSerialMap[ptr]
            // with itself (y_obj == it->second), so it never fired and was
            // removed. A real check needs a serial recorded per stable ID at
            // registration (14 sites); that belongs to the identity service
            // (Plan 3 Phase 5), not a local patch.
            for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
                if (y_obj == it->second)
                    return it->second;
            }
        }
        canvasMap.erase(it);
        // Clean up orphaned serial on eviction
        if (it->second) {
            session.mcpStableSerialMap.erase(it->second);
        }
    }

    // Index-based fallback: "0", "1", "obj_0", "obj_1", etc.
    if (objectId.startsWith("obj_") || objectId.containsOnly("0123456789")) {
        int idx = objectId.startsWith("obj_") ? objectId.substring(4).getIntValue() : objectId.getIntValue();
        if (!canvas && canvasName == "pd-main") canvas = pd_this->pd_canvaslist;
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
    t_canvas* canvas = getCanvasBySymbol(canvasName);
    if (!canvas && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
        canvas = pd_this->pd_canvaslist;
    if (!canvas)
        return;

    auto canvasStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
    auto& canvasMap = session.mcpStableObjectMap[canvasStr];

    // Step A: live pointer set from gl_list (the ground truth).
    std::unordered_set<t_gobj*> livePointers;
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next)
        livePointers.insert(y);

    // Step B: evict entries whose gobj no longer exists on the canvas.
    for (auto it = canvasMap.begin(); it != canvasMap.end();) {
        if (livePointers.find(it->second) == livePointers.end()) {
            session.mcpStableSerialMap.erase(it->second);
            it = canvasMap.erase(it);
            session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }

    // Step C: adopt objects the identity map doesn't track yet (GUI-created).
    std::unordered_set<t_gobj*> trackedPointers;
    for (auto& [_, ptr] : canvasMap)
        trackedPointers.insert(ptr);

    int adoptIdx = 0;
    for (t_gobj* y = canvas->gl_list; y; y = y->g_next) {
        if (trackedPointers.find(y) == trackedPointers.end()) {
            juce::String className = juce::String::fromUTF8(class_getname(pd_class(&y->g_pd))).replace("~", "_t");
            std::string autoId = ("gui_" + className + "_" + juce::String(adoptIdx++)).toStdString();
            while (canvasMap.count(autoId))
                autoId = ("gui_" + className + "_" + juce::String(adoptIdx++)).toStdString();
            canvasMap[autoId] = y;
            session.mcpStableSerialMap[y] = session.mcpSerialCounter++;
            session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void McpHost::sendMCPReply(const String& replyAddr, const SmallArray<pd::Atom>& atoms)
{
    processor.sendMessage("mcp_telemetry_reply", replyAddr.toRawUTF8(), atoms);
    if (mcpBridge) {
        mcpBridge->sendSelectionTelemetry(replyAddr, atoms);
    }
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
    juce::MessageManager::callAsync([this] {
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
        mcpBridge->handlePdDomain(tx->replayAction, tx->inverse);
        session.isExecutingMcpUndoRedo = false;
    }
    juce::MessageManager::callAsync([this] {
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
        mcpBridge->handlePdDomain(tx->replayAction, tx->forward);
        session.isExecutingMcpUndoRedo = false;
    }
    juce::MessageManager::callAsync([this] {
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
    juce::MessageManager::callAsync([this] {
        for (auto* editor : processor.getEditors()) {
            editor->triggerAsyncUpdate();
        }
    });
}
