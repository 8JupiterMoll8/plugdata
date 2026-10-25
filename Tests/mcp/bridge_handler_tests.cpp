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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Pd/Interface.h" // g_undo.h: the native undo queue (L6 check)
#include "Mcp/McpCeilingPlayer.h" // P9 round 1: the clamp on playVoiceTake's own callback
#include "Mcp/McpNativeUndo.h" // P6.2 UF: hand-made undo entries (motion) for /pd/undo_peek
#include "v2_test_client.h" // P4.1 S2: loopback v2 client
#include "Pd/MCPBridgeCmdCommon.h" // armedSpectralJson / renderSpectralJson (Plan 5 spectral window)
#include "Utility/PluginParameter.h" // PlugDataParameter: the master volume guard (P5.1 sink floor)

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

    // A one-line list of what arrived and is still unread (for a failure message).
    std::string describe()
    {
        std::lock_guard<std::mutex> const lock(mutex);
        std::string out;
        for (auto const& r : all)
            out += r.address + (r.args.empty() ? "" : "(" + r.str(0).substr(0, 60) + ") ");
        return out;
    }

    // The first console line (/pd/error or /pd/log) whose text contains `needle`, waiting up to timeoutMs.
    std::optional<Reply> waitForConsoleLine(std::string const& needle, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        std::optional<Reply> found;
        changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (size_t i = 0; i < all.size(); ++i) {
                bool const console = all[i].address == "/pd/error" || all[i].address == "/pd/log";
                if (console && all[i].str(0).find(needle) != std::string::npos) {
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
    A(juce::String const& v) : value(v) { }
};

juce::OSCMessage message(char const* address, std::initializer_list<A> args)
{
    juce::OSCMessage msg { juce::OSCAddressPattern(address) };
    for (auto const& a : args)
        msg.addArgument(a.value);
    return msg;
}

// P4.1 S3: one v2 session of the bridge's own listener, used by a Harness in v2 mode. Every request id sent and
// every final received is recorded, so a run can check that no request was left without a final.
struct V2Link {
    explicit V2Link(int port)
        : client(port)
    {
    }

    mcp::v2::test::Client client;
    std::vector<mcp::v2::json::Value> stash; // frames read while waiting for another one
    std::vector<std::string> sentIds;
    std::vector<std::string> finalIds;
    int next = 0;

    // S4: every request carries an idempotency key (mutating verbs need one); `key` "" = a fresh one.
    void sendReq(std::string const& id, char const* address, std::string const& args, bool noReply = false, std::string key = {})
    {
        static int keySeq = 0;
        if (key.empty())
            key = "mbt:" + std::to_string(++keySeq);
        sentIds.push_back(id);
        client.sendFrame(R"({"t":"req","id":)" + mcp::v2::json::quote(id) + ",\"verb\":" + mcp::v2::json::quote(address)
            + ",\"key\":" + mcp::v2::json::quote(key) + ",\"args\":" + args + (noReply ? ",\"noReply\":true}" : "}"));
    }

    // The next frame (stashed ones first) matching `want`, waiting up to timeoutMs; others are stashed.
    template<typename P>
    std::optional<mcp::v2::json::Value> take(P want, int timeoutMs)
    {
        for (size_t i = 0; i < stash.size(); ++i)
            if (want(stash[i])) {
                auto v = stash[i];
                stash.erase(stash.begin() + static_cast<long>(i));
                return v;
            }
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < until) {
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            auto f = client.next(static_cast<int>(std::max<long long>(1, left)));
            if (!f)
                continue;
            note(*f);
            if (want(*f))
                return f;
            stash.push_back(std::move(*f));
        }
        return std::nullopt;
    }

    void note(mcp::v2::json::Value const& f)
    {
        auto const* t = f.find("t");
        auto const* fin = f.find("final");
        auto const* id = f.find("id");
        if (t && t->string == "rep" && fin && fin->boolean && id)
            finalIds.push_back(id->string);
    }

    // Reads for up to `ms`, then the ids sent without a final.
    std::vector<std::string> withoutFinal(int ms)
    {
        take([](auto const&) { return false; }, ms);
        std::vector<std::string> missing;
        for (auto const& id : sentIds)
            if (std::find(finalIds.begin(), finalIds.end(), id) == finalIds.end())
                missing.push_back(id);
        return missing;
    }
};

std::string field(mcp::v2::json::Value const& v, char const* key)
{
    auto const* m = v.find(key);
    return m && m->is(mcp::v2::json::Value::Kind::String) ? m->string : std::string();
}

Reply replyOf(mcp::v2::json::Value const& rep)
{
    Reply r;
    r.address = field(rep, "address");
    if (auto const* args = rep.find("args"))
        for (auto const& a : args->items) {
            Arg arg;
            auto const* v = a.find("v");
            if (field(a, "t") == "s") {
                arg.text = v ? v->string : std::string();
            } else {
                arg.isNumber = true;
                arg.number = v ? v->number : 0;
            }
            r.args.push_back(std::move(arg));
        }
    return r;
}

struct Harness {
    MCPBridge& bridge;
    Replies& replies;
    V2Link* v2 = nullptr; // P4.1 S3: set = every call goes through this v2 session instead of a datagram

    // Sends one request through the bridge and waits for the reply on `replyAddress`.
    std::optional<Reply> call(char const* address, std::initializer_list<A> args, std::string const& replyAddress, int timeoutMs = 5000)
    {
        replies.clear();
        if (v2)
            return callV2(address, args, replyAddress, timeoutMs);
        bridge.oscMessageReceived(message(address, args));
        return replies.waitFor(replyAddress, timeoutMs);
    }

    // The same with the arguments built at run time (a batch of many objects). v1 only.
    std::optional<Reply> callList(char const* address, std::vector<A> const& args, std::string const& replyAddress, int timeoutMs = 5000)
    {
        replies.clear();
        juce::OSCMessage msg { juce::OSCAddressPattern(address) };
        for (auto const& a : args)
            msg.addArgument(a.value);
        bridge.oscMessageReceived(msg);
        return replies.waitFor(replyAddress, timeoutMs);
    }

    // The same request fed to a thread that only has to see what the bridge sent back.
    void fire(char const* address, std::initializer_list<A> args)
    {
        replies.clear();
        bridge.oscMessageReceived(message(address, args));
    }

    // v2: the corr id of the case gets a "v2" suffix (the v1 pass used it: the batch de-dup would replay it), the
    // request id is that corr id (the TS v2 contract), the reply must come back on this session, by id, and never
    // reach the UDP side (the sink).
    std::optional<Reply> callV2(char const* address, std::initializer_list<A> args, std::string const& replyAddress, int timeoutMs)
    {
        auto const slash = replyAddress.find_last_of('/');
        auto const token = replyAddress.substr(slash + 1);
        bool rewrite = false;
        for (auto const& a : args)
            rewrite = rewrite || (a.value.isString() && a.value.getString().toStdString() == token);
        std::string expect = replyAddress;
        std::string id = "H" + std::to_string(v2->next++);
        if (rewrite) {
            id = token + "v2";
            expect = replyAddress.substr(0, slash + 1) + id;
        }
        std::vector<mcp::v2::Arg> typed;
        for (auto const& a : args) {
            mcp::v2::Arg arg;
            if (a.value.isInt32()) {
                arg.type = 'i';
                arg.i = a.value.getInt32();
            } else if (a.value.isFloat32()) {
                arg.type = 'f';
                arg.f = a.value.getFloat32();
            } else {
                arg.s = a.value.getString().toStdString();
                if (rewrite && arg.s == token)
                    arg.s = id;
            }
            typed.push_back(std::move(arg));
        }
        v2->sendReq(id, address, mcp::v2::encodeArgs(typed));
        auto const got = v2->take([&](auto const& f) {
            if (field(f, "t") != "rep" || field(f, "id") != id)
                return false;
            auto const* fin = f.find("final");
            return field(f, "address") == expect || (fin && fin->boolean);
        }, timeoutMs);
        if (!got || field(*got, "address") != expect) {
            std::printf("  v2 %s (id %s): %s\n", address, id.c_str(),
                got ? ("final " + field(*got, "outcome") + " on " + field(*got, "address")).c_str() : "no reply");
            return std::nullopt;
        }
        CHECK(!replies.waitFor(expect, 0).has_value()); // a v2 reply never reaches the v1 side
        auto r = replyOf(*got);
        r.address = replyAddress; // as the v1 case knows it
        return r;
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

// L6 (P3.4 S9): an MCP flush of the native undo queue (load, clear, encapsulate since P4.4) must leave Pd's UNDO_INIT
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

// Plan 4 P4.4: ONE undo history per canvas (Pd's own queue) for AI and manual edits, in time order; one AI call is
// one undo step (decision 9); undo/redo keep the tempIds. A "manual" edit here is a move registered exactly as the
// GUI's drag registers it (UNDO_MOTION before the displacement).
void runSharedUndoCases(Harness& h, PluginProcessor& processor)
{
    std::printf("shared undo: AI and manual edits in one history\n");
    auto* host = processor.getMcp();
    int corr = 0;
    auto next = [&corr](char const* p) { return juce::String(p) + juce::String(++corr); };
    auto batch = [&](std::initializer_list<A> args, juce::String const& c) {
        auto r = h.call("/pd/batch_atomic", args, ("/pd/batch_atomic/reply/" + c).toStdString());
        return r.has_value();
    };
    // undo/redo on main; true when the bridge answered 1 (a step was taken).
    auto step = [&](char const* verb) {
        auto const c = next(verb);
        auto r = h.call((juce::String("/pd/") + verb).toRawUTF8(), { "main", c }, ("/pd/" + juce::String(verb) + "/reply/" + c).toStdString());
        sleepMs(300);
        return r.has_value() && r->num(0) == 1;
    };
    auto text = [&](char const* id) -> std::string {
        auto const c = next("sut");
        auto r = h.call("/pd/obj_get", { "main", c, id }, ("/pd/obj_get/reply/" + c).toStdString(), 2000);
        if (!r) return "";
        auto const json = parseJson(*r);
        return json["text"].toString().toStdString();
    };
    auto pos = [&](char const* id) -> std::pair<int, int> {
        auto const c = next("sup");
        auto r = h.call("/pd/typeof", { "main", id, c }, ("/pd/typeof/reply/" + c).toStdString(), 2000);
        if (!r || r->args.size() < 4) return { -1, -1 };
        return { static_cast<int>(r->num(2)), static_cast<int>(r->num(3)) };
    };
    auto wired = [&](char const* src, char const* dst) {
        auto const c = next("suw");
        auto r = h.call("/pd/connections", { "main", c }, ("/pd/connections/reply/" + c).toStdString(), 2000);
        if (!r) return false;
        auto const json = parseJson(*r);
        if (auto const* list = json["connections"].getArray())
            for (auto const& w : *list)
                if (w["srcId"].toString() == src && w["destId"].toString() == dst) return true;
        return false;
    };
    auto manualMove = [&](char const* id, int dx, int dy) {
        return host->pdWait([&] {
            t_canvas* cnv = host->getCanvasBySymbolStrict("main");
            t_gobj* obj = cnv ? host->resolveStableId("main", id) : nullptr;
            if (!obj) return;
            if (!cnv->gl_editor) canvas_create_editor(cnv);
            glist_noselect(cnv);
            glist_select(cnv, obj);
            canvas_undo_add(cnv, UNDO_MOTION, "motion", canvas_undo_set_move(cnv, 1));
            gobj_displace(obj, cnv, dx, dy);
            glist_noselect(cnv);
        }, 2000);
    };

    // 1. Two AI calls are two undo steps (L30), and redo brings both back with their tempIds.
    auto const ca = next("sua");
    CHECK(batch({ "main", ca, 0, 0, 0, 1, 0, "su_a", 700, 300, "obj", "f", 1, "1", 0, 5, 10, 0 }, ca));
    auto const cb = next("sub");
    CHECK(batch({ "main", cb, 0, 0, 0, 1, 0, "su_b", 760, 300, "obj", "f", 1, "2", 0, 5, 10, 0 }, cb));
    CHECK(text("su_a") == "f 1" && text("su_b") == "f 2");
    CHECK(step("undo"));
    CHECK(text("su_b").empty() && text("su_a") == "f 1");
    CHECK(step("undo"));
    CHECK(text("su_a").empty());
    CHECK(step("redo"));
    CHECK(text("su_a") == "f 1");
    CHECK(step("redo"));
    CHECK(text("su_b") == "f 2");

    // 2. A manual edit, then an AI call: undo takes back the AI call first, then the manual edit.
    auto const before = pos("su_a");
    CHECK(manualMove("su_a", 20, 30));
    CHECK(pos("su_a") == std::make_pair(before.first + 20, before.second + 30));
    auto const cc = next("suc");
    CHECK(batch({ "main", cc, 0, 0, 0, 1, 0, "su_c", 820, 300, "obj", "f", 1, "3", 0, 5, 10, 0 }, cc));
    CHECK(step("undo"));
    CHECK(text("su_c").empty() && pos("su_a") == std::make_pair(before.first + 20, before.second + 30));
    CHECK(step("undo"));
    CHECK(pos("su_a") == before);

    // 3. Deleting a wired object: undo restores the object, its wire and its tempId.
    auto const cw = next("suw");
    CHECK(batch({ "main", cw, 0, 0, 0, 2, 1,
        "su_o", 700, 400, "obj", "osc~", 1, "220",
        "su_g", 700, 450, "obj", "*~", 1, "0.1",
        "su_o", 0, "su_g", 0,
        0, 5, 10, 0 }, cw));
    CHECK(wired("su_o", "su_g"));
    auto const cd = next("sud");
    CHECK(batch({ "main", cd, 1, 0, 0, 0, 0, "su_o", 0, 5, 10, 0 }, cd));
    CHECK(text("su_o").empty());
    CHECK(step("undo"));
    CHECK(text("su_o") == "osc~ 220");
    CHECK(wired("su_o", "su_g"));

    // 4. Retype: undo restores the old class and keeps the id and the wire.
    auto const ce = next("sue");
    CHECK(batch({ "main", ce, 0, 0, 1, 0, 0, "su_g", "*~", 1, "0.5", 0, 5, 10, 0 }, ce));
    CHECK(text("su_g") == "*~ 0.5");
    CHECK(step("undo"));
    CHECK(text("su_g") == "*~ 0.1");
    CHECK(wired("su_o", "su_g"));

    // 5. Redo of the retype, then nothing left to redo: that answers 0 (an honest no-op), never 1.
    CHECK(step("redo"));
    CHECK(text("su_g") == "*~ 0.5");
    CHECK(!step("redo"));

    // 6. A layout verb that moves boxes is one undo step that puts them back (it no longer flushes the history).
    auto const cl = next("sul");
    CHECK(batch({ "main", cl, 0, 0, 0, 2, 0,
        "su_l1", 700, 600, "obj", "f", 1, "4",
        "su_l2", 700, 600, "obj", "f", 1, "5",
        0, 5, 10, 0 }, cl));
    auto const l1 = pos("su_l1"), l2 = pos("su_l2");
    auto const cg = next("sug");
    auto guard = h.call("/pd/layout_guard", { "main", 20, 10, 0, cg }, ("/pd/layout_guard/reply/" + cg).toStdString());
    CHECK(guard.has_value() && guard->num(0) > 0);
    CHECK(pos("su_l1") != l1 || pos("su_l2") != l2);
    CHECK(step("undo"));
    CHECK(pos("su_l1") == l1 && pos("su_l2") == l2);
    CHECK(step("undo")); // the build before it is still in the history
    CHECK(text("su_l1").empty() && text("su_l2").empty());

    // 7. Retype of a box that is NOT the last one (Pd moves a retyped box to the end; undo puts it back at its old
    //    index): undo and redo keep the id (review 2026-10-20, HIGH).
    auto const cm = next("sum");
    CHECK(batch({ "main", cm, 0, 0, 0, 3, 0,
        "su_m1", 900, 300, "obj", "f", 1, "1",
        "su_m2", 900, 350, "obj", "f", 1, "2",
        "su_m3", 900, 400, "obj", "f", 1, "3",
        0, 5, 10, 0 }, cm));
    auto const cr2 = next("sur");
    CHECK(batch({ "main", cr2, 0, 0, 1, 0, 0, "su_m2", "i", 1, "7", 0, 5, 10, 0 }, cr2));
    CHECK(text("su_m2") == "i 7");
    CHECK(step("undo"));
    CHECK(text("su_m2") == "f 2");
    CHECK(step("redo"));
    CHECK(text("su_m2") == "i 7");

    // 8. A batch with the layout guard on is still ONE step: undo takes back the box and the guard's moves.
    auto const m1 = pos("su_m1");
    auto const cy = next("suy");
    CHECK(batch({ "main", cy, 0, 0, 0, 1, 0, "su_y", 900, 300, "obj", "f", 1, "8", 1, 5, 10, 1 }, cy));
    CHECK(!text("su_y").empty());
    CHECK(step("undo"));
    CHECK(text("su_y").empty() && pos("su_m1") == m1);

    // 9. An AI call that changes nothing leaves no step and keeps the redo history (the step opens lazily).
    CHECK(step("redo")); // su_y back
    CHECK(step("undo")); // and gone again: one redo is waiting
    auto const cz = next("suz");
    CHECK(batch({ "main", cz, 1, 0, 0, 0, 0, "su_no_such_box", 0, 5, 10, 0 }, cz));
    CHECK(step("redo"));
    CHECK(!text("su_y").empty());

    // 10. A closed (non-GOP) subpatch has its own history; an AI edit there is undone there.
    auto const cs = next("sus");
    CHECK(batch({ "main", cs, 0, 0, 0, 1, 0, "su_sub", 1100, 300, "obj", "pd", 1, "su_sub", 0, 5, 10, 0 }, cs));
    auto const cs2 = next("sus");
    CHECK(batch({ "pd-su_sub", cs2, 0, 0, 0, 1, 0, "su_in", 40, 40, "obj", "f", 1, "5", 0, 5, 10, 0 }, cs2));
    auto inText = [&](char const* id) -> std::string {
        auto const c = next("sut");
        auto r = h.call("/pd/obj_get", { "pd-su_sub", c, id }, ("/pd/obj_get/reply/" + c).toStdString(), 2000);
        return r ? parseJson(*r)["text"].toString().toStdString() : std::string();
    };
    CHECK(inText("su_in") == "f 5");
    auto const cu = next("suu");
    auto u = h.call("/pd/undo", { "pd-su_sub", cu }, ("/pd/undo/reply/" + cu).toStdString());
    sleepMs(300);
    CHECK(u.has_value() && u->num(0) == 1);
    CHECK(inText("su_in").empty());
    CHECK(text("su_sub") == "pd su_sub"); // the parent's history is untouched
}

// P6.2 UF: /pd/undo_peek reads the canvas's undo queue without changing it: depth D (steps Ctrl+Z can take), redo R,
// the names of the newest steps (a Pd sequence is one step, named by its START; "AI edit" for every MCP call),
// capped at 32, and the canvas key. A GUI step is made here the way plugdata's Patch::startUndoSequence makes one (a
// sequence under a symbol name), with a motion entry inside.
void runUndoPeekCases(Harness& h, PluginProcessor& processor)
{
    std::printf("undo_peek: depth, redo, step names\n");
    auto* host = processor.getMcp();
    int corr = 0;
    auto next = [&corr](char const* p) { return juce::String(p) + juce::String(++corr); };
    struct Peeked {
        bool ok = false;
        int depth = -1, redo = -1;
        bool open = true; // P6.2 U1: a reply without the flag reads as open (the TS side refuses then)
        std::vector<std::string> top;
        std::string key;
    };
    auto peek = [&](char const* canvas) {
        Peeked p;
        auto const c = next("up");
        auto r = h.call("/pd/undo_peek", { canvas, c }, ("/pd/undo_peek/reply/" + c).toStdString(), 3000);
        if (!r) return p;
        auto const json = parseJson(*r);
        p.ok = json.isObject() && json["depth"].isInt() && json["redo"].isInt() && json["top"].isArray();
        p.depth = static_cast<int>(json["depth"]);
        p.redo = static_cast<int>(json["redo"]);
        if (auto const* top = json["top"].getArray())
            for (auto const& name : *top)
                p.top.push_back(name.toString().toStdString());
        p.key = json["canvasKey"].toString().toStdString();
        p.open = !json["open"].isBool() || static_cast<bool>(json["open"]);
        return p;
    };
    auto batch = [&](std::initializer_list<A> args, juce::String const& c) {
        auto r = h.call("/pd/batch_atomic", args, ("/pd/batch_atomic/reply/" + c).toStdString());
        return r.has_value();
    };
    // Hand-made entries on a canvas, under the Pd lock (as a GUI edit registers them).
    auto onCanvas = [&](char const* name, std::function<void(t_canvas*)> const& job) {
        return host->pdWait([&] {
            if (t_canvas* cnv = host->getCanvasBySymbolStrict(name)) job(cnv);
        }, 2000);
    };

    // 0. The capability is advertised; an unknown canvas is an error reply, never an empty queue.
    auto caps = h.call("/bridge/capabilities", { "up_caps" }, "/bridge/capabilities/reply");
    bool cap = false;
    if (caps)
        for (auto const& a : caps->args)
            cap = cap || a.text == "undo_peek";
    CHECK(caps.has_value() && cap);
    auto missing = h.call("/pd/undo_peek", { "pd-up_no_such_canvas", "up_err" }, "/pd/undo_peek/error/up_err", 3000);
    CHECK(missing.has_value());

    // 1. A fresh subpatch has an empty queue: D 0, R 0, no names.
    auto const cs = next("ups");
    CHECK(batch({ "main", cs, 0, 0, 0, 1, 0, "up_sub", 1300, 300, "obj", "pd", 1, "up_sub", 0, 5, 10, 0 }, cs));
    auto const empty = peek("pd-up_sub");
    CHECK(empty.ok && empty.depth == 0 && empty.redo == 0 && empty.top.empty());

    // 2. One AI construct plus a layout pass that moved boxes: two steps, both "AI edit". Reading changes nothing.
    auto const m0 = peek("main");
    CHECK(m0.ok && !m0.key.empty());
    auto const cl = next("upl");
    CHECK(batch({ "main", cl, 0, 0, 0, 2, 0,
        "up_l1", 1300, 600, "obj", "f", 1, "4",
        "up_l2", 1300, 600, "obj", "f", 1, "5",
        0, 5, 10, 0 }, cl));
    auto const cg = next("upg");
    auto guard = h.call("/pd/layout_guard", { "main", 20, 10, 0, cg }, ("/pd/layout_guard/reply/" + cg).toStdString());
    CHECK(guard.has_value() && guard->num(0) > 0);
    auto const m1 = peek("main");
    CHECK(m1.ok && m1.depth == m0.depth + 2 && m1.redo == 0);
    CHECK(m1.top.size() >= 2 && m1.top[0] == "AI edit" && m1.top[1] == "AI edit");
    CHECK(m1.key == m0.key);
    auto const m2 = peek("main");
    CHECK(m2.ok && m2.depth == m1.depth && m2.redo == m1.redo && m2.top == m1.top);

    // 3. In the subpatch: an AI edit, a GUI step named "Move" on top, then a nested sequence that counts once.
    auto const ci = next("upi");
    CHECK(batch({ "pd-up_sub", ci, 0, 0, 0, 1, 0, "up_in", 40, 40, "obj", "f", 1, "5", 0, 5, 10, 0 }, ci));
    auto const s1 = peek("pd-up_sub");
    CHECK(s1.ok && s1.depth == 1 && s1.top == std::vector<std::string> { "AI edit" });
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("Move")->s_name, nullptr);
        mcp::undo::motion(cnv);
        canvas_undo_add(cnv, UNDO_SEQUENCE_END, gensym("Move")->s_name, nullptr);
    }));
    auto const s2 = peek("pd-up_sub");
    CHECK(s2.ok && s2.depth == 2 && s2.redo == 0 && s2.top == (std::vector<std::string> { "Move", "AI edit" }));
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("Outer")->s_name, nullptr);
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("inner")->s_name, nullptr);
        mcp::undo::motion(cnv);
        canvas_undo_add(cnv, UNDO_SEQUENCE_END, gensym("inner")->s_name, nullptr);
        mcp::undo::motion(cnv);
        canvas_undo_add(cnv, UNDO_SEQUENCE_END, gensym("Outer")->s_name, nullptr);
    }));
    auto const s3 = peek("pd-up_sub");
    CHECK(s3.ok && s3.depth == 3 && s3.redo == 0 && s3.top == (std::vector<std::string> { "Outer", "Move", "AI edit" }));

    // 4. One undo takes the whole nested sequence: D - 1, R 1, "Move" on top again; a redo puts it back.
    auto const cu = next("upu");
    auto u = h.call("/pd/undo", { "pd-up_sub", cu }, ("/pd/undo/reply/" + cu).toStdString());
    CHECK(u.has_value() && u->num(0) == 1);
    auto const s4 = peek("pd-up_sub");
    CHECK(s4.ok && s4.depth == 2 && s4.redo == 1 && !s4.top.empty() && s4.top[0] == "Move");
    auto const cr = next("upr");
    auto rd = h.call("/pd/redo", { "pd-up_sub", cr }, ("/pd/redo/reply/" + cr).toStdString());
    CHECK(rd.has_value() && rd->num(0) == 1);
    auto const s5 = peek("pd-up_sub");
    CHECK(s5.ok && s5.depth == 3 && s5.redo == 0 && !s5.top.empty() && s5.top[0] == "Outer");

    // 5. The names are capped at 32, the depth is not: 40 single entries on top.
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        for (int i = 0; i < 40; ++i)
            mcp::undo::motion(cnv);
    }));
    auto const s6 = peek("pd-up_sub");
    CHECK(s6.ok && s6.depth == 43 && s6.redo == 0 && s6.top.size() == 32 && s6.top[0] == "motion" && s6.top[31] == "motion");

    // 6. P6.2 U1 (UF review LOW): a closed queue says open:false; a sequence still open (a GUI drag or an AI call in
    //    progress: a START with no END yet) says open:true, because its entries count as several steps now and as one
    //    once the END lands. After the END: open:false again, and the run is one step.
    CHECK(s6.ok && !s6.open);
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("Drag")->s_name, nullptr);
        mcp::undo::motion(cnv);
        mcp::undo::motion(cnv);
    }));
    auto const s7 = peek("pd-up_sub");
    CHECK(s7.ok && s7.open);
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        canvas_undo_add(cnv, UNDO_SEQUENCE_END, gensym("Drag")->s_name, nullptr);
    }));
    auto const s8 = peek("pd-up_sub");
    CHECK(s8.ok && !s8.open && s8.depth == 44 && !s8.top.empty() && s8.top[0] == "Drag");

    // 7. P6.2 review (fork L1): `open` stays true for a START with no END ANYWHERE behind the undo position, also below
    //    closed steps. Pd keeps no "sequence open" state (t_undo: u_queue, u_last, u_cleanstate, u_doing), so a stale
    //    START that never got its END and a sequence still open around a closed AI step look the same in the queue:
    //    narrowing the flag to "before the first closed step" would read the second as closed (a depth that drops
    //    when its END lands). The server refuses the turn undo and says how to clear it (save and reopen the patch).
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) {
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("Stale")->s_name, nullptr);
        mcp::undo::motion(cnv);
        canvas_undo_add(cnv, UNDO_SEQUENCE_START, gensym("AI edit")->s_name, nullptr);
        mcp::undo::motion(cnv);
        canvas_undo_add(cnv, UNDO_SEQUENCE_END, gensym("AI edit")->s_name, nullptr);
    }));
    auto const s9 = peek("pd-up_sub");
    CHECK(s9.ok && s9.open && !s9.top.empty() && s9.top[0] == "AI edit");
    CHECK(onCanvas("pd-up_sub", [](t_canvas* cnv) { mcp::undo::motion(cnv); }));
    auto const s10 = peek("pd-up_sub");
    CHECK(s10.ok && s10.open && s10.depth == s9.depth + 1);
}

// P3.5 S0: the identity service will detect GUI edits by recomputing a per-canvas stamp (object count plus a hash
// of the gl_list pointers) under the Pd lock on every version read. Measure one walk on a canvas the size of the
// DEV.md standard patch (37 objects) and on a large one (500): the plan stops and re-plans above 100 us.
// Plan 5 P5.1 (AI-lane gauntlet smoke 2026-10-20): a 55 Hz drone read "fundamental 0 Hz" because the spectral
// window was 1024 samples and the autocorrelation searched lags up to N/2 only (lowest pitch ~94 Hz at 48 kHz).
// A bass the artist asks for must be measurable: 55 Hz (and 41 Hz, low E) within 3 %.
void runSpectralPitchCases()
{
    std::printf("spectral pitch window\n");
    double const sr = 48000.0;
    auto tone = [sr](double hz, int n) {
        std::vector<float> v(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            double const t = i / sr;
            // A saw-ish drone: fundamental plus two harmonics.
            v[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * M_PI * hz * t) + 0.25 * std::sin(4 * M_PI * hz * t)
                                                          + 0.12 * std::sin(6 * M_PI * hz * t));
        }
        return v;
    };
    auto fundamentalOf = [](juce::String const& json) {
        auto v = juce::JSON::parse(json);
        return v.isObject() ? static_cast<double>(v.getProperty("fundamental", 0.0)) : -1.0;
    };
    for (double hz : { 41.2, 55.0, 110.0, 440.0 }) {
        auto buf = tone(hz, static_cast<int>(sr)); // 1 s, as measure_spectral captures
        double const armed = fundamentalOf(armedSpectralJson(buf.data(), static_cast<int>(buf.size()), sr));
        double const render = fundamentalOf(renderSpectralJson(buf.data(), static_cast<int>(buf.size()), sr));
        std::printf("  %.1f Hz -> armed %.2f, render %.2f\n", hz, armed, render);
        CHECK(std::abs(armed - hz) <= hz * 0.03);
        CHECK(std::abs(render - hz) <= hz * 0.03);
    }
}

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

// P3.5 S5: identity entries are validated by a weak reference per bound object, so a freed object's id never
// resolves to a new object Pd allocated at the same address (Report F3); an abstraction reload rebinds the ids of
// its instances through tombstones (L9). Both run in the Pd context on the default patch.
void runIdentityValidityCases(PluginProcessor& processor)
{
    std::printf("identity: address reuse, abstraction reload\n");
    auto* host = processor.getMcp();
    int reused = 0, staleResolves = 0, rounds = 0;
    bool reloadRebound = false, reloadRan = false;
    juce::String reloadNote;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        if (!cnv) return;
        auto newObj = [cnv](char const* cls) {
            t_atom a[3];
            SETFLOAT(a, 700);
            SETFLOAT(a + 1, 40);
            SETSYMBOL(a + 2, gensym(cls));
            pd_typedmess(&cnv->gl_pd, gensym("obj"), 3, a);
            return pd::Interface::getNewest(cnv);
        };
        for (int i = 0; i < 400 && reused < 20; ++i, ++rounds) {
            t_gobj* old = newObj("osc~");
            auto const id = "s5r" + std::to_string(i);
            host->identity.bind(cnv, id, old);
            glist_delete(cnv, old);
            t_gobj* fresh = newObj("osc~"); // untracked: what a GUI create looks like to the identity layer
            if (fresh == old)
                ++reused;
            if (host->resolveStableId("main", juce::String(id)) != nullptr)
                ++staleResolves;
            glist_delete(cnv, fresh);
        }

        // Abstraction reload: an instance bound to an id is recreated by reloadAbstractions; the id follows.
        juce::File dir(juce::String::fromUTF8(canvas_getdir(cnv)->s_name));
        auto absFile = dir.getChildFile("s5abs.pd");
        if (!absFile.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 inlet;\n")) {
            reloadNote = "could not write " + absFile.getFullPathName();
            return;
        }
        t_gobj* inst = newObj("s5abs");
        juce::String absName;
        if (!inst || !pd::getAbstractionFileName(inst, absName)) {
            reloadNote = "abstraction not instantiated";
            return;
        }
        host->identity.bind(cnv, "s5abs1", inst);
        host->tombstoneAbstractionInstances(absName);
        absFile.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 inlet;\n#X obj 10 40 outlet;\n");
        processor.reloadAbstractions(absFile, nullptr);
        reloadRan = true;
        host->rebindTombstones();
        t_gobj* after = host->resolveStableId("main", "s5abs1");
        juce::String afterName;
        reloadRebound = after && pd::getAbstractionFileName(after, afterName) && afterName == absName
            && pd::Interface::checkObject(after) && obj_noutlets(pd::Interface::checkObject(after)) == 1;
        if (after)
            glist_delete(cnv, after);
        absFile.deleteFile();
    }, 20000));
    std::printf("  address reuse: %d of %d rounds reused the address, %d stale resolves\n", reused, rounds, staleResolves);
    CHECK(staleResolves == 0);
    if (reused == 0)
        std::printf("  note: the allocator never reused an address here (the check above is then weaker)\n");
    if (reloadNote.isNotEmpty())
        std::printf("  abstraction reload not run: %s\n", reloadNote.toRawUTF8());
    CHECK(reloadRan && reloadRebound);
}

// P3.5 S7: identity sidecar v2 on a scratch subpatch: round trip, an edited .pd detected by its hash, entries whose
// class no longer matches get fresh names (owner decision O2), a truncated sidecar is ignored, v1 is still read.
void runSidecarCases(PluginProcessor& processor)
{
    std::printf("identity sidecar v2\n");
    auto* host = processor.getMcp();
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mcp_s7_sidecar");
    dir.createDirectory();
    auto pdFile = dir.getChildFile("s7.pd");
    bool roundTrip = false, editedSeen = false, classRejected = false, truncatedIgnored = false, v1Read = false;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        if (!cnv) return;
        auto obj = [](t_canvas* c, char const* cls, int x) {
            t_atom a[3];
            SETFLOAT(a, x);
            SETFLOAT(a + 1, 400);
            SETSYMBOL(a + 2, gensym(cls));
            pd_typedmess(&c->gl_pd, gensym("obj"), 3, a);
            return pd::Interface::getNewest(c);
        };
        t_atom sa[4];
        SETFLOAT(sa, 900);
        SETFLOAT(sa + 1, 400);
        SETSYMBOL(sa + 2, gensym("pd"));
        SETSYMBOL(sa + 3, gensym("s7sub"));
        pd_typedmess(&cnv->gl_pd, gensym("obj"), 4, sa);
        auto* sub = reinterpret_cast<t_canvas*>(pd::Interface::getNewest(cnv));
        t_gobj* osc = obj(sub, "osc~", 10);
        t_gobj* mul = obj(sub, "*~", 80);
        host->identity.bind(sub, "s7osc", osc);
        host->identity.bind(sub, "s7mul", mul);
        auto& identity = host->identity;

        pdFile.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 osc~;\n");
        auto sidecar = MCPBridge::buildIdentitySidecar(&processor, sub);
        CHECK(MCPBridge::writeIdentitySidecarFile(sidecar, pdFile).isEmpty());
        juce::String note;
        auto read = MCPBridge::readIdentitySidecar(pdFile, note);
        if (auto* o = read.getDynamicObject()) {
            auto stats = MCPBridge::mcpApplyIdentitySidecar(&processor, o, sub, pdFile);
            roundTrip = stats.fileMatches && stats.restored == 2 && stats.renamed == 0
                && identity.resolve(sub, "s7osc") == osc && identity.resolve(sub, "s7mul") == mul;
        }

        pdFile.appendText("#X obj 10 40 dac~;\n"); // edited behind the sidecar's back
        read = MCPBridge::readIdentitySidecar(pdFile, note);
        if (auto* o = read.getDynamicObject()) {
            auto stats = MCPBridge::mcpApplyIdentitySidecar(&processor, o, sub, pdFile);
            editedSeen = !stats.fileMatches && stats.restored == 2; // the objects still match by place and class
        }

        // The saved classes no longer match the objects at those places: no stale names, fresh ones instead.
        auto text = juce::File(pdFile.getFullPathName() + ".mcpids.json").loadFileAsString();
        juce::File(pdFile.getFullPathName() + ".mcpids.json").replaceWithText(text.replace("\"osc~\"", "\"lop~\"").replace("\"*~\"", "\"hip~\""));
        read = MCPBridge::readIdentitySidecar(pdFile, note);
        if (auto* o = read.getDynamicObject()) {
            auto stats = MCPBridge::mcpApplyIdentitySidecar(&processor, o, sub, pdFile);
            classRejected = stats.restored == 0 && stats.renamed == 2 && identity.resolve(sub, "s7osc") == nullptr
                && !identity.idOf(osc).empty() && !identity.idOf(mul).empty();
            CHECK(MCPBridge::sidecarReport(pdFile, stats, {}).contains("fresh names"));
        }

        auto full = juce::File(pdFile.getFullPathName() + ".mcpids.json").loadFileAsString();
        juce::File(pdFile.getFullPathName() + ".mcpids.json").replaceWithText(full.substring(0, full.length() / 2));
        note.clear();
        truncatedIgnored = MCPBridge::readIdentitySidecar(pdFile, note).isVoid() && note.contains("truncated");

        juce::File(pdFile.getFullPathName() + ".mcpids.json")
            .replaceWithText("{\"format\":\"MCP-IDENTITY\",\"v\":1,\"root\":[{\"i\":0,\"id\":\"v1osc\"},{\"i\":1,\"id\":\"v1mul\"}],\"sub\":{}}");
        read = MCPBridge::readIdentitySidecar(pdFile, note);
        if (auto* o = read.getDynamicObject()) {
            auto stats = MCPBridge::mcpApplyIdentitySidecar(&processor, o, sub, pdFile);
            v1Read = stats.restored == 2 && identity.resolve(sub, "v1osc") == osc && identity.resolve(sub, "v1mul") == mul;
        }
        glist_delete(cnv, &sub->gl_gobj);
    }, 10000));
    dir.deleteRecursively();
    std::printf("  round trip %d, edited .pd seen %d, class mismatch renamed %d, truncated ignored %d, v1 read %d\n",
        roundTrip, editedSeen, classRejected, truncatedIgnored, v1Read);
    CHECK(roundTrip && editedSeen && classRejected && truncatedIgnored && v1Read);
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

// T2b (armed signal trace): /meter/trace <canvas> <corr> arm <windowMs> <focusCsv> opens a tap on the signal outlets
// (at most the free probe slots) and keeps the peak of each over the window; /meter/trace <canvas> <corr> read closes
// it and answers the trace JSON computed from those window peaks, plus `window` and `levels`. A triggered voice is
// silent between notes, so the plain trace (the last audio block) cannot see it; the armed one does.
double levelOf(juce::var const& json, char const* tempId)
{
    auto const levels = json["levels"];
    if (auto* arr = levels.getArray())
        for (auto const& l : *arr)
            if (l["tempId"].toString() == tempId)
                return static_cast<double>(l["peakDb"]);
    return -1e9;
}

void runArmedTraceCases(Harness& h)
{
    std::printf("armed signal trace (T2b)\n");
    // A gated voice: osc~ -> vca (*~) -> out (*~ 0.5) -> dac~; the VCA's right inlet is [line~] fed by [r t2bgate].
    auto made = h.call("/pd/batch_atomic",
        { "main", "t2b1", 0, 0, 0, 6, 6,
            "t2b_osc", 600, 40, "obj", "osc~", 1, "440",
            "t2b_vca", 600, 100, "obj", "*~", 0,
            "t2b_out", 600, 140, "obj", "*~", 1, "0.5",
            "t2b_dac", 600, 180, "obj", "dac~", 0,
            "t2b_rx", 700, 40, "obj", "r", 1, "t2bgate",
            "t2b_env", 700, 70, "obj", "line~", 0,
            "t2b_osc", 0, "t2b_vca", 0,
            "t2b_env", 0, "t2b_vca", 1,
            "t2b_vca", 0, "t2b_out", 0,
            "t2b_out", 0, "t2b_dac", 0,
            "t2b_out", 0, "t2b_dac", 1,
            "t2b_rx", 0, "t2b_env", 0,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/t2b1");
    CHECK(made.has_value() && made->num(0) == 6);
    h.fire("/pd/dsp", { 1 });
    sleepMs(300);

    // The plain trace is unchanged: no window fields.
    auto plain = h.call("/meter/trace", { "main", "t2bp" }, "/meter/trace/reply/t2bp");
    CHECK(plain.has_value());
    if (plain) {
        auto const json = parseJson(*plain);
        CHECK(json.hasProperty("chokePoint"));
        CHECK(!json.hasProperty("window"));
    }

    // Armed, the gate opened and released inside the window: the VCA sounded, and the trace reads that.
    auto armed = h.call("/meter/trace", { "main", "t2ba", "arm", 900, "" }, "/meter/trace/reply/t2ba");
    CHECK(armed.has_value());
    if (armed) {
        auto const json = parseJson(*armed);
        CHECK(static_cast<bool>(json["armed"]));
        CHECK(static_cast<int>(json["taps"]) >= 4);
        CHECK(static_cast<int>(json["cap"]) == 16);
        CHECK(!static_cast<bool>(json["sampled"]));
    }
    // The gate: the note opens the envelope (a jump to 1, then back to 0), set on the envelope object itself so the
    // test does not depend on a receiver binding.
    auto up = h.call("/pd/obj_set", { "main", "t2bg1", "t2b_env", 0, "float", 1, 1.0 }, "/pd/obj_set/reply/t2bg1");
    CHECK(up.has_value() && up->num(0) == 1.0);
    sleepMs(200);
    h.call("/pd/obj_set", { "main", "t2bg0", "t2b_env", 0, "float", 1, 0.0 }, "/pd/obj_set/reply/t2bg0");
    sleepMs(150);
    auto read = h.call("/meter/trace", { "main", "t2br", "read" }, "/meter/trace/reply/t2br");
    CHECK(read.has_value());
    if (read) {
        auto const json = parseJson(*read);
        CHECK(static_cast<int>(json["window"]["blocks"]) > 0);
        CHECK(levelOf(json, "t2b_vca") > -20.0); // the voice sounded while the note was held
        CHECK(levelOf(json, "t2b_env") > -3.0);  // the envelope reached ~1
        CHECK(json["chokePoint"].toString() != "t2b_vca");
        auto const* silentEnv = json["silentEnvelopes"].getArray();
        CHECK(silentEnv != nullptr && !silentEnv->contains("t2b_env"));
    }

    // The same voice and a window with no note: the VCA reads silent over the whole window, and the trace says why.
    auto quietArm = h.call("/meter/trace", { "main", "t2bb", "arm", 400, "" }, "/meter/trace/reply/t2bb");
    CHECK(quietArm.has_value());
    sleepMs(250);
    auto quiet = h.call("/meter/trace", { "main", "t2bc", "read" }, "/meter/trace/reply/t2bc");
    CHECK(quiet.has_value());
    if (quiet) {
        auto const json = parseJson(*quiet);
        CHECK(levelOf(json, "t2b_vca") < -80.0);
        CHECK(levelOf(json, "t2b_osc") > -20.0); // the oscillator runs all the time
        auto const* silentEnv = json["silentEnvelopes"].getArray();
        CHECK(silentEnv != nullptr && silentEnv->contains("t2b_env"));
        CHECK(json["reason"].toString().containsIgnoreCase("envelope") || json["reason"].toString().containsIgnoreCase("multiplier"));
    }

    // A read with no window open is refused, not answered with a stale trace.
    auto none = h.call("/meter/trace", { "main", "t2bd", "read" }, "/meter/trace/reply/t2bd");
    CHECK(none.has_value() && parseJson(*none).hasProperty("error"));

    // More signal outlets than probe slots: sampled along the path, the cap reported, a focus object always tapped.
    std::vector<A> args { "main", "t2b2", 0, 0, 0, 20, 19 };
    for (int i = 0; i < 20; ++i) {
        args.push_back(juce::String("t2c_") + juce::String(i));
        args.push_back(300);
        args.push_back(40 + i * 20);
        args.push_back("obj");
        args.push_back(i == 0 ? "osc~" : "*~");
        args.push_back(1);
        args.push_back(i == 0 ? "220" : "0.9");
    }
    for (int i = 1; i < 20; ++i) {
        args.push_back(juce::String("t2c_") + juce::String(i - 1));
        args.push_back(0);
        args.push_back(juce::String("t2c_") + juce::String(i));
        args.push_back(0);
    }
    for (int v : { 0, 5, 10, 0 })
        args.push_back(v);
    auto big = h.callList("/pd/batch_atomic", args, "/pd/batch_atomic/reply/t2b2");
    CHECK(big.has_value() && big->num(0) == 20);
    sleepMs(100);
    auto capped = h.call("/meter/trace", { "main", "t2be", "arm", 300, "t2c_7" }, "/meter/trace/reply/t2be");
    CHECK(capped.has_value());
    if (capped) {
        auto const json = parseJson(*capped);
        CHECK(static_cast<bool>(json["armed"]));
        CHECK(static_cast<int>(json["taps"]) == 16);
        CHECK(static_cast<int>(json["cap"]) == 16);
        CHECK(static_cast<bool>(json["sampled"]));
        CHECK(static_cast<int>(json["signalOutlets"]) >= 20);
    }
    sleepMs(150);
    auto capRead = h.call("/meter/trace", { "main", "t2bf", "read" }, "/meter/trace/reply/t2bf");
    CHECK(capRead.has_value());
    if (capRead) {
        auto const json = parseJson(*capRead);
        CHECK(levelOf(json, "t2c_7") > -40.0); // the focus object was tapped and sounds
        auto const* levels = json["levels"].getArray();
        CHECK(levels != nullptr && levels->size() == 16);
    }
    // The probe slots are free again after the read.
    auto status = h.call("/meter/status", { "t2bs" }, "/meter/status/reply/t2bs");
    CHECK(status.has_value() && status->num(0) == 0);

    h.fire("/pd/dsp", { 0 });
    std::vector<A> cleanup { "main", "t2bx", 26, 0, 0, 0, 0 };
    for (char const* id : { "t2b_osc", "t2b_vca", "t2b_out", "t2b_dac", "t2b_rx", "t2b_env" })
        cleanup.push_back(id);
    for (int i = 0; i < 20; ++i)
        cleanup.push_back(juce::String("t2c_") + juce::String(i));
    for (int v : { 0, 5, 10, 0 })
        cleanup.push_back(v);
    h.callList("/pd/batch_atomic", cleanup, "/pd/batch_atomic/reply/t2bx");
}

// T2b (console errors carry their object): a console line raised by an object comes with that object's tempId as
// a second argument of /pd/error or /pd/log (the first stays the text, as before).
void runConsoleObjectCases(Harness& h)
{
    std::printf("console error carries its object (T2b)\n");
    auto made = h.call("/pd/batch_atomic",
        { "main", "t2e1", 0, 0, 0, 2, 1,
            "t2e_rx", 900, 40, "obj", "r", 1, "t2berr",
            "t2e_add", 900, 80, "obj", "+", 1, "1",
            "t2e_rx", 0, "t2e_add", 0,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/t2e1");
    CHECK(made.has_value() && made->num(0) == 2);
    // A message [+ 1] does not understand makes the object print "+: no method for 'foo'"; the line comes with the
    // object's tempId. Sent to the object itself (obj_set), not through a receiver.
    h.fire("/pd/obj_set", { "main", "t2ee", "t2e_add", 0, "foo", 0 });
    auto const line = h.replies.waitForConsoleLine("foo", 3000);
    CHECK(line.has_value());
    if (!line)
        std::printf("  unread replies: %s\n", h.replies.describe().c_str());
    if (line) {
        std::printf("  console line: %s [%s]\n", line->str(0).c_str(), line->address.c_str());
        CHECK(line->args.size() == 2);
        CHECK(line->str(1) == "t2e_add");
    }
    h.call("/pd/batch_atomic", { "main", "t2ex", 2, 0, 0, 0, 0, "t2e_rx", "t2e_add", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/t2ex");
}

// MIDI capture (P3.4 S6): the audio thread only queues raw note on/off; the bridge pairs them off the audio
// thread. Three notes played through processBlock come back as three closed notes.
void runMidiCaptureCases(Harness& h, PluginProcessor& processor, AudioControl& audio)
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
    h.replies.clear();
    host->onMessageThread([bridge] { bridge->stopMidiCaptureAndStage(); return true; }, 2000, "test");
    CHECK(!bridge->isMidiCapturing());
    // P11 take files: the take reaches the TS in the /pd/midi/event reply (the TS writes midi-take.json from it); the
    // bridge writes no file of its own (scripts/test-bridge-safety.sh refuses a hard-coded user path in the source).
    auto event = h.replies.waitFor("/pd/midi/event", 2000);
    CHECK(event.has_value());
    if (event) {
        auto const take = parseJson(*event);
        CHECK(take["id"].toString().startsWith("midi_"));
        CHECK(static_cast<int>(take["noteCount"]) == 3);
    }
}

// P5.1 sink floor S1: /pd/limiter answers the LIVE limiter state and the toolbar threshold
// ({"enabled":bool,"thresholdDb":-12|-6|0|3}), read-only; the capability `limiter` is advertised. The state is set
// here directly (the AI never switches it) and restored after.
void runLimiterCases(Harness& h, PluginProcessor& processor)
{
    std::printf("limiter read\n");
    auto* host = processor.getMcp();
    bool const before = processor.getEnableLimiter();
    int const thresholdBefore = host->onMessageThread(
        [] { return SettingsFile::getInstance()->getProperty<int>("limiter_threshold"); }, 2000, "test").value_or(1);
    auto setThreshold = [&](int index) {
        host->onMessageThread([&processor, index] { processor.setLimiterThreshold(index); return true; }, 2000, "test");
    };

    processor.setEnableLimiter(false);
    setThreshold(0);
    auto off = h.call("/pd/limiter", { "lim1" }, "/pd/limiter/reply/lim1");
    CHECK(off.has_value() && off->str(0).rfind("{\"enabled\":false,\"thresholdDb\":-12,\"ceiling\":{", 0) == 0);
    processor.setEnableLimiter(true);
    setThreshold(3);
    auto on = h.call("/pd/limiter", { "lim2" }, "/pd/limiter/reply/lim2");
    CHECK(on.has_value() && on->str(0).rfind("{\"enabled\":true,\"thresholdDb\":3,\"ceiling\":{", 0) == 0);
    CHECK(processor.getEnableLimiter()); // the read changed nothing
    processor.setEnableLimiter(before);
    setThreshold(thresholdBefore);

    auto caps = h.call("/bridge/capabilities", { "lim3" }, "/bridge/capabilities/reply");
    bool limiterCap = false;
    if (caps)
        for (auto const& a : caps->args)
            limiterCap = limiterCap || a.text == "limiter";
    CHECK(caps.has_value() && limiterCap);
}

// P9 Part B: the output ceiling and its breaker on the FINAL output (McpHost::audioBlockFinished, the processBlock
// tail). Hook level on hand-made buffers, then through processBlock with this file's audio thread paused (the test
// thread renders the blocks): limiter off, limiter on at +3 dB, master x2 with a 0.8 signal, the breaker. Then the
// /pd/limiter fields and the routes that must NOT switch the ceiling off (a patch message, [s mcp_host], a settings
// file reload, an AI verb). The unit tests on the DSP itself: Tests/mcp/output_ceiling_test.cpp.
float bufferPeak(juce::AudioBuffer<float> const& b, int fromCh = 0, int toCh = -1)
{
    float p = 0.0f;
    int const end = toCh < 0 ? b.getNumChannels() : toCh;
    for (int ch = fromCh; ch < end; ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            p = std::max(p, std::abs(b.getSample(ch, i)));
    return p;
}

bool bufferFinite(juce::AudioBuffer<float> const& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getSample(ch, i))) return false;
    return true;
}

void fillSine(juce::AudioBuffer<float>& b, float amplitude)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(ch, i, amplitude * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f));
}

void runOutputCeilingCases(Harness& h, PluginProcessor& processor, AudioControl& audio)
{
    std::printf("output ceiling (P9)\n");
    auto* host = processor.getMcp();
    auto& ceiling = host->session.outputCeiling;
    bool const limiterBefore = processor.getEnableLimiter();
    int const thresholdBefore = host->onMessageThread(
        [] { return SettingsFile::getInstance()->getProperty<int>("limiter_threshold"); }, 2000, "test").value_or(1);
    PlugDataParameter* volume = nullptr;
    for (auto* p : processor.getParameters())
        if (auto* param = dynamic_cast<PlugDataParameter*>(p); param && param->getTitle().toString() == "volume")
            volume = param;
    CHECK(volume != nullptr);
    if (!volume) return;
    float const volumeBefore = volume->getUnscaledValue();

    audio.paused = true; // the test thread renders from here on
    sleepMs(60);
    ceiling.setStandalone(true);
    ceiling.setEnabled(true);
    int const outputs = processor.getTotalNumOutputChannels();
    CHECK(outputs >= 1);
    // The console (identical consecutive lines fold into one entry, counted in the 5th field).
    auto consoleCount = [&](juce::String const& text) {
        return host->onMessageThread([&processor, text] {
            int n = 0;
            for (auto const& m : processor.getConsoleMessages())
                if (std::get<1>(m).contains(text)) n += std::get<4>(m);
            return n;
        }, 2000, "test").value_or(-1);
    };
    // The setting as stored: 1 = true, 0 = false, 2 = anything else.
    auto storedSetting = [&] {
        return host->onMessageThread([] {
            auto const v = SettingsFile::getInstance()->getPropertyAsValue("mcp_bridge_ai_ceiling").getValue();
            return v.isBool() ? (static_cast<bool>(v) ? 1 : 0) : 2;
        }, 2000, "test").value_or(-1);
    };
    auto waitUntil = [&](std::function<bool()> const& cond, int timeoutMs) {
        for (int t = 0; t < timeoutMs; t += 20) {
            if (cond()) return true;
            sleepMs(20);
        }
        return cond();
    };
    juce::String const tripLine = "Output ceiling: sustained full-scale sound, turned down 20 dB";
    juce::String const releaseLine = "Output ceiling: level back to normal, breaker released";

    // Hook level: +12 dBFS with a NaN and an inf in, at most 0 dBFS and finite out; the counters count.
    {
        auto const t0 = ceiling.read();
        juce::AudioBuffer<float> b(outputs, 512);
        fillSine(b, 3.98f);
        b.setSample(0, 7, std::numeric_limits<float>::infinity());
        b.setSample(outputs - 1, 9, std::numeric_limits<float>::quiet_NaN());
        host->audioBlockFinished(b);
        CHECK(bufferPeak(b) <= 1.0f);
        CHECK(bufferFinite(b));
        auto const t1 = ceiling.read();
        CHECK(t1.clampedSamples > t0.clampedSamples);
        CHECK(t1.nonFiniteSamples == t0.nonFiniteSamples + 2);
        CHECK(t1.clampedBlocks == t0.clampedBlocks + 1);
    }
    // Output channels only: a channel past the outputs is left alone.
    {
        juce::AudioBuffer<float> b(outputs + 2, 256);
        fillSine(b, 3.98f);
        host->audioBlockFinished(b);
        CHECK(bufferPeak(b, 0, outputs) <= 1.0f);
        CHECK(bufferPeak(b, outputs) > 3.0f);
    }
    // The armed window (review L2): its peak scans the output channels only; it counts the non-finite samples.
    {
        h.call("/meter/arm", { "ceilP" }, "/meter/arm/reply/ceilP");
        juce::AudioBuffer<float> b(outputs + 2, 256);
        b.clear();
        for (int ch = outputs; ch < outputs + 2; ++ch)
            for (int i = 0; i < 256; ++i)
                b.setSample(ch, i, 0.5f);
        b.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
        host->audioBlockFinished(b);
        auto r = h.call("/meter/read", { "ceilQ" }, "/meter/read/reply/ceilQ");
        CHECK(r.has_value() && r->num(1) < 0.01);                    // the input-only channels are not the output
        CHECK(r.has_value() && r->args.size() >= 10 && r->num(7) == 1); // one NaN in the window
        CHECK(r.has_value() && r->args.size() >= 10 && r->num(8) == 0); // no breaker trip
    }
    // The setting off (the owner's GUI path): bit-identical.
    {
        host->setOutputCeilingFromGui(false);
        juce::AudioBuffer<float> b(outputs, 512);
        fillSine(b, 3.98f);
        juce::AudioBuffer<float> before(b);
        host->audioBlockFinished(b);
        bool same = true;
        for (int ch = 0; ch < outputs; ++ch)
            same = same && std::memcmp(b.getReadPointer(ch), before.getReadPointer(ch), 512 * sizeof(float)) == 0;
        CHECK(same);
        auto off = h.call("/pd/limiter", { "ceil0" }, "/pd/limiter/reply/ceil0");
        auto const offJson = off ? parseJson(*off) : juce::var();
        CHECK(off.has_value() && !static_cast<bool>(offJson["ceiling"]["active"]));
        CHECK(offJson["ceiling"]["setting"].toString() == "off");
        CHECK(offJson["ceiling"]["reason"].toString() == "setting_off");
        host->setOutputCeilingFromGui(true);
        CHECK(ceiling.isEnabled());
    }

    // Through processBlock: [osc~ 440] -> [*~ 4] -> [dac~ 1 2] (+12 dBFS).
    auto made = h.call("/pd/batch_atomic",
        { "main", "ceil1", 0, 0, 0, 3, 3,
            "ceilo", 600, 40, "obj", "osc~", 1, "440",
            "ceilg", 600, 80, "obj", "*~", 1, "4",
            "ceild", 600, 120, "obj", "dac~", 0,
            "ceilo", 0, "ceilg", 0,
            "ceilg", 0, "ceild", 0,
            "ceilg", 0, "ceild", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ceil1");
    CHECK(made.has_value() && made->num(0) == 3);
    h.fire("/pd/dsp", { 1 });
    sleepMs(200);
    juce::AudioBuffer<float> buffer(std::max(2, outputs), 512);
    juce::MidiBuffer midi;
    // `blocks` processBlock calls on silence in; the peak of the last `tail` of them.
    auto render = [&](int blocks, int tail) {
        float peak = 0.0f;
        for (int k = 0; k < blocks; ++k) {
            buffer.clear();
            midi.clear();
            processor.processBlock(buffer, midi);
            if (k >= blocks - tail) peak = std::max(peak, bufferPeak(buffer));
        }
        return peak;
    };
    auto setThreshold = [&](int index) {
        host->onMessageThread([&processor, index] { processor.setLimiterThreshold(index); return true; }, 2000, "test");
    };
    auto quiet = [&] { // off for one block resets the breaker (full gain, a fresh window), then on again
        ceiling.setEnabled(false);
        render(1, 1);
        ceiling.setEnabled(true);
    };

    // Limiter off: the hot chain leaves at most 0 dBFS (8 blocks ~ 93 ms: under the breaker window).
    processor.setEnableLimiter(false);
    volume->setUnscaledValueNotifyingHost(0.8f); // unity
    render(20, 1);
    quiet();
    {
        auto const t0 = ceiling.read();
        float const peak = render(8, 6);
        CHECK(peak <= 1.0f);
        CHECK(peak > 0.9f); // clamped, not muted
        CHECK(ceiling.read().clampedSamples > t0.clampedSamples);
    }
    // Limiter on at +3 dB (PlugData's limiter lets up to +3 dB through): the ceiling still holds 0 dBFS.
    processor.setEnableLimiter(true);
    setThreshold(3);
    quiet();
    {
        float const peak = render(8, 6);
        CHECK(peak <= 1.0f);
    }
    processor.setEnableLimiter(false);
    // The breaker: the hot chain held for ~460 ms is sustained full-scale loudness: it trips once and the output then
    // sits 20 dB down. The armed window reports the trip (args 8, 9); the console says it once (message-thread
    // poller). Calm (volume to 0): released after ~1.4 s, said once too.
    quiet();
    {
        int const tripsSaid = consoleCount(tripLine), releasesSaid = consoleCount(releaseLine);
        h.call("/meter/arm", { "ceilA" }, "/meter/arm/reply/ceilA");
        auto const t0 = ceiling.read();
        float const peak = render(40, 5);
        auto const t1 = ceiling.read();
        CHECK(t1.breakerTrips == t0.breakerTrips + 1);
        CHECK(t1.breakerEngaged);
        CHECK(peak <= mcp::OutputCeiling::kBreakerGain + 1e-5f);
        auto r = h.call("/meter/read", { "ceilB" }, "/meter/read/reply/ceilB");
        CHECK(r.has_value() && r->args.size() >= 10 && r->num(8) == 1); // one trip in the window
        CHECK(r.has_value() && r->args.size() >= 10 && r->num(9) == 1); // engaged at the read
        auto lim = h.call("/pd/limiter", { "ceil2" }, "/pd/limiter/reply/ceil2");
        auto const json = lim ? parseJson(*lim) : juce::var();
        CHECK(lim.has_value() && static_cast<bool>(json["ceiling"]["breakerEngaged"]));
        CHECK(static_cast<juce::int64>(json["ceiling"]["breakerTrips"]) >= 1);
        CHECK(waitUntil([&] { return consoleCount(tripLine) == tripsSaid + 1; }, 2000));
        sleepMs(600);
        CHECK(consoleCount(tripLine) == tripsSaid + 1); // once, not once per poll

        volume->setUnscaledValueNotifyingHost(0.0f);
        float const calm = render(160, 1); // ~1.86 s
        CHECK(calm < 0.01f);
        auto const t2 = ceiling.read();
        CHECK(!t2.breakerEngaged);
        CHECK(t2.breakerReleases == t1.breakerReleases + 1);
        CHECK(waitUntil([&] { return consoleCount(releaseLine) == releasesSaid + 1; }, 2000));
        sleepMs(600);
        CHECK(consoleCount(releaseLine) == releasesSaid + 1);
        CHECK(consoleCount(tripLine) == tripsSaid + 1);
        volume->setUnscaledValueNotifyingHost(0.8f);
    }
    // Round 3 (MEDIUM): repeated prepares (`; pd oversample 1/0` -> prepareToPlay -> McpHost::prepare) every ~35, ~81
    // and ~232 ms (3, 7, 20 blocks of 512) during the full-scale chain neither stop the trip nor release the breaker.
    for (int every : { 3, 7, 20 }) {
        quiet();
        auto const t0 = ceiling.read();
        double const rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
        bool engaged = false, releasedWhileHot = false;
        for (int k = 0; k < 120; ++k) { // ~1.4 s
            if (k % every == 0) host->prepare(rate, 512);
            render(1, 1);
            bool const now = ceiling.read().breakerEngaged;
            releasedWhileHot = releasedWhileHot || (engaged && !now);
            engaged = engaged || now;
        }
        if (!engaged || releasedWhileHot) std::printf("  prepare every %d blocks: engaged %d, released while hot %d\n", every, engaged, releasedWhileHot);
        CHECK(engaged);
        CHECK(!releasedWhileHot);
        CHECK(ceiling.read().breakerTrips == t0.breakerTrips + 1);
    }
    // Master x2 with a 0.8 signal (1.6 after the gain): clamped.
    h.call("/pd/batch_atomic", { "main", "ceil3", 1, 0, 0, 0, 0, "ceilg", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ceil3");
    auto again = h.call("/pd/batch_atomic",
        { "main", "ceil4", 0, 0, 0, 1, 3,
            "ceilh", 600, 80, "obj", "*~", 1, "0.8",
            "ceilo", 0, "ceilh", 0,
            "ceilh", 0, "ceild", 0,
            "ceilh", 0, "ceild", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ceil4");
    CHECK(again.has_value() && again->num(0) == 1);
    sleepMs(100);
    volume->setUnscaledValueNotifyingHost(1.0f); // x2
    quiet();
    {
        auto const t0 = ceiling.read();
        float const peak = render(8, 4);
        CHECK(peak <= 1.0f);
        CHECK(peak > 0.9f);
        CHECK(ceiling.read().clampedSamples > t0.clampedSamples);
    }
    volume->setUnscaledValueNotifyingHost(volumeBefore);

    // /pd/limiter carries the ceiling.
    {
        auto lim = h.call("/pd/limiter", { "ceil5" }, "/pd/limiter/reply/ceil5");
        CHECK(lim.has_value());
        auto const json = lim ? parseJson(*lim) : juce::var();
        auto const c = json["ceiling"];
        CHECK(c.isObject());
        CHECK(static_cast<bool>(c["active"]));
        CHECK(c["setting"].toString() == "on");
        CHECK(!c.hasProperty("reason"));
        for (auto const* key : { "clampedSamples", "nonFiniteSamples", "clampedBlocks", "breakerTrips" })
            CHECK(c[key].isInt() || c[key].isInt64() || c[key].isDouble());
        CHECK(static_cast<juce::int64>(c["clampedSamples"]) > 0);
        CHECK(c["breakerReleases"].isInt() || c["breakerReleases"].isInt64() || c["breakerReleases"].isDouble());
        CHECK(static_cast<double>(c["startedAtMs"]) > 1.6e12); // wall-clock ms of this process's start
        CHECK(c["breakerEngaged"].isBool());
        CHECK(!c["lastClampMs"].isVoid() && static_cast<double>(c["lastClampMs"]) >= 0.0);
    }
    // The armed measure reports the window's pre-ceiling peak and clamp count.
    {
        volume->setUnscaledValueNotifyingHost(1.0f);
        quiet();
        render(4, 1); // the gain smoothing settles
        h.call("/meter/arm", { "ceil6" }, "/meter/arm/reply/ceil6");
        render(6, 1);
        auto r = h.call("/meter/read", { "ceil7" }, "/meter/read/reply/ceil7");
        CHECK(r.has_value() && r->args.size() >= 10);
        CHECK(r.has_value() && r->num(1) <= 1.0001);   // the peak after the ceiling
        CHECK(r.has_value() && r->num(5) > 1.2);       // before it: 0.8 x2
        CHECK(r.has_value() && r->num(6) > 0);         // samples clamped in the window
        volume->setUnscaledValueNotifyingHost(volumeBefore);
    }
    // A plugin: no ceiling, said.
    {
        ceiling.setStandalone(false);
        auto lim = h.call("/pd/limiter", { "ceil8" }, "/pd/limiter/reply/ceil8");
        auto const json = lim ? parseJson(*lim) : juce::var();
        CHECK(lim.has_value() && !static_cast<bool>(json["ceiling"]["active"]));
        CHECK(json["ceiling"]["reason"].toString() == "plugin");
        CHECK(json["ceiling"]["setting"].toString() == "on");
        ceiling.setStandalone(true);
    }

    // Nothing but the owner's GUI switches it off.
    {
        // Only a literal JSON `false` is off; anything malformed is on.
        CHECK(McpHost::ceilingSettingIsOn(juce::var(true)));
        CHECK(!McpHost::ceilingSettingIsOn(juce::var(false)));
        CHECK(McpHost::ceilingSettingIsOn(juce::var("off")));
        CHECK(McpHost::ceilingSettingIsOn(juce::var("false")));
        CHECK(McpHost::ceilingSettingIsOn(juce::var(0)));
        CHECK(McpHost::ceilingSettingIsOn(juce::var()));
        CHECK(McpHost::ceilingSettingIsOn(juce::JSON::parse("{\"a\":null}")["a"]));
        CHECK(!McpHost::ceilingSettingIsOn(juce::JSON::parse("{\"a\":false}")["a"]));

        // A non-GUI settings change to off: kept on, refused AND written back to true (file, panel, atomic agree).
        int const refusedSaid = consoleCount("Output ceiling stays on");
        host->onMessageThread([host] { host->settingsChanged("mcp_bridge_ai_ceiling", juce::var(false)); return true; }, 2000, "test");
        CHECK(ceiling.isEnabled());
        CHECK(waitUntil([&] { return consoleCount("Output ceiling stays on") == refusedSaid + 1; }, 2000));

        // The real reload path (review L4): the settings file edited to false while PlugData runs.
        auto const file = host->onMessageThread([] { return ProjectInfo::appDataDir.getChildFile("settings.json"); }, 2000, "test")
                              .value_or(juce::File());
        auto json = juce::JSON::parse(file.loadFileAsString());
        CHECK(json.isObject());
        if (auto* o = json.getDynamicObject()) {
            o->setProperty("mcp_bridge_ai_ceiling", false);
            CHECK(file.replaceWithText(juce::JSON::toString(json)));
            host->onMessageThread([] { SettingsFile::getInstance()->reloadSettings(); return true; }, 2000, "test");
            bool alwaysOn = true;
            bool const agreed = waitUntil([&] {
                alwaysOn = alwaysOn && ceiling.isEnabled();
                auto const onDisk = juce::JSON::parse(file.loadFileAsString());
                bool const fileOn = !onDisk.hasProperty("mcp_bridge_ai_ceiling") || onDisk["mcp_bridge_ai_ceiling"].isBool() && static_cast<bool>(onDisk["mcp_bridge_ai_ceiling"]);
                return storedSetting() == 1 && fileOn;
            }, 4000);
            CHECK(agreed);
            CHECK(alwaysOn);
            CHECK(ceiling.isEnabled());
        }

        // The owner's panel: off first in the host, then the setting: accepted, nothing written back.
        host->setOutputCeilingFromGui(false);
        host->onMessageThread([] { SettingsFile::getInstance()->setProperty("mcp_bridge_ai_ceiling", false); return true; }, 2000, "test");
        sleepMs(400);
        CHECK(!ceiling.isEnabled());
        CHECK(storedSetting() == 0);
        // Startup with it off: off, and a loud console line.
        int const loudSaid = consoleCount("OUTPUT CEILING IS OFF");
        host->onMessageThread([host] { host->applyOutputCeilingSettingAtStartup(); return true; }, 2000, "test");
        CHECK(!ceiling.isEnabled());
        CHECK(waitUntil([&] { return consoleCount("OUTPUT CEILING IS OFF") == loudSaid + 1; }, 2000)); // the console is async
        // Startup with a malformed value ("off"): on, and the setting normalised to true.
        host->onMessageThread([] { SettingsFile::getInstance()->setProperty("mcp_bridge_ai_ceiling", juce::var("off")); return true; }, 2000, "test");
        host->onMessageThread([host] { host->applyOutputCeilingSettingAtStartup(); return true; }, 2000, "test");
        CHECK(ceiling.isEnabled());
        CHECK(waitUntil([&] { return storedSetting() == 1; }, 2000));
        sleepMs(300);
        CHECK(consoleCount("OUTPUT CEILING IS OFF") == loudSaid + 1); // on: no second loud line
        host->setOutputCeilingFromGui(false);
        host->onMessageThread([host] { host->settingsChanged("mcp_bridge_ai_ceiling", juce::var(true)); return true; }, 2000, "test");
        CHECK(ceiling.isEnabled());

        // A patch: `; pd mcp_bridge_ai_ceiling 0` and `[s mcp_host]` (each route proven by a `limit 1` behind it).
        for (char const* receiver : { "pd", "mcp_host" }) {
            processor.setEnableLimiter(false);
            host->pdWait([receiver] {
                t_symbol* s = gensym(receiver);
                t_atom a;
                SETFLOAT(&a, 0.0f);
                if (s->s_thing) pd_typedmess(s->s_thing, gensym("mcp_bridge_ai_ceiling"), 1, &a);
                SETFLOAT(&a, 1.0f);
                if (s->s_thing) pd_typedmess(s->s_thing, gensym("limit"), 1, &a);
            }, 2000);
            bool delivered = false;
            for (int t = 0; t < 3000 && !delivered; t += 20) {
                delivered = processor.getEnableLimiter();
                sleepMs(20);
            }
            CHECK(delivered);
            CHECK(ceiling.isEnabled());
        }
        // The AI: no verb switches it (an unknown verb), and the limiter read takes no argument that would.
        auto unknown = h.call("/pd/output_ceiling", { "ceil9", 0 }, "/bridge/unknown_verb");
        CHECK(unknown.has_value());
        h.call("/pd/limiter", { "ceil10", 0 }, "/pd/limiter/reply/ceil10");
        CHECK(ceiling.isEnabled());
    }

    // MCPBridge::playVoiceTake's own device callback: the same clamp (while the ceiling is active), none when off.
    {
        struct HotSource final : juce::AudioSource {
            void prepareToPlay(int, double) override { }
            void releaseResources() override { }
            void getNextAudioBlock(juce::AudioSourceChannelInfo const& info) override
            {
                for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                    for (int i = 0; i < info.numSamples; ++i)
                        info.buffer->setSample(ch, info.startSample + i, i == 1 ? std::numeric_limits<float>::quiet_NaN() : 4.0f);
            }
        };
        HotSource source;
        mcp::CeilingSourcePlayer player(ceiling);
        player.prepareToPlay(44100.0, 256);
        player.setSource(&source);
        juce::AudioBuffer<float> out(2, 256);
        juce::AudioIODeviceCallbackContext const context {};
        player.audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, 256, context);
        CHECK(bufferPeak(out) <= 1.0f && bufferPeak(out) > 0.99f);
        CHECK(bufferFinite(out));
        host->setOutputCeilingFromGui(false);
        player.audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, 256, context);
        CHECK(bufferPeak(out) > 3.0f);
        host->setOutputCeilingFromGui(true);
        player.setSource(nullptr);
    }

    h.fire("/pd/dsp", { 0 });
    h.call("/pd/batch_atomic", { "main", "ceilx", 3, 0, 0, 0, 0, "ceilo", "ceilh", "ceild", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ceilx");
    processor.setEnableLimiter(limiterBefore);
    setThreshold(thresholdBefore);
    volume->setUnscaledValueNotifyingHost(volumeBefore);
    ceiling.requestBreakerReset();
    audio.paused = false;
}

// MIDI device awareness: /pd/midi_devices <corrId> answers {"inputs":[{name,identifier,enabled}],"outputs":[...]}:
// every device JUCE lists (as PlugData's MidiDeviceManager holds them) and whether PlugData's MIDI settings have it on
// (enabled = it sits in a numbered port; null in the plugin build, where the host routes MIDI). Read-only: the port
// map is the same after the read. Headless may list zero devices, so the shape and the agreement with the manager
// are pinned, not a count.
void runMidiDevicesCases(Harness& h, PluginProcessor& processor)
{
    std::printf("midi_devices read\n");
    auto* host = processor.getMcp();
    struct Dev {
        std::string name, id;
        int port;
    };
    auto snapshot = [&] {
        return host->onMessageThread([&processor] {
            auto& mdm = processor.getMidiDeviceManager();
            std::vector<Dev> in, out;
            for (auto info : mdm.getInputDevices())
                in.push_back({ info.name.toStdString(), info.identifier.toStdString(), mdm.getMidiDevicePort(true, info) });
            for (auto info : mdm.getOutputDevices())
                out.push_back({ info.name.toStdString(), info.identifier.toStdString(), mdm.getMidiDevicePort(false, info) });
            return std::make_pair(in, out);
        }, 2000, "test");
    };
    auto const before = snapshot();
    CHECK(before.has_value());

    auto r = h.call("/pd/midi_devices", { "md1" }, "/pd/midi_devices/reply/md1", 3000);
    CHECK(r.has_value());
    if (!r || !before)
        return;
    auto const json = parseJson(*r);
    CHECK(json.isObject() && json["inputs"].isArray() && json["outputs"].isArray());
    auto agrees = [&](juce::var const& list, std::vector<Dev> const& want) {
        auto const* arr = list.getArray();
        if (!arr || arr->size() != static_cast<int>(want.size()))
            return false;
        for (int i = 0; i < arr->size(); ++i) {
            auto const& d = (*arr)[i];
            auto const& w = want[static_cast<size_t>(i)];
            if (!d.isObject() || !d["name"].isString() || !d["identifier"].isString())
                return false;
            if (d["name"].toString().toStdString() != w.name || d["identifier"].toString().toStdString() != w.id)
                return false;
            auto const enabled = d["enabled"];
            if (ProjectInfo::isStandalone) {
                if (!enabled.isBool() || static_cast<bool>(enabled) != (w.port >= 0))
                    return false;
            } else if (!enabled.isVoid()) {
                return false; // the plugin build cannot say: null
            }
        }
        return true;
    };
    CHECK(agrees(json["inputs"], before->first));
    CHECK(agrees(json["outputs"], before->second));
    std::printf("  %d inputs, %d outputs listed\n", static_cast<int>(before->first.size()),
        static_cast<int>(before->second.size()));

    auto const after = snapshot(); // the read changed no port
    bool same = after.has_value() && after->first.size() == before->first.size()
        && after->second.size() == before->second.size();
    for (size_t i = 0; same && i < before->first.size(); ++i)
        same = after->first[i].port == before->first[i].port;
    for (size_t i = 0; same && i < before->second.size(); ++i)
        same = after->second[i].port == before->second[i].port;
    CHECK(same);

    auto caps = h.call("/bridge/capabilities", { "md2" }, "/bridge/capabilities/reply");
    bool cap = false;
    if (caps)
        for (auto const& a : caps->args)
            cap = cap || a.text == "midi_devices";
    CHECK(caps.has_value() && cap);
}

// P5.1 sink floor: a patch may switch the limiter ON, never OFF. `limit 0` through the sys path (directly, and as a
// real Pd message to `pd` and to `mcp_host`) leaves it on; `limit 1` from a patch still switches it on.
void runLimiterPatchCases(PluginProcessor& processor)
{
    std::printf("limit from a patch: on yes, off no\n");
    auto* host = processor.getMcp();
    bool const before = processor.getEnableLimiter();
    auto sys = [&](float v) {
        host->onMessageThread([&processor, v] {
            SmallArray<pd::Atom> list;
            list.add(pd::Atom(v));
            processor.receiveSysMessage(SmallString("limit"), list);
            return true;
        }, 2000, "test");
    };
    // A real Pd message to `receiver`; it reaches the processor later, on the message thread (handleAsyncUpdate).
    auto viaPd = [&](char const* receiver, float v) {
        host->pdWait([receiver, v] {
            t_symbol* s = gensym(receiver);
            t_atom a;
            SETFLOAT(&a, v);
            if (s->s_thing) pd_typedmess(s->s_thing, gensym("limit"), 1, &a);
        }, 2000);
    };
    // Bounded polls: the limiter state, and the console lines that say a `limit 0` was refused.
    auto waitLimiter = [&](bool want, int timeoutMs) {
        for (int t = 0; t < timeoutMs; t += 20) {
            if (processor.getEnableLimiter() == want) return true;
            sleepMs(20);
        }
        return processor.getEnableLimiter() == want;
    };
    auto refusals = [&] {
        return host->onMessageThread([&processor] {
            int n = 0;
            // The console folds identical consecutive lines into one entry and counts them in the 5th field.
            for (auto const& m : processor.getConsoleMessages())
                if (std::get<1>(m).contains("limit 0 from a patch ignored")) n += std::get<4>(m);
            return n;
        }, 2000, "test").value_or(-1);
    };
    auto waitRefusal = [&](int beforeCount, int timeoutMs) {
        for (int t = 0; t < timeoutMs; t += 20) {
            if (refusals() > beforeCount) return true;
            sleepMs(20);
        }
        return refusals() > beforeCount;
    };

    // Each Pd route: first a positive control (the route delivers: off -> `limit 1` -> on), then `limit 0` is
    // seen and refused (a new console line; the warning is at most one a second, so wait that out) and the limiter
    // stays on.
    for (char const* receiver : { "pd", "mcp_host" }) {
        std::printf("  route %s\n", receiver);
        processor.setEnableLimiter(false);
        viaPd(receiver, 1.0f);
        CHECK(waitLimiter(true, 3000)); // the route reaches receiveSysMessage
        sleepMs(1100);
        int const seen = refusals();
        viaPd(receiver, 0.0f);
        CHECK(waitRefusal(seen, 3000)); // the off message arrived and was refused
        CHECK(processor.getEnableLimiter());
    }

    processor.setEnableLimiter(true);
    sys(0.0f);
    CHECK(processor.getEnableLimiter()); // the direct sys path: ignored
    host->onMessageThread([&processor] {
        SmallArray<pd::Atom> list;
        list.add(pd::Atom(gensym("off")));
        processor.receiveSysMessage(SmallString("limit"), list);
        return true;
    }, 2000, "test");
    CHECK(processor.getEnableLimiter()); // a symbol is not "on"

    processor.setEnableLimiter(false); // the toolbar's way still switches it off
    CHECK(!processor.getEnableLimiter());
    sys(1.0f);
    CHECK(processor.getEnableLimiter()); // a patch may switch it on
    processor.setEnableLimiter(before);
}

// P5.1 sink floor: the master volume from a patch (`__param list volume ...`). Before the guard,
// `range 0 1000` then `float 1000` set the master gain ~5000x after Pd and before the limiter. Now a widened range is
// refused and a value above 1 is clamped to 1, both through handleParameterMessage directly and as a real Pd list to
// the `__param` receiver; a value inside 0..1 and a narrowed range still work (positive controls).
void runVolumeGuardCases(PluginProcessor& processor)
{
    std::printf("master volume from a patch: within 0..1 only\n");
    auto* host = processor.getMcp();
    PlugDataParameter* volume = nullptr;
    for (auto* p : processor.getParameters())
        if (auto* param = dynamic_cast<PlugDataParameter*>(p); param && param->getTitle().toString() == "volume")
            volume = param;
    CHECK(volume != nullptr);
    if (!volume) return;
    float const valueBefore = volume->getUnscaledValue();
    auto rangeEnd = [&] { return volume->getNormalisableRange().end; };
    auto atomsOf = [](char const* selector, std::initializer_list<float> values) {
        SmallArray<pd::Atom> list;
        list.add(pd::Atom(gensym("volume")));
        list.add(pd::Atom(gensym(selector)));
        for (float v : values) list.add(pd::Atom(v));
        return list;
    };
    auto direct = [&](char const* selector, std::initializer_list<float> values) {
        auto const list = atomsOf(selector, values);
        host->onMessageThread([&processor, &list] { processor.handleParameterMessage(list); return true; }, 2000, "test");
    };
    auto viaPd = [&](char const* selector, std::initializer_list<float> values) {
        std::vector<float> const vals(values);
        host->pdWait([selector, vals] {
            t_symbol* s = gensym("__param");
            std::vector<t_atom> argv(2 + vals.size());
            SETSYMBOL(&argv[0], gensym("volume"));
            SETSYMBOL(&argv[1], gensym(selector));
            for (size_t i = 0; i < vals.size(); ++i) SETFLOAT(&argv[2 + i], vals[i]);
            if (s->s_thing) pd_list(s->s_thing, &s_list, static_cast<int>(argv.size()), argv.data());
        }, 2000);
    };
    auto waitValue = [&](float want, int timeoutMs) {
        for (int t = 0; t < timeoutMs; t += 20) {
            if (std::abs(volume->getUnscaledValue() - want) < 1e-4f) return true;
            sleepMs(20);
        }
        return std::abs(volume->getUnscaledValue() - want) < 1e-4f;
    };

    // Direct (deterministic: on the message thread, as Instance::handleAsyncUpdate calls it).
    direct("float", { 0.5f });
    CHECK(waitValue(0.5f, 100)); // positive control: inside the range
    direct("range", { 0.0f, 1000.0f });
    CHECK(rangeEnd() <= 1.0f); // refused
    direct("create", { 0.8f, 0.0f, 1000.0f });
    CHECK(rangeEnd() <= 1.0f); // refused
    direct("float", { 1000.0f });
    CHECK(waitValue(1.0f, 100)); // clamped to the default maximum
    // The float guard on its own: even with a range widened some other way (saved state, a DAW), a patch's value
    // above 1 is clamped to 1 (without the guard it would set 1000 here).
    volume->setRange(0.0f, 1000.0f);
    direct("float", { 1000.0f });
    CHECK(waitValue(1.0f, 100));
    volume->setRange(0.0f, 1.0f);
    direct("range", { 0.0f, 0.5f });
    CHECK(std::abs(rangeEnd() - 0.5f) < 1e-6f); // narrowing is allowed
    direct("range", { 0.0f, 1.0f });
    CHECK(std::abs(rangeEnd() - 1.0f) < 1e-6f);

    // The Pd route ([; __param list volume ...( in a patch): positive control first, then the attack.
    direct("float", { 0.3f });
    viaPd("float", { 0.6f });
    CHECK(waitValue(0.6f, 3000)); // the route delivers
    viaPd("range", { 0.0f, 1000.0f });
    viaPd("float", { 1000.0f });
    CHECK(waitValue(1.0f, 3000)); // the float arrived (after the range: FIFO) and was clamped
    CHECK(rangeEnd() <= 1.0f);    // the range was refused
    CHECK(volume->getUnscaledValue() <= 1.0f);

    volume->setRange(0.0f, 1.0f);
    volume->setUnscaledValueNotifyingHost(valueBefore);
}

// P5.1 sink floor: an MCP ref is a tempId or nothing. A bare index ("12", "obj_12") or an empty ref no longer
// resolves to the gl_list object at that index (the AI could address a guard it was never named).
void runIndexRefCases(Harness& h, PluginProcessor& processor)
{
    std::printf("no index fallback for MCP refs\n");
    auto made = h.call("/pd/batch_atomic",
        { "main", "ix1", 0, 0, 0, 1, 0, "ix_osc", 900, 140, "obj", "osc~", 1, "220", 0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ix1");
    CHECK(made.has_value() && made->num(0) == 1);
    int index = -1;
    if (made)
        for (size_t i = 7; i + 1 < made->args.size() && made->str(i).rfind("[", 0) != 0; i += 2)
            if (made->str(i) == "ix_osc") index = static_cast<int>(made->num(i + 1));
    CHECK(index >= 0);

    auto* host = processor.getMcp();
    auto const idx = juce::String(index);
    bool resolvedByIndex = true;
    host->pdWait([&] {
        resolvedByIndex = host->resolveStableId("main", idx) != nullptr
            || host->resolveStableId("main", "obj_" + idx) != nullptr
            || host->resolveStableId("main", "") != nullptr;
    }, 2000);
    CHECK(!resolvedByIndex);

    // The reads too: /pd/typeof and /pd/ports by index answer their error, by tempId their reply.
    for (char const* verb : { "typeof", "ports" }) {
        auto const base = juce::String("/pd/") + verb;
        auto const cIdx = juce::String("ixr_") + verb;
        auto byIdx = h.call(base.toRawUTF8(), { "main", idx.toRawUTF8(), cIdx.toRawUTF8() }, (base + "/error/" + cIdx).toStdString(), 2000);
        CHECK(byIdx.has_value());
        auto const cId = juce::String("ixq_") + verb;
        auto byId = h.call(base.toRawUTF8(), { "main", "ix_osc", cId.toRawUTF8() }, (base + "/reply/" + cId).toStdString(), 2000);
        CHECK(byId.has_value());
    }

    int n = 0;
    for (auto const& ref : { idx, "obj_" + idx }) {
        auto const c = "ixs" + juce::String(++n);
        auto r = h.call("/pd/obj_set", { "main", c.toRawUTF8(), ref.toRawUTF8(), 1, "float", 1, 0 }, ("/pd/obj_set/reply/" + c).toStdString());
        CHECK(r.has_value() && !r->str(0).empty()); // an error text (success is the float 1): nothing set
    }
    auto byName = h.call("/pd/obj_set", { "main", "ixs9", "ix_osc", 1, "float", 1, 0 }, "/pd/obj_set/reply/ixs9");
    CHECK(byName.has_value() && byName->num(0) == 1); // the tempId still works

    h.call("/pd/batch_atomic", { "main", "ixx", 1, 0, 0, 0, 0, "ix_osc", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ixx");
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
    if (h.v2) {
        // v2 answers an unknown verb by id (rejected{unknown_verb}); /bridge/unknown_verb is v1's report
        std::printf("unknown verb (v2)\n");
        for (char const* address : { "/pd/no_such_verb", "/nodomain/anything", "/pd/create_id" }) {
            auto const id = "U" + std::to_string(h.v2->next++);
            h.v2->sendReq(id, address, R"([{"t":"s","v":"main"}])");
            auto const f = h.v2->take([&](auto const& v) { return field(v, "id") == id; }, 3000);
            auto const* e = f ? f->find("error") : nullptr;
            CHECK(f && field(*f, "outcome") == "rejected" && e && field(*e, "code") == "unknown_verb");
        }
        CHECK(!h.replies.waitFor("/bridge/unknown_verb", 200).has_value());
    } else {
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

    // ── move, undo, redo (one step of the canvas's Pd undo queue, P4.4) ───────────────────────────
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

// P4.1 S3: port and token from the bridge's discovery file (under the scratch HOME).
std::pair<int, std::string> v2Discovery()
{
    std::ifstream in(mcp::v2::defaultDiscoveryPath());
    std::stringstream text;
    text << in.rdbuf();
    std::string err;
    auto const file = mcp::v2::json::parse(text.str(), err);
    if (!file || !file->find("port") || !file->find("token"))
        return { 0, {} };
    return { static_cast<int>(file->find("port")->number), file->find("token")->string };
}

void clearMain(Harness& v1, char const* corr)
{
    auto const r = v1.call("/pd/clear", { "main", corr }, std::string("/pd/clear/reply/") + corr, 5000);
    CHECK(r.has_value());
    sleepMs(200);
}

// The args of a batch_atomic of `count` plain [f] boxes named n<first>... (corr id `id`).
std::string boxArgs(std::string const& id, int count, int first, std::string const& canvas = "main")
{
    std::vector<mcp::v2::Arg> args;
    auto const s = [&](std::string v) { mcp::v2::Arg a; a.s = std::move(v); args.push_back(std::move(a)); };
    auto const i = [&](int v) { mcp::v2::Arg a; a.type = 'i'; a.i = v; args.push_back(a); };
    s(canvas); s(id); i(0); i(0); i(0); i(count); i(0);
    for (int k = first; k < first + count; ++k) {
        s("n" + std::to_string(k)); i((k % 100) * 70); i((k / 100) * 30); s("obj"); s("f"); i(0);
    }
    i(0); i(5); i(10); i(0);
    return mcp::v2::encodeArgs(args);
}

// A batch_atomic of `count` plain [f] boxes, over v2; true when it created them all.
bool v2Boxes(V2Link& link, std::string const& id, int count, int first)
{
    link.sendReq(id, "/pd/batch_atomic", boxArgs(id, count, first));
    auto const f = link.take([&](auto const& v) { auto const* fin = v.find("final"); return field(v, "id") == id && fin && fin->boolean; }, 60000);
    if (!f || field(*f, "outcome") != "applied")
        return false;
    auto const r = replyOf(*f);
    return r.num(0) == count;
}

void runV2DispatchCases(Harness& h, MCPBridge& bridge)
{
    auto& link = *h.v2;
    {
        std::printf("v2: a fire-and-forget verb ends unknown{no_reply_verb}; the noReply lane gets nothing\n");
        link.sendReq("F1", "/pd/clear_selection", R"([{"t":"s","v":"main"}])");
        auto const f = link.take([](auto const& v) { return field(v, "id") == "F1"; }, 3000);
        auto const* e = f ? f->find("error") : nullptr;
        CHECK(f && field(*f, "outcome") == "unknown" && e && field(*e, "code") == "no_reply_verb");
        link.sendReq("R1", "/param/s3_nothing", R"([{"t":"f","v":0.5}])", true);
        CHECK(!link.take([](auto const& v) { return field(v, "id") == "R1"; }, 500).has_value());
        link.sentIds.pop_back(); // nothing is owed for a noReply request
    }
    {
        std::printf("v2: events (artist to the newest session only, telemetry to subscribers and UDP)\n");
        h.replies.clear();
        bridge.sendArtistNote("x1", "warmer");
        auto const a = link.take([](auto const& v) { return field(v, "t") == "evt" && field(v, "address") == "/pd/ui/note"; }, 2000);
        CHECK(a && field(*a, "class") == "artist");
        CHECK(!h.replies.waitFor("/pd/ui/note", 200).has_value());
        bridge.sendConsoleLog("v2 telemetry check", false);
        auto const t = link.take([](auto const& v) { return field(v, "t") == "evt" && field(v, "address") == "/pd/log"; }, 2000);
        CHECK(t && field(*t, "class") == "telemetry");
        CHECK(h.replies.waitFor("/pd/log", 1000).has_value());
    }
    {
        std::printf("v2: a census over 1 MB arrives whole\n");
        {
            Harness v1 { h.bridge, h.replies, nullptr };
            clearMain(v1, "c4"); // the case list leaves carrier, mul1 and an array behind
        }
        // [f] boxes take ~150 bytes each in the census: 7,200 of them pass 1 MB
        bool built = v2Boxes(link, "big1", 2000, 0) && v2Boxes(link, "big2", 2000, 2000) && v2Boxes(link, "big3", 2000, 4000)
            && v2Boxes(link, "big4", 1200, 6000);
        CHECK(built);
        link.sendReq("bigc", "/pd/census", R"([{"t":"s","v":"main"},{"t":"s","v":"bigc"}])");
        auto const f = link.take([](auto const& v) { return field(v, "id") == "bigc"; }, 30000);
        CHECK(f && field(*f, "address") == "/pd/census/reply/bigc");
        if (f) {
            auto const r = replyOf(*f);
            std::printf("  census reply: %zu bytes\n", r.str(0).size());
            CHECK(r.str(0).size() > 1000000);
            auto const json = juce::JSON::parse(juce::String::fromUTF8(r.str(0).c_str()));
            CHECK(static_cast<int>(json["objectCount"]) == 7200);
        }
        Harness v1 { h.bridge, h.replies, nullptr };
        clearMain(v1, "c5");
    }
    {
        // S3 review HIGH 2: list_tabs replies from a posted hop after its handler returned; its work token keeps the
        // request open until that hop ran, so the real reply is the final (not unknown{no_reply_verb}, not UDP)
        std::printf("v2: a reply from a posted hop is the final (list_tabs)\n");
        auto const r = h.call("/pd/list_tabs", { "lt1" }, "/pd/list_tabs/reply/lt1", 5000);
        CHECK(r.has_value());
    }
    {
        std::printf("v2: one worker for both transports (v1 and v2 see each other's changes)\n");
        CHECK(v2Boxes(link, "iv", 1, 0));
        Harness v1 { h.bridge, h.replies, nullptr };
        auto const census = v1.call("/pd/census", { "main", "ic1" }, "/pd/census/reply/ic1");
        CHECK(census && static_cast<int>(parseJson(*census)["objectCount"]) == 1);
        auto const rn = v1.call("/pd/rename_id", { "main", "ir1", "n0", "renamed_by_v1" }, "/pd/rename_id/reply/ir1");
        CHECK(rn && rn->num(0) == 1);
        auto const seen = h.call("/pd/census", { "main", "ic2" }, "/pd/census/reply/ic2");
        CHECK(seen && parseJson(*seen)["objects"][0]["id"].toString() == "renamed_by_v1");
        clearMain(v1, "c6");
    }
}

// P4.1 S4: a welcomed v2 session of the bridge's own listener.
std::unique_ptr<V2Link> v2Connect()
{
    auto const [port, token] = v2Discovery();
    auto link = std::make_unique<V2Link>(port);
    link->client.sendFrame(mcp::v2::test::helloJson(token));
    CHECK(link->take([](auto const& v) { return field(v, "t") == "welcome"; }, 3000).has_value());
    return link;
}

std::optional<mcp::v2::json::Value> finalFor(V2Link& link, std::string const& id, int ms = 5000)
{
    return link.take([&](auto const& v) { auto const* f = v.find("final"); return field(v, "id") == id && f && f->boolean; }, ms);
}

bool replayed(std::optional<mcp::v2::json::Value> const& f)
{
    auto const* r = f ? f->find("replayed") : nullptr;
    return r && r->boolean;
}

int objectCount(Harness& v1, char const* corr)
{
    auto const c = v1.call("/pd/census", { "main", corr }, std::string("/pd/census/reply/") + corr);
    return c ? static_cast<int>(parseJson(*c)["objectCount"]) : -1;
}

std::pair<int, int> positionOf(Harness& v1, char const* id, char const* corr)
{
    auto const r = v1.call("/pd/typeof", { "main", id, corr }, std::string("/pd/typeof/reply/") + corr);
    if (!r || r->args.size() < 4)
        return { -1, -1 };
    return { static_cast<int>(r->num(2)), static_cast<int>(r->num(3)) };
}

// `status` from a new session until the stored final is there (the first request may still be running).
std::optional<mcp::v2::json::Value> statusUntilFinal(V2Link& link, std::string const& id, std::string const& key)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        link.client.sendFrame(R"({"t":"status","id":)" + mcp::v2::json::quote(id) + ",\"key\":" + mcp::v2::json::quote(key) + "}");
        auto const f = link.take([&](auto const& v) { return field(v, "id") == id; }, 2000);
        if (f && field(*f, "t") == "rep")
            return f;
        if (f && field(*f, "state") == "in_flight")
            return finalFor(link, id, 5000); // joined: the final comes to this id
        sleepMs(100);
    }
    return std::nullopt;
}

// S6: two v2 sessions. The one that changed the patch last is the editor: the other is told once (owner_changed),
// a read changes nothing, and a second change by the editor tells nobody.
void runV2OwnershipCases(Harness& v1)
{
    std::printf("v2 session ownership\n");
    clearMain(v1, "o0");
    auto a = v2Connect();
    auto b = v2Connect();
    auto isOwnerChanged = [](auto const& v) { return field(v, "t") == "evt" && field(v, "address") == "owner_changed"; };

    a->sendReq("oa1", "/pd/batch_atomic", boxArgs("oa1", 1, 100), false, "own:1");
    auto const firstFinal = finalFor(*a, "oa1");
    CHECK(firstFinal && field(*firstFinal, "outcome") == "applied");
    CHECK(!a->take(isOwnerChanged, 200)); // the first editor displaced nobody

    b->sendReq("ob1", "/pd/batch_atomic", boxArgs("ob1", 1, 200), false, "own:2");
    auto const told = a->take(isOwnerChanged, 3000);
    CHECK(told && field(*told, "class") == "session");
    CHECK(!b->take(isOwnerChanged, 200)); // the new editor is not told

    b->sendReq("ob2", "/pd/batch_atomic", boxArgs("ob2", 1, 300), false, "own:3");
    auto const secondFinal = finalFor(*b, "ob2");
    CHECK(secondFinal && field(*secondFinal, "outcome") == "applied");
    CHECK(!a->take(isOwnerChanged, 200)); // already the editor: nobody is told again
}

void runV2IdempotencyCases(Harness& v1)
{
    std::printf("v2 idempotency\n");
    clearMain(v1, "i0");
    auto a = v2Connect();

    // 1. A batch sent twice with one key builds once; the second final is the first, replayed.
    a->sendReq("ib1", "/pd/batch_atomic", boxArgs("ib1", 1, 100), false, "idem:1");
    auto const first = finalFor(*a, "ib1");
    CHECK(first && field(*first, "outcome") == "applied" && !replayed(first));
    a->sendReq("ib1-r", "/pd/batch_atomic", boxArgs("ib1", 1, 100), false, "idem:1");
    auto const second = finalFor(*a, "ib1-r");
    CHECK(second && replayed(second) && field(*second, "address") == "/pd/batch_atomic/reply/ib1");
    CHECK(objectCount(v1, "ic1") == 1);
    // the same key for other content is refused, nothing applied
    a->sendReq("ib1-x", "/pd/batch_atomic", boxArgs("ib1", 1, 101), false, "idem:1");
    auto const conflict = finalFor(*a, "ib1-x");
    auto const* ce = conflict ? conflict->find("error") : nullptr;
    CHECK(conflict && ce && field(*ce, "code") == "key_conflict");
    CHECK(objectCount(v1, "ic2") == 1);

    // 2. rename_id twice with one key: renamed once, the second answer replayed (not "no such id").
    a->sendReq("ir1", "/pd/rename_id", R"([{"t":"s","v":"main"},{"t":"s","v":"ir1"},{"t":"s","v":"n100"},{"t":"s","v":"idem_box"}])", false, "idem:2");
    auto const rn = finalFor(*a, "ir1");
    a->sendReq("ir1-r", "/pd/rename_id", R"([{"t":"s","v":"main"},{"t":"s","v":"ir1"},{"t":"s","v":"n100"},{"t":"s","v":"idem_box"}])", false, "idem:2");
    auto const rn2 = finalFor(*a, "ir1-r");
    CHECK(rn && rn2 && replayed(rn2) && field(*rn2, "address") == field(*rn, "address"));
    CHECK(rn2 && rn2->find("args") && rn2->find("args")->items.size() > 0 && rn2->find("args")->items[0].find("v")->number == 1);

    // 3. An undo sent twice with one key undoes one step.
    auto const move = [&](char const* id, int x, int y) {
        a->sendReq(id, "/pd/move_batch_id", std::string(R"([{"t":"s","v":"main"},{"t":"s","v":")") + id
            + R"("},{"t":"s","v":"idem_box"},{"t":"i","v":)" + std::to_string(x) + R"(},{"t":"i","v":)" + std::to_string(y) + "}]");
        auto const f = finalFor(*a, id);
        CHECK(f && field(*f, "outcome") == "applied");
        sleepMs(300);
    };
    move("im1", 300, 200);
    move("im2", 400, 250);
    CHECK(positionOf(v1, "idem_box", "ip1") == std::make_pair(400, 250));
    a->sendReq("iu1", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"iu1"}])", false, "idem:3");
    CHECK(finalFor(*a, "iu1").has_value());
    sleepMs(800);
    CHECK(positionOf(v1, "idem_box", "ip2") == std::make_pair(300, 200));
    a->sendReq("iu1-r", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"iu1"}])", false, "idem:3");
    CHECK(replayed(finalFor(*a, "iu1-r")));
    sleepMs(800);
    CHECK(positionOf(v1, "idem_box", "ip3") == std::make_pair(300, 200)); // not a second undo

    // 3b. S4 review CRITICAL: a resend that arrives while the first is still QUEUED joins it and never runs. A slow
    // read (a census of 2,000 boxes in a subpatch) keeps the worker busy; an undo and its resend (same key) wait
    // behind it. One undo takes back the last move; a second run would take back the move before it too. (The MCP
    // undo stack is per root patch: the blocker must not be an undo step, and the boxes are built first.)
    {
        a->sendReq("sp1", "/pd/batch_atomic",
            R"([{"t":"s","v":"main"},{"t":"s","v":"sp1"},{"t":"i","v":0},{"t":"i","v":0},{"t":"i","v":0},{"t":"i","v":1},{"t":"i","v":0},)"
            R"({"t":"s","v":"idemsub"},{"t":"i","v":600},{"t":"i","v":400},{"t":"s","v":"obj"},{"t":"s","v":"pd"},{"t":"i","v":1},{"t":"s","v":"idemsub"},)"
            R"({"t":"i","v":0},{"t":"i","v":5},{"t":"i","v":10},{"t":"i","v":0}])");
        auto const sub = finalFor(*a, "sp1");
        CHECK(sub && field(*sub, "outcome") == "applied");
        a->sendReq("sb1", "/pd/batch_atomic", boxArgs("sb1", 2000, 1000, "idemsub"));
        auto const fb = finalFor(*a, "sb1", 60000);
        CHECK(fb && field(*fb, "outcome") == "applied");
        move("im4", 350, 220);
        move("im5", 380, 240);
        a->sendReq("sc1", "/pd/census", R"([{"t":"s","v":"idemsub"},{"t":"s","v":"sc1"}])"); // the blocker
        a->sendReq("su1", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"su1"}])", false, "idem:queued-undo");
        a->sendReq("su1-r", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"su1"}])", false, "idem:queued-undo");
        auto const fc = finalFor(*a, "sc1", 20000);
        auto const fu = finalFor(*a, "su1", 20000);
        auto const fr = finalFor(*a, "su1-r", 20000);
        CHECK(fc && field(*fc, "outcome") == "applied");
        CHECK(fu && !replayed(fu) && fr && replayed(fr));
        sleepMs(1500);
        auto const at = positionOf(v1, "idem_box", "ip3b");
        if (at != std::make_pair(350, 220))
            std::printf("  idem_box at %d,%d after the queued undo\n", at.first, at.second);
        CHECK(at == std::make_pair(350, 220)); // one step back, not two
    }

    // 4. Lost reply (batch): the session closes right after sending; a new session asks status, then resends:
    // the stored final, replayed, and the box exists once.
    int const before = objectCount(v1, "ic3");
    {
        auto lost = v2Connect();
        lost->sendReq("lb1", "/pd/batch_atomic", boxArgs("lb1", 1, 200), false, "idem:lost-batch");
    } // closed before reading anything
    auto b = v2Connect();
    auto const st = statusUntilFinal(*b, "lb1-s", "idem:lost-batch");
    CHECK(st && field(*st, "outcome") == "applied" && replayed(st));
    b->sendReq("lb1-r", "/pd/batch_atomic", boxArgs("lb1", 1, 200), false, "idem:lost-batch");
    CHECK(replayed(finalFor(*b, "lb1-r")));
    CHECK(objectCount(v1, "ic4") == before + 1);

    // 5. Lost reply (undo): one step undone, however often the client asks or resends.
    move("im3", 500, 300);
    CHECK(positionOf(v1, "idem_box", "ip4") == std::make_pair(500, 300));
    {
        auto lost = v2Connect();
        lost->sendReq("lu1", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"lu1"}])", false, "idem:lost-undo");
    }
    auto const ust = statusUntilFinal(*b, "lu1-s", "idem:lost-undo");
    CHECK(ust && replayed(ust));
    b->sendReq("lu1-r", "/pd/undo", R"([{"t":"s","v":"main"},{"t":"s","v":"lu1"}])", false, "idem:lost-undo");
    CHECK(replayed(finalFor(*b, "lu1-r")));
    sleepMs(800);
    CHECK(positionOf(v1, "idem_box", "ip5") == std::make_pair(350, 220)); // exactly one undo

    // 6. status of a key never seen: unknown_key.
    b->client.sendFrame(R"({"t":"status","id":"nk1","key":"idem:never"})");
    auto const nk = b->take([](auto const& v) { return field(v, "id") == "nk1"; }, 2000);
    CHECK(nk && field(*nk, "t") == "status" && field(*nk, "state") == "unknown_key");
    clearMain(v1, "i9");
}

// L31 (P4.1, before S5): 16 handlers read msg[0] unchecked; the schema's minArgs makes the dispatcher refuse a
// shorter request on both lines. Every verb row gets a message with NO arguments over v1 (a handler that still
// indexed past the end would crash this process), then the bridge must still answer; over v2 the guarded verbs are
// refused by id. Skipped: verbs whose zero-argument defaults start jobs or write files outside the test's concern.
void runZeroArgCases(Harness& h)
{
    std::printf("every verb with no arguments (L31)\n");
    static std::string const skip[] = { "pd/render", "pd/record_start", "pd/mcp_reload_lua", "pd/save_patch", "pd/save_content",
        "pd/encapsulate_to_file", "pd/gem_capture", "pd/screenshot_canvas", "transport/start", "seq/start" };
    int sent = 0;
    for (auto const& v : mcp::v2schema::kVerbs) {
        std::string const row = std::string(v.domain) + "/" + (*v.verb ? v.verb : "zero_args");
        if (std::find(std::begin(skip), std::end(skip), row) != std::end(skip))
            continue;
        h.fire(("/" + row).c_str(), {});
        ++sent;
        sleepMs(15);
    }
    sleepMs(1500);
    auto const alive = h.call("/pd/ping", {}, "/pd/pong", 5000);
    std::printf("  %d verbs sent with no arguments\n", sent);
    CHECK(alive.has_value());

    auto link = v2Connect();
    int guarded = 0, refused = 0;
    for (auto const& v : mcp::v2schema::kVerbs) {
        if (v.minArgs == 0)
            continue;
        ++guarded;
        auto const id = "Z" + std::to_string(guarded);
        link->sendReq(id, ("/" + std::string(v.domain) + "/" + v.verb).c_str(), "[]");
        auto const f = finalFor(*link, id, 3000);
        auto const* e = f ? f->find("error") : nullptr;
        refused += (f && field(*f, "outcome") == "rejected" && e && field(*e, "code") == "bad_frame") ? 1 : 0;
    }
    CHECK(guarded == 19 && refused == guarded); // 19 since /pd/midi_devices (minArgs 1)
}

// S6.6 (audit A20): the live props of an IEM GUI carry its label, so a control the builder named (`hsl ... morph_s
// morph_r morph`) can be read back as named. The label slot is iemgui_new_getnames' index + 2 in each constructor:
// hsl/vsl/nbx 8, tgl 4, bng 6, hradio/vradio 6. An `empty` label is no label: the property is absent.
void runIemLabelCases(Harness& h)
{
    std::printf("IEM props carry the label (S6.6)\n");
    clearMain(h, "il0");
    auto const created = h.call("/pd/batch_atomic",
        { "main", "il1", 0, 0, 0, 8, 0,
            "ilh", 40, 40, "obj", "hsl", 18,
                "128", "15", "0", "1", "0", "0", "morph_s", "morph_r", "morph", "-2", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0", "1",
            "ilv", 40, 80, "obj", "vsl", 18,
                "15", "128", "0", "1", "0", "0", "empty", "empty", "cut", "0", "-9", "0", "10", "#fcfcfc", "#000000", "#000000", "0", "1",
            "iln", 40, 120, "obj", "nbx", 18,
                "5", "14", "0", "1", "0", "0", "empty", "empty", "freq", "0", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0", "256",
            "ilt", 40, 160, "obj", "tgl", 14,
                "15", "0", "empty", "empty", "gate", "0", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0", "1",
            "ilb", 40, 200, "obj", "bng", 14,
                "15", "250", "50", "0", "empty", "empty", "hit", "0", "-8", "0", "10", "#fcfcfc", "#000000", "#000000",
            "ilr", 40, 240, "obj", "hradio", 15,
                "15", "1", "0", "8", "empty", "empty", "mode", "0", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0",
            "ilw", 40, 280, "obj", "vradio", 15,
                "15", "1", "0", "8", "empty", "empty", "wave", "0", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0",
            "ile", 40, 320, "obj", "hsl", 18,
                "128", "15", "0", "1", "0", "0", "empty", "empty", "empty", "-2", "-8", "0", "10", "#fcfcfc", "#000000", "#000000", "0", "1",
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/il1");
    CHECK(created.has_value() && created->num(0) == 8);

    int corr = 0;
    auto propsOf = [&](char const* id) -> juce::var {
        auto const c = "il_g" + std::to_string(++corr);
        auto r = h.call("/pd/obj_get", { "main", c.c_str(), id }, "/pd/obj_get/reply/" + c, 3000);
        return r ? parseJson(*r)["props"] : juce::var();
    };

    auto const slider = propsOf("ilh");
    CHECK(slider["label"].toString() == "morph");
    CHECK(slider["send"].toString() == "morph_s"); // the names the label sits next to are still read
    CHECK(slider["receive"].toString() == "morph_r");
    CHECK(static_cast<double>(slider["max"]) == 1.0);

    struct Row { char const* id; char const* label; };
    for (auto const& row : { Row { "ilv", "cut" }, Row { "iln", "freq" }, Row { "ilt", "gate" }, Row { "ilb", "hit" }, Row { "ilr", "mode" }, Row { "ilw", "wave" } }) {
        auto const props = propsOf(row.id);
        std::printf("  %s label '%s'\n", row.id, props["label"].toString().toRawUTF8());
        CHECK(props["label"].toString() == row.label);
    }

    auto const unlabelled = propsOf("ile");
    CHECK(unlabelled.isObject() && !unlabelled.hasProperty("label")); // `empty` is no label
    clearMain(h, "il9");
}

// P4.1 S3: the handler cases a second time, through a v2 session of the bridge's own listener, with the same
// expectations on the decoded replies (the evidence that the handlers did not change), then the v2-only cases.
void runV2Pass(Harness& v1, MCPBridge& bridge)
{
    std::printf("v2 pass\n");
    CHECK(bridge.startV2ForTests());
    auto const [port, token] = v2Discovery();
    CHECK(port > 0 && !token.empty());
    V2Link link(port);
    link.client.sendFrame(R"({"t":"hello","protocol":2,"schemaHash":")" + std::string(mcp::v2schema::kSchemaHash) + R"(","token":")"
        + token + R"(","client":{"name":"mcp_bridge_tests"},"telemetry":true})");
    auto const w = link.take([](auto const& v) { return field(v, "t") == "welcome"; }, 3000);
    CHECK(w.has_value());
    if (!w)
        return;
    clearMain(v1, "c0");
    Harness h { v1.bridge, v1.replies, &link };
    runCases(h);
    runV2DispatchCases(h, bridge);
    auto const missing = link.withoutFinal(1500);
    for (auto const& id : missing)
        std::printf("  no final for %s\n", id.c_str());
    CHECK(missing.empty());
    std::printf("  v2 requests: %zu, finals: %zu\n", link.sentIds.size(), link.finalIds.size());
    clearMain(v1, "c9");
}

// S3 review HIGH 3: another program holds the bridge's UDP port (L22). v2 still starts, with its command worker, and
// serves requests; the UDP side stays silent. Restores the port, the sink and the v2 listener afterwards.
void runV2WithoutUdpCases(MCPBridge& bridge, std::function<void()> const& restoreSink)
{
    std::printf("v2: serves while another program holds the UDP port\n");
    int const holder = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    socklen_t len = sizeof a;
    ::getsockname(holder, reinterpret_cast<sockaddr*>(&a), &len);
    int const takenPort = ntohs(a.sin_port);
    int const listenPort = bridge.getListenPort();
    int const sendPort = bridge.getSendPort();
    bridge.stop();
    bridge.rebind(takenPort, sendPort); // not running: only the ports change
    CHECK(!bridge.start());            // UDP refused...
    auto const [port, token] = v2Discovery();
    CHECK(port > 0);                    // ...v2 listening
    {
        V2Link link(port);
        link.client.sendFrame(mcp::v2::test::helloJson(token));
        CHECK(link.take([](auto const& v) { return field(v, "t") == "welcome"; }, 3000).has_value());
        link.sendReq("nu1", "/pd/census", R"([{"t":"s","v":"main"},{"t":"s","v":"nu1"}])");
        auto const f = link.take([](auto const& v) { return field(v, "id") == "nu1"; }, 5000);
        CHECK(f && field(*f, "outcome") == "applied" && field(*f, "address") == "/pd/census/reply/nu1");
    }
    bridge.stop();
    ::close(holder);
    bridge.rebind(listenPort, sendPort);
    restoreSink();
    CHECK(bridge.startV2ForTests());
}

// P4.1 S2: the bridge's own v2 listener over real loopback sockets. The discovery file lands under the scratch
// HOME (main() moved HOME and unset XDG_CONFIG_HOME before the processor started). Ends with the listener stopped
// and the reply sink restored, so later cases run as before.
void runV2HandshakeCases(Harness& h, MCPBridge& bridge, std::function<void()> const& restoreSink)
{
    using namespace mcp::v2::test;
    using mcp::v2::json::Value;
    std::printf("v2 handshake\n");
    CHECK(bridge.startV2ForTests());
    auto const path = mcp::v2::defaultDiscoveryPath();
    char const* home = std::getenv("HOME");
    CHECK(home && path.rfind(home, 0) == 0);
    struct stat st {};
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    std::string err;
    auto const file = mcp::v2::json::parse(text.str(), err);
    CHECK(file.has_value());
    if (!file)
        return;
    auto const str = [&](char const* k) { auto const* v = file->find(k); return v && v->is(Value::Kind::String) ? v->string : std::string(); };
    auto const num = [&](char const* k) { auto const* v = file->find(k); return v ? v->number : -1.0; };
    int const port = static_cast<int>(num("port"));
    auto const token = str("token");
    CHECK(num("pid") == ::getpid() && port > 0 && token.size() == 64 && num("protocol") == 2);
    CHECK(str("schemaHash") == mcp::v2schema::kSchemaHash && str("bridgeVersion") == "11.18");

    // v1 capabilities list protocol_v2 while the listener runs; boot:<token> stays last and matches the file.
    auto const caps = h.call("/bridge/capabilities", { "v2caps" }, "/bridge/capabilities/reply");
    CHECK(caps.has_value());
    std::vector<std::string> v1;
    if (caps)
        for (size_t i = 1; i + 1 < caps->args.size(); ++i)
            v1.push_back(caps->str(i));
    CHECK(caps && caps->str(0) == "11.18" && caps->str(caps->args.size() - 1) == "boot:" + str("bootToken"));
    CHECK(std::find(v1.begin(), v1.end(), "protocol_v2") != v1.end());

    // welcome: the same capability list (no version, no boot entry), the boot token, the schema hash.
    {
        Client cl(port);
        CHECK(cl.connected && cl.sendFrame(helloJson(token)));
        auto const w = cl.next();
        CHECK(Client::kindOf(w) == "welcome");
        if (w) {
            std::vector<std::string> wc;
            if (auto const* c = w->find("caps"))
                for (auto const& item : c->items)
                    wc.push_back(item.string);
            CHECK(wc == v1);
            CHECK(w->find("bootToken") && w->find("bootToken")->string == str("bootToken"));
            CHECK(w->find("bridgeVersion") && w->find("bridgeVersion")->string == "11.18");
            CHECK(w->find("schemaHash") && w->find("schemaHash")->string == mcp::v2schema::kSchemaHash);
            CHECK(!w->find("token"));
        }
        CHECK(cl.sendFrame(R"({"t":"ping"})") && Client::kindOf(cl.next()) == "pong");
    }

    // Refusals: typed bye, then the bridge closes.
    CHECK(refusalOf(port, mcp::v2::encodeFrame(helloJson(std::string(64, '0')))) == "unauthorized");
    CHECK(refusalOf(port, mcp::v2::encodeFrame(helloJson(token, "0000000000000000"))) == "schema_mismatch");
    CHECK(refusalOf(port, std::string("\xff\xff\xff\xff", 4)) == "too_large");
    CHECK(refusalOf(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n") == "too_large");
    {
        Client silent(port); // no hello: refused after the schema's 2 s
        auto const t0 = Clock::now();
        auto const f = silent.next(5000);
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        CHECK(Client::kindOf(f) == "bye" && errorCode(f) == "bad_frame" && ms >= 1900 && ms < 3500);
    }

    // stop(): a connected session gets bye{shutting_down}, the file goes, v1 caps drop protocol_v2.
    {
        Client cl(port);
        cl.sendFrame(helloJson(token));
        CHECK(Client::kindOf(cl.next()) == "welcome");
        bridge.stop();
        auto const f = cl.next(2000);
        CHECK(Client::kindOf(f) == "bye" && errorCode(f) == "shutting_down");
        CHECK(::stat(path.c_str(), &st) != 0);
    }
    restoreSink();
    auto const after = h.call("/bridge/capabilities", { "v2caps2" }, "/bridge/capabilities/reply");
    CHECK(after.has_value());
    bool listed = false;
    if (after)
        for (auto const& a : after->args)
            listed = listed || a.text == "protocol_v2";
    CHECK(!listed);
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
            runV2Pass(harness, *bridge);
            runIemLabelCases(harness);
            runV2IdempotencyCases(harness);
            runV2OwnershipCases(harness);
            runZeroArgCases(harness);
            runSpectralPitchCases();
            runV2WithoutUdpCases(*bridge, [&] {
                bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });
            });
            runPdContextCases(processor, audioControl);
            runNativeUndoCases(processor);
            runSharedUndoCases(harness, processor);
            runUndoPeekCases(harness, processor);
            runAudioStallCases(harness, audioControl);
            runArmedTraceCases(harness);
            runConsoleObjectCases(harness);
            runLimiterCases(harness, processor);
            runOutputCeilingCases(harness, processor, audioControl);
            runMidiDevicesCases(harness, processor);
            runLimiterPatchCases(processor);
            runVolumeGuardCases(processor);
            runIndexRefCases(harness, processor);
            runMidiCaptureCases(harness, processor, audioControl);
            runStampCostCases(processor);
            runIdentityValidityCases(processor);
            runSidecarCases(processor);
            runV2HandshakeCases(harness, *bridge, [&] {
                bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });
            });
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
