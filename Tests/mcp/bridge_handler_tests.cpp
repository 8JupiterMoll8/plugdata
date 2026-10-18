/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Handler tests of the MCP bridge (Plan 3 P3.3 S10). Real handlers run through the command table on a
// headless PluginProcessor (no editor, no window): requests are fed to MCPBridge::oscMessageReceived from a
// worker thread, the way the OSC receiver thread does it, while the main thread runs the message loop and a
// second thread drives processBlock like an audio callback. Replies are captured by the bridge's reply sink
// (MCPBridge::setReplySinkForTests), so no socket is opened.
//
//   cmake -S . -B build -DMCP_BRIDGE_TESTS=ON && cmake --build build --target mcp_bridge_tests
//   scripts/run-mcp-bridge-tests.sh                     (scratch HOME; see the script)
//
// HOME decides where plugdata keeps its settings and patches. This program therefore points HOME at a fresh
// temporary directory before anything else (MCP_TESTS_HOME reuses a directory: the first start unpacks the
// resources and is slow).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Pd/Interface.h" // g_undo.h: the native undo queue (L6 check)

namespace {

int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

struct Arg {
    bool isNumber = false;
    double number = 0;
    std::string text;
};

struct Reply {
    std::string address;
    std::vector<Arg> args;
    std::string str(size_t i) const { return i < args.size() ? args[i].text : std::string(); }
    double num(size_t i) const { return i < args.size() ? args[i].number : -1e9; }
};

// Replies from the bridge, in order. The sink runs on whichever thread sends.
class Replies {
public:
    void push(juce::OSCMessage const& msg)
    {
        Reply r;
        r.address = msg.getAddressPattern().toString().toStdString();
        for (auto const& a : msg) {
            Arg arg;
            if (a.isInt32()) {
                arg.isNumber = true;
                arg.number = a.getInt32();
            } else if (a.isFloat32()) {
                arg.isNumber = true;
                arg.number = a.getFloat32();
            } else if (a.isString()) {
                arg.text = a.getString().toStdString();
            }
            r.args.push_back(std::move(arg));
        }
        {
            std::lock_guard<std::mutex> const lock(mutex);
            all.push_back(std::move(r));
        }
        changed.notify_all();
    }

    void clear()
    {
        std::lock_guard<std::mutex> const lock(mutex);
        all.clear();
    }

    // The first reply on `address`, waiting up to timeoutMs. Replies before it are dropped.
    std::optional<Reply> waitFor(std::string const& address, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        std::optional<Reply> found;
        changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].address == address) {
                    found = all[i];
                    all.erase(all.begin(), all.begin() + static_cast<long>(i) + 1);
                    return true;
                }
            }
            return false;
        });
        return found;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Reply> all;
};

// One OSC argument of a request.
struct A {
    juce::OSCArgument value;
    A(int v) : value(static_cast<juce::int32>(v)) { }
    A(double v) : value(static_cast<float>(v)) { }
    A(char const* v) : value(juce::String(v)) { }
};

juce::OSCMessage message(char const* address, std::initializer_list<A> args)
{
    juce::OSCMessage msg { juce::OSCAddressPattern(address) };
    for (auto const& a : args)
        msg.addArgument(a.value);
    return msg;
}

struct Harness {
    MCPBridge& bridge;
    Replies& replies;

    // Sends one request through the bridge and waits for the reply on `replyAddress`.
    std::optional<Reply> call(char const* address, std::initializer_list<A> args, std::string const& replyAddress, int timeoutMs = 5000)
    {
        replies.clear();
        bridge.oscMessageReceived(message(address, args));
        return replies.waitFor(replyAddress, timeoutMs);
    }

    // The same request fed to a thread that only has to see what the bridge sent back.
    void fire(char const* address, std::initializer_list<A> args)
    {
        replies.clear();
        bridge.oscMessageReceived(message(address, args));
    }
};

juce::var parseJson(Reply const& r)
{
    return juce::JSON::parse(juce::String(r.str(0)));
}

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The Pd context (Plan 3 P3.4 S2/S3): McpHost::enqueuePd / pdWait run jobs on the message thread, under the
// Pd lock (plugdata's own Pd editing context); a job the caller gave up on never runs; a held lock delays a
// job until it is released.
struct AudioControl {
    std::atomic<bool> paused { false };
    std::atomic<int> notePitch { -1 };   // P3.4 S6: >= 0 = the next block carries a note-on of this pitch
    std::atomic<int> noteOffPitch { -1 }; // >= 0 = the next block carries a note-off
    std::atomic<std::thread::id> threadId {};
    std::atomic<std::thread::id> messageThreadId {};
};

void runPdContextCases(PluginProcessor& processor, AudioControl& audio)
{
    std::printf("pd context\n");
    auto* host = processor.getMcp();

    // 1. pdWait runs the job on the message thread and returns once it ran.
    {
        std::atomic<bool> ran { false };
        std::atomic<std::thread::id> where {};
        CHECK(host->pdWait([&] { ran = true; where = std::this_thread::get_id(); }, 2000));
        CHECK(ran.load());
        CHECK(where.load() == audio.messageThreadId.load());
    }

    // 2. The job holds the Pd lock while it runs: another thread cannot take it.
    {
        std::atomic<bool> inside { false };
        std::atomic<bool> release { false };
        host->enqueuePd([&] {
            inside = true;
            for (int i = 0; i < 200 && !release.load(); ++i)
                sleepMs(1);
        });
        for (int i = 0; i < 500 && !inside.load(); ++i)
            sleepMs(1);
        CHECK(inside.load());
        bool const got = processor.audioLock.tryEnter();
        CHECK(!got);
        if (got)
            processor.audioLock.exit();
        release = true;
        sleepMs(30);
    }

    // 3. With the message thread stalled, pdWait gives up and the job never runs afterwards.
    {
        juce::MessageManager::callAsync([] { sleepMs(300); });
        sleepMs(20);
        std::atomic<bool> ran { false };
        CHECK(!host->pdWait([&] { ran = true; }, 100));
        sleepMs(400);
        CHECK(!ran.load());
    }

    // 4. A held Pd lock delays a job; it runs once the lock is released.
    {
        std::atomic<bool> ran { false };
        processor.audioLock.enter();
        host->enqueuePd([&] { ran = true; });
        sleepMs(200);
        CHECK(!ran.load());
        processor.audioLock.exit();
        for (int i = 0; i < 200 && !ran.load(); ++i)
            sleepMs(1);
        CHECK(ran.load());
    }
}

// L6 (P3.4 S9): an MCP flush of the native undo queue (every batch does one) must leave Pd's UNDO_INIT
// head action in place: canvas_undo_undo walks back to it, and without it the first GUI action after an
// AI edit could be undone off the end of the queue, after which the next GUI edit crashed.
void runNativeUndoCases(PluginProcessor& processor)
{
    std::printf("native undo queue after an MCP flush\n");
    auto* host = processor.getMcp();
    bool headIsInit = false;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbolStrict("main");
        if (!cnv) cnv = pd_this->pd_canvaslist;
        t_undo* udo = cnv ? canvas_undo_get(cnv) : nullptr;
        headIsInit = udo && udo->u_queue && udo->u_queue->type == UNDO_INIT;
    }, 2000));
    CHECK(headIsInit);
}

// P3.5 S0: the identity service will detect GUI edits by recomputing a per-canvas stamp (object count plus a hash
// of the gl_list pointers) under the Pd lock on every version read. Measure one walk on a canvas the size of the
// DEV.md standard patch (37 objects) and on a large one (500): the plan stops and re-plans above 100 us.
void runStampCostCases(PluginProcessor& processor)
{
    std::printf("identity stamp walk cost\n");
    auto* host = processor.getMcp();
    double us37 = -1, us500 = -1;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbolStrict("main");
        if (!cnv) return;
        int before = 0;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) ++before;
        for (int i = 0; i < 500; ++i) {
            t_atom a[3];
            SETFLOAT(a, 600 + (i % 20) * 30);
            SETFLOAT(a + 1, 40 + (i / 20) * 30);
            SETSYMBOL(a + 2, gensym("f"));
            pd_typedmess(&cnv->gl_pd, gensym("obj"), 3, a);
        }
        auto walk = [cnv](int limit) {
            uint64_t h = 1469598103934665603ull;
            int n = 0;
            for (t_gobj* g = cnv->gl_list; g && n < limit; g = g->g_next, ++n)
                h = (h ^ reinterpret_cast<uintptr_t>(g)) * 1099511628211ull;
            return h ^ static_cast<uint64_t>(n);
        };
        auto time = [&](int limit) {
            constexpr int rounds = 2000;
            volatile uint64_t sink = 0;
            auto const t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < rounds; ++r)
                sink = sink + walk(limit);
            auto const dt = std::chrono::steady_clock::now() - t0;
            return std::chrono::duration<double, std::micro>(dt).count() / rounds;
        };
        us37 = time(37);
        us500 = time(before + 500);
        // Remove the timing objects again (the newest 500, at the end of gl_list).
        std::vector<t_gobj*> extra;
        int idx = 0;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next, ++idx)
            if (idx >= before) extra.push_back(g);
        for (auto* g : extra)
            glist_delete(cnv, g);
    }, 10000));
    std::printf("  stamp walk: %.3f us at 37 objects, %.3f us at 500\n", us37, us500);
    CHECK(us37 >= 0 && us37 < 100.0);
    CHECK(us500 >= 0 && us500 < 100.0);
}

// Bug #83 (P3.4 S10): with the audio callback stopped, Pd's signal vectors keep the last block. /meter/master
// read them and reported that stale level as live, while every armed read (which needs processBlock) was
// silent: the "master audible, armed silent" split of the bug report. The test's audio loop stands in for
// the device and is paused here.
void runAudioStallCases(Harness& h, AudioControl& audio)
{
    std::printf("master meter with the audio callback stopped (#83)\n");
    auto made = h.call("/pd/batch_atomic",
        { "main", "s83", 0, 0, 0, 2, 2,
            "s83o", 400, 40, "obj", "osc~", 1, "440",
            "s83d", 400, 100, "obj", "dac~", 0,
            "s83o", 0, "s83d", 0,
            "s83o", 0, "s83d", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/s83");
    CHECK(made.has_value() && made->num(0) == 2);
    h.fire("/pd/dsp", { 1 });
    sleepMs(300);
    auto live = h.call("/meter/master", { "main", "s83m1" }, "/meter/master/reply/s83m1");
    CHECK(live.has_value() && live->num(0) > -60.0);

    audio.paused = true;
    sleepMs(400);
    h.call("/meter/arm", { "s83a" }, "/meter/arm/reply/s83a");
    sleepMs(100);
    auto armed = h.call("/meter/read", { "s83r" }, "/meter/read/reply/s83r");
    CHECK(armed.has_value() && armed->num(2) == 0); // blocksElapsed: the audio thread is not ticking
    auto stalled = h.call("/meter/master", { "main", "s83m2" }, "/meter/master/reply/s83m2");
    CHECK(stalled.has_value() && stalled->num(0) <= -99.0);
    CHECK(stalled.has_value() && stalled->str(3) == "audio_not_running");

    audio.paused = false;
    sleepMs(300);
    auto again = h.call("/meter/master", { "main", "s83m3" }, "/meter/master/reply/s83m3");
    CHECK(again.has_value() && again->num(0) > -60.0);
    h.fire("/pd/dsp", { 0 });
    h.call("/pd/batch_atomic", { "main", "s83x", 2, 0, 0, 0, 0, "s83o", "s83d", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/s83x");
}

// MIDI capture (P3.4 S6): the audio thread only queues raw note on/off; the bridge pairs them off the audio
// thread. Three notes played through processBlock come back as three closed notes.
void runMidiCaptureCases(PluginProcessor& processor, AudioControl& audio)
{
    std::printf("midi capture\n");
    auto* bridge = processor.getMcp()->mcpBridge.get();
    auto* host = processor.getMcp();
    CHECK(host->onMessageThread([bridge] { bridge->startMidiCapture(10, "canvas"); return true; }, 2000, "test").has_value());
    sleepMs(50);
    for (int p : { 60, 64, 67 }) {
        audio.notePitch = p;
        sleepMs(40);
        audio.noteOffPitch = p;
        sleepMs(40);
    }
    sleepMs(100);
    CHECK(bridge->isMidiCapturing());
    CHECK(bridge->getMidiCaptureNoteCount() == 3);
    host->onMessageThread([bridge] { bridge->stopMidiCaptureAndStage(); return true; }, 2000, "test");
    CHECK(!bridge->isMidiCapturing());
}

// Teardown during work (P3.4 S8): start a long offline render and return; main() then destroys the processor
// while the render job runs. McpHost::shutdown (first thing in ~PluginProcessor) must cancel it before the
// patches go: the job used to be a detached std::thread holding raw processor and bridge pointers.
void startRenderForTeardown(Harness& h)
{
    std::printf("teardown during a render\n");
    auto r = h.call("/pd/render", { "teardown-render.wav", 20, 0, "td1" }, "/pd/render/started/td1", 3000);
    CHECK(r.has_value());
}

void runCases(Harness& h)
{
    // ── answers without a canvas ─────────────────────────────────────────────────────────────────
    {
        auto r = h.call("/pd/ping", {}, "/pd/pong");
        std::printf("ping\n");
        CHECK(r.has_value());
    }
    {
        std::printf("unknown verb, unknown domain\n");
        auto r = h.call("/pd/no_such_verb", { "main", "c1" }, "/bridge/unknown_verb");
        CHECK(r.has_value() && r->args.size() == 3);
        if (r && r->args.size() == 3) {
            CHECK(r->str(0) == "/pd/no_such_verb");
            CHECK(r->str(1) == "pd");
            CHECK(r->str(2) == "no_such_verb");
        }
        auto d = h.call("/nodomain/anything", {}, "/bridge/unknown_verb");
        CHECK(d.has_value() && d->str(0) == "/nodomain/anything" && d->str(1) == "nodomain" && d->str(2) == "anything");
        // The report is never answered with a report.
        h.fire("/bridge/unknown_verb", { "/pd/x", "pd", "x" });
        CHECK(!h.replies.waitFor("/bridge/unknown_verb", 300).has_value());
        // A retired id-addressed verb is unknown (P3.3 S9).
        auto g = h.call("/pd/create_id", { "main", "c2", "x1", 10, 10, "obj", "osc~" }, "/bridge/unknown_verb");
        CHECK(g.has_value() && g->str(2) == "create_id");
    }
    {
        std::printf("capabilities\n");
        auto r = h.call("/bridge/capabilities", { "k1" }, "/bridge/capabilities/reply");
        bool unknownVerbCap = false, batchCap = false;
        if (r)
            for (auto const& a : r->args) {
                unknownVerbCap = unknownVerbCap || a.text == "unknown_verb_reply";
                batchCap = batchCap || a.text == "batch_atomic";
            }
        CHECK(r.has_value() && unknownVerbCap && batchCap);
    }

    // ── build: batch_atomic with 2 objects and 1 wire ────────────────────────────────────────────
    int osc = -1, mul = -1;
    {
        std::printf("batch_atomic\n");
        auto r = h.call("/pd/batch_atomic",
            { "main", "b1", 0, 0, 0, 2, 1,
                "osc1", 40, 40, "obj", "osc~", 1, "440",
                "mul1", 40, 100, "obj", "*~", 1, "0.5",
                "osc1", 0, "mul1", 0,
                0, 5, 10, 0 },
            "/pd/batch_atomic/reply/b1");
        CHECK(r.has_value());
        if (r) {
            CHECK(r->num(0) == 2); // created
            CHECK(r->num(1) == 1); // connected
            for (size_t i = 7; i + 1 < r->args.size() && r->str(i).rfind("[", 0) != 0; i += 2) {
                if (r->str(i) == "osc1") osc = static_cast<int>(r->num(i + 1));
                if (r->str(i) == "mul1") mul = static_cast<int>(r->num(i + 1));
            }
            CHECK(osc >= 0 && mul >= 0 && osc != mul);
        }
    }

    // ── reads ────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf("connections, identity_snapshot, census, obj_get, params\n");
        auto r = h.call("/pd/connections", { "main", "k2" }, "/pd/connections/reply/k2");
        CHECK(r.has_value());
        if (r) {
            auto json = parseJson(*r);
            CHECK(static_cast<int>(json["count"]) == 1);
            auto const& list = *json["connections"].getArray();
            CHECK(list.size() == 1 && static_cast<int>(list[0]["srcIndex"]) == osc && static_cast<int>(list[0]["destIndex"]) == mul);
        }

        auto snap = h.call("/pd/identity_snapshot", { "main", "k3" }, "/pd/identity_snapshot/reply/k3");
        CHECK(snap.has_value());
        if (snap) {
            auto json = parseJson(*snap);
            CHECK(static_cast<int>(json["entryCount"]) == 2);
            bool sawOsc = false, sawMul = false;
            for (auto const& e : *json["entries"].getArray()) {
                sawOsc = sawOsc || e["id"].toString() == "osc1";
                sawMul = sawMul || e["id"].toString() == "mul1";
            }
            CHECK(sawOsc && sawMul);
        }

        auto census = h.call("/pd/census", { "main", "k4" }, "/pd/census/reply/k4");
        CHECK(census.has_value());
        if (census) {
            auto json = parseJson(*census);
            CHECK(static_cast<int>(json["objectCount"]) == 2);
        }

        auto get = h.call("/pd/obj_get", { "main", "k5", "osc1" }, "/pd/obj_get/reply/k5");
        CHECK(get.has_value());
        if (get) {
            auto json = parseJson(*get);
            CHECK(json["type"].toString() == "osc~");
            CHECK(json["text"].toString() == "osc~ 440");
        }

        auto params = h.call("/pd/params", { "main", "k6" }, "/pd/params/reply/k6");
        CHECK(params.has_value());
        if (params) CHECK(parseJson(*params).hasProperty("params"));
    }

    // ── a write: obj_set ─────────────────────────────────────────────────────────────────────────
    {
        std::printf("obj_set\n");
        auto r = h.call("/pd/obj_set", { "main", "k7", "osc1", 0, "float", 1, 330 }, "/pd/obj_set/reply/k7");
        CHECK(r.has_value() && r->num(0) == 1);
    }

    // ── move, undo, redo (McpHost replays the transaction through the command table) ─────────────
    auto position = [&](char const* id, char const* corr) -> std::pair<int, int> {
        auto r = h.call("/pd/typeof", { "main", id, corr }, std::string("/pd/typeof/reply/") + corr);
        if (!r || r->args.size() < 4) return { -1, -1 };
        return { static_cast<int>(r->num(2)), static_cast<int>(r->num(3)) };
    };
    {
        std::printf("move_batch_id, undo, redo\n");
        CHECK(position("osc1", "p0") == std::make_pair(40, 40));
        auto mv = h.call("/pd/move_batch_id", { "main", "m1", "osc1", 60, 50 }, "/pd/mcp_move_batch_id/reply/m1");
        CHECK(mv.has_value() && mv->num(0) == 1);
        sleepMs(300);
        CHECK(position("osc1", "p1") == std::make_pair(60, 50));

        auto undo = h.call("/pd/undo", { "main", "u1" }, "/pd/undo/reply/u1");
        CHECK(undo.has_value() && undo->num(0) == 1);
        sleepMs(800);
        CHECK(position("osc1", "p2") == std::make_pair(40, 40));

        auto redo = h.call("/pd/redo", { "main", "u2" }, "/pd/redo/reply/u2");
        CHECK(redo.has_value() && redo->num(0) == 1);
        sleepMs(800);
        CHECK(position("osc1", "p3") == std::make_pair(60, 50));
    }

    // ── identity: rename ─────────────────────────────────────────────────────────────────────────
    {
        std::printf("rename_id\n");
        auto r = h.call("/pd/rename_id", { "main", "r1", "osc1", "carrier" }, "/pd/rename_id/reply/r1");
        CHECK(r.has_value() && r->num(0) == 1);
        auto census = h.call("/pd/census", { "main", "r2" }, "/pd/census/reply/r2");
        bool renamed = false, oldGone = true;
        if (census) {
            auto const json = parseJson(*census); // keep the var alive while its array is walked
            for (auto const& o : *json["objects"].getArray()) {
                renamed = renamed || o["id"].toString() == "carrier";
                oldGone = oldGone && o["id"].toString() != "osc1";
            }
        }
        CHECK(renamed && oldGone);
    }

    // ── arrays (P3.4 S9): a write creates the array; a missing array answers its error (L21) ───
    {
        std::printf("array write_bulk, stats, missing array\n");
        auto w = h.call("/array/write_bulk", { "s9arr", "main", "a1", 0, 0.5, -0.5, 0.25, -0.25 }, "/array/write_bulk/reply/a1");
        CHECK(w.has_value() && w->num(0) == 4 && w->num(1) == 4);
        auto s = h.call("/array", { "stats", "s9arr", 44100, "a2" }, "/array/stats/reply/a2");
        CHECK(s.has_value() && s->str(0) == "s9arr" && s->num(1) == 4 && s->num(3) == 0.5);
        auto ms = h.call("/array", { "stats", "s9_no_such", 44100, "a3" }, "/array/stats/error/a3", 2000);
        CHECK(ms.has_value() && ms->str(0) == "array 's9_no_such' not found");
        auto mr = h.call("/array/read_bulk", { "s9_no_such", "main", "a4", 0, 8 }, "/array/read_bulk/error/a4", 2000);
        CHECK(mr.has_value());
        auto mm = h.call("/array/mutate", { "s9_no_such", "main", "a5", "reverse" }, "/array/mutate/error/a5", 2000);
        CHECK(mm.has_value());
    }
}

// Where plugdata keeps its files follows HOME: never the real one.
std::string chooseScratchHome()
{
    if (char const* given = std::getenv("MCP_TESTS_HOME"); given && *given) {
        ::setenv("HOME", given, 1);
        return given;
    }
    char pattern[] = "/tmp/mcp_bridge_tests_home_XXXXXX";
    if (!::mkdtemp(pattern)) {
        std::printf("mcp_bridge_tests: cannot create a scratch HOME\n");
        std::exit(2);
    }
    ::setenv("HOME", pattern, 1);
    return pattern;
}

} // namespace

int main()
{
    auto const home = chooseScratchHome();
    for (char const* name : { "XDG_DOCUMENTS_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME" })
        ::unsetenv(name);
    std::printf("mcp_bridge_tests: HOME=%s\n", home.c_str());

    juce::ScopedJuceInitialiser_GUI const juceInitialiser;

    {
        // Declared before the processor so it outlives it: the teardown case destroys the processor while a render
        // still runs, and the bridge's last replies arrive during that destruction (P3.4 S11, TSan).
        Replies replies;
        PluginProcessor processor;
        processor.setRateAndBufferSizeDetails(44100.0, 512);
        processor.prepareToPlay(44100.0, 512);
        auto patch = processor.loadPatch(juce::String()); // the default patch: one empty canvas, no editor
        if (!patch) {
            std::printf("mcp_bridge_tests: could not open a patch\n");
            return 2;
        }

        auto* host = processor.getMcp();
        auto* bridge = host ? host->mcpBridge.get() : nullptr;
        if (!bridge) {
            std::printf("mcp_bridge_tests: the processor has no bridge\n");
            return 2;
        }
        bridge->stop(); // the processor started it on the configured UDP port: nothing may listen here
        bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });

        std::atomic<bool> casesDone { false };
        std::atomic<bool> audioStop { false };
        AudioControl audioControl;

        // The audio callback: queued work (enqueuePd, enqueueFunctionAsync) runs inside processBlock.
        std::thread audio([&] {
            audioControl.threadId = std::this_thread::get_id();
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            while (!audioStop.load()) {
                if (!audioControl.paused.load()) {
                    buffer.clear();
                    midi.clear();
                    if (int const p = audioControl.notePitch.exchange(-1); p >= 0)
                        midi.addEvent(juce::MidiMessage::noteOn(1, p, static_cast<juce::uint8>(100)), 0);
                    if (int const p = audioControl.noteOffPitch.exchange(-1); p >= 0)
                        midi.addEvent(juce::MidiMessage::noteOff(1, p), 0);
                    processor.processBlock(buffer, midi);
                }
                sleepMs(8);
            }
        });

        auto stopMessageLoop = [] {
            juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
        };

        // The OSC receiver thread: requests arrive here, replies come back through the sink.
        std::thread worker([&] {
            Harness harness { *bridge, replies };
            runCases(harness);
            runPdContextCases(processor, audioControl);
            runNativeUndoCases(processor);
            runAudioStallCases(harness, audioControl);
            runMidiCaptureCases(processor, audioControl);
            runStampCostCases(processor);
            startRenderForTeardown(harness); // last: the processor is destroyed with the render running
            casesDone.store(true);
            stopMessageLoop();
        });

        // A hung handler must not hang the run.
        std::thread watchdog([&] {
            for (int waited = 0; waited < 1200 && !casesDone.load(); ++waited)
                sleepMs(100);
            if (!casesDone.load()) {
                std::printf("  FAIL the cases did not finish in 120 s\n");
                ++failures;
                casesDone.store(true);
                stopMessageLoop();
            }
        });

        // The message thread: handlers hand work to it (callAsync) while the worker waits.
        audioControl.messageThreadId = std::this_thread::get_id();
        juce::MessageManager::getInstance()->runDispatchLoop();

        worker.join();
        watchdog.join();
        audioStop.store(true);
        audio.join();
        bridge->setReplySinkForTests({});
    }

    std::printf(failures ? "mcp_bridge_tests: %d FAILED\n" : "mcp_bridge_tests: OK\n", failures);
    return failures ? 1 : 0;
}
