/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include "MCPBridgeCmdCommon.h"
#include "Mcp/McpRtGuard.h"
#include "Mcp/McpAudit.h"
#include <juce_cryptography/juce_cryptography.h> // SHA-256 of the .pd in the identity sidecar (P3.5 S7)
#include <map>
extern "C" {
#include <m_imp.h> // SYMTABHASHSIZE (P3.4 S7 symbol count)
}


void MCPBridge::mcpCtrlCapture(t_symbol* selector, int argc, t_atom* argv)
{
    if (controlWindow) controlWindow->record(selector, argc, argv);
}

// ═══════════════════════════════════════════════════════════════════════════

MCPBridge::MCPBridge(PluginProcessor* proc, int inPort, int outPort)
    : processor(proc)
    , listenPort(inPort)
    , sendPort(outPort)
    , probeManager(this)
{
    commandTable(); // a bad verb table must fail here, at startup, not on the first message
    sender.setRouter(&v2Router); // P4.1 S3
    v2Done->queue = &sender;
    controlWindow = std::make_unique<McpControlWindow>();
    voiceCaptureBuffer.resize(MAX_VOICE_CAPTURE_SAMPLES, 0.0f);
    bootToken = juce::String(juce::Time::getMillisecondCounter()) + "-"
        + juce::String::toHexString(juce::Random::getSystemRandom().nextInt());

    // Persist the identity sidecar on ANY save — GUI Ctrl+S, Save-As, or the
    // MCP save. Without this a manual save left the sidecar stale and every
    // object added since the last MCP save lost its tempId on reload.
    if (processor) {
        // P3.5 S5: a GUI retype recreates the box (text_setto appends it, so no index rule can follow it): the
        // id moves to the new box. The old pointer is only a key here; moveId does nothing if the box survived.
        processor->onObjectRetyped = [p = processor](t_gobj* from, t_gobj* to) {
            McpHost::PdLockScope lock(*p);
            p->getMcp()->identity.moveId(from, to);
        };
        processor->onCanvasSaved = [p = processor](t_glist* g, juce::String const& path) {
            // P3.5 S5 (L9): plugdata reloads every instance of a saved abstraction right after this hook; leave a
            // tombstone per bound instance so the ids rebind to the recreated boxes.
            auto fileName = juce::File(path).getFileName();
            if (fileName.endsWith(".m~.pd"))
                fileName = fileName.dropLastCharacters(3); // getAbstractionFileName's spelling
            p->getMcp()->tombstoneAbstractionInstances(fileName);
            // Review M5: build the sidecar now (the hook runs inside plugdata's save, under the Pd lock), hash the .pd
            // and write it after the save returned, off the lock.
            juce::var sidecar;
            {
                McpHost::PdLockScope lock(*p);
                sidecar = buildIdentitySidecar(p, reinterpret_cast<t_canvas*>(g));
            }
            juce::MessageManager::callAsync([p, sidecar, path] {
                auto const error = writeIdentitySidecarFile(sidecar, juce::File(path));
                if (error.isNotEmpty())
                    p->logMessage("MCP identity: " + error);
            });
        };
    }

    start();
}

MCPBridge::~MCPBridge()
{
    shutdown();
    {
        std::lock_guard<std::mutex> const l(v2Done->lock); // P4.1 S3: a work token released from now on reports nothing
        v2Done->queue = nullptr;
    }
    // Phase 4: release any still-armed control receiver (the Pd instance is
    // still alive at this point — members destruct before ~PdInstance).
    if (controlWindow && controlWindow->receiver) {
        sys_lock();
        controlWindow->unbindReceiver();
        sys_unlock();
    }
}

void MCPBridge::rebind(int const newListenPort, int const newSendPort)
{
    bool const wasActive = active.load() || v2Server.running();
    stop();
    listenPort = newListenPort;
    sendPort = newSendPort;
    if (wasActive)
        start();
}

void MCPBridge::shutdown()
{
    if (shuttingDown.exchange(true, std::memory_order_acq_rel))
        return;
    stop(); // receiver, command worker, timer
    // The render loop checks isShuttingDown() every block; voice analysis is short. A job that does not end
    // in 10 s is logged and waited for by the pool's own destructor.
    if (!jobPool.removeAllJobs(true, 10000))
        std::fprintf(stderr, "MCP bridge: a background job did not stop within 10 s at shutdown\n");
}

void MCPBridge::postMessage(std::function<void()> fn)
{
    // P4.1 S3: the posted work replies for the request that posted it (e.g. /pd/clear's /pd/cleared, rule 2b).
    juce::MessageManager::callAsync([weak = juce::WeakReference<MCPBridge>(this), fn = std::move(fn), work = mcp::v2::captureWork()] {
        mcp::v2::WorkScope const scope(work); // the request is done only when this hop is
        if (weak.get() != nullptr)
            fn();
    });
}

void MCPBridge::runJob(std::function<void()> job)
{
    if (isShuttingDown())
        return;
    jobPool.addJob([job = std::move(job), work = mcp::v2::captureWork()] {
        mcp::v2::WorkScope const scope(work);
        job();
    });
}

bool MCPBridge::start()
{
    if (active.load()) {
        startV2(); // the v2 slot may have been taken by another bridge of this process at the first start
        return true;
    }

    // Loopback only: other hosts on the LAN must not be able to drive the patch.
    listenSocket = std::make_unique<juce::DatagramSocket>(false);
    bool const recvOk = listenSocket->bindToPort(listenPort, "127.0.0.1")
        && receiver.connectToSocket(*listenSocket);
    if (!recvOk)
        listenSocket.reset();
    sender.setUdpEnabled(recvOk);
    startCommandWorker(); // before any listener: nothing may arrive with no worker (P4.1 S3: v2 needs it without UDP)
    if (recvOk) {
        receiver.addListener(this);
        active.store(true);
        reportedConnected = false;
        reportedLost = false;
        statusMessage = {};
        if (processor) {
            processor->logMessage("MCP bridge listening on UDP 127.0.0.1:" + juce::String(listenPort));
        }
    } else {
        statusMessage = "port " + juce::String(listenPort) + " in use";
        if (processor) {
            processor->logError("MCP bridge: could not bind UDP " + juce::String(listenPort) + " (port in use?)");
        }
    }

    sender.connect("127.0.0.1", sendPort);
    senderHost = "127.0.0.1";
    senderPort = sendPort;

    // P4.1 S2: v2 has its own TCP port and token, so it starts even when the UDP port is taken (L22).
    startV2();
    return active.load();
}

bool MCPBridge::startV2()
{
    if (v2Server.running())
        return true;
    mcp::v2::ServerConfig config;
    config.preferredPort = listenPort; // TCP and UDP port numbers are separate
    config.discoveryPath = mcp::v2::defaultDiscoveryPath();
    config.bridgeVersion = "11.18";
    config.bootToken = bootToken.toStdString();
    config.caps = capabilityList();
    config.caps.push_back("protocol_v2");
    config.log = [proc = processor](std::string const& line) {
        if (proc)
            proc->logMessage(juce::String::fromUTF8(line.c_str())); // a lock-free console queue: any thread
    };
    // P4.1 S3: requests in, sessions in and out, the async deadline clock. All on V2 threads: they only post.
    config.onRequest = [this](int session, mcp::v2::Request request) { return admitV2(session, std::move(request)); };
    config.onSessionOpened = [this](int session, bool telemetry) { v2Router.sessionOpened(session, telemetry); };
    config.onSessionClosed = [this](int session) { v2Router.sessionClosed(session); };
    config.onTick = [this] {
        if (v2Router.hasPending())
            sender.tick();
    };
    v2Router.reopen();
    std::string reason;
    if (v2Server.start(std::move(config), reason))
        return true;
    if (processor)
        processor->logMessage("MCP bridge: no v2 listener (" + juce::String::fromUTF8(reason.c_str()) + "); v1 UDP only");
    return false;
}

void MCPBridge::stopV2()
{
    v2Server.stop(); // sessions get bye{shutting_down}; their pending requests are dropped as they close
    v2Router.close();
}

namespace {
std::int64_t v2NowMs() // the router's clock (ReplyQueue::nowMs is the same steady clock)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

// V2Read thread: the same verb resolution as dispatchRegistered, the schema row, admission, then the identical
// juce::OSCMessage a datagram would give, posted to the one command worker.
std::optional<mcp::v2::RequestAnswer> MCPBridge::admitV2(int session, mcp::v2::Request request)
{
    auto const address = juce::String::fromUTF8(request.verb.c_str());
    auto path = address;
    while (path.startsWith("/"))
        path = path.substring(1);
    auto parts = juce::StringArray::fromTokens(path, "/", "");
    parts.removeEmptyStrings();
    mcp::v2schema::VerbMeta const* meta = nullptr;
    if (!parts.isEmpty()) {
        auto const& table = commandTable();
        auto const domain = parts[0].toStdString();
        auto const pathVerb = parts.size() > 1 ? parts[1].toStdString() : std::string();
        auto const source = table.sourceOf(domain);
        std::string arg0;
        if (source != mcp::VerbSource::Path && !request.args.empty()) {
            auto const& a = request.args.front();
            arg0 = a.type == 's' ? a.s : a.type == 'i' ? std::to_string(a.i) : std::string();
        }
        auto const verb = mcp::resolveVerb(source, pathVerb, arg0);
        if (table.find(domain, verb) != nullptr) {
            meta = mcp::v2schema::findVerb(domain, verb);
            if (!meta && table.hasDomain(domain))
                meta = mcp::v2schema::findVerb(domain, "");
        }
    }
    // The identical message a datagram would give. Built first: an address JUCE refuses is a bad frame, not a crash.
    std::optional<juce::OSCMessage> message;
    try {
        message.emplace(juce::OSCAddressPattern(address));
        for (auto const& a : request.args) {
            if (a.type == 'i')
                message->addArgument(static_cast<juce::int32>(a.i));
            else if (a.type == 'f')
                message->addArgument(a.f);
            else
                message->addArgument(juce::String::fromUTF8(a.s.c_str())); // NUL-free and valid UTF-8 (codec)
        }
    } catch (...) { // juce::OSCFormatError: its what() is generic
        return mcp::v2::RequestAnswer { mcp::v2::encodeSynth(request.id, mcp::v2schema::Outcome::Rejected, mcp::v2::ErrorCode::BadFrame,
            "not a valid OSC message: " + mcp::v2::json::truncateUtf8(request.verb, 128)), false };
    }

    auto admission = v2Router.admit(session, request, meta, v2NowMs());
    if (admission.reply)
        return mcp::v2::RequestAnswer { std::move(*admission.reply), admission.closeSession };
    auto work = std::make_shared<mcp::v2::Work>(admission.origin, [gate = v2Done](mcp::v2::OriginPtr const& o) { gate->done(o); });
    if (!commandWorker.post(Command { std::move(*message), work })) {
        work->disarm(); // answered here instead
        v2Router.drop(admission.origin);
        return mcp::v2::RequestAnswer { mcp::v2::encodeSynth(request.id, mcp::v2schema::Outcome::Rejected, mcp::v2::ErrorCode::ShuttingDown,
            "the bridge is not running commands"), false };
    }
    return std::nullopt;
}

bool MCPBridge::startV2ForTests()
{
    return startV2();
}

void MCPBridge::setReplySinkForTests(std::function<void(juce::OSCMessage const&)> sink)
{
    sender.setSink(std::move(sink));
    sender.setUdpEnabled(true); // the sink stands for the UDP target
    startCommandWorker();
    active.store(true);
}

void MCPBridge::stop()
{
    stopTimer();
    bool const v2WasRunning = v2Server.running();
    stopV2(); // P3.4 S8 order: listeners before the worker
    if (!active.load()) {
        commandWorker.stop(); // P4.1 S3: started for v2 even when the UDP bind failed
        if (v2WasRunning && processor)
            processor->logMessage("MCP bridge stopped");
        return;
    }

    receiver.removeListener(this);
    receiver.disconnect();
    listenSocket.reset();
    commandWorker.stop(); // the running command finishes; queued ones are dropped
    sender.disconnect();
    sender.setUdpEnabled(false);
    active.store(false);
    reportedConnected = false;
    reportedLost = false;
    statusMessage = {};
    if (processor) {
        processor->logMessage("MCP bridge stopped");
    }
}

bool MCPBridge::isConnected() const
{
    return active.load();
}

juce::String MCPBridge::getStatus() const
{
    if (!active.load()) return statusMessage.isNotEmpty() ? ("error: " + statusMessage) : juce::String("disabled");

    auto const last = lastServerActivity.load();
    if (last == 0) return "waiting for server";

    auto const elapsed = juce::Time::getMillisecondCounter() - last;
    if (elapsed > 15000) {
        // Server went quiet. Emit once (lazily — the Advanced panel polls
        // getStatus(), which is where an artist would look).
        if (!reportedLost && processor) {
            processor->logError("MCP server connection lost");
            reportedLost = true;
        }
        return "disconnected";
    }

    return "connected";
}

void MCPBridge::noteServerActivity()
{
    bool const first = (lastServerActivity.load() == 0);
    lastServerActivity.store(juce::Time::getMillisecondCounter());
    if (first && !reportedConnected && processor) {
        processor->logMessage("MCP server connected");
        reportedConnected = true;
    }
    reportedLost = false;
}

juce::String MCPBridge::normalizeCanvas(const juce::String& name)
{
    juce::String s = name.trim();
    if (s.isEmpty() || s == "main") return "pd-main";
    // PRD Phase 3.1: root-file tab symbols are the raw file name (the bound
    // gl_name, e.g. "phase3_roundtrip_test.pd") — pass through unmangled so
    // canvas resolution and identity map keys agree.
    if (s.endsWith(".pd")) return s;
    while (s.startsWith("pd-")) s = s.substring(3);
    if (s.isEmpty()) return "pd-main";
    return "pd-" + s;
}

juce::String MCPBridge::canonicalCanvasKey(t_canvas* cnv)
{
    if (!cnv) return {};

    // Named subcanvases (gl_owner != nullptr) keep "pd-<gl_name>"
    if (cnv->gl_owner != nullptr) {
        juce::String name = cnv->gl_name ? juce::String::fromUTF8(cnv->gl_name->s_name) : juce::String();
        if (name.startsWith("pd-")) return name;
        if (name.isNotEmpty()) return "pd-" + name;
        return juce::String::formatted("pd-%p", cnv);
    }

    // Root canvas: bound file name (its gl_name, e.g. "X.pd") when bound
    if (cnv->gl_name && cnv->gl_name->s_name && cnv->gl_name->s_name[0] != '\0') {
        juce::String name = juce::String::fromUTF8(cnv->gl_name->s_name);
        if (name != "main" && name != "pd-main")
            return name;
    }

    // Fallback pointer-derived stable key
    return juce::String::formatted("canvas-%p", cnv);
}


// Compute native diagnostic graph facts under sys_lock(). Shared by standalone
// /pd/diagnose and inline conditional X-ray in batch_atomic.
juce::String MCPBridge::computeDiagnoseFacts(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return {};

    // 1. Objects: index, stable tempId (fallback: class#idx)
    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto const* canvasIds = processor->getMcp()->identity.findCanvas(cnv);
    if (canvasIds)
        for (auto& [tid, ptr] : *canvasIds)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        classNames.push_back(juce::String(cName ? cName : ""));
        names.push_back(ptrToId.count(y)
            ? ptrToId[y]
            : (juce::String(cName ? cName : "unknown") + "#" + juce::String(idx)));
    }

    // 2. One linetraverser walk: signal edges + ALL wired inlets +
    //    rate-mismatched wires (PRD §2.2 `mismatched`)
    std::vector<std::pair<int, int>> sigEdges;         // (srcIdx, destIdx), signal only
    std::set<std::pair<int, int>> anyInletWired;       // (destIdx, inletNo), any type
    std::set<std::pair<int, int>> anyOutletWired;      // (srcIdx, outletNo), any type
    struct MismatchedWire { int src, srcOut, dest, destIn; bool srcIsSig; };
    std::vector<MismatchedWire> mismatchedWires;
    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        int si = -1, di = -1;
        for (int i = 0; i < (int)objs.size(); ++i) {
            if (objs[i] == &lt.tr_ob->ob_g) si = i;
            if (objs[i] == &lt.tr_ob2->ob_g) di = i;
        }
        if (si < 0 || di < 0) continue;
        anyInletWired.insert({ di, lt.tr_inno });
        anyOutletWired.insert({ si, lt.tr_outno });
        bool srcIsSig = (lt.tr_outlet->o_sym == gensym("signal"));
        bool destIsSig = obj_issignalinlet(lt.tr_ob2, lt.tr_inno);
        // Control float → signal PARAMETER inlet (inlet >= 1) is the documented
        // Pd pattern for setting DSP params (e.g. hsl → [lop~] inlet 1 cutoff):
        // Pd stores the scalar in the signal inlet. Only flag ctl→sig on
        // inlet 0 (usually a mistaken audio input) and all sig→ctl wires.
        bool controlToParamInlet = !srcIsSig && destIsSig && lt.tr_inno >= 1;
        if (srcIsSig != destIsSig && !controlToParamInlet)
            mismatchedWires.push_back({ si, lt.tr_outno, di, lt.tr_inno, srcIsSig });
        if (srcIsSig)
            sigEdges.push_back({ si, di });
    }

    // 3. DSP cycles: DFS over signal edges (white/gray/black)
    std::vector<std::vector<int>> adj(objs.size());
    for (auto& e : sigEdges) adj[e.first].push_back(e.second);
    std::vector<int> color(objs.size(), 0);
    std::vector<int> path;
    std::vector<std::vector<int>> cycles;
    std::function<void(int)> dfs = [&](int u) {
        color[u] = 1;
        path.push_back(u);
        for (int v : adj[u]) {
            if (color[v] == 1) {
                std::vector<int> cyc;
                for (int k = (int)path.size() - 1; k >= 0; --k) {
                    cyc.push_back(path[k]);
                    if (path[k] == v) break;
                }
                std::reverse(cyc.begin(), cyc.end());
                cycles.push_back(cyc);
            } else if (color[v] == 0) {
                dfs(v);
            }
        }
        path.pop_back();
        color[u] = 2;
    };
    for (int i = 0; i < (int)objs.size(); ++i)
        if (color[i] == 0) dfs(i);

    // 4. Main-signal-inlet check + zeroed-VCA check
    std::set<std::pair<int, int>> mainSigUnwired; // (idx, 0)
    std::set<std::pair<int, int>> zeroedVcas;     // (idx, 1)
    idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        t_object* ob = pd::Interface::checkObject(y);
        bool hasDspMethod = (ob != nullptr && zgetfn(&y->g_pd, gensym("dsp")) != nullptr);
        bool mainWired = anyInletWired.count({ idx, 0 }) > 0;
        if (hasDspMethod && ob && obj_issignalinlet(ob, 0) && !mainWired)
            mainSigUnwired.insert({ idx, 0 });

        if (!ob) continue;
        char* tb = nullptr; int tsz = 0;
        mcpGetObjectText(ob, &tb, &tsz);
        juce::String text = (tb && tsz > 0)
            ? juce::String::fromUTF8(tb, static_cast<size_t>(tsz)).trim()
            : juce::String();
        if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));

        juce::String firstTok = text.upToFirstOccurrenceOf(" ", false, false);
        if (firstTok == "*~") {
            // Zeroed-VCA detection: multiplier is silent if it has NO float arg
            // and no inlet-1 wire (bare [*~]), OR an explicit arg of exactly 0
            // ([*~ 0] — a numeric arg parses as zero and locks the gain to silence).
            bool hasNonZeroFloatArg = false;
            bool hasZeroFloatArg = false;
            juce::String argTok = text
                .fromFirstOccurrenceOf(" ", false, false)
                .upToFirstOccurrenceOf(" ", false, false)
                .trim();
            if (argTok.isNotEmpty()) {
                char* endPtr = nullptr;
                const char* raw = argTok.toRawUTF8();
                double argVal = std::strtod(raw, &endPtr);
                if (endPtr != raw) { // numeric atom (symbol args like '*~' don't count)
                    if (argVal == 0.0) hasZeroFloatArg = true;
                    else hasNonZeroFloatArg = true;
                }
            }
            bool inlet1Wired = anyInletWired.count({ idx, 1 }) > 0;
            if ((!hasNonZeroFloatArg && !inlet1Wired) || (hasZeroFloatArg && !inlet1Wired))
                zeroedVcas.insert({ idx, 1 });
        }
    }

    // 4b. Structural completeness: orphans (zero wires on both sides) + dead-ends
    //     (inlet wired, output discarded). Wireless classes connect via send/receive
    //     SYMBOLS (no wire), GUI objects connect via their send/receive PROPERTIES,
    //     and comment/text/inlet/outlet are never wireable — all excluded, so the
    //     check fires only on a genuinely-dead DSP/control object (a forgotten wire),
    //     never on a legitimate wireless or cosmetic one.
    auto isWirelessOrCosmetic = [](const juce::String& c) {
        static const char* skip[] = {
            // wireless (symbol-addressed, no wire needed)
            "s", "r", "send", "receive", "s~", "r~", "send~", "receive~", "throw~", "catch~",
            // cosmetic / structural (never wireable)
            "comment", "text", "cnv", "canvas", "pd", "graph",
            // subpatch boundary ports
            "inlet", "inlet~", "outlet", "outlet~",
            // GUI (connect via send/receive properties, not wires)
            "bng", "tgl", "hsl", "vsl", "hradio", "vradio", "numbox", "floatatom", "symbolatom", "vu"
        };
        for (auto* k : skip) if (c == k) return true;
        return false;
    };
    auto isSink = [](const juce::String& c) {
        return c == "dac~" || c == "print" || c == "outlet" || c == "outlet~";
    };
    std::set<int> orphans;
    std::set<int> deadEnds;
    for (int i = 0; i < (int)objs.size(); ++i) {
        if (isWirelessOrCosmetic(classNames[(size_t)i])) continue;
        // Container (inline [pd] subpatch OR file-backed abstraction): a 0-port
        // container can still be alive via an internal [loadbang]/[metro]/[r], and
        // we cannot see inside from here — so NEVER flag a container as an orphan.
        // canvas_class covers BOTH; class-name strings miss abstractions, whose
        // class name is the abstraction's own name (not "pd").
        if (pd_class(&objs[(size_t)i]->g_pd) == canvas_class) continue;
        t_object* ob = pd::Interface::checkObject(objs[(size_t)i]);
        if (!ob) continue;
        int nin = obj_ninlets(ob), nout = obj_noutlets(ob);
        if (nin + nout <= 0) continue;
        bool hasIn = false, hasOut = false;
        for (auto& p : anyInletWired) if (p.first == i) { hasIn = true; break; }
        for (auto& p : anyOutletWired) if (p.first == i) { hasOut = true; break; }
        if (!hasIn && !hasOut) orphans.insert(i);
        else if (hasIn && !hasOut && nout > 0 && !isSink(classNames[(size_t)i])) deadEnds.insert(i);
    }

    // 5. Build JSON — cycles, unscheduled, dangling, zeroed
    auto nameOf = [&](int i) { return jsonEscape((i >= 0 && i < (int)names.size()) ? names[(size_t)i] : juce::String("?")); };
    juce::String json = "{\"canvas\":\"" + jsonEscape(canvasName) + "\",\"objectCount\":" + juce::String(objs.size());
    json += ",\"dsp_cycles\":[";
    for (size_t c = 0; c < cycles.size(); ++c) {
        if (c > 0) json += ",";
        json += "[";
        for (size_t k = 0; k < cycles[c].size(); ++k) {
            if (k > 0) json += ",";
            json += "\"" + nameOf(cycles[c][k]) + "\"";
        }
        json += "]";
    }
    json += "],\"unscheduled\":[";
    {
        std::set<int> unscheduled;
        for (auto& cyc : cycles) for (int m : cyc) unscheduled.insert(m);
        bool first = true;
        for (int m : unscheduled) {
            if (!first) json += ",";
            json += "\"" + nameOf(m) + "\"";
            first = false;
        }
    }
    json += "],\"dangling_main_sig\":[";
    {
        bool first = true;
        for (auto& z : mainSigUnwired) {
            if (!first) json += ",";
            json += "\"" + nameOf(z.first) + "\"";
            first = false;
        }
    }
    json += "],\"zeroed_vcgs\":[";
    {
        bool first = true;
        for (auto& z : zeroedVcas) {
            if (!first) json += ",";
            json += "{\"tempId\":\"" + nameOf(z.first) + "\",\"inlet\":" + juce::String(z.second) + "}";
            first = false;
        }
    }
    json += "],\"mismatched\":[";
    {
        bool first = true;
        for (auto& w : mismatchedWires) {
            if (!first) json += ",";
            json += "{\"srcId\":\"" + nameOf(w.src) + "\",\"srcOut\":" + juce::String(w.srcOut)
                  + ",\"destId\":\"" + nameOf(w.dest) + "\",\"destIn\":" + juce::String(w.destIn)
                  + ",\"dir\":\"" + (w.srcIsSig ? "sig->ctl" : "ctl->sig") + "\"}";
            first = false;
        }
    }
    json += "],\"orphans\":[";
    {
        bool first = true;
        for (int o : orphans) {
            if (!first) json += ",";
            json += "\"" + nameOf(o) + "\"";
            first = false;
        }
    }
    json += "],\"dead_ends\":[";
    {
        bool first = true;
        for (int o : deadEnds) {
            if (!first) json += ",";
            json += "\"" + nameOf(o) + "\"";
            first = false;
        }
    }
    json += "]}";
    return json;
}

MCPBridge::MasterMeterResult MCPBridge::computeMasterMeter(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    MasterMeterResult res;
    if (!processor || !cnv) return res;

    // Search for master sinks on this canvas in priority order:
    // 1. [dac~]
    // 2. [throw~]
    // 3. [catch~]
    // 4. [out~]
    std::vector<t_object*> dacObjs;
    std::vector<t_object*> throwObjs;
    std::vector<t_object*> catchObjs;
    std::vector<t_object*> outObjs;

    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (!y) continue;
        t_object* ob = pd::Interface::checkObject(y);
        if (!ob) continue;
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        if (!cName) continue;
        juce::String cStr(cName);
        if (cStr == "dac~") dacObjs.push_back(ob);
        else if (cStr == "throw~") throwObjs.push_back(ob);
        else if (cStr == "catch~") catchObjs.push_back(ob);
        else if (cStr == "out~") outObjs.push_back(ob);
    }

    std::vector<t_object*> targetSinks;
    bool isOutgoingCatch = false;
    if (!dacObjs.empty()) {
        targetSinks = dacObjs;
        res.masterType = "dac~";
        res.masterFound = true;
    } else if (!throwObjs.empty()) {
        targetSinks = throwObjs;
        res.masterType = "throw~";
        res.masterFound = true;
    } else if (!catchObjs.empty()) {
        targetSinks = catchObjs;
        res.masterType = "catch~";
        res.masterFound = true;
        isOutgoingCatch = true;
    } else if (!outObjs.empty()) {
        targetSinks = outObjs;
        res.masterType = "out~";
        res.masterFound = true;
    } else {
        res.masterFound = false;
        res.masterType = "none";
        return res;
    }

    std::set<t_object*> sinkSet(targetSinks.begin(), targetSinks.end());

    // Traverse all wires and measure signal on connections to/from the target sinks
    float maxPeak = 0.0f;
    double totalSumSq = 0.0;
    int64_t totalSamples = 0;

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        bool match = false;
        if (isOutgoingCatch) {
            if (sinkSet.count(lt.tr_ob) && lt.tr_outno == 0) {
                match = true;
            }
        } else {
            if (sinkSet.count(lt.tr_ob2)) {
                match = true;
            }
        }

        if (!match) continue;

        t_signal* sig = outconnect_get_signal(oc);
        if (sig && sig->s_vec && sig->s_n > 0) {
            for (int i = 0; i < sig->s_n; ++i) {
                float s = sig->s_vec[i];
                float abs_s = std::abs(s);
                totalSumSq += (double)(s * s);
                if (abs_s > maxPeak) maxPeak = abs_s;
            }
            totalSamples += sig->s_n;
        }
    }

    float rms = (totalSamples > 0) ? (float)std::sqrt(totalSumSq / (double)totalSamples) : 0.0f;
    res.rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    res.peakDb = (maxPeak > 1e-7f) ? (20.0f * std::log10(maxPeak)) : -100.0f;

    return res;
}

juce::String MCPBridge::computeSignalTrace(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return "{\"error\":\"canvas not available\"}";

    // 1. Map objects and tempIds
    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto const* canvasIds = processor->getMcp()->identity.findCanvas(cnv);
    if (canvasIds)
        for (auto& [tid, ptr] : *canvasIds)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        juce::String clStr = cName ? juce::String(cName) : juce::String("unknown");
        classNames.push_back(clStr);
        names.push_back(ptrToId.count(y)
            ? ptrToId[y]
            : (clStr + "#" + juce::String(idx)));
    }

    int numObjs = (int)objs.size();
    std::vector<float> maxInPeak(numObjs, 0.0f);
    std::vector<float> maxOutPeak(numObjs, 0.0f);
    std::vector<int> incomingSigWires(numObjs, 0);
    std::vector<int> outgoingSigWires(numObjs, 0);
    std::vector<std::unordered_map<int, float>> inletPeaks(numObjs);
    std::vector<std::unordered_map<int, std::vector<int>>> inletSources(numObjs);
    // CONTROL wires (non-signal): inlet -> upstream object indices. Needed to
    // name WHY a silent envelope never triggered (e.g. [sel 1] on the gate path
    // never matches the note velocity — the classic silent VCA).
    std::vector<std::unordered_map<int, std::vector<int>>> controlSources(numObjs);

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        int si = -1, di = -1;
        for (int i = 0; i < numObjs; ++i) {
            if (objs[i] == &lt.tr_ob->ob_g) si = i;
            if (objs[i] == &lt.tr_ob2->ob_g) di = i;
        }
        if (si < 0 || di < 0) continue;

        bool isSig = (lt.tr_outlet->o_sym == gensym("signal"));
        if (!isSig) {
            controlSources[di][lt.tr_inno].push_back(si);
            continue;
        }

        float wirePeak = 0.0f;
        t_signal* sig = outconnect_get_signal(oc);
        if (sig && sig->s_vec && sig->s_n > 0) {
            for (int k = 0; k < sig->s_n; ++k) {
                float val = std::abs(sig->s_vec[k]);
                if (val > wirePeak) wirePeak = val;
            }
        }

        outgoingSigWires[si]++;
        if (wirePeak > maxOutPeak[si]) maxOutPeak[si] = wirePeak;

        incomingSigWires[di]++;
        if (wirePeak > maxInPeak[di]) maxInPeak[di] = wirePeak;
        inletPeaks[di][lt.tr_inno] = std::max(inletPeaks[di][lt.tr_inno], wirePeak);
        inletSources[di][lt.tr_inno].push_back(si);
    }

    constexpr float ALIVE_THRESH = 1e-4f; // -80 dBFS
    std::vector<juce::String> liveSources;
    std::vector<juce::String> silentGenerators;
    std::vector<juce::String> silentEnvelopes;
    float loudestLivePeak = 0.0f; // peak of the loudest live source — real "input" level

    int dacIndex = -1;
    for (int i = 0; i < numObjs; ++i) {
        const juce::String& c = classNames[i];
        if (c == "dac~") dacIndex = i;

        bool isGen = (c == "osc~" || c == "phasor~" || c == "noise~" || c == "sig~" ||
                      c == "tabread4~" || c == "tabread~" || c == "tabplay~" || c == "adc~");
        bool isEnv = (c == "vline~" || c == "line~" || c == "adsr~" || c == "envgen~");

        if (isGen) {
            if (maxOutPeak[i] >= ALIVE_THRESH) {
                liveSources.push_back(names[i]);
                // [sig~] is a DC/control generator — its "peak" is a constant
                // (e.g. [sig~ 700] -> 700), not an audio level. Counting it as
                // the loudest live source reported an impossible +56.9 dBFS
                // "input level" (20*log10(700)) that contradicted /meter/master
                // (-7.1 dBFS). It still counts as a live source for path detection.
                if (c != "sig~")
                    loudestLivePeak = std::max(loudestLivePeak, maxOutPeak[i]);
            } else if (outgoingSigWires[i] > 0) {
                silentGenerators.push_back(names[i]);
            }
        } else if (isEnv) {
            if (maxOutPeak[i] < ALIVE_THRESH && outgoingSigWires[i] > 0) {
                silentEnvelopes.push_back(names[i]);
            }
        } else if (incomingSigWires[i] == 0 && outgoingSigWires[i] > 0 && maxOutPeak[i] >= ALIVE_THRESH) {
            liveSources.push_back(names[i]);
            loudestLivePeak = std::max(loudestLivePeak, maxOutPeak[i]);
        }
    }

    // ── Cause naming for silent envelopes ───────────────────────────────────
    // The harness delivers the note gate as VELOCITY (1..127; 0 = release) —
    // see midi-gate.ts sendNote(). A [sel N] with N != velocity on the gate
    // path NEVER matches → no bang → no envelope → silent VCA. Walk the control
    // wires upstream from a silent envelope; if a numeric [sel] AND a gate
    // receiver share that chain, name the trap instead of leaving the generic
    // "envelope did not trigger" verdict.
    auto objTextOf = [&](int i) -> juce::String {
        t_object* o = pd::Interface::checkObject(objs[i]);
        if (!o) return {};
        char* text = nullptr; int len = 0;
        mcpGetObjectText(o, &text, &len);
        if (!text || len <= 0) return {};
        juce::String s = juce::String::fromUTF8(text, len);
        freebytes(text, len);
        return s;
    };
    auto gateSelUpstreamNote = [&](int envIndex) -> juce::String {
        std::vector<int> frontier{ envIndex };
        std::unordered_set<int> seen{ envIndex };
        int selIdx = -1, gateIdx = -1;
        for (int depth = 0; depth < 6 && !frontier.empty() && (selIdx < 0 || gateIdx < 0); ++depth) {
            std::vector<int> next;
            for (int idx : frontier) {
                for (auto& [inletNo, srcs] : controlSources[idx]) {
                    juce::ignoreUnused(inletNo);
                    for (int s : srcs) {
                        if (!seen.insert(s).second) continue;
                        next.push_back(s);
                        const juce::String& sc = classNames[s];
                        // NOTE: Pd's raw class name for [sel] is "select" ("sel" is
                        // only the creator alias — the census normalizes it for
                        // display, class_getname does not).
                        if (selIdx < 0 && (sc == "select" || sc == "sel")) {
                            juce::String t = objTextOf(s);
                            juce::String after = t.startsWith("select") ? t.substring(6).trim()
                                                                       : t.substring(3).trim();
                            if (after.isNotEmpty() && after.containsOnly("0123456789.-+ "))
                                selIdx = s;
                        } else if (gateIdx < 0 && (sc == "r" || sc == "receive")) {
                            juce::String nm = objTextOf(s).fromFirstOccurrenceOf(" ", false, true).trim();
                            if (nm.startsWith("gate")) gateIdx = s;
                        }
                    }
                }
            }
            frontier = next;
        }
        if (selIdx >= 0 && gateIdx >= 0) {
            // ASCII only: non-ASCII in a reply string is mangled to mojibake on
            // the C++ -> OSC -> TS path (an em-dash arrives as 'â').
            return " - cause: [" + objTextOf(selIdx).trim() + "] ('" + names[selIdx] + "') filters the gate; the harness sends the note VELOCITY (1..127, e.g. 100), so only an exact match passes ([sel 1] never fires). Trigger on any non-zero ([t b] / [> 0]).";
        }
        return {};
    };

    int chokeIndex = -1;
    juce::String chokeReason;

    for (int i = 0; i < numObjs; ++i) {
        const juce::String& c = classNames[i];
        // dac~/out~ are sinks; throw~/send~ are bus SENDERS with no signal
        // outlet — their signal continues at the matching catch~/receive~.
        // Measuring a bus sender's outlet reads silence, so an [s~] with live
        // input and an (empty) outlet was falsely reported as a choke on every
        // working send~. Skip them; a genuinely dangling send~ (no matching
        // receiver) still surfaces via the "never reaches dac~" fallback below.
        if (c == "dac~" || c == "throw~" || c == "out~" || c == "send~" || c == "s~") continue;

        if (maxInPeak[i] >= ALIVE_THRESH && maxOutPeak[i] < ALIVE_THRESH) {
            chokeIndex = i;
            if (c == "*~") {
                float inlet1Signal = inletPeaks[i].count(1) ? inletPeaks[i][1] : 0.0f;
                if (inletSources[i].count(1) && !inletSources[i][1].empty()) {
                    int srcI = inletSources[i][1][0];
                    if (inlet1Signal < ALIVE_THRESH) {
                        chokeReason = "multiplier control inlet 1 is zero from envelope '" + names[srcI] + "' (envelope did not trigger or [r gate] missing)"
                                    + gateSelUpstreamNote(srcI);
                    } else {
                        chokeReason = "multiplier output is silent despite control signal on inlet 1";
                    }
                } else {
                    chokeReason = "multiplier control inlet 1 is zero or disconnected (silent VCA)";
                }
            } else if (c == "vcf~" || c == "lop~" || c == "hip~" || c == "bp~") {
                chokeReason = "filter attenuated signal to silence (cutoff may be at 0 Hz)";
            } else {
                float inDb = 20.0f * std::log10(maxInPeak[i] + 1e-7f);
                float outDb = 20.0f * std::log10(maxOutPeak[i] + 1e-7f);
                chokeReason = "signal died at this object (input " + juce::String(inDb, 1) + " dBFS, output " + juce::String(outDb, 1) + " dBFS)";
            }
            break;
        }
    }

    if (chokeIndex < 0) {
        if (!silentGenerators.empty() && liveSources.empty()) {
            chokeReason = "sound generator '" + silentGenerators[0] + "' is silent (check frequency or pitch receiver)";
            for (int i = 0; i < numObjs; ++i) {
                if (names[i] == silentGenerators[0]) { chokeIndex = i; break; }
            }
        } else if (!silentEnvelopes.empty() && liveSources.empty()) {
            chokeReason = "envelope generator '" + silentEnvelopes[0] + "' output is zero (envelope did not trigger)";
            for (int i = 0; i < numObjs; ++i) {
                if (names[i] == silentEnvelopes[0]) {
                    chokeIndex = i;
                    chokeReason += gateSelUpstreamNote(i);
                    break;
                }
            }
        } else if (!liveSources.empty()) {
            if (dacIndex >= 0 && maxInPeak[dacIndex] < ALIVE_THRESH) {
                chokeReason = "signal generated by '" + liveSources[0] + "' never reaches dac~ (broken signal path)";
                chokeIndex = dacIndex;
            } else {
                // Healthy: a live source exists AND the signal is reaching the
                // master sink. Previously this branch emitted the alarming
                // "sound generated but not reaching master sink" reason even
                // though the dac~ was fed — a false choke report.
                chokeReason = "signal reaches master sink";
            }
        } else {
            chokeReason = "no active audio sources found on canvas";
        }
    }

    juce::String chokeName = (chokeIndex >= 0 && chokeIndex < numObjs) ? names[chokeIndex] : "none";
    juce::String chokeClass = (chokeIndex >= 0 && chokeIndex < numObjs) ? classNames[chokeIndex] : "none";
    // Report real levels. When there is no choke point, fall back to the
    // loudest live source (input) and the peak arriving at dac~ (output) so
    // the trace agrees with /meter/master instead of a hardcoded -100 dBFS.
    float inDb = -100.0f;
    float outDb = -100.0f;
    if (chokeIndex >= 0 && chokeIndex < numObjs) {
        inDb = maxInPeak[chokeIndex] > 1e-7f ? (20.0f * std::log10(maxInPeak[chokeIndex])) : -100.0f;
        outDb = maxOutPeak[chokeIndex] > 1e-7f ? (20.0f * std::log10(maxOutPeak[chokeIndex])) : -100.0f;
    } else if (dacIndex >= 0 && dacIndex < numObjs) {
        inDb = loudestLivePeak > 1e-7f ? (20.0f * std::log10(loudestLivePeak)) : -100.0f;
        outDb = maxInPeak[dacIndex] > 1e-7f ? (20.0f * std::log10(maxInPeak[dacIndex])) : -100.0f;
    }

    juce::String json = "{";
    json += "\"canvas\":\"" + jsonEscape(canvasName) + "\",";
    json += "\"chokePoint\":\"" + jsonEscape(chokeName) + "\",";
    json += "\"class\":\"" + jsonEscape(chokeClass) + "\",";
    json += "\"inputLevelDb\":" + juce::String(inDb, 1) + ",";
    json += "\"outputLevelDb\":" + juce::String(outDb, 1) + ",";
    json += "\"reason\":\"" + jsonEscape(chokeReason) + "\",";
    json += "\"liveSources\":[";
    for (size_t i = 0; i < liveSources.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + jsonEscape(liveSources[i]) + "\"";
    }
    json += "],\"silentEnvelopes\":[";
    for (size_t i = 0; i < silentEnvelopes.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + jsonEscape(silentEnvelopes[i]) + "\"";
    }
    json += "]";

    // ── STAGES: the measured staircase along the path feeding dac~ ──────────
    // Every value is ALREADY measured above (maxOutPeak/maxInPeak); this only
    // orders the nodes along the signal path (source → dac~) so silence has a
    // LOCATION, not just a choke: "osc -12 → filt -14 → vca -100 ⛔ → dac -100".
    // Bounded to 40 rows; loops terminate via the visited set.
    std::vector<int> stageOrder;
    if (dacIndex >= 0 && dacIndex < numObjs) {
        std::vector<bool> seen(numObjs, false);
        std::vector<int> stack{dacIndex};
        seen[dacIndex] = true;
        while (!stack.empty() && (int)stageOrder.size() < 40) {
            int i = stack.back(); stack.pop_back();
            stageOrder.push_back(i);
            for (auto& kv : inletSources[i])
                for (int s : kv.second)
                    if (s >= 0 && s < numObjs && !seen[s]) { seen[s] = true; stack.push_back(s); }
        }
        // DFS post-order reversed = source → dac order (a DAG topo order).
        std::reverse(stageOrder.begin(), stageOrder.end());
    }
    json += ",\"stages\":[";
    for (size_t k = 0; k < stageOrder.size(); ++k) {
        int i = stageOrder[k];
        if (k > 0) json += ",";
        float inDbS = maxInPeak[i] > 1e-7f ? (20.0f * std::log10(maxInPeak[i])) : -100.0f;
        bool sink = (classNames[i] == "dac~" || classNames[i] == "out~");
        float outDbS = sink ? inDbS : (maxOutPeak[i] > 1e-7f ? (20.0f * std::log10(maxOutPeak[i])) : -100.0f);
        json += "{\"tempId\":\"" + jsonEscape(names[i]) + "\",\"class\":\"" + jsonEscape(classNames[i]) + "\",";
        json += "\"inDb\":" + juce::String(inDbS, 1) + ",\"outDb\":" + juce::String(outDbS, 1);
        if (i == chokeIndex && chokeReason.isNotEmpty())
            json += ",\"note\":\"" + jsonEscape(chokeReason) + "\"";
        json += "}";
    }
    json += "],\"stagesTruncated\":" + juce::String((int)stageOrder.size() >= 40 ? "true" : "false");
    json += "}";

    return json;
}

juce::String MCPBridge::computeClusters(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return "{\"clusters\":[]}";

    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, int> ptrToIdx;

    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto const* canvasIds = processor->getMcp()->identity.findCanvas(cnv);
    if (canvasIds)
        for (auto& [tid, ptr] : *canvasIds)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        ptrToIdx[y] = static_cast<int>(objs.size());
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        juce::String clStr = cName ? juce::String::fromUTF8(cName) : juce::String("unknown");
        classNames.push_back(clStr);
        names.push_back(ptrToId.count(y) ? ptrToId[y] : (clStr + "#" + juce::String(idx)));
    }

    int n = static_cast<int>(objs.size());
    if (n == 0) return "{\"clusters\":[]}";

    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    auto findRoot = [&](int a) {
        while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
        return a;
    };
    auto unite = [&](int a, int b) { int ra = findRoot(a), rb = findRoot(b); if (ra != rb) parent[ra] = rb; };

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
        auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
        if (si != ptrToIdx.end() && di != ptrToIdx.end()) unite(si->second, di->second);
    }

    std::unordered_map<int, std::vector<int>> groups;
    for (int i = 0; i < n; ++i) groups[findRoot(i)].push_back(i);

    juce::String json = "{\"clusters\":[";
    bool firstC = true;
    int cid = 0;
    for (auto& kv : groups) {
        auto& members = kv.second;
        bool hasSignal = false, hasBus = false;
        for (int m : members) {
            const juce::String& c = classNames[m];
            if (c.endsWithChar('~')) hasSignal = true;
            if (c == "catch~" || c == "dac~" || c == "out~") hasBus = true;
        }
        juce::String kind = hasBus ? "bus" : (hasSignal ? "signal" : "control");
        if (!firstC) json += ",";
        firstC = false;
        json += "{\"id\":\"c" + juce::String(++cid) + "\",\"kind\":\"" + jsonEscape(kind) + "\",\"objects\":[";
        for (size_t mi = 0; mi < members.size(); ++mi) {
            if (mi > 0) json += ",";
            json += "\"" + jsonEscape(names[members[mi]]) + "\"";
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

// ── /pd/perf — CPU / DSP cost introspection (PRD 1.2) ───────────────────
// Live total host load (statusbar CPU meter) + a per-subpatch cost ESTIMATE
// from a weighted census of DSP objects. Weights are heuristic (kernel
// classes cost more than a bare *~), NOT measured block-time. Read-only.
namespace {

double mcpDspWeight(const juce::String& c)
{
    if (c == "fft~" || c == "ifft~" || c == "rfft~" || c == "rifft~" ||
        c == "conv~" || c == "convolve~" || c == "partconv~") return 8.0;
    if (c == "rev2~" || c == "rev3~" || c == "freeverb~" || c == "reverb~" ||
        c == "comb~" || c == "rev~") return 6.0;
    if (c == "delwrite~" || c == "delread~" || c == "delread4~" || c == "vd~" ||
        c == "tabread4~" || c == "tabread~" || c == "tabplay~" || c == "writesf~") return 3.0;
    if (c == "block~" || c == "switch~") return 2.0;
    return 1.0;
}

struct PerfNode {
    juce::String name;
    double weight = 0.0;
    int signalObjects = 0;
    std::map<juce::String, int> heavies;
};

// Sum ALL descendants of a canvas into weight/count/heavies with NO per-canvas
// output. Used to treat an abstraction instance as a single cost unit in the
// canvas that hosts it (so we don't leak rev3~.pd internals and the hosting
// subpatch shows a real weight).
double mcpSubtreeWeight(t_canvas* cnv, std::unordered_set<t_canvas*>& visited,
                        int depth, int& sig, std::map<juce::String, int>& heavies)
{
    if (!cnv || depth > 32 || visited.count(cnv)) return 0.0;
    visited.insert(cnv);

    double total = 0.0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        t_class* cl = pd_class(&y->g_pd);
        if (cl == canvas_class) {
            total += mcpSubtreeWeight(reinterpret_cast<t_canvas*>(y), visited, depth + 1, sig, heavies);
            continue;
        }
        if (cl == clone_class) {
            int n = clone_get_n(y);
            for (int i = 0; i < n; ++i) {
                auto* inst = clone_get_instance(y, i);
                if (inst) total += mcpSubtreeWeight(reinterpret_cast<t_canvas*>(inst), visited, depth + 1, sig, heavies);
            }
            continue;
        }
        if (!zgetfn(&y->g_pd, gensym("dsp"))) continue;
        juce::String c = (cl && class_getname(cl))
            ? juce::String::fromUTF8(class_getname(cl)) : juce::String("~");
        double w = mcpDspWeight(c);
        total += w;
        sig += 1;
        if (w >= 3.0) heavies[c] += 1;
    }
    return total;
}

void mcpWalkPerf(t_canvas* cnv, const juce::String& path,
                 std::vector<PerfNode>& out,
                 std::unordered_set<t_canvas*>& visited, int depth)
{
    if (!cnv || depth > 24 || visited.count(cnv)) return;
    visited.insert(cnv);

    PerfNode node;
    node.name = path;

    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        t_class* cl = pd_class(&y->g_pd);
        if (cl == canvas_class) {
            auto* sub = reinterpret_cast<t_canvas*>(y);
            juce::String cname = (sub->gl_name && sub->gl_name->s_name)
                ? juce::String::fromUTF8(sub->gl_name->s_name) : juce::String("pd");
            if (canvas_isabstraction(sub)) {
                // Abstraction: fold its whole subtree into THIS canvas as one unit.
                std::unordered_set<t_canvas*> localVisited;
                int csig = 0; std::map<juce::String, int> ch;
                double w = mcpSubtreeWeight(sub, localVisited, depth + 1, csig, ch);
                node.weight += w;
                node.signalObjects += csig;
                for (auto& kv : ch) node.heavies[kv.first] += kv.second;
                node.heavies["[" + cname + "]"] += 1; // surface the abstraction itself
            } else {
                // Inline [pd name]: a real subpatch — emit it separately.
                mcpWalkPerf(sub, path + "/" + cname, out, visited, depth + 1);
            }
            continue;
        }
        if (cl == clone_class) {
            int n = clone_get_n(y);
            for (int i = 0; i < n; ++i) {
                auto* inst = clone_get_instance(y, i);
                if (!inst) continue;
                std::unordered_set<t_canvas*> localVisited;
                int csig = 0; std::map<juce::String, int> ch;
                double w = mcpSubtreeWeight(reinterpret_cast<t_canvas*>(inst), localVisited, depth + 1, csig, ch);
                node.weight += w;
                node.signalObjects += csig;
                for (auto& kv : ch) node.heavies[kv.first] += kv.second;
            }
            node.heavies["[clone]"] += 1;
            continue;
        }
        if (!zgetfn(&y->g_pd, gensym("dsp"))) continue;
        juce::String c = (cl && class_getname(cl))
            ? juce::String::fromUTF8(class_getname(cl)) : juce::String("~");
        double w = mcpDspWeight(c);
        node.weight += w;
        node.signalObjects += 1;
        if (w >= 3.0) node.heavies[c] += 1;
    }
    out.push_back(node);
}

} // anonymous namespace

juce::String MCPBridge::computePerfFacts(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    std::vector<PerfNode> nodes;
    {
        std::unordered_set<t_canvas*> visited;
        mcpWalkPerf(cnv, canvasName, nodes, visited, 0);
    }

    double totalWeight = 0.0;
    int totalSignal = 0;
    for (auto& n : nodes) { totalWeight += n.weight; totalSignal += n.signalObjects; }

    auto sorted = nodes;
    std::sort(sorted.begin(), sorted.end(), [](const PerfNode& a, const PerfNode& b) {
        return a.weight > b.weight;
    });

    juce::Array<juce::var> topArr;
    int limit = juce::jmin(static_cast<int>(sorted.size()), 8);
    for (int i = 0; i < limit; ++i) {
        auto& n = sorted[static_cast<size_t>(i)];
        auto* o = new juce::DynamicObject();
        o->setProperty("canvas", n.name);
        o->setProperty("weight", n.weight);
        o->setProperty("signalObjects", n.signalObjects);
        auto* h = new juce::DynamicObject();
        for (auto& kv : n.heavies) h->setProperty(kv.first, kv.second);
        o->setProperty("heavies", juce::var(h));
        topArr.add(juce::var(o));
    }

    float cpu = processor ? processor->getMcp()->gui().cpuUsage() : 0.0f;

    auto* root = new juce::DynamicObject();
    root->setProperty("dsp", pd_getdspstate() != 0);
    root->setProperty("cpu", static_cast<double>(cpu));
    root->setProperty("xruns", processor ? processor->getMcp()->xrunCount() : -1);
    if (mcp::audit::enabled()) { // P3.4 S7: Pd's symbol table size (symbols are never freed); audit runs only
        long symbols = 0;
        if (pd_this && pd_this->pd_symhash)
            for (int i = 0; i < SYMTABHASHSIZE; ++i)
                for (t_symbol* sym = pd_this->pd_symhash[i]; sym; sym = sym->s_next)
                    ++symbols;
        root->setProperty("symbols", static_cast<juce::int64>(symbols));
    }
    if (mcp::rt::guardLinked()) { // P3.4 S6: realtime-guard builds only
        root->setProperty("rtAllocs", static_cast<juce::int64>(mcp::rt::allocs()));
        root->setProperty("rtLocks", static_cast<juce::int64>(mcp::rt::locks()));
    }
    root->setProperty("canvases", static_cast<int>(nodes.size()));
    root->setProperty("signalObjects", totalSignal);
    root->setProperty("estimatedWeight", totalWeight);
    // Compiled Mode (hvcc) — expose it so MCP tools can warn: heavy ELSE /
    // MERDA abstractions + a DSP recompile can crash in Compiled Mode.
    {
        bool const compiled = SettingsFile::getInstance()->getProperty<bool>("hvcc_mode");
        root->setProperty("compiledMode", compiled);
    }
    root->setProperty("top", topArr);
    root->setProperty("note", "cpu is live host load; weight is an object-census estimate (kernels weighted heavier)");

    return juce::JSON::toString(juce::var(root));
}

// ── sanitizeLayout — inline minimal-deoverlap post-guard ────────────────
// Same algorithm as /pd/deoverlap (PAD 5, 10px snap, minimal-axis push) but
// using Pd-only bounds so it is safe to call inline under sys_lock from the
// batch_atomic path. Returns the number of objects moved.
int MCPBridge::sanitizeLayout(PluginProcessor* processor, t_canvas* cnv, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (pad < 0) pad = 0;
    if (snap < 1) snap = 1;

    std::vector<t_gobj*> objs;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (mcpIsDecorativeBackground(y)) continue;
        objs.push_back(y);
    }
    if (objs.size() < 2) return 0;

    struct DR { t_gobj* g; int x, y, w, h; };
    std::vector<DR> rects;
    rects.reserve(objs.size());
    for (auto* g : objs) {
        int x = 0, y = 0, w = 0, h = 0;
        pd::Interface::getObjectBounds(cnv, g, &x, &y, &w, &h);
        rects.push_back({ g, x, y, w, h });
    }

    const int PAD = pad;
    const int MAXPASS = 24;
    auto snapTo = [snap](int v) { return (v / snap) * snap; };
    for (int pass = 0; pass < MAXPASS; pass++) {
        bool anyHit = false;
        for (size_t i = 0; i < rects.size(); i++) {
            for (size_t j = i + 1; j < rects.size(); j++) {
                auto& a = rects[i];
                auto& b = rects[j];
                bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                        && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                if (!hit) continue;
                anyHit = true;
                int overlapX = std::min(a.x + a.w + PAD - b.x, b.x + b.w + PAD - a.x);
                int overlapY = std::min(a.y + a.h + PAD - b.y, b.y + b.h + PAD - a.y);
                if (overlapX <= overlapY) {
                    int acx = a.x + a.w / 2, bcx = b.x + b.w / 2;
                    int dx = std::max(snap, snapTo(overlapX + snap - 1));
                    b.x += (bcx >= acx ? dx : -dx);
                } else {
                    int acy = a.y + a.h / 2, bcy = b.y + b.h / 2;
                    int dy = std::max(snap, snapTo(overlapY + snap - 1));
                    b.y += (bcy >= acy ? dy : -dy);
                }
            }
        }
        // De-overlap objects that overlap region title bars
        for (auto const& rg : processor->getMcp()->session.mcpRegions) {
            if (rg.title.isEmpty()) continue;
            int rx = static_cast<int>(rg.x), ry = static_cast<int>(rg.y);
            int rw = static_cast<int>(rg.w), rh = 26;
            for (auto& r : rects) {
                if (r.x < rx + rw + PAD && r.x + r.w + PAD > rx
                 && r.y < ry + rh + PAD && r.y + r.h + PAD > ry) {
                    anyHit = true;
                    int targetY = ry + rh + PAD;
                    int snappedY = snapTo(targetY + snap - 1);
                    if (snappedY <= targetY) snappedY += snap;
                    r.y = snappedY;
                }
            }
        }
        if (!anyHit) break;
    }

    int moved = 0;
    for (auto& r : rects) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
        if (r.x != ox || r.y != oy) {
            pd::Interface::moveObject(cnv, r.g, r.x, r.y);
            moved++;
        }
    }
    if (moved > 0) canvas_dirty(cnv, 1);
    return moved;
}

// ── fixOcclusions — nudge boxes off wire paths ──────────────────────────
// Anchor-line vs box test (Liang-Barsky, same model as /pd/wire_occlusions).
// For each wire that passes through a box, shift that box horizontally (the
// minimal snapped amount) off the wire's x at the box's vertical centre.
// Bounded passes; geometry-only, no DSP touch.
int MCPBridge::fixOcclusions(PluginProcessor* processor, t_canvas* cnv, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (snap < 1) snap = 1;

    auto lineHitsBox = [](double x1, double y1, double x2, double y2,
                          double bx, double by, double bw, double bh) {
        double minX = std::min(x1, x2), maxX = std::max(x1, x2);
        double minY = std::min(y1, y2), maxY = std::max(y1, y2);
        if (maxX < bx || minX > bx + bw || maxY < by || minY > by + bh) return false;
        double dx = x2 - x1, dy = y2 - y1, t0 = 0.0, t1 = 1.0;
        double p[4] = { -dx, dx, -dy, dy };
        double q[4] = { x1 - bx, bx + bw - x1, y1 - by, by + bh - y1 };
        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0) { if (q[i] < 0) return false; }
            else {
                double t = q[i] / p[i];
                if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
                else { if (t < t0) return false; if (t < t1) t1 = t; }
            }
        }
        return t0 < t1 && t0 > 0.05 && t1 < 0.95;
    };

    int totalMoved = 0;
    for (int pass = 0; pass < 3; pass++) {
        std::vector<t_gobj*> objs;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { if (mcpIsDecorativeBackground(y)) continue; objs.push_back(y); }
        if (objs.size() < 3) break;

        struct R { int x, y, w, h; };
        std::vector<R> rects(objs.size());
        std::unordered_map<t_gobj*, int> ptrToIdx;
        for (size_t i = 0; i < objs.size(); ++i) {
            int x = 0, yy = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(cnv, objs[i], &x, &yy, &w, &h);
            rects[i] = { x, yy, w, h };
            ptrToIdx[objs[i]] = static_cast<int>(i);
        }

        struct Edge { int si, di, outno, nout, inno, nin; };
        std::vector<Edge> edges;
        t_linetraverser lt;
        t_outconnect* oc = nullptr;
        linetraverser_start(&lt, cnv);
        while ((oc = linetraverser_next_nosize(&lt))) {
            auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
            auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
            if (si == ptrToIdx.end() || di == ptrToIdx.end()) continue;
            edges.push_back({ si->second, di->second, lt.tr_outno, lt.tr_nout, lt.tr_inno, lt.tr_nin });
        }

        auto snapUp = [snap](int v) { return ((v + snap - 1) / snap) * snap; };
        bool anyMove = false;
        for (auto& e : edges) {
            int si = e.si, di = e.di;
            double x1 = mcpIoletX(rects[si].x, rects[si].w, e.outno, e.nout);
            double y1 = rects[si].y + rects[si].h;
            double x2 = mcpIoletX(rects[di].x, rects[di].w, e.inno, e.nin);
            double y2 = rects[di].y;
            for (size_t k = 0; k < objs.size(); ++k) {
                if ((int)k == si || (int)k == di) continue;
                if (!lineHitsBox(x1, y1, x2, y2, rects[k].x, rects[k].y, rects[k].w, rects[k].h)) continue;
                double wyc = rects[k].y + rects[k].h / 2.0;
                double t = (y2 != y1) ? (wyc - y1) / (y2 - y1) : 0.5;
                t = std::max(0.0, std::min(1.0, t));
                double wx = x1 + t * (x2 - x1);
                int bx = rects[k].x, bw = rects[k].w;
                int by = rects[k].y, bh = rects[k].h;
                // Shift perpendicular to the wire: horizontal-ish wire → move the
                // box vertically; vertical-ish wire → move it horizontally.
                double dxw = x2 - x1, dyw = y2 - y1;
                int newbx = bx, newby = by;
                if (std::abs(dxw) >= std::abs(dyw)) {
                    double wxc = bx + bw / 2.0;
                    double tw = (dxw != 0.0) ? (wxc - x1) / dxw : 0.5;
                    tw = std::max(0.0, std::min(1.0, tw));
                    double wy = y1 + tw * dyw;
                    int shiftUp   = static_cast<int>(std::ceil((by + bh + pad) - wy)); // >0 → move up
                    int shiftDown = static_cast<int>(std::ceil(wy + pad - by));        // >0 → move down
                    if (shiftUp > 0 && (shiftUp <= shiftDown || shiftDown <= 0))
                        newby = by - snapUp(shiftUp);
                    else if (shiftDown > 0)
                        newby = by + snapUp(shiftDown);
                } else {
                int shiftLeft  = static_cast<int>(std::ceil((bx + bw + pad) - wx)); // >0 → move left
                int shiftRight = static_cast<int>(std::ceil(wx + pad - bx));        // >0 → move right
                if (shiftLeft > 0 && (shiftLeft <= shiftRight || shiftRight <= 0))
                    newbx = bx - snapUp(shiftLeft);
                else if (shiftRight > 0)
                    newbx = bx + snapUp(shiftRight);
                }
                if (newbx != bx || newby != by) { rects[k].x = newbx; rects[k].y = newby; anyMove = true; totalMoved++; }
            }
        }
        if (!anyMove) break;

        for (size_t k = 0; k < objs.size(); ++k) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            pd::Interface::getObjectBounds(cnv, objs[k], &ox, &oy, &ow, &oh);
            if (rects[k].x != ox || rects[k].y != oy) pd::Interface::moveObject(cnv, objs[k], rects[k].x, rects[k].y);
        }
        canvas_dirty(cnv, 1);
    }
    return totalMoved;
}

// ── composeLayout — P4 role-based composition ───────────────────────────
// Arranges the canvas by cluster role (PRD_CONTEXT_LAYOUT_GUARD P4):
//   bus/mix cluster -> left edge column
//   synth cluster   -> one column per cluster (left->right), signal objects
//                      stacked top->bottom, controls/gui in a gutter above
// On-demand (like dagre/flow), never forced. Returns objects moved.
int MCPBridge::composeLayout(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (snap < 1) snap = 1;

    std::vector<t_gobj*> objs;
    std::unordered_map<t_gobj*, int> idx;
    int i = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { idx[y] = i++; objs.push_back(y); }
    int n = static_cast<int>(objs.size());
    if (n < 2) return 0;

    std::vector<juce::String> cls(n);
    std::vector<int> ox(n), oy(n), ow(n), oh(n);
    for (int k = 0; k < n; ++k) {
        pd::Interface::getObjectBounds(cnv, objs[k], &ox[k], &oy[k], &ow[k], &oh[k]);
        t_class* cl = pd_class(&objs[k]->g_pd);
        const char* c = cl ? class_getname(cl) : nullptr;
        cls[k] = c ? juce::String::fromUTF8(c) : juce::String("unknown");
    }

    // union-find over wires
    std::vector<int> parent(n);
    for (int k = 0; k < n; ++k) parent[k] = k;
    auto findRoot = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    auto unite = [&](int a, int b) { int ra = findRoot(a), rb = findRoot(b); if (ra != rb) parent[ra] = rb; };
    t_linetraverser lt; t_outconnect* oc = nullptr;
    std::vector<std::pair<int, int>> edges;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        auto si = idx.find(&lt.tr_ob->ob_g);
        auto di = idx.find(&lt.tr_ob2->ob_g);
        if (si != idx.end() && di != idx.end()) { unite(si->second, di->second); edges.emplace_back(si->second, di->second); }
    }

    std::unordered_map<int, std::vector<int>> groups;
    for (int k = 0; k < n; ++k) groups[findRoot(k)].push_back(k);

    auto isSignal = [](const juce::String& c) {
        return c.endsWithChar('~') || c == "catch~" || c == "dac~" || c == "throw~" || c == "out~" || c == "pd" || c == "table";
    };
    auto isGui = [](const juce::String& c) {
        return c == "bng" || c == "tgl" || c == "hsl" || c == "vsl" || c == "hradio" || c == "vradio"
            || c == "numbox" || c == "nbx" || c == "floatatom" || c == "symbolatom" || c == "vu"
            || c == "knob" || c == "slider" || c == "radio" || c == "toggle";
    };
    auto isSynth = [](const juce::String& c) {
        return c == "osc~" || c == "phasor~" || c == "noise~" || c == "sig~" || c.startsWith("bl.")
            || c.startsWith("plaits") || c == "tabread4~" || c == "tabread~" || c == "tabplay~";
    };

    struct Cl { std::vector<int> members; juce::String role; int minx; };
    std::vector<Cl> clusters;
    for (auto& kv : groups) {
        Cl c; c.members = kv.second;
        bool bus = false, synth = false, sig = false, allGui = true;
        int mx = INT_MAX;
        for (int k : c.members) {
            const juce::String& cc = cls[k];
            // The master bus is catch~/dac~/out~ ONLY. throw~ is a voice SEND.
            if (cc == "catch~" || cc == "dac~" || cc == "out~") bus = true;
            if (isSynth(cc)) synth = true;
            if (isSignal(cc)) sig = true;
            if (!isGui(cc)) allGui = false;
            mx = std::min(mx, ox[k]);
        }
        c.role = bus ? "bus" : (synth ? "synth" : (allGui ? "gui" : (sig ? "signal" : "control")));
        c.minx = mx;
        clusters.push_back(c);
    }

    std::sort(clusters.begin(), clusters.end(), [](const Cl& a, const Cl& b) {
        if ((a.role == "bus") != (b.role == "bus")) return a.role == "bus";
        return a.minx < b.minx;
    });

    const int COL_W = 300;
    const int COL_X = 30;
    const int GUTTER_Y = 40;
    const int ROW_GAP = 60;
    const int SIGNAL_TOP = 160;

    int moved = 0;
    int col = 0;
    for (auto& c : clusters) {
        int cx = COL_X + col * COL_W;
        std::vector<int> ctrl, sigv;
        for (int k : c.members) {
            if (isSignal(cls[k])) sigv.push_back(k);
            else ctrl.push_back(k);
        }
        // controls/gui in the gutter, left→right, spaced by their width
        int gx = cx;
        for (int k : ctrl) {
            pd::Interface::moveObject(cnv, objs[k], gx, GUTTER_Y);
            moved++;
            gx += ow[k] + 20;
        }
        // signal objects stacked top→bottom in SIGNAL-FLOW order (topological:
        // sources first, then processors, then the send/sink), fallback to y.
        std::unordered_set<int> inC(sigv.begin(), sigv.end());
        std::unordered_map<int, int> indeg;
        for (int k : sigv) indeg[k] = 0;
        for (auto& e : edges) if (inC.count(e.first) && inC.count(e.second)) indeg[e.second]++;
        std::vector<int> order;
        std::vector<int> remaining = sigv;
        while (!remaining.empty()) {
            bool progress = false;
            for (auto it = remaining.begin(); it != remaining.end(); ) {
                if (indeg[*it] <= 0) {
                    int node = *it;
                    order.push_back(node);
                    for (auto& e : edges) if (e.first == node && inC.count(e.second)) indeg[e.second]--;
                    it = remaining.erase(it);
                    progress = true;
                } else ++it;
            }
            if (!progress) {
                std::sort(remaining.begin(), remaining.end(), [&](int a, int b) { return oy[a] < oy[b]; });
                order.insert(order.end(), remaining.begin(), remaining.end());
                break;
            }
        }
        int yy = SIGNAL_TOP;
        for (int k : order) {
            pd::Interface::moveObject(cnv, objs[k], cx, yy);
            moved++;
            yy += ROW_GAP;
        }
        col++;
    }

    if (moved > 0) canvas_dirty(cnv, 1);
    (void)pad; (void)snap;
    return moved;
}

// Network thread: decode (done by JUCE) and enqueue, nothing else (Plan 3 P3.4 S1).
void MCPBridge::oscMessageReceived(const juce::OSCMessage& message)
{
    commandWorker.post(Command { message, nullptr });
}

void MCPBridge::startCommandWorker()
{
    commandWorker.start([this](Command command) { runCommand(std::move(command)); });
}

void MCPBridge::runCommand(Command command)
{
    if (!command.work) {
        handleMessage(command.message);
        return;
    }
    if (!v2Router.begin(command.work->origin, v2NowMs()))
        return; // too late: answered rejected{deadline}, nothing run (the token still reports done)
    mcp::v2::WorkScope const scope(std::move(command.work)); // its end may be the request's "done"
    handleMessage(command.message);
}

bool MCPBridge::runInPdContext(CommandHandler handler, const juce::String& action, const juce::OSCMessage& msg)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (processor == nullptr || mm == nullptr || mm->isThisTheMessageThread())
        return false;
    auto const work = mcp::v2::captureWork(); // P4.1 S3: the handler replies for the same v2 request there
    if (!processor->getMcp()->pdWait([&] {
            mcp::v2::WorkScope const scope(work);
            (this->*handler)(action, msg);
        }, 5000))
        processor->logError("MCP bridge: " + msg.getAddressPattern().toString() + " was not run: the message thread was busy for 5 s");
    return true;
}

// Command worker: parse the address and dispatch through the command table.
void MCPBridge::handleMessage(const juce::OSCMessage& message)
{
    auto addr = message.getAddressPattern().toString();
    while (addr.startsWith("/")) addr = addr.substring(1);
    auto parts = juce::StringArray::fromTokens(addr, "/", "");
    parts.removeEmptyStrings();

    if (parts.isEmpty()) return;

    // Any inbound message means the MCP server is alive and talking to us.
    noteServerActivity();

    auto const domain = parts[0];

    auto const pathVerb = parts.size() > 1 ? parts[1] : juce::String();
    if (dispatchRegistered(domain, pathVerb, message))
        return;

    // Every verb of every domain is a table row (dispatchRegistered above); an unknown one is reported.
    replyUnknownVerb(domain, pathVerb, message);
}

void MCPBridge::oscBundleReceived(const juce::OSCBundle& bundle)
{
    for (auto const& elem : bundle) {
        if (elem.isMessage()) {
            oscMessageReceived(elem.getMessage());
        }
    }
}

// ─── Identity sidecar (<file>.mcpids.json), P3.5 S7 ─────────────────────────────────────────────────────────
// Written beside the .pd by BOTH save paths (the MCP save and plugdata's GUI save hook). Format v2:
//   {format:"MCP-IDENTITY", v:2, objectCount, pdHash, root:[{i,id,c}], sub:{<name>[#k]:{objectCount, ids:[{i,id,c}]}}}
// pdHash is the SHA-256 of the .pd bytes written beside it; c is each object's class (an abstraction's file name,
// "pd" for a subpatch); sub keys are the subpatch's name, "#k" for its k-th namesake in that canvas (k >= 2).
// v1 ({i,id} only, sub keyed by name) is still read. One applier for load_patch and open_patch validates:
// same hash -> every entry; a changed .pd (or v1) -> only entries whose index exists and whose class (v2)
// matches; the rest get fresh names (owner decision O2) and one console line says how many.

// The class recorded per entry (and checked on load).
static juce::String sidecarClassOf(t_gobj* g)
{
    if (!g) return {};
    if (pd_class(&g->g_pd) == canvas_class) {
        juce::String absName;
        return pd::getAbstractionFileName(g, absName) ? absName : juce::String("pd");
    }
    char const* name = class_getname(pd_class(&g->g_pd));
    return name ? juce::String::fromUTF8(name) : juce::String();
}

// Sub keys: the subpatch's name, "<name>#k" for its k-th namesake in the same canvas.
static std::vector<std::pair<juce::String, t_canvas*>> sidecarSubKeys(t_canvas* cnv)
{
    std::vector<std::pair<juce::String, t_canvas*>> keys;
    std::map<juce::String, int> seen;
    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
        if (pd_class(&g->g_pd) != canvas_class) continue;
        auto* child = reinterpret_cast<t_canvas*>(g);
        if (!child->gl_name) continue;
        auto const name = juce::String::fromUTF8(child->gl_name->s_name);
        int const k = ++seen[name];
        keys.emplace_back(k == 1 ? name : name + "#" + juce::String(k), child);
    }
    return keys;
}

juce::String MCPBridge::sidecarPdHash(juce::File const& pdFile)
{
    if (!pdFile.existsAsFile()) return {};
    return juce::SHA256(pdFile).toHexString();
}

// buildIdentitySidecar: CALLER MUST HOLD sys_lock. pdHash is added by the caller once the .pd is on disk.
juce::var MCPBridge::buildIdentitySidecar(PluginProcessor* processor, t_canvas* cnv)
{
    if (!processor || !cnv) return {};
    auto& identity = processor->getMcp()->identity;

    auto buildIds = [&](t_canvas* c, int& count) -> juce::var {
        juce::Array<juce::var> arr;
        std::unordered_map<t_gobj*, juce::String> ptrToId;
        if (auto const* ids = identity.findCanvas(c))
            for (auto const& [id, ptr] : *ids)
                if (ptr && identity.valid(c, id)) ptrToId[ptr] = juce::String(id);
        count = 0;
        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++count) {
            auto it = ptrToId.find(g);
            if (it == ptrToId.end()) continue;
            auto* o = new juce::DynamicObject();
            o->setProperty("i", count);
            o->setProperty("id", it->second);
            o->setProperty("c", sidecarClassOf(g));
            arr.add(juce::var(o));
        }
        return juce::var(arr);
    };

    auto* root = new juce::DynamicObject();
    root->setProperty("format", juce::String("MCP-IDENTITY"));
    root->setProperty("v", 2);
    int rootCount = 0;
    auto rootIds = buildIds(cnv, rootCount);
    root->setProperty("objectCount", rootCount);
    root->setProperty("root", rootIds);
    auto* subObj = new juce::DynamicObject();
    for (auto const& [key, child] : sidecarSubKeys(cnv)) {
        int childCount = 0;
        auto childIds = buildIds(child, childCount);
        if (childIds.getArray() == nullptr || childIds.getArray()->isEmpty()) continue;
        auto* entry = new juce::DynamicObject();
        entry->setProperty("objectCount", childCount);
        entry->setProperty("ids", childIds);
        subObj->setProperty(key, juce::var(entry));
    }
    root->setProperty("sub", juce::var(subObj));
    return juce::var(root);
}

juce::String MCPBridge::writeIdentitySidecarFile(juce::var sidecar, juce::File const& pdFile)
{
    if (auto* o = sidecar.getDynamicObject())
        o->setProperty("pdHash", sidecarPdHash(pdFile));
    else
        return "error: could not build identity sidecar";
    // replaceWithText writes a temp file and renames it: a crash leaves the old sidecar or the new one, never half.
    juce::File sidecarFile(pdFile.getFullPathName() + ".mcpids.json");
    if (!sidecarFile.replaceWithText(juce::JSON::toString(sidecar, true)))
        return "error: could not write " + sidecarFile.getFullPathName();
    return {};
}

juce::String MCPBridge::writeIdentitySidecar(PluginProcessor* processor, t_canvas* cnv,
                                             const juce::String& destFilePath)
{
    if (!processor || !cnv) return "error: canvas not found";
    juce::var sidecar;
    {
        McpHost::PdLockScope lock(*processor);
        sidecar = buildIdentitySidecar(processor, cnv);
    }
    return writeIdentitySidecarFile(sidecar, juce::File(destFilePath));
}

juce::var MCPBridge::readIdentitySidecar(juce::File const& pdFile, juce::String& note)
{
    juce::File sidecarFile(pdFile.getFullPathName() + ".mcpids.json");
    if (!sidecarFile.existsAsFile()) return {};
    auto const text = sidecarFile.loadFileAsString().trim();
    // JUCE's JSON parser returns the part it read of a truncated file: refuse anything that does not end the object.
    if (!text.endsWithChar('}')) {
        note = "ignored a truncated " + sidecarFile.getFileName();
        return {};
    }
    auto parsed = juce::JSON::parse(text);
    auto* o = parsed.getDynamicObject();
    if (!o || o->getProperty("format") != juce::String("MCP-IDENTITY")) {
        note = "ignored an unreadable " + sidecarFile.getFileName();
        return {};
    }
    if (static_cast<int>(o->getProperty("v")) >= 2 && !o->hasProperty("pdHash")) {
        note = "ignored " + sidecarFile.getFileName() + " (no file hash)";
        return {};
    }
    return parsed;
}

MCPBridge::SidecarStats MCPBridge::mcpApplyIdentitySidecar(PluginProcessor* processor, juce::DynamicObject* sidecarObj,
                                                         t_canvas* cnv, juce::File const& pdFile)
{
    SidecarStats stats;
    if (!processor || !sidecarObj || !cnv) return stats;
    auto& identity = processor->getMcp()->identity;
    int const version = sidecarObj->hasProperty("v") ? static_cast<int>(sidecarObj->getProperty("v")) : 1;
    stats.fileMatches = version >= 2 && sidecarObj->getProperty("pdHash").toString() == sidecarPdHash(pdFile);

    auto applyIds = [&](juce::var const& idArray, t_canvas* target) {
        auto const* arr = idArray.getArray();
        if (arr == nullptr) return;
        std::vector<t_gobj*> byIndex;
        for (t_gobj* g = target->gl_list; g; g = g->g_next) byIndex.push_back(g);
        std::vector<t_gobj*> rejected;
        for (auto const& v : *arr) {
            auto* o = v.getDynamicObject();
            if (!o) continue;
            auto const iv = o->getProperty("i");
            auto const id = o->getProperty("id").toString();
            if (id.isEmpty()) continue;
            double const di = (iv.isInt() || iv.isInt64() || iv.isDouble()) ? static_cast<double>(iv) : -1.0;
            if (!(di >= 0.0 && di < static_cast<double>(byIndex.size()))) { ++stats.renamed; continue; } // missing, non-finite or out of range
            int const i = static_cast<int>(di);
            t_gobj* g = byIndex[static_cast<size_t>(i)];
            // A v2 entry names its class: an edited .pd (another object at that index) keeps no stale name (O2).
            if (o->hasProperty("c") && o->getProperty("c").toString() != sidecarClassOf(g)) {
                rejected.push_back(g);
                continue;
            }
            if (identity.bind(target, id.toStdString(), g) == mcp::IdentityService::BindResult::Refused)
                ++stats.skipped; // the id already names another live object (#38)
            else
                ++stats.restored;
        }
        for (t_gobj* g : rejected) { // fresh names for the objects whose saved name no longer fits
            identity.mint(target, g, McpHost::mintPrefix(g).toStdString());
            ++stats.renamed;
        }
    };

    identity.clearCanvas(cnv);
    applyIds(sidecarObj->getProperty("root"), cnv);
    if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
        for (auto const& [key, child] : sidecarSubKeys(cnv)) {
            auto entry = subObj->getProperty(juce::Identifier(key));
            if (entry.isVoid()) continue;
            identity.clearCanvas(child);
            // v2: {objectCount, ids}; v1: the array itself (keyed by name only)
            applyIds(entry.getDynamicObject() ? entry.getDynamicObject()->getProperty("ids") : entry, child);
        }
    }
    identity.bumpVersion(cnv);
    return stats;
}

// The console line of one sidecar apply (empty when all ids came back unchanged).
juce::String MCPBridge::sidecarReport(juce::File const& pdFile, SidecarStats const& stats, juce::String const& note)
{
    juce::String line;
    if (note.isNotEmpty())
        line = note;
    if (stats.renamed > 0 || stats.skipped > 0) {
        if (line.isNotEmpty()) line << "; ";
        line << pdFile.getFileName() << ": restored " << stats.restored << " names, gave " << stats.renamed
             << " objects fresh names (the patch changed since its names were saved)";
        if (stats.skipped > 0) line << ", skipped " << stats.skipped << " names already in use";
    }
    return line.isEmpty() ? line : "MCP identity: " + line;
}

void MCPBridge::advanceSequencer(int blockSize)
{
    double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;

    {
    juce::ScopedTryLock const sl(seqLock); // P3.4 S6: never wait; a busy block fires one sub-block later
    if (!sl.isLocked()) {
        flushSeqFires(); // fires collected earlier still go out
        return;
    }
    for (auto& job : seqJobs) {
        if (!job.active) continue;

        // Free jobs advance their own sample counter; locked jobs read the transport.
        job.jobSampleCounter += blockSize;
        int64_t const pos = job.syncToClock
            ? transport.sampleCounter.load(std::memory_order_relaxed)
            : job.jobSampleCounter;

        if (job.nextStepSample < 0)
            job.nextStepSample = pos;  // first tick: anchor to now

        double const effBpm = job.bpm * (1.0 + job.drift / 100.0);
        double const stepSamples = sampleRate * 60.0 / (effBpm * job.subdivision);

        int guard = 0;
        while (job.nextStepSample <= pos && guard++ < 64) {
            // Fire every non-rest step value across all tracks for this step.
            for (auto const& track : job.tracks) {
                if (track.steps.empty()) continue;
                int const ti = job.stepIndex % (int)track.steps.size();
                float const val = track.steps[ti];
                if (val == 0.0f) continue;
                enqueueSeqFire(track.isBang, track.sym, val);
            }

            job.stepIndex = (job.stepIndex + 1) % job.patternLength;

            double len = stepSamples;
            if (job.swing > 0.0f && (job.stepIndex % 2) == 1)
                len += stepSamples * job.swing * 0.5;
            job.nextStepSample += (int64_t)len;
        }
    }
    }
    flushSeqFires();
}

void MCPBridge::enqueueSeqFire(bool isBang, t_symbol* sym, float value)
{
    if (sym == nullptr)
        return;
    if (seqPendingCount >= kMaxSeqFires) {
        seqFiresDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    seqPendingFires[static_cast<size_t>(seqPendingCount++)] = { sym, value, isBang };
}

// Audio thread, after a Pd sub-block: send the collected step fires before the next performDSP (where the
// old functionQueue route ran them), under the Pd lock. If another thread holds it, wait: it is the same lock
// libpd_process_raw takes for the next sub-block a moment later, so this adds no new kind of stall, and the
// steps stay block-accurate (S6 deferred them instead and lost up to two blocks under GUI load, S8 gate).
// That one wait is deliberately not counted by the realtime guard.
void MCPBridge::flushSeqFires()
{
    if (seqPendingCount == 0 || processor == nullptr)
        return;
    auto const& pdLock = processor->audioLock;
    if (!pdLock.tryEnter()) {
        mcp::rt::Pause const sameWaitAsPerformDsp;
        pdLock.enter();
    }
    mcp::rt::Pause const patchWork; // what the patch does with the message is not the bridge's (realtime guard)
    for (int i = 0; i < seqPendingCount; ++i) {
        auto const& f = seqPendingFires[static_cast<size_t>(i)];
        if (auto* target = f.sym->s_thing) {
            if (f.isBang)
                pd_bang(target);
            else
                pd_float(target, f.value);
        }
    }
    seqPendingCount = 0;
    pdLock.exit();
}

void MCPBridge::audioTick()
{
    probeManager.audioTick();

    // Advance the native transport clock — one PD block per call, so the running
    // sample counter gives sample-accurate bar/beat/step position.
    int const blockSize = pd::Instance::getBlockSize();
    if (blockSize > 0) {
        if (transport.running.load(std::memory_order_relaxed))
            transport.sampleCounter.fetch_add(blockSize, std::memory_order_relaxed);
        advanceSequencer(blockSize);
    }
}

float estimateFrequency(const float* buf, int n, float sampleRate)
{
    if (!buf || n < 128 || sampleRate <= 0.0f) return 0.0f;

    int minLag = std::max(2, static_cast<int>(sampleRate / 4000.0f));  // max freq 4kHz
    int maxLag = std::min(n / 2, static_cast<int>(sampleRate / 40.0f));  // min freq 40Hz

    if (minLag >= maxLag) return 0.0f;

    std::vector<float> corrs(maxLag + 1, 0.0f);

    for (int lag = minLag; lag <= maxLag; lag++) {
        float sumCross = 0.0f;
        float sumSq0 = 0.0f;
        float sumSqLag = 0.0f;
        int count = n - lag;
        for (int i = 0; i < count; i++) {
            float a = buf[i];
            float b = buf[i + lag];
            sumCross += a * b;
            sumSq0 += a * a;
            sumSqLag += b * b;
        }
        float denom = std::sqrt(sumSq0 * sumSqLag);
        if (denom > 1e-6f) {
            corrs[lag] = sumCross / denom;
        }
    }

    int peakLag = 0;
    for (int lag = minLag + 1; lag < maxLag; lag++) {
        if (corrs[lag] > 0.75f && corrs[lag] >= corrs[lag - 1] && corrs[lag] >= corrs[lag + 1]) {
            peakLag = lag;
            break;
        }
    }

    if (peakLag > 0) {
        float alpha = corrs[peakLag - 1];
        float beta = corrs[peakLag];
        float gamma = corrs[peakLag + 1];
        float denom = (alpha - 2.0f * beta + gamma);
        float delta = (std::abs(denom) > 1e-9f) ? (0.5f * (alpha - gamma) / denom) : 0.0f;
        float trueLag = static_cast<float>(peakLag) + delta;
        return (trueLag > 0.0f) ? (sampleRate / trueLag) : 0.0f;
    }

    return 0.0f;
}

// ── Precision pitch: YIN (de Cheveigné & Kawahara, 2002) ─────────────────────
// Difference function → cumulative-mean normalized difference → absolute
// threshold → parabolic interpolation. Returns Hz (0 = unvoiced) and a
// per-frame confidence. Far fewer octave errors than FFT-peak picking.
static float estimatePitchYIN(const float* buf, int n, float sampleRate, float& outConfidence)
{
    outConfidence = 0.0f;
    if (!buf || n < 256 || sampleRate <= 0.0f) return 0.0f;

    int const maxTau = std::min(n / 2, static_cast<int>(sampleRate / 55.0f)); // down to ~55 Hz
    if (maxTau < 8) return 0.0f;
    int const halfN = n / 2;

    std::vector<float> dp(maxTau, 1.0f);
    float running = 0.0f;
    for (int tau = 1; tau < maxTau; ++tau) {
        float sum = 0.0f;
        for (int i = 0; i < halfN; ++i) {
            float const diff = buf[i] - buf[i + tau];
            sum += diff * diff;
        }
        running += sum;
        dp[tau] = running > 0.0f ? (sum * static_cast<float>(tau)) / running : 1.0f;
    }

    constexpr float threshold = 0.15f;
    int tauEstimate = -1;
    for (int tau = 2; tau < maxTau; ++tau) {
        if (dp[tau] < threshold) {
            while (tau + 1 < maxTau && dp[tau + 1] < dp[tau]) ++tau;
            tauEstimate = tau;
            break;
        }
    }
    if (tauEstimate < 0) {
        int best = 1;
        for (int tau = 2; tau < maxTau; ++tau) {
            if (dp[tau] < dp[best]) best = tau;
        }
        if (dp[best] > 0.35f) return 0.0f; // too aperiodic → unvoiced
        tauEstimate = best;
    }

    outConfidence = juce::jlimit(0.0f, 1.0f, 1.0f - dp[tauEstimate] * 1.6f);

    float betterTau = static_cast<float>(tauEstimate);
    if (tauEstimate > 1 && tauEstimate < maxTau - 1) {
        float const s0 = dp[tauEstimate - 1];
        float const s1 = dp[tauEstimate];
        float const s2 = dp[tauEstimate + 1];
        float const denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (std::abs(denom) > 1e-9f) betterTau = static_cast<float>(tauEstimate) + (s2 - s0) / denom;
    }
    return betterTau > 0.0f ? sampleRate / betterTau : 0.0f;
}

void MCPBridge::voiceInputTick(float const* inputSamples, int numSamples)
{
    if (!inputSamples || numSamples <= 0) return;

    // Track input peak for live UI meter
    float maxSample = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float const s = std::abs(inputSamples[i]);
        if (s > maxSample) maxSample = s;
    }
    float const prevLevel = voiceLiveLevel.load(std::memory_order_relaxed);
    float const newLevel = (maxSample > prevLevel) ? maxSample : (prevLevel * 0.88f + maxSample * 0.12f);
    voiceLiveLevel.store(newLevel, std::memory_order_relaxed);

    if (!voiceCapturing.load(std::memory_order_relaxed))
        return;

    int const writePos = voiceCaptureWritePos.load(std::memory_order_relaxed);
    int const maxSamples = static_cast<int>(voiceCaptureBuffer.size());
    if (writePos + numSamples > maxSamples) {
        // Buffer full = auto-stop. Hand staging to the SAME terminal path as a
        // manual stop. Do NOT pre-clear voiceCapturing here: the old code
        // cleared it first, so stopVoiceCaptureAndAnalyze()'s own
        // exchange(false) early-returned and the take vanished unstaged.
        if (voiceCapturing.exchange(false, std::memory_order_acq_rel))
            voiceAnalyzePending.store(true, std::memory_order_release); // P3.4 S6: the timer analyses it
        return;
    }

    std::memcpy(voiceCaptureBuffer.data() + writePos, inputSamples, static_cast<size_t>(numSamples) * sizeof(float));
    voiceCaptureWritePos.store(writePos + numSamples, std::memory_order_release);
}

void MCPBridge::startVoiceCapture(int maxSeconds, int mode)
{
    // NEVER re-open the audio device here. setAudioDeviceSetup() can BLOCK the
    // message thread on flaky hardware (e.g. the ALC298 mic-jack bug) and freeze
    // the whole UI. Capture from whatever input the device already provides;
    // the watchdog reports clearly when nothing arrives.
    if (auto* dm = ProjectInfo::getDeviceManager()) {
        if (dm->getAudioDeviceSetup().inputChannels.isZero()) {
            sendConsoleLog("MCP: audio device has no input channels - enable an input in Audio settings", false);
        }
    }

    int const sr = processor ? static_cast<int>(processor->getSampleRate()) : 48000;
    int const neededSamples = juce::jlimit(48000, 48000 * 70, sr * maxSeconds);
    if (static_cast<int>(voiceCaptureBuffer.size()) < neededSamples)
        voiceCaptureBuffer.resize(neededSamples, 0.0f);

    voiceCaptureWritePos.store(0, std::memory_order_release);
    voiceLiveLevel.store(0.0f, std::memory_order_release);
    voiceCaptureMode.store(mode, std::memory_order_release);
    voiceCapturing.store(true, std::memory_order_release);
    if (!isTimerRunning())
        startTimer(15); // P3.4 S6: the timer runs the analysis when the buffer fills
    if (mode == 1)
        sendConsoleLog("MCP: Recording MELODY - sing! (tap the mic again to stop)", false);
    else if (mode == 2)
        sendConsoleLog("MCP: Recording BEAT - beatbox! (tap the drum again to stop)", false);
    else
        sendConsoleLog("MCP: Voice recording started (hum melody or beatbox)", false);

    // Watchdog: the buffer auto-stops+analyzes once filled, but if NO input
    // samples ever arrive (mic muted / input device not running) it would hang
    // forever. After 1.6s with the write head still at 0, say exactly what's
    // wrong and release the capture — never a silent hang.
    juce::Timer::callAfterDelay(1600, [this] {
        if (voiceCapturing.load(std::memory_order_relaxed) &&
            voiceCaptureWritePos.load(std::memory_order_relaxed) == 0) {
            voiceCapturing.store(false, std::memory_order_relaxed);
            sendConsoleLog("MCP: No microphone input detected - check the audio input device", true);
            sendReply("/pd/voice/event",
                      "{\"status\":\"no_input\",\"message\":\"No microphone input detected - check the audio input device (mic muted / input not running)\"}");
        }
    });
}

juce::String MCPBridge::getPlayingVoiceTakeId()
{
    juce::ScopedLock sl(previewLock);
    return previewTakeId;
}

void MCPBridge::stopVoiceTake()
{
    juce::ScopedLock sl(previewLock);
    if (previewTransport) previewTransport->stop();
    if (previewPlayer) {
        previewPlayer->setSource(nullptr);
        if (auto* dm = ProjectInfo::getDeviceManager()) dm->removeAudioCallback(previewPlayer.get());
    }
    previewTransport.reset();
    previewPlayer.reset();
    previewTakeId.clear();
}

void MCPBridge::playVoiceTake(const juce::String& takeId, const juce::String& wavPath)
{
    // Toggle: tapping the playing chip's ▶ stops it.
    if (getPlayingVoiceTakeId() == takeId) {
        stopVoiceTake();
        return;
    }
    stopMidiTake();

    auto* dm = ProjectInfo::getDeviceManager();
    if (!dm) return;
    juce::File const wav(wavPath);
    if (!wav.existsAsFile()) return;

    if (!previewFormatManager) {
        previewFormatManager = std::make_unique<juce::AudioFormatManager>();
        previewFormatManager->registerBasicFormats();
    }
    std::unique_ptr<juce::AudioFormatReader> reader(previewFormatManager->createReaderFor(wav));
    if (!reader) return;

    stopVoiceTake();

    juce::ScopedLock sl(previewLock);
    previewTransport = std::make_unique<juce::AudioTransportSource>();
    previewTransport->setSource(new juce::AudioFormatReaderSource(reader.release(), true));
    previewPlayer = std::make_unique<juce::AudioSourcePlayer>();
    previewPlayer->setSource(previewTransport.get());

    double const sr = dm->getCurrentAudioDevice() ? dm->getCurrentAudioDevice()->getCurrentSampleRate() : 48000.0;
    int const bs = dm->getCurrentAudioDevice() ? dm->getCurrentAudioDevice()->getCurrentBufferSizeSamples() : 512;
    previewTransport->prepareToPlay(bs, sr);
    dm->addAudioCallback(previewPlayer.get());
    previewTransport->start();
    previewTakeId = takeId;

    double const lengthSec = previewTransport->getLengthInSeconds();
    juce::Timer::callAfterDelay(static_cast<int>((lengthSec + 0.15) * 1000.0), [this, takeId] {
        if (getPlayingVoiceTakeId() == takeId) stopVoiceTake();
    });
}

void MCPBridge::stopVoiceCaptureAndAnalyze()
{
    if (!voiceCapturing.exchange(false))
        return;
    analyzeCapturedVoice();
}

void MCPBridge::analyzeCapturedVoice()
{
    int const totalSamples = voiceCaptureWritePos.load(std::memory_order_acquire);
    if (totalSamples < 2048) {
        sendConsoleLog("MCP: Voice recording too short (<50ms)", false);
        return;
    }

    double const sampleRate = processor ? processor->getSampleRate() : 48000.0;
    int const declaredMode = voiceCaptureMode.exchange(0, std::memory_order_acq_rel);
    sendConsoleLog("MCP: Analyzing voice take (" + juce::String(totalSamples) + " samples)...", false);

    runJob([this, totalSamples, sampleRate, declaredMode] { // P3.4 S8: owned, joined at shutdown
        parseVoiceBufferToJson(voiceCaptureBuffer.data(), totalSamples, sampleRate, declaredMode);
    });
}

// ── MIDI Melodic-Intent Capture ──────────────────────────────────────────────
// The artist plays a MIDI keyboard; we log EXACT note events (no transcription:
// no YIN, no octave referee, no tuning guesswork). Monitor-through is handled by
// the app's normal MIDI path (Pd [notein]); this only records + stages.
void MCPBridge::startMidiCapture(int maxSeconds, const juce::String& voiceBinding)
{
    if (midiCapturing.load(std::memory_order_relaxed)) return;

    juce::ScopedLock sl(midiCaptureLock);
    midiCaptureEvents.clear();
    MidiRawEvent stale;
    while (midiCaptureRing.try_dequeue(stale)) { }
    midiCaptureDropped.store(0, std::memory_order_relaxed);
    midiCaptureAutoStopPending.store(false, std::memory_order_relaxed);
    midiCapturing.store(false, std::memory_order_relaxed);
    if (!isTimerRunning())
        startTimer(15); // P3.4 S6: the timer pairs the ring's events and acts on auto-stop

    int const sr = processor ? static_cast<int>(processor->getSampleRate()) : 48000;
    midiCaptureSamples.store(0, std::memory_order_release);
    midiCaptureMaxSamples.store(sr * juce::jlimit(1, 300, maxSeconds), std::memory_order_release);
    midiCaptureVoice = voiceBinding.isEmpty() ? juce::String("canvas") : voiceBinding;
    midiCaptureStartBpm = transport.running.load() ? static_cast<int>(transport.bpm.load()) : 0;

    // Reset Option B monitor voices
    for (auto& v : monitorVoices) {
        v.active = false;
        v.pitch = -1;
        v.env = 0.0f;
    }

    midiCapturing.store(true, std::memory_order_release);

    sendConsoleLog("MCP: Recording MIDI - play! (tap again to stop)", false);
    sendReply("/pd/midi/event", "{\"status\":\"armed\",\"voice\":\"" + jsonEscape(midiCaptureVoice) + "\"}");
}

void MCPBridge::midiInputTick(const juce::MidiBuffer& midi, int numSamples, juce::AudioBuffer<float>* audioBuffer, double sampleRate)
{
    bool const capturing = midiCapturing.load(std::memory_order_acquire);
    bool const playing = midiPlaying.load(std::memory_order_acquire);
    if (!capturing && !playing) return;

    double const effectiveSr = (sampleRate > 1000.0) ? sampleRate : 48000.0;

    // ── 1. Recording Live Input ──────────────────────────────
    // P3.4 S6: raw events into a preallocated ring (no lock, no allocation); the timer pairs them.
    if (capturing) {
        juce::int64 const blockStart = midiCaptureSamples.fetch_add(numSamples, std::memory_order_acq_rel);
        int const maxSamples = midiCaptureMaxSamples.load(std::memory_order_relaxed);

        for (auto const meta : midi) {
            auto const msg = meta.getMessage();
            if (msg.isNoteOn() || msg.isNoteOff()) {
                MidiRawEvent const raw { msg.isNoteOn(), msg.getNoteNumber(), msg.getVelocity(), msg.getChannel(),
                                         blockStart + meta.samplePosition };
                if (!midiCaptureRing.try_enqueue(raw))
                    midiCaptureDropped.fetch_add(1, std::memory_order_relaxed);
            }

            if (msg.isNoteOn()) {
                // Option B: Auto-Monitor-Through voice allocation for immediate auditory feedback
                if (monitorThroughEnabled.load(std::memory_order_relaxed)) {
                    int voiceIdx = -1;
                    float lowestEnv = 100.0f;
                    for (int i = 0; i < kMaxMonitorVoices; ++i) {
                        if (!monitorVoices[i].active) {
                            voiceIdx = i;
                            break;
                        }
                        if (monitorVoices[i].env < lowestEnv) {
                            lowestEnv = monitorVoices[i].env;
                            voiceIdx = i;
                        }
                    }
                    if (voiceIdx >= 0) {
                        auto& v = monitorVoices[voiceIdx];
                        v.pitch = msg.getNoteNumber();
                        v.velocity = static_cast<float>(msg.getVelocity()) / 127.0f;
                        float const freq = 440.0f * std::pow(2.0f, static_cast<float>(v.pitch - 69) / 12.0f);
                        v.phaseInc = static_cast<float>(2.0 * juce::MathConstants<double>::pi * freq / effectiveSr);
                        v.phase = 0.0f;
                        v.env = v.velocity;
                        v.active = true;
                        v.released = false;
                    }
                }
            } else if (msg.isNoteOff()) {
                if (monitorThroughEnabled.load(std::memory_order_relaxed)) {
                    for (int i = 0; i < kMaxMonitorVoices; ++i) {
                        if (monitorVoices[i].active && monitorVoices[i].pitch == msg.getNoteNumber()) {
                            monitorVoices[i].released = true;
                        }
                    }
                }
            }
        }

        if (blockStart >= maxSamples)
            midiCaptureAutoStopPending.store(true, std::memory_order_release); // the timer stops and stages
    }

    // ── 2. Playback of Staged Take ───────────────────────────
    if (playing) {
        juce::int64 const pStart = midiPlaybackPos.fetch_add(numSamples, std::memory_order_acq_rel);
        juce::int64 const pEnd = pStart + numSamples;
        juce::int64 const totalEnd = midiPlaybackEndSample.load(std::memory_order_relaxed);

        // P3.4 S6: never wait for the take's writer (it fills the notes before setting midiPlaying).
        juce::ScopedTryLock const sl(midiPlaybackLock);
        if (sl.isLocked()) {
            for (auto const& pn : midiPlaybackNotes) {
                // Note-on within this block
                if (pn.startSample >= pStart && pn.startSample < pEnd) {
                    int voiceIdx = -1;
                    float lowestEnv = 100.0f;
                    for (int i = 0; i < kMaxMonitorVoices; ++i) {
                        if (!monitorVoices[i].active) {
                            voiceIdx = i;
                            break;
                        }
                        if (monitorVoices[i].env < lowestEnv) {
                            lowestEnv = monitorVoices[i].env;
                            voiceIdx = i;
                        }
                    }
                    if (voiceIdx >= 0) {
                        auto& v = monitorVoices[voiceIdx];
                        v.pitch = pn.pitch;
                        v.velocity = pn.velocity;
                        float const freq = 440.0f * std::pow(2.0f, static_cast<float>(v.pitch - 69) / 12.0f);
                        v.phaseInc = static_cast<float>(2.0 * juce::MathConstants<double>::pi * freq / effectiveSr);
                        v.phase = 0.0f;
                        v.env = v.velocity;
                        v.active = true;
                        v.released = false;
                    }
                }
                // Note-off within this block
                if (pn.endSample >= pStart && pn.endSample < pEnd) {
                    for (int i = 0; i < kMaxMonitorVoices; ++i) {
                        if (monitorVoices[i].active && monitorVoices[i].pitch == pn.pitch) {
                            monitorVoices[i].released = true;
                        }
                    }
                }
            }
        }

        // P3.4 S6: past the end nothing more sounds; the callAfterDelay set in startMidiTake stops the take
        // (the audio thread used to callAsync stopMidiTake on every block after the end until it landed).
        ignoreUnused(totalEnd);
    }

    // ── 3. Synth Audio Generation (Warm EPiano on Blank Canvas) ──
    if (audioBuffer != nullptr && monitorThroughEnabled.load(std::memory_order_relaxed)) {
        float patchRms = 0.0f;
        if (audioBuffer->getNumChannels() > 0 && numSamples > 0) {
            patchRms = audioBuffer->getRMSLevel(0, 0, numSamples);
        }
        // If the patch is already sounding (> 0.02, ~ -34dB), duck monitor tone to avoid clashing
        float const monitorGain = (patchRms > 0.02f) ? 0.0f : 0.16f;

        if (monitorGain > 0.0f) {
            int const numChannels = audioBuffer->getNumChannels();
            for (int s = 0; s < numSamples; ++s) {
                float sampleSum = 0.0f;
                for (int i = 0; i < kMaxMonitorVoices; ++i) {
                    auto& v = monitorVoices[i];
                    if (!v.active) continue;

                    v.phase += v.phaseInc;
                    if (v.phase >= juce::MathConstants<float>::twoPi)
                        v.phase -= juce::MathConstants<float>::twoPi;

                    // Warm electric piano tone: fundamental + 22% 2nd harmonic + 6% 3rd harmonic
                    float const wave = std::sin(v.phase) + 0.22f * std::sin(2.0f * v.phase) + 0.06f * std::sin(3.0f * v.phase);
                    sampleSum += wave * v.env;

                    if (v.released) {
                        v.env *= 0.9993f; // ~300ms release tail
                        if (v.env < 0.0005f) {
                            v.active = false;
                        }
                    } else {
                        v.env *= 0.99996f; // natural acoustic decay
                    }
                }

                float const out = sampleSum * monitorGain;
                for (int ch = 0; ch < numChannels; ++ch) {
                    audioBuffer->addSample(ch, s, out);
                }
            }
        }
    }
}

int MCPBridge::getMidiCaptureNoteCount() const
{
    auto* self = const_cast<MCPBridge*>(this);
    juce::ScopedLock sl(self->midiCaptureLock);
    self->drainMidiCaptureRing();
    return static_cast<int>(midiCaptureEvents.size());
}

// Pairs the raw on/off events the audio thread queued (P3.4 S6). Caller holds midiCaptureLock. A note-off
// closes the newest open note of its pitch, as the audio thread used to do in place.
void MCPBridge::drainMidiCaptureRing()
{
    double const sr = (processor && processor->getSampleRate() > 1000.0) ? processor->getSampleRate() : 48000.0;
    MidiRawEvent raw;
    while (midiCaptureRing.try_dequeue(raw)) {
        if (raw.on) {
            MidiNoteEvent e;
            e.pitch = raw.pitch;
            e.velocity = raw.velocity;
            e.channel = raw.channel;
            e.sampleTime = raw.sampleTime;
            midiCaptureEvents.push_back(e);
        } else {
            for (auto it = midiCaptureEvents.rbegin(); it != midiCaptureEvents.rend(); ++it) {
                if (!it->closed && it->pitch == raw.pitch) {
                    it->closed = true;
                    it->durationSec = static_cast<double>(raw.sampleTime - it->sampleTime) / sr;
                    break;
                }
            }
        }
    }
}

void MCPBridge::stopMidiCaptureAndStage()
{
    if (!midiCapturing.exchange(false)) return;

    // Silence all Option B monitor voices
    for (auto& v : monitorVoices) {
        v.active = false;
        v.pitch = -1;
        v.env = 0.0f;
    }

    // Grab a snapshot of the events (audio thread may still be logging; it stops
    // because midiCapturing is now false).
    std::vector<MidiNoteEvent> events;
    {
        juce::ScopedLock sl(midiCaptureLock);
        drainMidiCaptureRing();
        events = midiCaptureEvents;
        midiCaptureEvents.clear();
    }

    if (events.size() < 1) {
        sendConsoleLog("MCP: MIDI recording empty", false);
        sendReply("/pd/midi/event", "{\"status\":\"empty\"}");
        return;
    }

    // Close any still-open notes at "now".
    double const sr = processor ? processor->getSampleRate() : 48000.0;
    for (auto& e : events) {
        if (!e.closed) {
            e.durationSec = juce::jmax(0.05, (midiCaptureSamples.load() - e.sampleTime) / sr);
        }
    }

    double const captureStartSample = static_cast<double>(events.front().sampleTime);
    double const captureEndSample = [&] {
        double mx = 0.0;
        for (auto const& e : events) mx = juce::jmax(mx, static_cast<double>(e.sampleTime) + e.durationSec * sr);
        return mx;
    }();
    double const durationSec = juce::jmax(0.1, (captureEndSample - captureStartSample) / sr);

    juce::Array<juce::var> notesArray;
    for (auto const& e : events) {
        auto* noteObj = new juce::DynamicObject();
        noteObj->setProperty("pitch", e.pitch);
        noteObj->setProperty("start", (e.sampleTime - captureStartSample) / sr);
        noteObj->setProperty("duration", e.durationSec);
        noteObj->setProperty("vel", e.velocity);
        notesArray.add(juce::var(noteObj));
    }

    auto* root = new juce::DynamicObject();
    juce::String const takeId = "midi_" + juce::String(juce::Time::getCurrentTime().toMilliseconds());
    root->setProperty("id", takeId);
    root->setProperty("mode", "midi");
    root->setProperty("bpm", midiCaptureStartBpm);
    root->setProperty("durationSec", durationSec);
    root->setProperty("voice", midiCaptureVoice);
    root->setProperty("noteCount", static_cast<int>(events.size()));
    root->setProperty("notes", notesArray);
    juce::String const jsonStr = juce::JSON::toString(juce::var(root));

    juce::File("/home/alphi/Desktop/plugdata/mcp-server/midi-take.json").replaceWithText(jsonStr);

    // Register as a first-class moodboard artifact.
    McpSession::McpMidiTake take;
    take.id = takeId;
    take.json = jsonStr;
    take.voice = midiCaptureVoice;
    take.bpm = midiCaptureStartBpm;
    take.noteCount = static_cast<int>(events.size());
    take.durationSec = durationSec;
    take.visible = true;
    if (processor) processor->getMcp()->session.addMcpMidiTake(std::move(take));

    sendReply("/pd/midi/event", jsonStr);
    sendConsoleLog("MCP: MIDI take staged -> " + takeId + " (" + juce::String(events.size())
                   + " notes, voice: " + midiCaptureVoice + ")", false);
}


void MCPBridge::playMidiTake(const juce::String& takeId, const juce::String& jsonStr)
{
    if (getPlayingMidiTakeId() == takeId) {
        stopMidiTake();
        return;
    }

    stopMidiTake();
    stopVoiceTake();

    auto parsed = juce::JSON::parse(jsonStr);
    if (!parsed.isObject()) return;

    auto* root = parsed.getDynamicObject();
    if (!root) return;

    auto notesVar = root->getProperty("notes");
    if (!notesVar.isArray()) return;

    auto* notesArr = notesVar.getArray();
    if (!notesArr || notesArr->isEmpty()) return;

    double const sr = processor ? processor->getSampleRate() : 48000.0;
    double const durationSec = static_cast<double>(root->getProperty("durationSec"));

    std::vector<MidiPlaybackNote> notes;
    for (auto const& n : *notesArr) {
        if (!n.isObject()) continue;
        auto* no = n.getDynamicObject();
        if (!no) continue;

        MidiPlaybackNote pn;
        pn.pitch = static_cast<int>(no->getProperty("pitch"));
        double const startSec = static_cast<double>(no->getProperty("start"));
        double const durSec = static_cast<double>(no->getProperty("duration"));
        int const vel = static_cast<int>(no->getProperty("vel"));

        pn.velocity = (vel > 0) ? (static_cast<float>(vel) / 127.0f) : 0.8f;
        pn.startSample = static_cast<juce::int64>(startSec * sr);
        pn.endSample = pn.startSample + static_cast<juce::int64>(juce::jmax(0.05, durSec) * sr);
        notes.push_back(pn);
    }

    if (notes.empty()) return;

    // Reset monitor voices
    for (auto& v : monitorVoices) {
        v.active = false;
        v.pitch = -1;
        v.env = 0.0f;
    }

    {
        juce::ScopedLock sl(midiPlaybackLock);
        midiPlaybackNotes = std::move(notes);
        midiPlaybackPos.store(0, std::memory_order_release);
        midiPlaybackEndSample.store(static_cast<juce::int64>((durationSec + 0.3) * sr), std::memory_order_release);
        playingMidiTakeId = takeId;
        midiPlaying.store(true, std::memory_order_release);
    }

    juce::Timer::callAfterDelay(static_cast<int>((durationSec + 0.4) * 1000.0), [this, takeId] {
        if (getPlayingMidiTakeId() == takeId) {
            stopMidiTake();
        }
    });

    sendConsoleLog("MCP: Auditioning MIDI take -> " + takeId, false);
}

void MCPBridge::stopMidiTake()
{
    if (!midiPlaying.exchange(false)) {
        playingMidiTakeId.clear();
        return;
    }
    playingMidiTakeId.clear();

    for (auto& v : monitorVoices) {
        v.active = false;
        v.pitch = -1;
        v.env = 0.0f;
    }

    if (processor) {
        // P3.4 S3: the caller may be a timer, the audio-side stop path or the command worker.
        postMessage([p = processor] { p->getMcp()->gui().repaintAll(); });
    }
}

void MCPBridge::parseVoiceBufferToJson(float const* buffer, int totalSamples, double sampleRate, int forcedMode)
{
    if (!buffer || totalSamples < 1024 || sampleRate <= 0.0) return;

    int const N = 1024;
    int const HOP = 512;
    int const numFrames = (totalSamples - N) / HOP;
    if (numFrames <= 0) return;

    // Global stats pass
    double sumSq = 0.0;
    float globalPeak = 0.0f;
    for (int i = 0; i < totalSamples; ++i) {
        float const s = std::abs(buffer[i]);
        sumSq += static_cast<double>(buffer[i] * buffer[i]);
        if (s > globalPeak) globalPeak = s;
    }
    float const globalRms = static_cast<float>(std::sqrt(sumSq / totalSamples));

    if (globalRms < 0.001f) {
        auto* rep = new juce::DynamicObject();
        rep->setProperty("status", "silent");
        rep->setProperty("rms", globalRms);
        rep->setProperty("message", "No audio detected from microphone");
        juce::String const jsonStr = juce::JSON::toString(juce::var(rep));
        sendReply("/pd/voice/event", jsonStr);
        juce::File("/home/alphi/Desktop/plugdata/mcp-server/voice-take.json").replaceWithText(jsonStr);
        sendConsoleLog("MCP: Voice recording silent (RMS: " + juce::String(globalRms, 4) + ")", true);
        return;
    }

    std::vector<float> frameRms(numFrames, 0.0f);
    std::vector<float> framePitch(numFrames, 0.0f);
    std::vector<float> frameCentroid(numFrames, 0.0f);
    std::vector<float> frameFlatness(numFrames, 0.0f);
    std::vector<float> frameConf(numFrames, 0.0f);

    for (int f = 0; f < numFrames; ++f) {
        float const* framePtr = buffer + f * HOP;
        double frameSumSq = 0.0;
        for (int i = 0; i < N; ++i) {
            frameSumSq += static_cast<double>(framePtr[i] * framePtr[i]);
        }
        frameRms[f] = static_cast<float>(std::sqrt(frameSumSq / N));

        auto specVar = spectralVarFromSamples(framePtr, N, sampleRate);
        if (auto* specObj = specVar.getDynamicObject()) {
            frameCentroid[f] = static_cast<float>(specObj->getProperty("spectralCentroid"));
            frameFlatness[f] = static_cast<float>(specObj->getProperty("spectralFlatness"));
        }
        if (forcedMode == 1) {
            // Declared melody → precision lane: YIN + per-frame confidence.
            framePitch[f] = estimatePitchYIN(framePtr, N, static_cast<float>(sampleRate), frameConf[f]);
        } else {
            framePitch[f] = estimateFrequency(framePtr, N, static_cast<float>(sampleRate));
        }
    }

    // Adaptive noise/onset thresholds relative to recording
    float const rmsFloor = std::max(0.003f, globalRms * 0.20f);
    float const diffFloor = std::max(0.003f, globalRms * 0.15f);

    // Onset detection (transient / energy jump)
    struct OnsetHit {
        double timeSec;
        float velocity;
        float centroid;
        juce::String type;
    };
    std::vector<OnsetHit> hits;

    for (int f = 1; f < numFrames; ++f) {
        float const diff = frameRms[f] - frameRms[f - 1];
        if (diff > diffFloor && frameRms[f] > rmsFloor) {
            double const t = (f * HOP) / sampleRate;
            if (hits.empty() || (t - hits.back().timeSec > 0.08)) {
                OnsetHit h;
                h.timeSec = t;
                h.velocity = juce::jlimit(0.1f, 1.0f, (frameRms[f] / std::max(0.01f, globalPeak)) * 1.2f);
                h.centroid = frameCentroid[f];
                if (h.centroid < 450.0f)
                    h.type = "kick";
                else if (h.centroid > 2600.0f && frameFlatness[f] > 0.20f)
                    h.type = "hat";
                else
                    h.type = "snare";
                hits.push_back(h);
            }
        }
    }

    // Melodic note segmentation
    struct MelodicNote {
        int midiNote;
        float freqHz;
        double startSec;
        double durationSec;
        float velocity;
        float conf = 0.0f;
        bool onsetAttack = false;
    };
    std::vector<MelodicNote> notes;

    if (forcedMode == 1) {
        // ── Precision melody pipeline (declared melody) ──────────────────────
        // 1) OCTAVE-FOLD + continuity: 3-state DP over {f/2, f, 2f} picks the
        //    octave nearest the running reference (greedy Viterbi). Kills the
        //    classic octave errors.
        // 2) 5-tap median filter on the folded track (single-frame spikes).
        // 3) Segmentation: same note while within 90 cents; 3 dead frames end it.
        // 4) Merge same-pitch notes across <70ms breath gaps.
        std::vector<float> track(numFrames, 0.0f);
        std::vector<float> trackConf(numFrames, 0.0f);

        std::vector<float> voiced;
        voiced.reserve(static_cast<size_t>(numFrames));
        for (int f = 0; f < numFrames; ++f) {
            if (framePitch[f] > 55.0f && framePitch[f] < 1600.0f && frameConf[f] > 0.4f && frameRms[f] > rmsFloor) {
                voiced.push_back(framePitch[f]);
            }
        }
        float reference = 0.0f;
        if (!voiced.empty()) {
            std::sort(voiced.begin(), voiced.end());
            reference = voiced[voiced.size() / 2]; // median → robust to octave jumps
        }

        for (int f = 0; f < numFrames; ++f) {
            float const hz0 = framePitch[f];
            if (!(hz0 > 55.0f && hz0 < 1600.0f && frameConf[f] > 0.35f && frameRms[f] > rmsFloor)) continue;
            float const candidates[3] = { hz0 * 0.5f, hz0, hz0 * 2.0f };
            float const ref = reference > 0.0f ? reference : hz0;
            float best = hz0;
            float bestCents = 1e9f;
            for (float c : candidates) {
                if (c < 55.0f || c > 1600.0f) continue;
                float const cents = std::abs(120.0f * std::log2(c / ref));
                if (cents < bestCents) {
                    bestCents = cents;
                    best = c;
                }
            }
            track[f] = best;
            trackConf[f] = frameConf[f];
            reference = 0.85f * reference + 0.15f * best;
        }

        std::vector<float> smoothed = track;
        for (int f = 0; f < numFrames; ++f) {
            if (track[f] <= 0.0f) continue;
            float window[5];
            int count = 0;
            for (int k = -2; k <= 2; ++k) {
                int const idx = f + k;
                if (idx >= 0 && idx < numFrames && track[idx] > 0.0f) window[count++] = track[idx];
            }
            for (int a = 1; a < count; ++a) {
                for (int b = a; b > 0 && window[b] < window[b - 1]; --b) std::swap(window[b], window[b - 1]);
            }
            smoothed[f] = window[count / 2];
        }

        // Onset flags (attack frames) for onset-gated splitting.
        std::vector<char> onsetFlag(numFrames, 0);
        for (auto const& h : hits) {
            int const hf = static_cast<int>(std::round(h.timeSec * sampleRate / HOP));
            for (int d = -1; d <= 1; ++d) {
                int const idx = hf + d;
                if (idx >= 0 && idx < numFrames) onsetFlag[idx] = 1;
            }
        }

        int segStart = -1;
        int unvoicedRun = 0;
        float segAnchor = 0.0f;   // pitch at note onset: FIXED stability reference
        float segPitch = 0.0f;    // running estimate (for reporting)
        float segConfSum = 0.0f;
        int segFrames = 0;
        float segPeakRms = 0.0f;
        int overRun = 0;          // consecutive frames past the sustained-move gate
        bool segFromOnset = false;

        auto flushNote = [&](int endFrame) {
            if (segStart < 0) return;
            double const dur = (endFrame - segStart) * static_cast<double>(HOP) / sampleRate;
            if (dur >= 0.045 && segFrames > 0) {
                MelodicNote n;
                n.freqHz = segPitch;
                n.midiNote = juce::jlimit(24, 96, static_cast<int>(std::round(69.0 + 12.0 * std::log2(segPitch / 440.0))));
                n.startSec = segStart * static_cast<double>(HOP) / sampleRate;
                n.durationSec = dur;
                n.velocity = juce::jlimit(0.2f, 1.0f, (segPeakRms / std::max(0.01f, globalPeak)) * 1.2f);
                n.conf = segConfSum / static_cast<float>(segFrames);
                n.onsetAttack = segFromOnset;
                notes.push_back(n);
            }
            segStart = -1;
            segFrames = 0;
            segConfSum = 0.0f;
            segPeakRms = 0.0f;
            overRun = 0;
            segFromOnset = false;
        };

        auto startNote = [&](int f, float hz, bool fromOnset) {
            segStart = f;
            segAnchor = hz;
            segPitch = hz;
            segConfSum = trackConf[f];
            segFrames = 1;
            segPeakRms = frameRms[f];
            overRun = 0;
            segFromOnset = fromOnset;
        };

        for (int f = 0; f < numFrames; ++f) {
            float const hz = smoothed[f];
            bool const voicedFrame = hz > 0.0f && trackConf[f] > 0.35f;
            if (!voicedFrame) {
                if (segStart >= 0 && ++unvoicedRun >= 3) flushNote(f - unvoicedRun + 1);
                continue;
            }
            unvoicedRun = 0;
            if (segStart < 0) { startNote(f, hz, onsetFlag[f] != 0); continue; }

            // Compare against the note's START pitch (fixed anchor). The old EMA
            // reference drifted with the voice, so glides never split a phrase.
            float const centsAnchor = std::abs(120.0f * std::log2(hz / segAnchor));
            bool const onsetHere = onsetFlag[f] != 0;
            int const ageFrames = f - segStart;

            // Split rules:
            //  a) clear leap (>= 180 cents) — immediate;
            //  b) sustained move (>= 100 cents held 3 frames, ~35ms);
            //  c) fresh onset after the first frames (re-articulation / new syllable).
            bool const leap = centsAnchor >= 180.0f;
            bool const onsetSplit = onsetHere && ageFrames >= 2;
            overRun = (centsAnchor >= 100.0f) ? overRun + 1 : 0;
            bool const sustainedMove = overRun >= 3;

            if (leap || sustainedMove || onsetSplit) {
                flushNote(f);
                startNote(f, hz, onsetHere);
            } else {
                segPitch = 0.85f * segPitch + 0.15f * hz;
                segConfSum += trackConf[f];
                segFrames++;
                segPeakRms = std::max(segPeakRms, frameRms[f]);
            }
        }
        flushNote(numFrames);

        // Merge same-midi neighbors across a short breath gap — but never merge
        // a re-articulated note (onset attack): that separation IS the rhythm.
        // EXCEPTION (jitter-split merge, 2026-10-06): when BOTH segments are
        // micro-short (<200ms) with a near-touching gap (<60ms) and the pitch
        // is within 30 cents, the split is tracker jitter (an onset-attacked
        // micro pair inside one sung gesture) — merge it; keep real repeats.
        if (notes.size() >= 2) {
            std::vector<MelodicNote> merged;
            for (auto const& n : notes) {
                if (!merged.empty()) {
                    auto& prev = merged.back();
                    double const gap = n.startSec - (prev.startSec + prev.durationSec);
                    bool const closePitch = std::abs(120.0f * std::log2(n.freqHz / prev.freqHz)) < 30.0f;
                    bool const microPair = (n.durationSec < 0.20 && prev.durationSec < 0.20 && gap < 0.06);
                    if ((!n.onsetAttack || microPair) && (n.midiNote == prev.midiNote || closePitch) && gap < 0.07) {
                        prev.freqHz = static_cast<float>((prev.freqHz * prev.durationSec + n.freqHz * n.durationSec) / std::max(1e-6, prev.durationSec + n.durationSec));
                        prev.durationSec = prev.durationSec + n.durationSec + std::max(0.0, gap);
                        prev.conf = 0.5f * (prev.conf + n.conf);
                        continue;
                    }
                }
                merged.push_back(n);
            }
            notes = std::move(merged);
        }
    } else {
        // Legacy path (auto / beat): keep the original naive segmentation.
        int noteStartFrame = -1;
        float noteAccumPitch = 0.0f;
        int noteFrameCount = 0;
        float noteMaxRms = 0.0f;

        for (int f = 0; f < numFrames; ++f) {
            float const pitch = framePitch[f];
            bool const isVoiced = (pitch >= 50.0f && pitch <= 1400.0f && frameRms[f] > rmsFloor);

            if (isVoiced) {
                if (noteStartFrame < 0) {
                    noteStartFrame = f;
                    noteAccumPitch = pitch;
                    noteFrameCount = 1;
                    noteMaxRms = frameRms[f];
                } else {
                    float const avgPitch = noteAccumPitch / noteFrameCount;
                    float const semitoneDiff = std::abs(12.0f * std::log2(pitch / avgPitch));
                    if (semitoneDiff < 1.5f) {
                        noteAccumPitch += pitch;
                        noteFrameCount++;
                        if (frameRms[f] > noteMaxRms) noteMaxRms = frameRms[f];
                    } else {
                        if (noteFrameCount >= 3) {
                            float const finalHz = noteAccumPitch / noteFrameCount;
                            int const midi = juce::jlimit(24, 96, static_cast<int>(std::round(69.0 + 12.0 * std::log2(finalHz / 440.0))));
                            notes.push_back({ midi, finalHz, (noteStartFrame * HOP) / sampleRate, (noteFrameCount * HOP) / sampleRate, juce::jlimit(0.2f, 1.0f, (noteMaxRms / std::max(0.01f, globalPeak)) * 1.2f) });
                        }
                        noteStartFrame = f;
                        noteAccumPitch = pitch;
                        noteFrameCount = 1;
                        noteMaxRms = frameRms[f];
                    }
                }
            } else {
                if (noteStartFrame >= 0) {
                    if (noteFrameCount >= 3) {
                        float const finalHz = noteAccumPitch / noteFrameCount;
                        int const midi = juce::jlimit(24, 96, static_cast<int>(std::round(69.0 + 12.0 * std::log2(finalHz / 440.0))));
                        notes.push_back({ midi, finalHz, (noteStartFrame * HOP) / sampleRate, (noteFrameCount * HOP) / sampleRate, juce::jlimit(0.2f, 1.0f, (noteMaxRms / std::max(0.01f, globalPeak)) * 1.2f) });
                    }
                    noteStartFrame = -1;
                }
            }
        }
        if (noteStartFrame >= 0 && noteFrameCount >= 3) {
            float const finalHz = noteAccumPitch / noteFrameCount;
            int const midi = juce::jlimit(24, 96, static_cast<int>(std::round(69.0 + 12.0 * std::log2(finalHz / 440.0))));
            notes.push_back({ midi, finalHz, (noteStartFrame * HOP) / sampleRate, (noteFrameCount * HOP) / sampleRate, juce::jlimit(0.2f, 1.0f, (noteMaxRms / std::max(0.01f, globalPeak)) * 1.2f) });
        }
    }

    juce::String mode = "beatbox";
    if (notes.size() >= 2) {
        double totalNoteDuration = 0.0;
        for (auto const& n : notes) totalNoteDuration += n.durationSec;
        if (totalNoteDuration > 0.35)
            mode = "melody";
    }

    int estimatedBpm = 120;
    if (hits.size() >= 3) {
        std::vector<double> intervals;
        for (size_t i = 1; i < hits.size(); ++i) {
            double const dt = hits[i].timeSec - hits[i-1].timeSec;
            if (dt > 0.1 && dt < 1.5)
                intervals.push_back(dt);
        }
        if (!intervals.empty()) {
            double avgDt = 0.0;
            for (auto dt : intervals) avgDt += dt;
            avgDt /= static_cast<double>(intervals.size());
            double bpm = 60.0 / avgDt;
            while (bpm < 75.0) bpm *= 2.0;
            while (bpm > 165.0) bpm /= 2.0;
            estimatedBpm = juce::jlimit(60, 200, static_cast<int>(std::round(bpm)));
        }
    }

    // Declared intent beats inference: if the artist chose Melody or Beat before
    // recording, that mode is truth — no guessing (an FX take never dresses as
    // "6 hats" again). Auto mode (0) keeps the heuristic for legacy paths.
    if (forcedMode == 1) mode = "melody";
    else if (forcedMode == 2) mode = "beatbox";

    auto* root = new juce::DynamicObject();
    root->setProperty("mode", mode);
    root->setProperty("bpm", estimatedBpm);
    root->setProperty("durationSec", static_cast<float>(totalSamples / sampleRate));
    root->setProperty("peak", globalPeak);
    root->setProperty("rms", globalRms);

    juce::Array<juce::var> hitsArray;
    for (auto const& h : hits) {
        auto* hitObj = new juce::DynamicObject();
        hitObj->setProperty("type", h.type);
        hitObj->setProperty("time", static_cast<float>(h.timeSec));
        hitObj->setProperty("vel", h.velocity);
        hitObj->setProperty("centroid", h.centroid);
        hitsArray.add(juce::var(hitObj));
    }
    root->setProperty("hits", hitsArray);

    // ── Spectral octave referee ──────────────────────────────────────────
    // The YIN+fold pipeline can land a whole note an octave HIGH when the
    // fundamental is weak (quiet hums): sub-octave energy at f/2 is the
    // giveaway — natural voices do not energize below their fundamental.
    // For each note, compare windowed DFT magnitude at f/2 vs f across its
    // frames; ratio > 0.5 → halve (loop twice to catch double-octave slips).
    // FFT referee validated 2026-10-06: held D3 147Hz (480k) vs D4 294Hz
    // (137k) while the fold claimed D4.
    {
        auto toneMag = [&](float const* frame, float freq) {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < N; ++i) {
                double const w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (N - 1));
                double const ph = 2.0 * juce::MathConstants<double>::pi * freq * i / sampleRate;
                re += frame[i] * w * std::cos(ph);
                im += frame[i] * w * std::sin(ph);
            }
            return std::sqrt(re * re + im * im);
        };

        for (auto& n : notes) {
            for (int pass = 0; pass < 2; ++pass) {
                int const f0 = juce::jlimit(0, numFrames - 1, static_cast<int>(n.startSec * sampleRate / HOP));
                int const f1 = juce::jlimit(f0 + 1, numFrames, static_cast<int>((n.startSec + n.durationSec) * sampleRate / HOP));
                double sumFull = 0.0, sumHalf = 0.0;
                int cnt = 0;
                for (int f = f0; f < f1; ++f) {
                    float const* fp = buffer + f * HOP;
                    sumFull += toneMag(fp, n.freqHz);
                    sumHalf += toneMag(fp, n.freqHz * 0.5f);
                    ++cnt;
                }
                if (cnt <= 0) break;
                double const ratio = sumHalf / std::max(1.0, sumFull);
                if (ratio > 0.5 && n.freqHz * 0.5f >= 40.0f) {
                    n.freqHz *= 0.5f;
                    n.midiNote = juce::jlimit(24, 96, static_cast<int>(std::round(69.0 + 12.0 * std::log2(n.freqHz / 440.0))));
                } else {
                    break;
                }
            }
        }
    }

    juce::Array<juce::var> notesArray;
    for (auto const& n : notes) {
        auto* noteObj = new juce::DynamicObject();
        noteObj->setProperty("midi", n.midiNote);
        noteObj->setProperty("freq", n.freqHz);
        noteObj->setProperty("start", static_cast<float>(n.startSec));
        noteObj->setProperty("duration", static_cast<float>(n.durationSec));
        noteObj->setProperty("vel", n.velocity);
        noteObj->setProperty("conf", n.conf);
        // 12-TET snap companion: raw freqHz stays untouched as the honest
        // measurement; freqSnap is the in-tune playback pitch (midiNote is
        // already the rounded semitone); centsOff shows the performance drift.
        {
            float const snapHz = 440.0f * std::pow(2.0f, static_cast<float>(n.midiNote - 69) / 12.0f);
            noteObj->setProperty("freqSnap", snapHz);
            noteObj->setProperty("centsOff", 1200.0f * std::log2(n.freqHz / snapHz));
        }
        notesArray.add(juce::var(noteObj));
    }
    root->setProperty("notes", notesArray);

    juce::String const jsonStr = juce::JSON::toString(juce::var(root));

    // Save directly to disk
    juce::File("/home/alphi/Desktop/plugdata/mcp-server/voice-take.json").replaceWithText(jsonStr);

    // Phase 3 (slice): persist the take as a real WAV and register it as a
    // first-class moodboard artifact, so any agent can audition/analyse/route it.
    juce::String wavPath;
    {
        auto const takeDir = ProjectInfo::appDataDir.getChildFile("recordings");
        takeDir.createDirectory();
        auto const takeFile = takeDir.getChildFile("take_" + juce::String(juce::Time::getCurrentTime().toMilliseconds()) + ".wav");
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> stream(takeFile.createOutputStream());
        if (stream) {
            std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(stream.get(), sampleRate, 1, 16, {}, 0));
            if (writer) {
                stream.release(); // writer owns the stream on success
                juce::AudioBuffer<float> takeBuffer(1, totalSamples);
                takeBuffer.copyFrom(0, 0, buffer, totalSamples);
                if (writer->writeFromAudioSampleBuffer(takeBuffer, 0, totalSamples)) {
                    wavPath = takeFile.getFullPathName();
                }
            }
        }
    }
    if (processor) {
        McpSession::McpVoiceTake take;
        take.id = wavPath.isNotEmpty() ? juce::File(wavPath).getFileNameWithoutExtension()
                                       : "take_" + juce::String(juce::Time::getCurrentTime().toMilliseconds());
        take.wavPath = wavPath;
        take.json = jsonStr;
        take.mode = mode.toLowerCase();
        take.bpm = estimatedBpm;
        take.hitCount = static_cast<int>(hits.size());
        take.noteCount = static_cast<int>(notes.size());
        take.durationSec = static_cast<double>(totalSamples) / sampleRate;

        // 64-bucket peak envelope for the on-canvas chip waveform.
        {
            constexpr int numBuckets = 64;
            take.peaks.assign(numBuckets, 0.0f);
            int const per = juce::jmax(1, totalSamples / numBuckets);
            for (int bk = 0; bk < numBuckets; ++bk) {
                int const start = bk * per;
                int const end = juce::jmin(totalSamples, start + per);
                float peak = 0.0f;
                for (int i = start; i < end; ++i) {
                    peak = juce::jmax(peak, std::abs(buffer[i]));
                }
                take.peaks[bk] = peak;
            }
        }

        processor->getMcp()->session.addMcpVoiceTake(std::move(take));
    }

    sendReply("/pd/voice/event", jsonStr);
    sendConsoleLog("MCP: Voice take analyzed -> mode: " + mode + " (" + juce::String(hits.size()) + " hits, " + juce::String(notes.size()) + " notes, " + juce::String(estimatedBpm) + " BPM)", false);

    // STAGING LAW: the take parks (WAV on disk + Staging Bay row + moodboard rail).
    // Nothing is sent to the AI here — recording must never auto-build a patch.
    // The artist commits explicitly (Enter / ⚡), and the rail carries the take.
}

extern "C" int ugen_getsortno(void); // d_ugen.c: number of DSP sortings so far (P3.4 S5)

ProbeManager::ProbeManager(MCPBridge* owner)
    : bridge(owner)
{
}

void ProbeManager::audioTick()
{
    bool any = false;
    for (auto& probe : probes)
        any = any || probe.active.load(std::memory_order_relaxed);
    if (!any || !bridge || !bridge->processor)
        return;
    // P3.4 S5: read the probed connections only under the Pd lock (nothing frees them meanwhile) and only
    // while the DSP sort number still equals the one they were resolved at (a rebuild may have freed them).
    // A busy lock skips this block's reading; it never makes the audio thread wait.
    juce::ScopedTryLock const pdLock(bridge->processor->audioLock);
    if (!pdLock.isLocked() || pd_getdspstate() == 0)
        return;
    int const sortNo = ugen_getsortno();

    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_acquire)) continue; // pairs with startProbe's release (P3.4 S11, TSan)
        if (probe.epoch.load(std::memory_order_acquire) != sortNo) {
            probe.needsResolve.store(true, std::memory_order_release);
            continue;
        }

        auto* oc = probe.outconnect.load(std::memory_order_acquire);
        if (!oc) continue;

        auto* signal = outconnect_get_signal(oc);
        if (!signal || !signal->s_vec) continue;

        int n = signal->s_n;
        if (n <= 0) continue;
        float* vec = signal->s_vec;

        float sumSq = 0.0f, peak = 0.0f;
        for (int i = 0; i < n; i++) {
            float s = vec[i];
            float abs_s = std::abs(s);
            sumSq += s * s;
            if (abs_s > peak) peak = abs_s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(n));

        ProbeResult result { probe.probeId, rms, peak, n };
        probe.resultQueue.try_enqueue(result);

        int writePos = probe.ringWritePos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            probe.ringBuffer[(writePos + i) % PROBE_RING_SIZE].store(vec[i], std::memory_order_relaxed);
        }
        probe.ringWritePos.store((writePos + n) % PROBE_RING_SIZE, std::memory_order_release);
    }
}

int ProbeManager::startProbe(t_outconnect* oc, int const epoch, const juce::String& canvasName, const juce::String& tempId, int outletIndex, const juce::String& correlationId, int durationMs)
{
    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_acquire)) {
            ProbeResult discard;
            while (probe.resultQueue.try_dequeue(discard)) {}

            probe.probeId = nextProbeId.fetch_add(1);
            if (probe.probeId == 0) probe.probeId = nextProbeId.fetch_add(1);
            probe.correlationId = correlationId;
            probe.canvasName = canvasName;
            probe.tempId = tempId;
            probe.outletIndex = outletIndex;
            probe.durationMs = std::max(20, durationMs);
            probe.startTimeMs = juce::Time::getMillisecondCounter();
            probe.accRms = 0.0f;
            probe.accPeak = 0.0f;
            probe.accBlocks = 0;
            probe.ringWritePos.store(0, std::memory_order_relaxed);
            for (auto& s : probe.ringBuffer) s.store(0.0f, std::memory_order_relaxed);
            probe.needsResolve.store(false, std::memory_order_relaxed);
            probe.epoch.store(epoch, std::memory_order_release);
            probe.outconnect.store(oc, std::memory_order_release);
            probe.active.store(true, std::memory_order_release);
            return static_cast<int>(probe.probeId);
        }
    }
    return -1;
}

void ProbeManager::stopProbe(uint32_t probeId, const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_acquire) && probe.probeId == probeId) {
            ProbeResult item;
            while (probe.resultQueue.try_dequeue(item)) {
                probe.accRms += item.rms * item.rms;
                if (item.peak > probe.accPeak) probe.accPeak = item.peak;
                probe.accBlocks++;
            }

            float meanRms = (probe.accBlocks > 0) ? std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks)) : 0.0f;
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;

            float sampleRate = 44100.0f;
            if (bridge && bridge->processor) {
                sampleRate = static_cast<float>(bridge->processor->getSampleRate());
            }
            std::array<float, PROBE_RING_SIZE> ring;
            for (int i = 0; i < PROBE_RING_SIZE; i++) ring[i] = probe.ringBuffer[i].load(std::memory_order_relaxed);
            float freq = estimateFrequency(ring.data(), PROBE_RING_SIZE, sampleRate);

            auto corr = correlationId.isNotEmpty() ? correlationId : probe.correlationId;
            if (corr.isNotEmpty() && bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + corr) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

void ProbeManager::stopAllProbes(const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_acquire)) {
            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            if (probe.correlationId.isNotEmpty() && bridge) {
                bridge->sendReply("/meter/error/" + probe.correlationId, "Probe cancelled");
            }
        }
    }
    if (correlationId.isNotEmpty() && bridge) {
        bridge->sendRawReply("/meter/stop_all/reply/" + correlationId);
    }
}

void ProbeManager::collectResults()
{
    auto now = juce::Time::getMillisecondCounter();
    float sampleRate = 44100.0f;
    if (bridge && bridge->processor) {
        sampleRate = static_cast<float>(bridge->processor->getSampleRate());
    }

    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_acquire)) continue; // pairs with startProbe's release (P3.4 S11, TSan)

        // P3.4 S5: the DSP chain was rebuilt since the probe was resolved: resolve it again under the Pd lock,
        // or end it with an error when its source is gone (it used to read freed memory).
        if (probe.needsResolve.load(std::memory_order_acquire) && bridge) {
            juce::String err;
            sys_lock();
            t_outconnect* oc = bridge->resolveProbeTarget(probe.canvasName, probe.tempId, probe.outletIndex, err);
            int const sortNo = ugen_getsortno();
            if (oc) {
                probe.outconnect.store(oc, std::memory_order_release);
                probe.epoch.store(sortNo, std::memory_order_release);
                probe.needsResolve.store(false, std::memory_order_release);
            } else {
                probe.active.store(false, std::memory_order_release);
                probe.outconnect.store(nullptr, std::memory_order_release);
            }
            sys_unlock();
            if (!oc) {
                bridge->sendReply((probe.spectral ? "/meter/spectral/error/" : "/meter/error/") + probe.correlationId,
                                  "probe target deleted: " + (err.isNotEmpty() ? err : probe.tempId));
                probe.spectral = false;
                continue;
            }
        }

        ProbeResult item;
        while (probe.resultQueue.try_dequeue(item)) {
            probe.accRms += item.rms * item.rms;
            if (item.peak > probe.accPeak) probe.accPeak = item.peak;
            probe.accBlocks++;
        }

        if (now - probe.startTimeMs >= static_cast<juce::int64>(probe.durationMs)) {
            if (probe.accBlocks == 0) {
                juce::String errCorr = probe.correlationId;
                bool wasSpectral = probe.spectral;
                probe.spectral = false;
                probe.active.store(false, std::memory_order_release);
                probe.outconnect.store(nullptr, std::memory_order_release);
                if (bridge) {
                    juce::String errAddr = wasSpectral ? "/meter/spectral/error/" : "/meter/error/";
                    bridge->sendReply(errAddr + errCorr, "Timeout: No audio blocks processed (is DSP running?)");
                }
                continue;
            }

            float meanRms = std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks));
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;
            std::array<float, PROBE_RING_SIZE> linearBuf;
            int wPos = probe.ringWritePos.load(std::memory_order_acquire);
            for (int i = 0; i < PROBE_RING_SIZE; i++) {
                linearBuf[i] = probe.ringBuffer[(wPos + i) % PROBE_RING_SIZE].load(std::memory_order_relaxed);
            }
            float freq = estimateFrequency(linearBuf.data(), PROBE_RING_SIZE, sampleRate);

            if (probe.spectral && bridge) {
                // ONE FFT implementation: delegate to the shared helper (#78),
                // then overlay the probe's own windowed loudness/pitch + metadata
                // so the spectral result stays consistent with the non-spectral
                // /meter/result path (same rmsDb/peakDb/fundamental).
                juce::var specVar = spectralVarFromSamples(linearBuf.data(), PROBE_RING_SIZE, sampleRate);
                if (auto* rootObj = specVar.getDynamicObject()) {
                    rootObj->setProperty("rmsDb", rmsDb);
                    rootObj->setProperty("peakDb", peakDb);
                    rootObj->setProperty("fundamental", freq);
                    rootObj->setProperty("sampleRate", sampleRate);
                    rootObj->setProperty("blocks", static_cast<int>(probe.accBlocks));

                    juce::String jsonString = juce::JSON::toString(specVar, true);
                    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/spectral/result/" + probe.correlationId) };
                    rep.addArgument(jsonString);
                    bridge->sender.send(rep);
                } else {
                    bridge->sendReply("/meter/spectral/error/" + probe.correlationId, "spectral analysis failed");
                }
                probe.spectral = false;

            } else if (bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + probe.correlationId) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
        }
    }
}

int ProbeManager::getActiveProbeCount() const
{
    int count = 0;
    for (auto const& probe : probes) {
        if (probe.active.load(std::memory_order_acquire)) {
            count++;
        }
    }
    return count;
}

bool MCPBridge::activateProbing()
{
    if (!plugdata_debugging_enabled()) {
        set_plugdata_debugging_enabled(1);
        sys_lock();
        canvas_update_dsp();
        sys_unlock();
        probeManager.setDebugEnabledByUs(true);
        return true;
    }
    return false;
}

t_outconnect* MCPBridge::resolveProbeTarget(const juce::String& canvasName, const juce::String& targetId, int outletIndex, juce::String& errorOut)
{
    if (!processor) {
        errorOut = "Processor unavailable";
        return nullptr;
    }

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) {
        errorOut = "Canvas not found: " + canvasName;
        return nullptr;
    }

    t_gobj* gobj = processor->getMcp()->resolveStableId(canvasName, targetId);

    if (!gobj) {
        bool isNumber = targetId.isNotEmpty();
        for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
            if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                isNumber = false;
                break;
            }
        }
        if (isNumber) {
            int idx = targetId.getIntValue();
            gobj = glistObjectAt(cnv, idx);
        }
    }

    if (!gobj) {
        errorOut = "Object not found: " + targetId;
        return nullptr;
    }

    t_object* obj = pd::Interface::checkObject(gobj);
    if (!obj) {
        errorOut = "Target is not a valid Pd object: " + targetId;
        return nullptr;
    }

    t_outlet* outlet = obj->ob_outlet;
    for (int i = 0; i < outletIndex && outlet; i++) {
        outlet = outlet->o_next;
    }
    if (!outlet) {
        errorOut = "Outlet " + juce::String(outletIndex) + " not found on object " + targetId;
        return nullptr;
    }

    if (outlet->o_sym != gensym("signal")) {
        errorOut = "Outlet " + juce::String(outletIndex) + " is not a signal outlet (control rate)";
        return nullptr;
    }

    if (!outlet->o_connections) {
        errorOut = "Signal outlet has no connections";
        return nullptr;
    }

    return outlet->o_connections;
}

void MCPBridge::timerCallback()
{
    // P3.4 S6: what the audio thread only flags (it used to callAsync these from the callback).
    if (voiceAnalyzePending.exchange(false, std::memory_order_acq_rel))
        analyzeCapturedVoice();
    if (midiCaptureAutoStopPending.exchange(false, std::memory_order_acq_rel))
        stopMidiCaptureAndStage();
    if (midiCapturing.load(std::memory_order_relaxed)) {
        juce::ScopedLock sl(midiCaptureLock);
        drainMidiCaptureRing(); // keeps the ring short during a long take
    }

    probeManager.collectResults();

    // Monitor tap (solo/mute): re-resolve the target each tick so a deleted or
    // rewired node can never leave a dangling s_vec on the audio thread; if the
    // node is gone, the tap turns itself off.
    if (processor && processor->getMcp()->session.mcpTapMode.load(std::memory_order_relaxed) != McpSession::mcpTapOff
        && mcpTapTempId.isNotEmpty()) {
        juce::String err;
        // P3.4 S5: under the Pd lock (this ran unlocked), with the DSP sort number of the same moment.
        sys_lock();
        t_outconnect* oc = resolveProbeTarget(mcpTapCanvas, mcpTapTempId, mcpTapOutlet, err);
        t_signal* sig = oc ? outconnect_get_signal(oc) : nullptr;
        bool const ok = sig && sig->s_vec;
        if (ok) {
            processor->getMcp()->session.mcpTapEpoch.store(ugen_getsortno(), std::memory_order_relaxed);
            processor->getMcp()->session.mcpTapSig.store(static_cast<void*>(sig), std::memory_order_release);
        }
        sys_unlock();
        if (!ok) {
            processor->getMcp()->session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
            processor->getMcp()->session.mcpTapSig.store(nullptr, std::memory_order_relaxed);
            mcpTapTempId.clear();
        }
    }

    std::vector<MorphJob> activeJobs;
    std::vector<juce::String> completedIds;

    {
        const juce::ScopedLock sl(morphLock);
        for (auto& job : morphJobs) {
            job.currentStep++;
            float t = static_cast<float>(job.currentStep) / static_cast<float>(job.totalSteps);
            if (t > 1.0f) t = 1.0f;

            for (auto const& p : job.params) {
                float val = p.from + (p.to - p.from) * t;
                if (processor) {
                    processor->sendFloat(p.name.toRawUTF8(), val);
                }
            }

            if (job.currentStep >= job.totalSteps) {
                completedIds.push_back(job.correlationId);
            } else {
                activeJobs.push_back(job);
            }
        }
        morphJobs = activeJobs;
    }

    for (auto const& id : completedIds) {
        sendRawReply("/morph/run/done/" + id);
    }

    {
        const juce::ScopedLock sl(morphLock);
        // P3.4 S6: also keep ticking while a capture or the monitor tap needs it (the tap's re-resolve after
        // a DSP rebuild runs here too; /meter/tap never started the timer before).
        bool const tapOn = processor && processor->getMcp()->session.mcpTapMode.load(std::memory_order_relaxed) != McpSession::mcpTapOff;
        if (morphJobs.empty() && probeManager.getActiveProbeCount() == 0 && !tapOn
            && !midiCapturing.load(std::memory_order_relaxed) && !voiceCapturing.load(std::memory_order_relaxed)
            && !voiceAnalyzePending.load(std::memory_order_relaxed) && !midiCaptureAutoStopPending.load(std::memory_order_relaxed)) {
            stopTimer();
        }
    }
}

void MCPBridge::sendArtistNote(const juce::String& targetId, const juce::String& text)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/ui/note") };
    msg.addArgument(targetId);
    msg.addArgument(text);
    sender.send(msg);
}

void MCPBridge::sendSelectionTelemetry(const juce::String& selector, const SmallArray<pd::Atom>& list)
{
    if (!canReply()) return;

    juce::String addr;
    if (selector.startsWith("/")) addr = selector;
    else if (selector == "selected_object") addr = "/pd/ui/selection";
    else if (selector == "selection_count") addr = "/pd/ui/count";
    else if (selector == "selection_indices") addr = "/pd/ui/indices";
    else addr = "/pd/ui/" + selector;

    juce::OSCMessage msg { juce::OSCAddressPattern(addr) };
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].isFloat()) {
            msg.addArgument(list[i].getFloat());
        } else if (list[i].isSymbol()) {
            msg.addArgument(juce::String::fromUTF8(list[i].getSymbol()->s_name));
        }
    }
    sender.send(msg);
}

void MCPBridge::sendConsoleLog(const juce::String& message, bool isError)
{
    if (!canReply()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(isError ? "/pd/error" : "/pd/log") };
    msg.addArgument(message);
    sender.send(msg);
}

void MCPBridge::setChatCallback(std::function<void(const juce::String& role, const juce::String& text)> cb)
{
    juce::ScopedLock sl(chatLock);
    chatCallback = std::move(cb);
}

void MCPBridge::postChatMessage(const juce::String& role, const juce::String& text)
{
    postMessage([this, role, text]() {
        juce::ScopedLock sl(chatLock);
        if (chatCallback) {
            chatCallback(role, text);
        }
    });
}

void MCPBridge::sendPrompt(const juce::String& promptText)
{
    if (!canReply()) return;

    postChatMessage("user", promptText);

    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/mcp_prompt") };
    msg.addArgument(promptText);
    sender.send(msg);
}

void MCPBridge::sendLens(const juce::String& lens)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/lens") };
    msg.addArgument(lens);
    sender.send(msg);
}

void MCPBridge::sendSelectionPrompt(const juce::String& promptText, const juce::StringArray& targetTempIds, bool queue)
{
    if (!canReply()) return;

    postChatMessage("user", promptText);

    // /pd/ui/prompt <prompt> <mode> <count> <id...>
    // mode: "spawn" (default, Enter) = run the headless canvas agent now;
    //       "queue" (Shift+Enter) = leave it for the interactive chat to read.
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/ui/prompt") };
    msg.addArgument(promptText);
    msg.addArgument(queue ? juce::String("queue") : juce::String("spawn"));
    msg.addArgument(static_cast<int>(targetTempIds.size()));
    for (auto const& id : targetTempIds) {
        msg.addArgument(id);
    }
    sender.send(msg);
}

void MCPBridge::sendSketchStroke(const juce::String& subpatch, const juce::String& jsonPoints, bool isFastPath)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/sketch/stroke") };
    msg.addArgument(subpatch);
    msg.addArgument(isFastPath ? 1 : 0);
    msg.addArgument(jsonPoints);
    sender.send(msg);
}

void MCPBridge::sendSketchBuild(const juce::String& subpatch, const juce::String& jsonStrokes, const juce::String& jsonBBox, const juce::String& prompt)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/sketch/build") };
    msg.addArgument(subpatch);
    msg.addArgument(jsonStrokes);
    msg.addArgument(jsonBBox);
    msg.addArgument(prompt);
    sender.send(msg);
}

void MCPBridge::sendReferenceImage(const juce::String& path, float x, float y, float w, float h)
{
    if (!canReply()) return;
    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/reference/image") };
    msg.addArgument(path);
    msg.addArgument(x);
    msg.addArgument(y);
    msg.addArgument(w);
    msg.addArgument(h);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::Array<juce::var>& args)
{
    if (!canReply()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    for (auto const& v : args) {
        if (v.isDouble() || v.isInt() || v.isInt64()) {
            msg.addArgument(static_cast<float>(v));
        } else {
            msg.addArgument(v.toString());
        }
    }
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, float val)
{
    if (!canReply()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(val);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::String& str)
{
    if (!canReply()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(str);
    sender.send(msg);
}

void MCPBridge::sendRawReply(const juce::String& addressPattern)
{
    if (!canReply()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    sender.send(msg);
}
