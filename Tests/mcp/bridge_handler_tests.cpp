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
#include <pwd.h>
#include <sys/stat.h>
#include <climits>
#include <cstdlib>
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
#include <juce_cryptography/juce_cryptography.h> // juce::SHA256: the save_content hash (P11.2 V11)
#include "Utility/PluginParameter.h" // PlugDataParameter: the master volume guard (P5.1 sink floor)
#include "Mcp/McpPdFile.h" // P11.8 item 3: mcp::fileInPdDir
#include "Pd/MCPBridgeSaveTarget.h" // P11.8 delta review: mcp::saveTargetOf with tabs

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

    // P11 V7 (J7): an 808-style one-shot in a 0.5 s armed capture at 44.1 kHz. Pre-roll silence, then a pitch sweep
    // f(t) = body + (start - body) * max(0, 1 - t / 0.09) Hz, amplitude exp(-t / 0.45), tanh(2x), and a 6 ms high-passed
    // noise click at 0.25 s of the capture. The pitch is the BODY's (median over hop-spaced frames after the onset),
    // the strongest bin comes from a Welch average of those frames; the attack frame's own pitch is reported apart.
    double const ksr = 44100.0;
    auto kick = [ksr](double body, double start, double prerollMs) {
        int const n = static_cast<int>(ksr * 0.5);
        std::vector<float> v(static_cast<size_t>(n), 0.0f);
        int const onset = static_cast<int>(ksr * prerollMs / 1000.0);
        double phase = 0.0;
        for (int i = onset; i < n; ++i) {
            double const t = (i - onset) / ksr;
            double const f = body + (start - body) * std::max(0.0, 1.0 - t / 0.09);
            phase += 2.0 * M_PI * f / ksr;
            v[static_cast<size_t>(i)] = static_cast<float>(std::tanh(2.0 * std::exp(-t / 0.45) * std::sin(phase)));
        }
        uint32_t seed = 12345u;
        float prev = 0.0f;
        int const click = static_cast<int>(ksr * 0.25);
        for (int i = click; i < click + static_cast<int>(ksr * 0.006) && i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            float const noise = static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
            v[static_cast<size_t>(i)] += 0.3f * (noise - prev); // first difference: a high-passed click
            prev = noise;
        }
        return v;
    };
    struct KickCase { double body, start, prerollMs; };
    // Review L1: a chord or noise has no clear pitch: fundamental 0, pitchMethod says so; the confidence and the voiced
    // ratio are reported.
    {
        int const n = static_cast<int>(ksr);
        std::vector<float> chord(static_cast<size_t>(n)), noise(static_cast<size_t>(n));
        uint32_t seed = 777u;
        for (int i = 0; i < n; ++i) {
            double const t = i / ksr;
            chord[static_cast<size_t>(i)] = static_cast<float>(0.2 * (std::sin(2 * M_PI * 220 * t) + std::sin(2 * M_PI * 261.63 * t) + std::sin(2 * M_PI * 329.63 * t)));
            seed = seed * 1664525u + 1013904223u;
            noise[static_cast<size_t>(i)] = static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }
        for (auto* v : { &chord, &noise }) {
            auto const json = juce::JSON::parse(armedSpectralJson(v->data(), n, ksr));
            std::printf("  %s -> fundamental %.2f, confidence %.2f, voiced ratio %.2f, %s\n", v == &chord ? "A minor triad" : "noise",
                static_cast<double>(json["fundamental"]), static_cast<double>(json["pitchConfidence"]),
                static_cast<double>(json["voicedRatio"]), json["pitchMethod"].toString().toRawUTF8());
            CHECK(static_cast<double>(json["fundamental"]) == 0.0);
            CHECK(json["pitchMethod"].toString().contains("no clear pitch"));
            CHECK(json.hasProperty("pitchConfidence") && json.hasProperty("voicedRatio"));
        }
    }
    for (auto const k : { KickCase { 48, 158, 20 }, KickCase { 45, 155, 20 }, KickCase { 48, 300, 20 },
             KickCase { 48, 158, 0 }, KickCase { 48, 158, 200 } }) {
        auto buf = kick(k.body, k.start, k.prerollMs);
        auto const json = juce::JSON::parse(armedSpectralJson(buf.data(), static_cast<int>(buf.size()), ksr));
        double const f0 = json.isObject() ? static_cast<double>(json.getProperty("fundamental", -1.0)) : -1.0;
        double const peakHz = json.isObject() ? static_cast<double>(json.getProperty("peakFrequency", -1.0)) : -1.0;
        int const voiced = json.isObject() ? static_cast<int>(json.getProperty("voicedFrames", -1)) : -1;
        bool const hasAttack = json.isObject() && json.hasProperty("attackPitchHz");
        std::printf("  kick %.0f Hz (from %.0f, pre-roll %.0f ms) -> fundamental %.2f, peak %.1f Hz, voiced %d\n",
            k.body, k.start, k.prerollMs, f0, peakHz, voiced);
        CHECK(std::abs(f0 - k.body) <= k.body * 0.05);
        CHECK(peakHz > 0.0 && peakHz < 70.0);
        CHECK(voiced >= 3);
        CHECK(hasAttack);
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

// P11 V1: every armed window has a generation id. /meter/arm and /meter/arm_spectral answer it (reply arg 0); a read
// that names an older (or any other) id is refused on /meter/<read|read_spectral>/error/<corr> ("stale_gen ...") and
// leaves the open window alone; a read without an id is the legacy path. The capability `meter_window_gen` says so.
bool hasCap(Harness& h, char const* cap, char const* corr)
{
    auto caps = h.call("/bridge/capabilities", { corr }, "/bridge/capabilities/reply");
    bool found = false;
    if (caps)
        for (auto const& a : caps->args)
            found = found || a.text == cap;
    return found;
}

void runMeterWindowGenCases(Harness& h, PluginProcessor& processor)
{
    std::printf("meter window generation id (P11 V1)\n");
    auto& session = processor.getMcp()->session;
    CHECK(hasCap(h, "meter_window_gen", "wg0"));

    auto a1 = h.call("/meter/arm", { "wg1" }, "/meter/arm/reply/wg1");
    auto a2 = h.call("/meter/arm", { "wg2" }, "/meter/arm/reply/wg2");
    CHECK(a1.has_value() && a1->args.size() >= 1 && a1->args[0].isNumber);
    CHECK(a2.has_value() && a2->args.size() >= 1 && a2->args[0].isNumber);
    int const g1 = a1 && !a1->args.empty() ? static_cast<int>(a1->num(0)) : -1;
    int const g2 = a2 && !a2->args.empty() ? static_cast<int>(a2->num(0)) : -1;
    CHECK(g1 >= 0 && g2 > g1);
    // The older id is refused and the newer window stays open.
    auto stale = h.call("/meter/read", { "wg3", g1 }, "/meter/read/error/wg3", 2000);
    CHECK(stale.has_value() && stale->str(0).rfind("stale_gen", 0) == 0);
    CHECK(session.mcpArmActive.load());
    // The current id reads and names its window (arg 15).
    auto cur = h.call("/meter/read", { "wg4", g2 }, "/meter/read/reply/wg4", 2000);
    CHECK(cur.has_value() && cur->args.size() >= 16 && static_cast<int>(cur->num(15)) == g2);
    CHECK(!session.mcpArmActive.load());
    // The legacy read (no id) still answers.
    auto legacy = h.call("/meter/read", { "wg5" }, "/meter/read/reply/wg5", 2000);
    CHECK(legacy.has_value() && legacy->args.size() >= 10);

    // Review L5: a window id past int32 (a float 3e9) is a stale id, never a legacy read that closes the window.
    {
        h.call("/meter/arm", { "wg10" }, "/meter/arm/reply/wg10");
        auto huge = h.call("/meter/read", { "wg11", 3.0e9 }, "/meter/read/error/wg11", 2000);
        CHECK(huge.has_value());
        CHECK(session.mcpArmActive.load());
        h.call("/meter/read", { "wg12" }, "/meter/read/reply/wg12");
    }

    // The spectral window: the same scheme.
    auto s1 = h.call("/meter/arm_spectral", { 100, "wg6" }, "/meter/arm_spectral/reply/wg6");
    auto s2 = h.call("/meter/arm_spectral", { 100, "wg7" }, "/meter/arm_spectral/reply/wg7");
    CHECK(s1.has_value() && s1->args.size() >= 1 && s1->args[0].isNumber);
    CHECK(s2.has_value() && s2->args.size() >= 1 && s2->args[0].isNumber);
    int const sg1 = s1 && !s1->args.empty() ? static_cast<int>(s1->num(0)) : -1;
    int const sg2 = s2 && !s2->args.empty() ? static_cast<int>(s2->num(0)) : -1;
    CHECK(sg1 >= 0 && sg2 > sg1);
    auto sStale = h.call("/meter/read_spectral", { "wg8", sg1 }, "/meter/read_spectral/error/wg8", 2000);
    CHECK(sStale.has_value() && sStale->str(0).rfind("stale_gen", 0) == 0);
    sleepMs(250); // the audio thread fills the 100 ms window
    auto sCur = h.call("/meter/read_spectral", { "wg9", sg2 }, "/meter/read_spectral/reply/wg9", 3000);
    CHECK(sCur.has_value());
    if (sCur) {
        auto const json = parseJson(*sCur);
        CHECK(json.hasProperty("gen") && static_cast<int>(json["gen"]) == sg2);
    }
}

// P11 V2 (J3b): one armed window, three named taps. /meter/read args 10-14: the PRE-LIMITER tap (after PlugData's
// master gain, before its limiter; the existing midiTick hook): peak (10) and RMS (11); the FINAL tap's RMS (12); the
// master volume as the linear gain PlugData applies (13); the limiter's largest gain reduction in the window, dB (14).
// Arg 15 is the window id (V1), arg 16 Pd's own output peak before the master gain. The PRE-CEILING tap is arg 5 (P9),
// the FINAL peak arg 1. Rendered through processBlock with this file's audio thread paused.
void runMeterTapCases(Harness& h, PluginProcessor& processor, AudioControl& audio)
{
    std::printf("meter taps: pre-limiter, pre-ceiling, final (P11 V2)\n");
    auto* host = processor.getMcp();
    PlugDataParameter* volume = nullptr;
    for (auto* p : processor.getParameters())
        if (auto* param = dynamic_cast<PlugDataParameter*>(p); param && param->getTitle().toString() == "volume")
            volume = param;
    CHECK(volume != nullptr);
    if (!volume) return;
    float const volumeBefore = volume->getUnscaledValue();
    bool const limiterBefore = processor.getEnableLimiter();
    int const thresholdBefore = host->onMessageThread(
        [] { return SettingsFile::getInstance()->getProperty<int>("limiter_threshold"); }, 2000, "test").value_or(1);
    auto setThreshold = [&](int index) {
        host->onMessageThread([&processor, index] { processor.setLimiterThreshold(index); return true; }, 2000, "test");
    };

    audio.paused = true;
    sleepMs(60);
    // [osc~ 440] -> [dac~ 1 2]: a full-scale sine at Pd's output.
    auto made = h.call("/pd/batch_atomic",
        { "main", "tap1", 0, 0, 0, 2, 2,
            "tapo", 900, 40, "obj", "osc~", 1, "440",
            "tapd", 900, 120, "obj", "dac~", 0,
            "tapo", 0, "tapd", 0,
            "tapo", 0, "tapd", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/tap1");
    CHECK(made.has_value() && made->num(0) == 2);
    h.fire("/pd/dsp", { 1 });
    sleepMs(200);
    int const outputs = processor.getTotalNumOutputChannels();
    juce::AudioBuffer<float> buffer(std::max(2, outputs), 512);
    juce::MidiBuffer midi;
    auto render = [&](int blocks) {
        for (int k = 0; k < blocks; ++k) {
            buffer.clear();
            midi.clear();
            processor.processBlock(buffer, midi);
        }
    };
    auto window = [&](char const* armCorr, char const* readCorr, int blocks) -> std::optional<Reply> {
        auto arm = h.call("/meter/arm", { armCorr }, std::string("/meter/arm/reply/") + armCorr);
        int const gen = arm && !arm->args.empty() ? static_cast<int>(arm->num(0)) : 0;
        render(blocks);
        return h.call("/meter/read", { readCorr, gen }, std::string("/meter/read/reply/") + readCorr, 2000);
    };

    // Limiter on at -12 dB, unity master: pre-limiter ~1.0, final ~0.2512, about 12 dB of gain reduction.
    processor.setEnableLimiter(true);
    setThreshold(0);
    volume->setUnscaledValueNotifyingHost(0.8f);
    render(60);
    {
        auto r = window("tapA", "tapB", 30);
        CHECK(r.has_value() && r->args.size() >= 17);
        if (r && r->args.size() >= 17) {
            std::printf("  limiter -12: pre %.4f/%.4f rms, final %.4f/%.4f rms, volume %.4f, gr %.2f dB, pd %.4f\n",
                r->num(10), r->num(11), r->num(1), r->num(12), r->num(13), r->num(14), r->num(16));
            CHECK(r->num(10) > 0.95 && r->num(10) < 1.05);    // pre-limiter peak
            CHECK(r->num(11) > 0.65 && r->num(11) < 0.75);    // pre-limiter RMS (sine: 0.707)
            CHECK(r->num(1) > 0.20 && r->num(1) < 0.32);      // final peak, held by the limiter
            CHECK(r->num(12) > 0.12 && r->num(12) < 0.25);    // final RMS
            CHECK(std::abs(r->num(13) - 1.0) < 1e-3);         // master volume: unity
            CHECK(r->num(14) > 9.0 && r->num(14) < 15.0);     // gain reduction, dB
            CHECK(r->num(16) > 0.95 && r->num(16) < 1.05);    // Pd's own output
        }
    }
    // Review L3: a re-arm starts from empty sums even when the audio thread stores a sum of the old window after the
    // arm (simulated: a huge sum written right after the arm); the next block drops it.
    {
        auto arm = h.call("/meter/arm", { "tapG" }, "/meter/arm/reply/tapG");
        int const gen = arm && !arm->args.empty() ? static_cast<int>(arm->num(0)) : 0;
        host->session.mcpArmFinalSumSq.store(1.0e12);
        host->session.mcpArmFinalCount.store(1);
        host->session.mcpArmPreLimiterPeak.store(50.0f);
        render(10);
        auto r = h.call("/meter/read", { "tapH", gen }, "/meter/read/reply/tapH", 2000);
        CHECK(r.has_value() && r->args.size() >= 17 && r->num(12) < 1.0 && r->num(10) < 2.0);
    }
    // Master volume 0, limiter off: the patch plays (Pd's output ~1.0), nothing passes the master gain.
    processor.setEnableLimiter(false);
    volume->setUnscaledValueNotifyingHost(0.0f);
    render(60);
    {
        auto r = window("tapC", "tapD", 20);
        CHECK(r.has_value() && r->args.size() >= 17);
        if (r && r->args.size() >= 17) {
            CHECK(r->num(10) < 1e-4);                         // pre-limiter (after the master gain)
            CHECK(r->num(1) < 1e-4);                          // final
            CHECK(r->num(13) == 0.0);                         // master volume 0
            CHECK(r->num(14) == 0.0);                         // limiter off: no reduction
            CHECK(r->num(16) > 0.95);                         // the patch itself plays
        }
    }
    // Master slider 0.4: the gain PlugData applies, (0.4 / 0.8) ^ 2.5; the pre-limiter tap shows exactly that.
    volume->setUnscaledValueNotifyingHost(0.4f);
    render(60);
    {
        auto r = window("tapE", "tapF", 20);
        CHECK(r.has_value() && r->args.size() >= 17);
        if (r && r->args.size() >= 17) {
            double const want = std::pow(0.5, 2.5);
            CHECK(std::abs(r->num(13) - want) < 1e-3);
            CHECK(std::abs(r->num(10) - r->num(16) * r->num(13)) < 0.02);
        }
    }

    volume->setUnscaledValueNotifyingHost(volumeBefore);
    processor.setEnableLimiter(limiterBefore);
    setThreshold(thresholdBefore);
    h.call("/pd/batch_atomic", { "main", "tap2", 2, 0, 0, 0, 0, "tapo", "tapd", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/tap2");
    render(4);
    audio.paused = false;
}

// V1 AC taps (2026-11-06): a [phasor~] through a [lop~] stuck at cutoff 0 put out pure DC; every meter said "-22
// dBFS, audible" and the artist heard nothing. Every tap now also reports the level left after a ~10 Hz DC blocker
// (or the block mean, for the one-block snapshots) and `tone` (AC RMS above -60 dBFS); the old fields stay as they
// were. Layout: /meter/read args 17-20, /meter/result (probe), /meter/master and /meter/query args 4-7, each
// acRmsDb, acPeakDb, dcDb, tone (0/1; the one-block snapshots /meter/master and /meter/query: 1 tone, 0 no sound, -1
// unknown); the armed trace's levels[] and the spectral result JSON carry the same names. Capability `meter_ac`.
// Review F1-F3: a snapshot never calls a bass "no sound"; a DC step is no tone (header test); the final tap judges
// each channel alone (unequal L/R here). [sig~ 0.0794] is the DC drone (-22 dBFS); [phasor~ 110] is a tone with a DC of 0.5.
void runAcTapCases(Harness& h, PluginProcessor& processor, AudioControl& audio)
{
    std::printf("AC taps: DC is no tone (V1)\n");
    CHECK(hasCap(h, "meter_ac", "ac0"));
    auto made = h.call("/pd/batch_atomic",
        { "main", "ac1", 0, 0, 0, 2, 2,
            "ac_dc", 1200, 40, "obj", "sig~", 1, "0.0794",
            "ac_dac", 1200, 120, "obj", "dac~", 0,
            "ac_dc", 0, "ac_dac", 0,
            "ac_dc", 0, "ac_dac", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ac1");
    CHECK(made.has_value() && made->num(0) == 2);
    h.fire("/pd/dsp", { 1 });
    sleepMs(300);

    // The probe (/meter/start): raw RMS -22 dBFS as before, AC at the floor, DC -22, no tone.
    auto probe = h.call("/meter/start", { "main", "ac_dc", 0, 300, "ac2" }, "/meter/result/ac2", 3000);
    CHECK(probe.has_value() && probe->args.size() >= 8);
    if (probe && probe->args.size() >= 8) {
        std::printf("  probe dc: rms %.1f, ac %.1f/%.1f, dc %.1f, tone %d\n", probe->num(0), probe->num(4), probe->num(5), probe->num(6), static_cast<int>(probe->num(7)));
        CHECK(std::abs(probe->num(0) - (-22.0)) < 0.5); // the old field, unchanged: it reads the DC
        CHECK(probe->num(4) <= -99.0);
        CHECK(probe->num(5) <= -99.0);
        CHECK(std::abs(probe->num(6) - (-22.0)) < 0.5);
        CHECK(probe->num(7) == 0.0);
    }
    // The same probe slot again with another DC (0.5 = -6.02 dBFS): the blocker starts fresh, no step from the old one.
    {
        auto set = h.call("/pd/obj_set", { "main", "ac2s", "ac_dc", 0, "float", 1, 0.5 }, "/pd/obj_set/reply/ac2s");
        CHECK(set.has_value());
        sleepMs(60);
        auto again = h.call("/meter/start", { "main", "ac_dc", 0, 300, "ac2b" }, "/meter/result/ac2b", 3000);
        CHECK(again.has_value() && again->args.size() >= 8);
        if (again && again->args.size() >= 8) {
            std::printf("  probe reuse dc 0.5: ac %.1f/%.1f, dc %.2f, tone %d\n", again->num(4), again->num(5), again->num(6), static_cast<int>(again->num(7)));
            CHECK(again->num(4) <= -99.0 && again->num(5) <= -99.0 && again->num(7) == 0.0);
            CHECK(std::abs(again->num(6) - (-6.02)) < 0.2);
        }
        h.call("/pd/obj_set", { "main", "ac2r", "ac_dc", 0, "float", 1, 0.0794 }, "/pd/obj_set/reply/ac2r");
        sleepMs(60);
    }
    // The snapshots: /meter/master and /meter/query.
    auto master = h.call("/meter/master", { "main", "ac3" }, "/meter/master/reply/ac3");
    CHECK(master.has_value() && master->args.size() >= 8);
    if (master && master->args.size() >= 8) {
        CHECK(master->num(0) > -60.0); // the old verdict: "audible"
        CHECK(master->num(4) <= -99.0 && master->num(7) == 0.0);
        CHECK(std::abs(master->num(6) - (-22.0)) < 0.5);
    }
    auto query = h.call("/meter/query", { "main", "ac_dc", 0, "ac4" }, "/meter/query/reply/ac4");
    CHECK(query.has_value() && query->args.size() >= 8);
    if (query && query->args.size() >= 8)
        CHECK(query->num(4) <= -99.0 && query->num(7) == 0.0 && std::abs(query->num(6) - (-22.0)) < 0.5);
    // The armed trace: the tap's level entry.
    auto armed = h.call("/meter/trace", { "main", "ac5", "arm", 300, "" }, "/meter/trace/reply/ac5");
    CHECK(armed.has_value());
    sleepMs(350);
    auto read = h.call("/meter/trace", { "main", "ac6", "read" }, "/meter/trace/reply/ac6");
    CHECK(read.has_value());
    if (read) {
        auto const json = parseJson(*read);
        bool seen = false;
        if (auto* arr = json["levels"].getArray())
            for (auto const& l : *arr)
                if (l["tempId"].toString() == "ac_dc") {
                    seen = true;
                    CHECK(static_cast<double>(l["peakDb"]) > -23.0); // unchanged
                    CHECK(static_cast<double>(l["acRmsDb"]) <= -99.0);
                    CHECK(static_cast<double>(l["acPeakDb"]) <= -99.0);
                    CHECK(std::abs(static_cast<double>(l["dcDb"]) - (-22.0)) < 0.5);
                    CHECK(l.hasProperty("tone") && !static_cast<bool>(l["tone"]));
                }
        CHECK(seen);
    }

    // The final tap (/meter/read), rendered through processBlock: limiter off, unity master.
    PlugDataParameter* volume = nullptr;
    for (auto* p : processor.getParameters())
        if (auto* param = dynamic_cast<PlugDataParameter*>(p); param && param->getTitle().toString() == "volume")
            volume = param;
    CHECK(volume != nullptr);
    float const volumeBefore = volume ? volume->getUnscaledValue() : 0.8f;
    bool const limiterBefore = processor.getEnableLimiter();
    audio.paused = true;
    sleepMs(60);
    processor.setEnableLimiter(false);
    if (volume) volume->setUnscaledValueNotifyingHost(0.8f);
    juce::AudioBuffer<float> buffer(std::max(2, processor.getTotalNumOutputChannels()), 512);
    juce::MidiBuffer midi;
    auto render = [&](int blocks) {
        for (int k = 0; k < blocks; ++k) {
            buffer.clear();
            midi.clear();
            processor.processBlock(buffer, midi);
        }
    };
    auto window = [&](char const* armCorr, char const* readCorr, int blocks) -> std::optional<Reply> {
        auto arm = h.call("/meter/arm", { armCorr }, std::string("/meter/arm/reply/") + armCorr);
        int const gen = arm && !arm->args.empty() ? static_cast<int>(arm->num(0)) : 0;
        render(blocks);
        return h.call("/meter/read", { readCorr, gen }, std::string("/meter/read/reply/") + readCorr, 2000);
    };
    render(20);
    {
        auto r = window("ac7", "ac8", 40);
        CHECK(r.has_value() && r->args.size() >= 21);
        if (r && r->args.size() >= 21) {
            std::printf("  final dc: peak %.1f, ac %.1f/%.1f, dc %.1f, tone %d\n", r->num(0), r->num(17), r->num(18), r->num(19), static_cast<int>(r->num(20)));
            CHECK(std::abs(r->num(0) - (-22.0)) < 0.5); // the old peak: still reads the DC
            CHECK(r->num(17) <= -99.0);
            CHECK(r->num(18) <= -99.0);
            CHECK(std::abs(r->num(19) - (-22.0)) < 0.5);
            CHECK(r->num(20) == 0.0);
        }
    }
    // The same window with a tone: [phasor~ 110] (saw 0..1: AC RMS 0.2887 = -10.8 dBFS, DC 0.5 = -6 dBFS), through
    // [*~ 0.5] to the dac~ (-16.8 / -12 dBFS there, well under the output ceiling).
    h.call("/pd/batch_atomic", { "main", "ac9", 1, 0, 0, 0, 0, "ac_dc", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ac9");
    auto tone = h.call("/pd/batch_atomic",
        { "main", "ac10", 0, 0, 0, 2, 3,
            "ac_ph", 1200, 40, "obj", "phasor~", 1, "110",
            "ac_half", 1200, 80, "obj", "*~", 1, "0.5",
            "ac_ph", 0, "ac_half", 0,
            "ac_half", 0, "ac_dac", 0,
            "ac_half", 0, "ac_dac", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ac10");
    CHECK(tone.has_value() && tone->num(0) == 2);
    render(20);
    {
        auto r = window("ac11", "ac12", 60);
        CHECK(r.has_value() && r->args.size() >= 21);
        if (r && r->args.size() >= 21) {
            std::printf("  final phasor: peak %.1f, ac %.1f/%.1f, dc %.1f, tone %d\n", r->num(0), r->num(17), r->num(18), r->num(19), static_cast<int>(r->num(20)));
            CHECK(r->num(20) == 1.0);
            CHECK(std::abs(r->num(17) - (-16.8)) < 1.0);
            CHECK(std::abs(r->num(19) - (-12.0)) < 1.0);
        }
    }
    if (volume) volume->setUnscaledValueNotifyingHost(volumeBefore);
    processor.setEnableLimiter(limiterBefore);
    audio.paused = false;
    sleepMs(100);
    // The probe on the tone.
    auto probeTone = h.call("/meter/start", { "main", "ac_ph", 0, 300, "ac13" }, "/meter/result/ac13", 3000);
    CHECK(probeTone.has_value() && probeTone->args.size() >= 8);
    if (probeTone && probeTone->args.size() >= 8) {
        CHECK(probeTone->num(7) == 1.0);
        CHECK(std::abs(probeTone->num(4) - (-10.8)) < 1.0);
        CHECK(std::abs(probeTone->num(6) - (-6.0)) < 0.5);
    }
    auto masterTone = h.call("/meter/master", { "main", "ac14" }, "/meter/master/reply/ac14");
    CHECK(masterTone.has_value() && masterTone->args.size() >= 8 && masterTone->num(7) == 1.0);
    // The armed trace on the tone: the phasor's tap reads tone true at its AC level.
    {
        auto arm = h.call("/meter/trace", { "main", "ac16", "arm", 300, "" }, "/meter/trace/reply/ac16");
        CHECK(arm.has_value());
        sleepMs(350);
        auto rd = h.call("/meter/trace", { "main", "ac17", "read" }, "/meter/trace/reply/ac17");
        CHECK(rd.has_value());
        bool seen = false;
        juce::var const json = rd ? parseJson(*rd) : juce::var();
        if (rd)
            if (auto* arr = json["levels"].getArray())
                for (auto const& l : *arr)
                    if (l["tempId"].toString() == "ac_ph") {
                        seen = true;
                        CHECK(static_cast<bool>(l["tone"]));
                        CHECK(std::abs(static_cast<double>(l["acRmsDb"]) - (-10.8)) < 1.0);
                        CHECK(std::abs(static_cast<double>(l["dcDb"]) - (-6.0)) < 0.5);
                    }
        CHECK(seen);
    }
    // Review F1: a 30 Hz sine in one 64-sample block: the snapshot cannot see it, but never says "no sound" (0).
    {
        auto bass = h.call("/pd/batch_atomic",
            { "main", "ac18", 0, 0, 0, 2, 1,
                "ac_bass", 1400, 40, "obj", "osc~", 1, "30",
                "ac_sink", 1400, 80, "obj", "*~", 1, "0",
                "ac_bass", 0, "ac_sink", 0,
                0, 5, 10, 0 },
            "/pd/batch_atomic/reply/ac18");
        CHECK(bass.has_value() && bass->num(0) == 2);
        sleepMs(150); // let the new oscillator run (a fresh signal vector is all zeros: rightly "no sound")
        int unknown = 0, sounding = 0;
        for (int k = 0; k < 8; ++k) {
            auto const corr = std::string("ac19_") + std::to_string(k);
            auto q = h.call("/meter/query", { "main", "ac_bass", 0, corr.c_str() }, "/meter/query/reply/" + corr);
            CHECK(q.has_value() && q->args.size() >= 8);
            if (q && q->args.size() >= 8) {
                CHECK(q->num(7) != 0.0);
                unknown += q->num(7) == -1.0;
                sounding += q->num(7) == 1.0;
            }
            sleepMs(7);
        }
        std::printf("  snapshot 30 Hz osc: tone 1 x%d, -1 x%d\n", sounding, unknown);
        h.call("/pd/batch_atomic", { "main", "ac20", 2, 0, 0, 0, 0, "ac_bass", "ac_sink", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ac20");
    }

    h.call("/pd/batch_atomic", { "main", "ac15", 3, 0, 0, 0, 0, "ac_ph", "ac_half", "ac_dac", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ac15");

    // Review F3: unequal L/R at the final tap. The tone on the left (-16.8 dBFS AC), DC alone on the right: the reply
    // carries the left channel's AC level (not the two channels averaged, -19.8) and the louder DC (left, -12).
    audio.paused = true;
    sleepMs(60);
    processor.setEnableLimiter(false);
    if (volume) volume->setUnscaledValueNotifyingHost(0.8f);
    auto lr = h.call("/pd/batch_atomic",
        { "main", "ac21", 0, 0, 0, 4, 3,
            "ac_lph", 1200, 40, "obj", "phasor~", 1, "110",
            "ac_lhalf", 1200, 80, "obj", "*~", 1, "0.5",
            "ac_rdc", 1300, 80, "obj", "sig~", 1, "0.0794",
            "ac_lrdac", 1200, 140, "obj", "dac~", 0,
            "ac_lph", 0, "ac_lhalf", 0,
            "ac_lhalf", 0, "ac_lrdac", 0,
            "ac_rdc", 0, "ac_lrdac", 1,
            0, 5, 10, 0 },
        "/pd/batch_atomic/reply/ac21");
    CHECK(lr.has_value() && lr->num(0) == 4);
    render(20);
    {
        auto r = window("ac22", "ac23", 60);
        CHECK(r.has_value() && r->args.size() >= 21);
        if (r && r->args.size() >= 21) {
            std::printf("  final L tone / R dc: ac %.1f/%.1f, dc %.1f, tone %d\n", r->num(17), r->num(18), r->num(19), static_cast<int>(r->num(20)));
            CHECK(r->num(20) == 1.0);
            CHECK(std::abs(r->num(17) - (-16.8)) < 0.7);
            CHECK(std::abs(r->num(19) - (-12.0)) < 0.7);
        }
    }
    if (volume) volume->setUnscaledValueNotifyingHost(volumeBefore);
    processor.setEnableLimiter(limiterBefore);
    h.call("/pd/batch_atomic", { "main", "ac24", 4, 0, 0, 0, 0, "ac_lph", "ac_lhalf", "ac_rdc", "ac_lrdac", 0, 5, 10, 0 }, "/pd/batch_atomic/reply/ac24");
    render(4);
    audio.paused = false;
    h.fire("/pd/dsp", { 0 });
}

// P11.2 V10: /pd/load_patch reads and checks the file BEFORE it clears the canvas. An unreadable file or one that is
// not a Pd patch is an error and the canvas keeps every object; a valid empty patch is a valid load. Reply: arg0 the
// object count (-1 on an error, old clients read only this), arg1 1 (undo reset) or "error: ...", arg2 the JSON
// report {canvasFound, fileObjects, created, failedCreates:[{index,text}], sidecar:{applied,skipped}} (capability
// `load_patch_report`). Failed creates are Pd's red boxes (text_class, T_OBJECT). Loaded into a subpatch so the main
// canvas of the other cases stays as it is.
void runLoadReportCases(Harness& h, PluginProcessor& processor)
{
    std::printf("load_patch validates before clearing, report (P11.2 V10)\n");
    auto* host = processor.getMcp();
    CHECK(hasCap(h, "load_patch_report", "lr0"));
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mcp_p11_load");
    dir.deleteRecursively();
    dir.createDirectory();
    auto good = dir.getChildFile("good.pd");
    good.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n#X obj 10 40 p11_no_such_class_xyz 1 2;\n"
                         "#X msg 10 70 hello;\n#X connect 0 0 2 0;\n");
    auto empty = dir.getChildFile("empty.pd");
    empty.replaceWithText("#N canvas 0 50 450 300 12;\n");
    auto garbage = dir.getChildFile("garbage.pd");
    garbage.replaceWithText("this is not a patch ; at all ;\n");
    auto locked = dir.getChildFile("locked.pd");
    locked.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n");
    ::chmod(locked.getFullPathName().toRawUTF8(), 0);

    t_canvas* sub = nullptr;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        if (!cnv) return;
        t_atom a[4];
        SETFLOAT(a, 900);
        SETFLOAT(a + 1, 600);
        SETSYMBOL(a + 2, gensym("pd"));
        SETSYMBOL(a + 3, gensym("p11load"));
        pd_typedmess(&cnv->gl_pd, gensym("obj"), 4, a);
        sub = reinterpret_cast<t_canvas*>(pd::Interface::getNewest(cnv));
        for (char const* cls : { "f", "t" }) {
            t_atom b[3];
            SETFLOAT(b, 10);
            SETFLOAT(b + 1, 10);
            SETSYMBOL(b + 2, gensym(cls));
            pd_typedmess(&sub->gl_pd, gensym("obj"), 3, b);
        }
    }, 3000));
    CHECK(sub != nullptr);
    if (!sub) return;
    auto count = [&] {
        int n = -1;
        host->pdWait([&] { n = 0; for (t_gobj* g = sub->gl_list; g; g = g->g_next) ++n; }, 3000);
        return n;
    };
    auto load = [&](juce::File const& f, char const* canvas, char const* corr) {
        return h.call("/pd/load_patch", { canvas, f.getFullPathName(), corr }, std::string("/pd/load_patch/reply/") + corr, 5000);
    };
    auto report = [](std::optional<Reply> const& r) {
        return r && r->args.size() >= 3 ? juce::JSON::parse(juce::String(r->str(2))) : juce::var();
    };
    CHECK(count() == 2);
    for (auto const& [file, corr] : { std::make_pair(garbage, "lr1"), std::make_pair(locked, "lr2") }) {
        auto r = load(file, "pd-p11load", corr);
        CHECK(r.has_value() && r->num(0) == -1);
        CHECK(r.has_value() && r->args.size() >= 2 && r->str(1).rfind("error:", 0) == 0);
        CHECK(count() == 2); // untouched
    }
    {
        auto r = load(good, "pd-p11load", "lr3");
        CHECK(r.has_value() && r->num(0) == 3);
        auto const json = report(r);
        CHECK(static_cast<bool>(json["canvasFound"]));
        CHECK(static_cast<int>(json["fileObjects"]) == 3);
        CHECK(static_cast<int>(json["created"]) == 3);
        auto const* failed = json["failedCreates"].getArray();
        CHECK(failed && failed->size() == 1);
        if (failed && failed->size() == 1) {
            CHECK(static_cast<int>((*failed)[0]["index"]) == 1);
            CHECK((*failed)[0]["text"].toString() == "p11_no_such_class_xyz 1 2");
        }
        CHECK(json["sidecar"].hasProperty("applied") && json["sidecar"].hasProperty("skipped"));
        CHECK(count() == 3);
    }
    {
        auto r = load(empty, "pd-p11load", "lr4");
        CHECK(r.has_value() && r->num(0) == 0);
        auto const json = report(r);
        CHECK(static_cast<int>(json["fileObjects"]) == 0 && static_cast<int>(json["created"]) == 0);
        CHECK(json["failedCreates"].isArray() && json["failedCreates"].getArray()->isEmpty());
        CHECK(count() == 0);
    }
    {
        auto r = load(good, "pd-p11_no_such_canvas", "lr5");
        CHECK(r.has_value() && r->num(0) == -1);
        CHECK(!static_cast<bool>(report(r)["canvasFound"]));
        auto m = load(dir.getChildFile("missing.pd"), "pd-p11load", "lr6");
        CHECK(m.has_value() && m->num(0) == -1);
        CHECK(m.has_value() && m->args.size() >= 2 && m->str(1).rfind("error:", 0) == 0);
    }
    {
        // L8: a red box inside clone instances is named (the clone itself is created).
        juce::File(juce::String::fromUTF8(canvas_getdir(sub)->s_name)).getChildFile("p11redvoice.pd")
            .replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 p11_missing_in_clone;\n");
        auto f = dir.getChildFile("clone.pd");
        f.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 clone p11redvoice 2;\n");
        auto const json = report(load(f, "pd-p11load", "lr30"));
        auto const* failed = json["failedCreates"].getArray();
        CHECK(failed && failed->size() == 2);
        juce::File(juce::String::fromUTF8(canvas_getdir(sub)->s_name)).getChildFile("p11redvoice.pd").deleteFile();
    }

    // Review H1: refused BEFORE the clear (the canvas keeps its objects): a header followed by garbage, a file cut off
    // inside a subpatch, a stray restore at depth 0, atoms after the last ';'. A patch with `#N struct` templates
    // before its canvas loads (L6 review: it was refused). Wires the file lists but the canvas does not get are named
    // (missingWires) and the reply is not ok; so are array contents (#A) the paste does not restore. L8: an empty box
    // is not a failed create. L6: a symbol that is not UTF-8 keeps its bytes.
    load(good, "pd-p11load", "lr20");
    CHECK(count() == 3);
    auto bad = [&](char const* name, char const* text, char const* corr) {
        auto f = dir.getChildFile(name);
        f.replaceWithText(text);
        auto r = load(f, "pd-p11load", corr);
        bool const refused = r.has_value() && r->num(0) == -1 && r->args.size() >= 2 && r->str(1).rfind("error:", 0) == 0;
        if (!refused) std::printf("  %s was not refused\n", name);
        CHECK(refused);
        CHECK(count() == 3);
    };
    bad("hdrgarbage.pd", "#N canvas 0 50 450 300 12;\nhello world;\nfoo 1 2;\n", "lr21");
    bad("truncated.pd", "#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n#N canvas 0 0 100 100 inner 0;\n#X obj 1 1 f;\n", "lr22");
    bad("stray.pd", "#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n#X restore 10 10 pd x;\n", "lr23");
    bad("trailing.pd", "#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n#X obj 10 40 t b", "lr24");
    {
        auto f = dir.getChildFile("struct.pd");
        f.replaceWithText("#N struct p11tpl float x float y;\n#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n");
        auto r = load(f, "pd-p11load", "lr25");
        CHECK(r.has_value() && r->num(0) == 1);
        CHECK(static_cast<bool>(report(r)["ok"]));
        CHECK(count() == 1);
    }
    {
        auto f = dir.getChildFile("wires.pd");
        f.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 f;\n#X obj 10 40 f;\n#X connect 0 0 1 0;\n#X connect 0 0 1 5;\n");
        auto r = load(f, "pd-p11load", "lr26");
        auto const json = report(r);
        CHECK(r.has_value() && r->num(0) == 2);
        CHECK(static_cast<int>(json["fileWires"]) == 2 && static_cast<int>(json["wires"]) == 1 && static_cast<int>(json["missingWires"]) == 1);
        CHECK(!static_cast<bool>(json["ok"]));
        CHECK(r.has_value() && r->args.size() >= 2 && r->str(1).rfind("error:", 0) == 0);
    }
    {
        auto f = dir.getChildFile("emptybox.pd");
        f.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10;\n#X obj 10 40 f;\n");
        auto const json = report(load(f, "pd-p11load", "lr27"));
        CHECK(json["failedCreates"].isArray() && json["failedCreates"].getArray()->isEmpty());
        CHECK(static_cast<int>(json["created"]) == 2 && static_cast<bool>(json["ok"]));
    }
    {
        auto f = dir.getChildFile("array.pd");
        f.replaceWithText("#N canvas 0 50 450 300 12;\n#N canvas 0 0 450 300 (subpatch) 0;\n#X array p11arrA 3 float 3;\n"
                          "#A 0 0.5 0.25 0.125;\n#X coords 0 1 3 -1 200 140 1 0 0;\n#X restore 20 20 graph;\n");
        auto r = load(f, "pd-p11load", "lr28");
        auto const json = report(r);
        CHECK(static_cast<int>(json["arrayLines"]) == 1 && !static_cast<bool>(json["ok"]));
        CHECK(json["arraysNote"].toString().contains("not restored"));
    }
    {
        auto f = dir.getChildFile("latin1.pd");
        static char const latin1[] = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r p11\xe9x;\n";
        f.replaceWithData(latin1, sizeof(latin1) - 1);
        load(f, "pd-p11load", "lr29");
        std::string text;
        host->pdWait([&] {
            if (sub->gl_list) {
                char* buf = nullptr;
                int len = 0;
                binbuf_gettext(reinterpret_cast<t_text*>(sub->gl_list)->te_binbuf, &buf, &len);
                if (buf) { text.assign(buf, static_cast<size_t>(len)); freebytes(buf, len); }
            }
        }, 3000);
        CHECK(text == "r p11\xe9x");
    }

    // V11: save_content writes the bytes save_patch writes (one canvas, two files) and answers their hash (arg1, the
    // sidecar's pdHash). V12: save_patch answers arg1 "" (or "ids not saved: ...") and arg2 {rebound, idsSaved}; with
    // no editor showing the canvas nothing is re-pointed (rebound null).
    load(good, "pd-p11load", "lr7");
    auto viaContent = dir.getChildFile("content.pd"), viaPatch = dir.getChildFile("patch.pd");
    auto sc = h.call("/pd/save_content", { "pd-p11load", viaContent.getFullPathName(), "lr8" }, "/pd/save_content/reply/lr8");
    auto sp = h.call("/pd/save_patch", { "pd-p11load", viaPatch.getFullPathName(), "lr9" }, "/pd/save_patch/reply/lr9");
    CHECK(sc.has_value() && sp.has_value());
    CHECK(viaContent.loadFileAsString() == viaPatch.loadFileAsString());
    CHECK(sc.has_value() && sc->args.size() >= 2 && sc->str(1) == juce::SHA256(viaContent).toHexString().toStdString());
    CHECK(sp.has_value() && sp->args.size() >= 3 && sp->str(1).empty());
    if (sp && sp->args.size() >= 3) {
        auto const json = juce::JSON::parse(juce::String(sp->str(2)));
        CHECK(json.hasProperty("rebound") && json["rebound"].isVoid());
        CHECK(static_cast<bool>(json["idsSaved"]));
    }

    // V15a: the census marks containers (subpatch, abstraction) with isCanvas and counts what they hold.
    auto census = h.call("/pd/census", { "main", "lr10" }, "/pd/census/reply/lr10");
    bool sawSub = false, plainFalse = true;
    if (census) {
        auto const json = parseJson(*census);
        for (auto const& o : *json["objects"].getArray()) {
            bool const isCanvas = static_cast<bool>(o["isCanvas"]);
            if (o["type"].toString() == "pd" && o["args"][0].toString() == "p11load") {
                sawSub = isCanvas && static_cast<int>(o["childCount"]) == 3;
            } else if (o["kind"].toString() != "canvas") {
                plainFalse = plainFalse && o.hasProperty("isCanvas") && !isCanvas;
            }
        }
    }
    CHECK(sawSub);
    CHECK(plainFalse);

    // P11.6 F4: the identity snapshot carries the same container fields (the TS mirror is replaced from it; without
    // them an abstraction weighed 1 in the delete budget). The census above adopted the subpatch, so it has an id.
    auto snap = h.call("/pd/identity_snapshot", { "main", "lr10s" }, "/pd/identity_snapshot/reply/lr10s");
    bool snapSub = false, snapPlainFalse = true;
    if (snap) {
        auto const json = parseJson(*snap);
        for (auto const& e : *json["entries"].getArray()) {
            if (e["class"].toString() == "canvas" && e["text"].toString() == "pd p11load")
                snapSub = static_cast<bool>(e["isContainer"]) && static_cast<int>(e["childCount"]) == 3;
            else if (e["class"].toString() != "canvas")
                snapPlainFalse = snapPlainFalse && e.hasProperty("isContainer") && !static_cast<bool>(e["isContainer"]);
        }
    }
    CHECK(snapSub);
    CHECK(snapPlainFalse);

    host->pdWait([&] { if (auto* cnv = host->getCanvasBySymbol("main")) glist_delete(cnv, &sub->gl_gobj); }, 3000);
    ::chmod(locked.getFullPathName().toRawUTF8(), 0644);
    dir.deleteRecursively();
}

// P11.4 V19: /pd/bound <corrId> <name...> asks Pd who listens on each name, over every open root canvas, into
// subpatches, abstractions and clone instances. Per name: {name, bound, count, receivers:[{canvasKey, path, index,
// tempId?, class}], other:[{kind}]}; `count` holds objects on a canvas only; a canvas's own pd-<name> binding, the
// bridge's control probes and other non-canvas binders are `other`. A name nothing ever used is unbound and is not
// added to Pd's symbol table. Three [r] on one name exercise Pd's bindlist (its layout is read by the bridge: a Pd
// update that changes it fails here).
std::optional<juce::var> boundEntry(std::optional<Reply> const& r, std::string const& name)
{
    if (!r) return std::nullopt;
    auto const json = parseJson(*r);
    if (auto const* names = json["names"].getArray())
        for (auto const& n : *names)
            if (n["name"].toString().toStdString() == name)
                return n;
    return std::nullopt;
}

// P11.6 (found live 2026-11-02): a root canvas given GOP with `coords 0 0 264 488 264 488 2 25 25` (sent to
// pd-<file>.pd) showed the frame live, but /pd/save_patch wrote the file without its `#X coords` line, so Plugin mode
// and the GOP panel were lost on reopen. The saved file must carry the coords line with the GOP fields.
void runSaveCoordsCases(Harness& h, PluginProcessor& processor)
{
    std::printf("save_patch keeps the root #X coords (P11.6)\n");
    auto* host = processor.getMcp();
    juce::String rootName;
    host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        if (!cnv || !cnv->gl_name) return;
        rootName = juce::String::fromUTF8(cnv->gl_name->s_name);
        // the message the coordinator sent: to the canvas's own receiver pd-<file>.pd
        t_atom a[9];
        float const v[9] = { 0, 0, 264, 488, 264, 488, 2, 25, 25 };
        for (int i = 0; i < 9; ++i) SETFLOAT(&a[i], v[i]);
        auto* target = gensym(("pd-" + rootName).toRawUTF8())->s_thing;
        if (target) pd_typedmess(target, gensym("coords"), 9, a);
        else pd_typedmess(&cnv->gl_pd, gensym("coords"), 9, a);
    }, 3000);
    CHECK(rootName.isNotEmpty());
    bool isGraph = false;
    host->pdWait([&] { if (auto* cnv = host->getCanvasBySymbol("main")) isGraph = cnv->gl_isgraph; }, 3000);
    CHECK(isGraph);
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mcp_p116_coords");
    dir.createDirectory();
    auto dest = dir.getChildFile("coords.pd");
    auto r = h.call("/pd/save_patch", { "main", dest.getFullPathName(), "sc1" }, "/pd/save_patch/reply/sc1", 3000);
    CHECK(r.has_value());
    if (r && r->args.size() >= 3) {
        auto const report = juce::JSON::parse(juce::String(r->str(2)));
        CHECK(report["canvasKey"].toString() == rootName);
        CHECK(static_cast<bool>(report["isGraph"]) && static_cast<bool>(report["coordsLine"]));
    } else {
        CHECK(false);
    }
    // the content write is synchronous in the handler; give the file a moment anyway
    for (int i = 0; i < 20 && !dest.existsAsFile(); ++i) juce::Thread::sleep(50);
    auto const text = dest.loadFileAsString();
    bool const hasCoords = text.contains("#X coords 0 0 264 488 264 488 2 25 25");
    if (!hasCoords) std::printf("  saved file:\n%s\n", text.toRawUTF8());
    CHECK(hasCoords);
    // the same bytes through save_content
    auto viaContent = dir.getChildFile("coords_content.pd");
    h.call("/pd/save_content", { "main", viaContent.getFullPathName(), "sc2" }, "/pd/save_content/reply/sc2", 3000);
    CHECK(viaContent.loadFileAsString().contains("#X coords 0 0 264 488 264 488 2 25 25"));
    // back to a plain canvas for the cases after this one
    host->pdWait([&] {
        if (auto* cnv = host->getCanvasBySymbol("main")) {
            t_atom a[7];
            float const v[7] = { 0, 0, 1, 1, 0, 0, 0 };
            for (int i = 0; i < 7; ++i) SETFLOAT(&a[i], v[i]);
            pd_typedmess(&cnv->gl_pd, gensym("coords"), 7, a);
        }
    }, 3000);
    // Review L1: coordsLine is the ROOT's line: a plain root holding a GOP subpatch reports isGraph false, coordsLine
    // false (the subpatch's own #X coords does not count).
    t_gobj* gopSub = nullptr;
    host->pdWait([&] {
        if (auto* cnv = host->getCanvasBySymbol("main")) {
            t_atom o[4];
            SETFLOAT(&o[0], 600); SETFLOAT(&o[1], 600); SETSYMBOL(&o[2], gensym("pd")); SETSYMBOL(&o[3], gensym("p116gop"));
            pd_typedmess(&cnv->gl_pd, gensym("obj"), 4, o);
            gopSub = pd::Interface::getNewest(cnv);
            t_atom a[9];
            float const v[9] = { 0, 0, 100, 100, 100, 100, 1, 0, 0 };
            for (int i = 0; i < 9; ++i) SETFLOAT(&a[i], v[i]);
            pd_typedmess(&reinterpret_cast<t_canvas*>(gopSub)->gl_pd, gensym("coords"), 9, a);
        }
    }, 3000);
    auto plain = dir.getChildFile("plain.pd");
    auto r2 = h.call("/pd/save_patch", { "main", plain.getFullPathName(), "sc3" }, "/pd/save_patch/reply/sc3", 3000);
    if (r2 && r2->args.size() >= 3) {
        auto const report = juce::JSON::parse(juce::String(r2->str(2)));
        CHECK(!static_cast<bool>(report["isGraph"]));
        CHECK(!static_cast<bool>(report["coordsLine"]));
    } else {
        CHECK(false);
    }
    host->pdWait([&] { if (auto* cnv = host->getCanvasBySymbol("main")) if (gopSub) glist_delete(cnv, gopSub); }, 3000);
    dir.deleteRecursively();
}

// P11.6 review M1: a canvas named by the full path of its file resolves to exactly that open root, also when two
// roots share the file name (a/twin.pd, b/twin.pd); a path no open root has resolves to nothing (never the first tab of
// that name).
void runPathCanvasCases(PluginProcessor& processor)
{
    std::printf("canvas by full file path (P11.6 M1)\n");
    auto* host = processor.getMcp();
    auto base = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mcp_p116_twins");
    base.deleteRecursively();
    auto fa = base.getChildFile("a").getChildFile("p116twin.pd");
    auto fb = base.getChildFile("b").getChildFile("p116twin.pd");
    fa.getParentDirectory().createDirectory();
    fb.getParentDirectory().createDirectory();
    fa.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 r p116_a;\n");
    fb.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 r p116_b;\n");
    auto open = [&](juce::File const& f) {
        host->pdWait([&] {
            t_atom a[2];
            SETSYMBOL(&a[0], gensym(f.getFileName().toRawUTF8()));
            SETSYMBOL(&a[1], gensym(f.getParentDirectory().getFullPathName().toRawUTF8()));
            pd_typedmess(gensym("pd")->s_thing, gensym("open"), 2, a);
        }, 3000);
    };
    open(fa);
    open(fb);
    t_canvas* ca = nullptr;
    t_canvas* cb = nullptr;
    t_canvas* none = nullptr;
    t_canvas* byName = nullptr;
    host->pdWait([&] {
        ca = host->getCanvasBySymbol(fa.getFullPathName());
        cb = host->getCanvasBySymbol(fb.getFullPathName());
        none = host->getCanvasBySymbol(base.getChildFile("c").getChildFile("p116twin.pd").getFullPathName());
        byName = host->getCanvasBySymbol("p116twin.pd");
    }, 3000);
    CHECK(ca != nullptr && cb != nullptr && ca != cb);
    CHECK(none == nullptr);
    bool aHoldsA = false, bHoldsB = false;
    host->pdWait([&] {
        auto firstText = [](t_canvas* c) {
            std::string t;
            if (c && c->gl_list) {
                char* buf = nullptr; int len = 0;
                binbuf_gettext(reinterpret_cast<t_text*>(c->gl_list)->te_binbuf, &buf, &len);
                if (buf) { t.assign(buf, static_cast<size_t>(len)); freebytes(buf, len); }
            }
            return t;
        };
        aHoldsA = firstText(ca) == "r p116_a";
        bHoldsB = firstText(cb) == "r p116_b";
    }, 3000);
    CHECK(aHoldsA && bHoldsB);
    CHECK(byName != nullptr); // the bare name still answers (its first tab): TS never sends it for a path
    host->pdWait([&] {
        for (auto* c : { ca, cb }) {
            if (!c) continue;
            t_atom force;
            SETFLOAT(&force, 1);
            pd_typedmess(&c->gl_pd, gensym("menuclose"), 1, &force); // Pd's own close (plugdata hears it)
        }
    }, 3000);
    base.deleteRecursively();
}

// P11.8 item 1 (lost work, live check 2026-11-03): `manage_patch_files save filename:X` wrote the FOCUSED canvas into
// X, which was another open tab's file (the GOP tab lost its coords line), and answered ok. /pd/save_patch now refuses
// another open patch's file whatever arg3 says, refuses an existing file that is not the canvas's own unless arg3
// (overwrite) is 1, saves a root into its own file with no flag and marks it clean (the live check saw a tool save
// leave the tab dirty). Item 3: a canvas dir that is not absolute has no file (juce::File asserted on ".").
void runSaveGuardCases(Harness& h, PluginProcessor& processor)
{
    std::printf("save_patch never overwrites another patch without the flag (P11.8)\n");
    CHECK(mcp::fileInPdDir(".", "x.pd") == juce::File());
    CHECK(mcp::fileInPdDir("", "x.pd") == juce::File());
    CHECK(mcp::fileInPdDir("/tmp", "") == juce::File());
    CHECK(mcp::fileInPdDir("/tmp", "x.pd") == juce::File("/tmp/x.pd"));

    auto* host = processor.getMcp();
    auto base = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mcp_p118_save");
    base.deleteRecursively();
    auto fa = base.getChildFile("a").getChildFile("p118own.pd");
    auto fb = base.getChildFile("b").getChildFile("p118other.pd");
    auto fx = base.getChildFile("x").getChildFile("p118stranger.pd");
    auto fn = base.getChildFile("n").getChildFile("p118new.pd");
    for (auto const& f : { fa, fb, fx, fn }) f.getParentDirectory().createDirectory();
    juce::String const textA = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r p118_a;\n";
    juce::String const textB = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r p118_b;\n#X coords 0 0 100 100 100 100 2 0 0;\n";
    juce::String const textX = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r p118_x;\n";
    fa.replaceWithText(textA);
    fb.replaceWithText(textB);
    fx.replaceWithText(textX);
    auto const onDiskB = fb.loadFileAsString(); // replaceWithText writes \r\n: compare against what is on disk
    auto const onDiskX = fx.loadFileAsString();
    auto open = [&](juce::File const& f) {
        host->pdWait([&] {
            t_atom a[2];
            SETSYMBOL(&a[0], gensym(f.getFileName().toRawUTF8()));
            SETSYMBOL(&a[1], gensym(f.getParentDirectory().getFullPathName().toRawUTF8()));
            pd_typedmess(gensym("pd")->s_thing, gensym("open"), 2, a);
        }, 3000);
    };
    open(fa);
    open(fb);
    t_canvas* ca = nullptr;
    t_canvas* cb = nullptr;
    host->pdWait([&] {
        ca = host->getCanvasBySymbol(fa.getFullPathName());
        cb = host->getCanvasBySymbol(fb.getFullPathName());
    }, 3000);
    CHECK(ca != nullptr && cb != nullptr && ca != cb);
    auto const A = fa.getFullPathName();
    auto save = [&](juce::String const& canvas, juce::File const& dest, int overwrite, char const* id) {
        return h.call("/pd/save_patch", { canvas, dest.getFullPathName(), id, overwrite }, std::string("/pd/save_patch/reply/") + id, 3000);
    };
    auto isError = [](std::optional<Reply> const& r, char const* words) {
        return r.has_value() && !r->args.empty() && juce::String(r->str(0)).startsWith("error:") && juce::String(r->str(0)).contains(words);
    };

    // another open patch's file: refused, with and without the flag; the file is untouched
    CHECK(isError(save(A, fb, 0, "sg1"), "another open patch"));
    CHECK(isError(save(A, fb, 1, "sg2"), "another open patch"));
    CHECK(fb.loadFileAsString() == onDiskB);
    // an existing file that is nobody's open patch: refused without the flag, written with it
    CHECK(isError(save(A, fx, 0, "sg3"), "already exists"));
    CHECK(fx.loadFileAsString() == onDiskX);
    auto withFlag = save(A, fx, 1, "sg4");
    CHECK(withFlag.has_value() && !juce::String(withFlag->str(0)).startsWith("error:"));
    CHECK(fx.loadFileAsString().contains("r p118_a"));
    // a new file: no flag needed
    auto fresh = save(A, fn, 0, "sg5");
    CHECK(fresh.has_value() && !juce::String(fresh->str(0)).startsWith("error:") && fn.existsAsFile());
    // a relative path: refused (juce::File would assert on it)
    CHECK(isError(h.call("/pd/save_patch", { A, "rel/p118.pd", "sg6" }, "/pd/save_patch/reply/sg6", 3000), "not an absolute path"));

    // its own file: no flag, saved, and the canvas is clean afterwards
    host->pdWait([&] { if (ca) canvas_dirty(ca, 1); }, 3000);
    int dirtyBefore = -1;
    host->pdWait([&] { if (ca) dirtyBefore = static_cast<int>(ca->gl_dirty); }, 3000);
    CHECK(dirtyBefore == 1);
    auto own = save(A, fa, 0, "sg7");
    CHECK(own.has_value() && !juce::String(own->str(0)).startsWith("error:"));
    if (own && own->args.size() >= 3) {
        auto const report = juce::JSON::parse(juce::String(own->str(2)));
        CHECK(static_cast<bool>(report["ownFile"]));
        CHECK(static_cast<bool>(report["dirtyCleared"]));
    } else {
        CHECK(false);
    }
    int dirtyAfter = -1;
    host->pdWait([&] { if (ca) dirtyAfter = static_cast<int>(ca->gl_dirty); }, 3000);
    CHECK(dirtyAfter == 0);

    // a subpatch of A saved into A's file would replace the whole patch with the subpatch: refused
    host->pdWait([&] {
        if (!ca) return;
        t_atom o[4];
        SETFLOAT(&o[0], 200); SETFLOAT(&o[1], 200); SETSYMBOL(&o[2], gensym("pd")); SETSYMBOL(&o[3], gensym("p118sub"));
        pd_typedmess(&ca->gl_pd, gensym("obj"), 4, o);
    }, 3000);
    auto const ownText = fa.loadFileAsString();
    CHECK(isError(save("pd-p118sub", fa, 1, "sg8"), "another open patch"));
    CHECK(fa.loadFileAsString() == ownText);

    // Review M1 + the old "#A saved" strip: an abstraction's [savestate] writes its state into the parent's file as an
    // "#A saved ..." line (g_readwrite.c savestate_list; MERDA's MIDI-learn uses it). A tool save dropped that line, so
    // the state was lost on reopen, and the dirty-clear then marked the tab clean although the file lacked it. The
    // state is written now (save_patch and save_content alike), and the tab is clean because the file holds everything.
    auto fs = base.getChildFile("s").getChildFile("p118state.pd");
    fs.getParentDirectory().createDirectory();
    fs.getSiblingFile("p118st.pd").replaceWithText(
        "#N canvas 0 50 450 300 12;\n#X obj 10 10 savestate;\n#X msg 10 60 42 43;\n#X connect 0 1 1 0;\n#X connect 1 0 0 0;\n");
    fs.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 p118st;\n");
    open(fs);
    t_canvas* cs = nullptr;
    host->pdWait([&] { cs = host->getCanvasBySymbol(fs.getFullPathName()); if (cs) canvas_dirty(cs, 1); }, 3000);
    CHECK(cs != nullptr);
    auto const S = fs.getFullPathName();
    auto stateSave = save(S, fs, 0, "sg9");
    CHECK(stateSave.has_value() && !juce::String(stateSave->str(0)).startsWith("error:"));
    CHECK(fs.loadFileAsString().contains("#A saved 42 43"));
    auto viaContent = fs.getSiblingFile("p118state_content.pd");
    h.call("/pd/save_content", { S, viaContent.getFullPathName(), "sg10" }, "/pd/save_content/reply/sg10", 3000);
    CHECK(viaContent.loadFileAsString().contains("#A saved 42 43"));
    int stateDirty = -1;
    host->pdWait([&] { if (cs) stateDirty = static_cast<int>(cs->gl_dirty); }, 3000);
    CHECK(stateDirty == 0);

    // Delta review HIGH: patch P holds an instance of [p118abs]; that instance is open in a tab. Saving P into
    // p118abs.pd replaced the abstraction with its own parent (self-recursive) after one click, re-pointed P and marked
    // it clean: the instance tab's canvas has P as its root, so the tab counted as P's own. An abstraction's file is
    // never the parent's own: refused, with and without the flag, tab or no tab. A plain subpatch tab of P still is.
    auto fp = base.getChildFile("p").getChildFile("p118par.pd");
    auto fabs = fp.getSiblingFile("p118abs.pd");
    fp.getParentDirectory().createDirectory();
    fabs.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 r p118_inside;\n");
    fp.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 p118abs;\n#N canvas 0 50 450 300 p118plain 0;\n#X obj 10 10 r p118_plain;\n#X restore 10 60 pd p118plain;\n");
    auto const absText = fabs.loadFileAsString();
    open(fp);
    t_canvas* cp = nullptr;
    t_canvas* inst = nullptr;
    t_canvas* plainSub = nullptr;
    host->pdWait([&] {
        cp = host->getCanvasBySymbol(fp.getFullPathName());
        for (t_gobj* g = cp ? cp->gl_list : nullptr; g; g = g->g_next) {
            if (pd_class(&g->g_pd) != canvas_class) continue;
            auto* c = reinterpret_cast<t_canvas*>(g);
            if (canvas_isabstraction(c)) inst = c; else plainSub = c;
        }
    }, 3000);
    CHECK(cp != nullptr && inst != nullptr && plainSub != nullptr);
    bool absRefusedWithTab = false, absOwnWithTab = true, plainOwn = false, plainOther = true;
    host->pdWait([&] {
        if (!cp || !inst || !plainSub) return;
        std::vector<McpGuiHost::TabInfo> tabs {
            { cp, "p118par.pd", fp, true },
            { inst, "p118abs.pd", fabs, false },
            { plainSub, "p118plain", fp, false },
        };
        auto const toAbs = mcp::saveTargetOf(cp, fabs, tabs);
        absRefusedWithTab = toAbs.otherOwner.isNotEmpty();
        absOwnWithTab = toAbs.ownFile;
        auto const toOwn = mcp::saveTargetOf(cp, fp, tabs);
        plainOwn = toOwn.ownFile;
        plainOther = toOwn.otherOwner.isNotEmpty();
    }, 3000);
    CHECK(absRefusedWithTab);
    CHECK(!absOwnWithTab);
    CHECK(plainOwn);
    CHECK(!plainOther);
    auto const P = fp.getFullPathName();
    CHECK(isError(save(P, fabs, 1, "sg11"), "another open patch")); // no tab here (headless): the instance alone
    CHECK(fabs.loadFileAsString() == absText);

    // Final delta M1: the artist edits p118abs.pd in its own ROOT tab while P uses [p118abs] (an instance tab is open
    // too): saving that root into its own file is its own file, not "another patch's" (the abstraction check refused it).
    open(fabs);
    t_canvas* absRoot = nullptr;
    bool absRootOwn = false, absRootOther = true;
    host->pdWait([&] {
        absRoot = host->getCanvasBySymbol(fabs.getFullPathName());
        if (!absRoot || !inst || !cp) return;
        std::vector<McpGuiHost::TabInfo> tabs {
            { cp, "p118par.pd", fp, false },
            { inst, "p118abs.pd", fabs, false },
            { absRoot, "p118abs.pd", fabs, true },
        };
        auto const own = mcp::saveTargetOf(absRoot, fabs, tabs);
        absRootOwn = own.ownFile;
        absRootOther = own.otherOwner.isNotEmpty();
    }, 3000);
    CHECK(absRoot != nullptr);
    CHECK(absRootOwn);
    CHECK(!absRootOther);
    auto ownAbs = save(fabs.getFullPathName(), fabs, 0, "sg12");
    CHECK(ownAbs.has_value() && !juce::String(ownAbs->str(0)).startsWith("error:"));
    CHECK(isError(save(P, fabs, 1, "sg13"), "another open patch")); // the parent into it: still refused

    host->pdWait([&] {
        for (auto* c : { ca, cb, cs, cp, absRoot }) {
            if (!c) continue;
            t_atom force;
            SETFLOAT(&force, 1);
            pd_typedmess(&c->gl_pd, gensym("menuclose"), 1, &force);
        }
    }, 3000);
    base.deleteRecursively();
}

void runBoundCases(Harness& h, PluginProcessor& processor)
{
    std::printf("/pd/bound (P11.4 V19)\n");
    auto* host = processor.getMcp();
    CHECK(hasCap(h, "bound", "bd0"));
    t_canvas* sub = nullptr;
    std::vector<t_gobj*> made;
    CHECK(host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        if (!cnv) return;
        auto obj = [&](t_canvas* c, std::vector<char const*> words) {
            std::vector<t_atom> a(words.size() + 2);
            SETFLOAT(&a[0], 1000);
            SETFLOAT(&a[1], 700);
            for (size_t i = 0; i < words.size(); ++i) {
                char* end = nullptr;
                double const f = std::strtod(words[i], &end);
                if (end && *end == 0 && end != words[i]) SETFLOAT(&a[i + 2], f);
                else SETSYMBOL(&a[i + 2], gensym(words[i]));
            }
            pd_typedmess(&c->gl_pd, gensym("obj"), static_cast<int>(a.size()), a.data());
            return pd::Interface::getNewest(c);
        };
        juce::File dir(juce::String::fromUTF8(canvas_getdir(cnv)->s_name));
        dir.getChildFile("p11voice.pd").replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 r p11cl;\n");
        made.push_back(obj(cnv, { "r", "p11k" }));
        made.push_back(obj(cnv, { "pd", "p11bsub" }));
        sub = reinterpret_cast<t_canvas*>(made.back());
        obj(sub, { "r", "p11k" });
        made.push_back(obj(cnv, { "r~", "p11sig" })); // r~ binds nothing: Pd binds the bus's s~ (and catch~, not throw~)
        made.push_back(obj(cnv, { "s~", "p11sig" }));
        made.push_back(obj(cnv, { "catch~", "p11c" }));
        made.push_back(obj(cnv, { "clone", "p11voice", "4" }));
        made.push_back(obj(cnv, { "p11voice" }));
        for (int i = 0; i < 3; ++i) made.push_back(obj(cnv, { "r", "p11three" }));
        made.push_back(obj(cnv, { "table", "p11tab" }));
        made.push_back(obj(cnv, { "delwrite~", "p11dl", "100" }));
        host->identity.bind(cnv, "p11k_main", made[0]);
    }, 3000));

    auto r = h.call("/pd/bound", { "bd1", "p11k", "p11sig", "p11c", "p11cl", "p11three", "pd-p11bsub", "p11_never_used_name" },
        "/pd/bound/reply/bd1", 3000);
    CHECK(r.has_value());
    auto k = boundEntry(r, "p11k");
    CHECK(k && static_cast<bool>((*k)["bound"]) && static_cast<int>((*k)["count"]) == 2);
    if (k) {
        bool mainId = false, inSub = false;
        for (auto const& rc : *(*k)["receivers"].getArray()) {
            mainId = mainId || rc["tempId"].toString() == "p11k_main";
            inSub = inSub || (rc["canvasKey"].toString() == "pd-p11bsub" && rc["path"].toString().contains("[pd p11bsub]"));
            CHECK(rc["class"].toString() == "receive");
        }
        CHECK(mainId && inSub);
    }
    auto sig = boundEntry(r, "p11sig");
    CHECK(sig && static_cast<int>((*sig)["count"]) == 1); // the s~: a signal bus is bound by its sender
    CHECK(sig && (*sig)["receivers"][0]["class"].toString().contains("send~"));
    auto c = boundEntry(r, "p11c");
    CHECK(c && static_cast<int>((*c)["count"]) == 1);
    auto cl = boundEntry(r, "p11cl");
    CHECK(cl && static_cast<int>((*cl)["count"]) == 5); // 4 clone instances + 1 abstraction instance
    auto three = boundEntry(r, "p11three");
    CHECK(three && static_cast<int>((*three)["count"]) == 3); // the bindlist
    auto pdSub = boundEntry(r, "pd-p11bsub");
    CHECK(pdSub && !static_cast<bool>((*pdSub)["bound"]) && static_cast<int>((*pdSub)["count"]) == 0);
    CHECK(pdSub && (*pdSub)["other"].getArray() && (*pdSub)["other"].getArray()->size() == 1
        && (*pdSub)["other"][0]["kind"].toString() == "canvas");
    // Review M3: each receiver says what kind of name it is.
    if (k) CHECK((*k)["receivers"][0]["kind"].toString() == "control");
    if (sig) CHECK((*sig)["receivers"][0]["kind"].toString() == "signal");
    auto kinds = h.call("/pd/bound", { "bd1k", "p11tab", "p11dl" }, "/pd/bound/reply/bd1k", 3000);
    auto tab = boundEntry(kinds, "p11tab");
    auto dl = boundEntry(kinds, "p11dl");
    CHECK(tab && static_cast<int>((*tab)["count"]) == 1 && (*tab)["receivers"][0]["kind"].toString() == "table");
    CHECK(dl && static_cast<int>((*dl)["count"]) == 1 && (*dl)["receivers"][0]["kind"].toString() == "delay");
    // A name nothing binds is answered without a walk.
    auto unboundOnly = h.call("/pd/bound", { "bd1u", "p11_never_used_name" }, "/pd/bound/reply/bd1u", 3000);
    CHECK(unboundOnly.has_value() && static_cast<int>(parseJson(*unboundOnly)["walkObjects"]) == 0);
    auto never = boundEntry(r, "p11_never_used_name");
    CHECK(never && !static_cast<bool>((*never)["bound"]) && static_cast<int>((*never)["count"]) == 0);

    // P11.6 F10: two canvases with ONE name (two tabs a/twin.pd and b/twin.pd; here two [pd p11twin]) answer two
    // different canvasKeys: the identity wire keys ("pd-p11twin" and "pd-p11twin#2"), each one resolving to its own
    // canvas (it was the plain name for both, so every call aimed at the second landed on the first).
    {
        t_canvas* twinA = nullptr;
        t_canvas* twinB = nullptr;
        host->pdWait([&] {
            t_canvas* cnv = host->getCanvasBySymbol("main");
            auto mk = [&](t_canvas* c, char const* a0, char const* a1) {
                t_atom a[4];
                SETFLOAT(a, 1200);
                SETFLOAT(a + 1, 700);
                SETSYMBOL(a + 2, gensym(a0));
                SETSYMBOL(a + 3, gensym(a1));
                pd_typedmess(&c->gl_pd, gensym("obj"), 4, a);
                return pd::Interface::getNewest(c);
            };
            twinA = reinterpret_cast<t_canvas*>(mk(cnv, "pd", "p11twin"));
            made.push_back(&twinA->gl_gobj);
            mk(twinA, "r", "p11tw_a");
            twinB = reinterpret_cast<t_canvas*>(mk(cnv, "pd", "p11twin"));
            made.push_back(&twinB->gl_gobj);
            mk(twinB, "r", "p11tw_b");
        }, 3000);
        auto tw = h.call("/pd/bound", { "bdtw", "p11tw_a", "p11tw_b" }, "/pd/bound/reply/bdtw", 3000);
        auto ta = boundEntry(tw, "p11tw_a");
        auto tb = boundEntry(tw, "p11tw_b");
        CHECK(ta && tb && static_cast<int>((*ta)["count"]) == 1 && static_cast<int>((*tb)["count"]) == 1);
        if (ta && tb) {
            auto const ka = (*ta)["receivers"][0]["canvasKey"].toString();
            auto const kb = (*tb)["receivers"][0]["canvasKey"].toString();
            CHECK(ka != kb);
            CHECK(ka.startsWith("pd-p11twin") && kb.startsWith("pd-p11twin"));
            t_canvas* ra = nullptr;
            t_canvas* rb = nullptr;
            host->pdWait([&] { ra = host->getCanvasBySymbol(ka); rb = host->getCanvasBySymbol(kb); }, 3000);
            CHECK(ra == twinA && rb == twinB);
        }
    }

    // An armed control probe on a name with no [r]: not bound, the probe is `other`.
    h.call("/meter/arm_control", { "p11kick", "bd2" }, "/meter/arm_control/reply/bd2");
    auto probe = boundEntry(h.call("/pd/bound", { "bd3", "p11kick" }, "/pd/bound/reply/bd3", 3000), "p11kick");
    CHECK(probe && !static_cast<bool>((*probe)["bound"]) && static_cast<int>((*probe)["count"]) == 0);
    CHECK(probe && (*probe)["other"].getArray() && !(*probe)["other"].getArray()->isEmpty() && (*probe)["other"][0]["kind"].toString() == "bridge_probe");
    h.call("/meter/read_control", { "bd4" }, "/meter/read_control/reply/bd4");

    // Cost: 2,000 objects, 64 names, under 5 ms for the walk.
    int before = 0;
    host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) ++before;
        for (int i = 0; i < 2000; ++i) {
            t_atom a[3];
            SETFLOAT(a, 10 + (i % 40) * 20);
            SETFLOAT(a + 1, 900 + (i / 40) * 20);
            SETSYMBOL(a + 2, gensym("f"));
            pd_typedmess(&cnv->gl_pd, gensym("obj"), 3, a);
        }
    }, 10000);
    std::vector<A> many { A("bd5") };
    std::vector<juce::String> names;
    for (int i = 0; i < 64; ++i) names.push_back("p11k" + juce::String(i));
    for (auto const& n : names) many.push_back(A(n));
    // Early exit: the walk stops once every binder of the asked names is found (the three [r p11three] come before
    // the 2,000 boxes).
    auto early = h.call("/pd/bound", { "bd6", "p11three" }, "/pd/bound/reply/bd6", 3000);
    CHECK(early.has_value() && static_cast<int>(parseJson(*early)["walkObjects"]) < 2000);
    // The budget (test hook: PLUGDATA_MCP_BOUND_BUDGET_OBJECTS): a walk that would pass it stops and says truncated;
    // the binders it did not reach are named as unresolved.
    ::setenv("PLUGDATA_MCP_BOUND_BUDGET_OBJECTS", "3", 1);
    auto budget = h.call("/pd/bound", { "bd7", "p11dl" }, "/pd/bound/reply/bd7", 3000);
    ::unsetenv("PLUGDATA_MCP_BOUND_BUDGET_OBJECTS");
    CHECK(budget.has_value() && static_cast<bool>(parseJson(*budget)["truncated"]));
    auto const cut = boundEntry(budget, "p11dl");
    CHECK(cut && (*cut)["other"].getArray() && !(*cut)["other"].getArray()->isEmpty() && (*cut)["other"][0]["kind"].toString() == "unresolved");
    auto timed = h.callList("/pd/bound", many, "/pd/bound/reply/bd5", 5000);
    CHECK(timed.has_value());
    if (timed) {
        double const us = static_cast<double>(parseJson(*timed)["walkUs"]);
        std::printf("  /pd/bound walk over %d objects, 64 names: %.0f us\n", before + 2000, us);
        CHECK(us >= 0 && us < 5000);
    }
    host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        std::vector<t_gobj*> extra;
        int idx = 0;
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next, ++idx)
            if (idx >= before) extra.push_back(g);
        for (auto* g : extra) glist_delete(cnv, g);
        for (auto* g : made) glist_delete(cnv, g);
        juce::File(juce::String::fromUTF8(canvas_getdir(cnv)->s_name)).getChildFile("p11voice.pd").deleteFile();
    }, 10000);
}

// P11.5 V23: contentVersion. The read-time stamp (McpHost::registerOpenCanvases) also hashes the wires, each object's
// x/y and its text atoms, folded up into the parent canvases. /pd/identity_version answers it as arg 2 (arg 1 is the
// canvas key, "" for the global counter) and the census header carries it. Edits made behind the bridge's back (as a
// hand edit is) bump it; a wire, a move or a text edit leaves identity_version alone.
void runContentVersionCases(Harness& h, PluginProcessor& processor)
{
    std::printf("contentVersion (P11.5 V23)\n");
    auto* host = processor.getMcp();
    auto versions = [&](char const* corr) -> std::pair<double, double> {
        auto r = h.call("/pd/identity_version", { corr, "main" }, std::string("/pd/identity_version/reply/") + corr, 2000);
        if (!r || r->args.size() < 3) return { -1, -1 };
        return { r->num(0), r->num(2) };
    };
    t_gobj *a = nullptr, *b = nullptr, *msg = nullptr;
    host->pdWait([&] {
        t_canvas* cnv = host->getCanvasBySymbol("main");
        auto obj = [&](char const* sel, char const* cls, int y) {
            t_atom at[3];
            SETFLOAT(at, 1200);
            SETFLOAT(at + 1, y);
            SETSYMBOL(at + 2, gensym(cls));
            pd_typedmess(&cnv->gl_pd, gensym(sel), 3, at);
            return pd::Interface::getNewest(cnv);
        };
        a = obj("obj", "f", 40);
        b = obj("obj", "f", 80);
        msg = obj("msg", "hello", 120);
    }, 3000);
    auto const v0 = versions("cv0");
    CHECK(v0.second >= 0);
    auto edit = [&](char const* what, std::function<void(t_canvas*)> const& fn, bool identityStays, char const* corr) {
        auto const before = versions((std::string(corr) + "a").c_str());
        host->pdWait([&] { fn(host->getCanvasBySymbol("main")); }, 3000);
        auto const after = versions((std::string(corr) + "b").c_str());
        if (after.second == before.second) std::printf("  %s: contentVersion did not change\n", what);
        CHECK(after.second >= 0 && after.second != before.second);
        if (identityStays) CHECK(after.first == before.first);
    };
    edit("hand wire", [&](t_canvas* c) { obj_connect(pd_checkobject(&a->g_pd), 0, pd_checkobject(&b->g_pd), 0); (void)c; }, true, "cv1");
    edit("move", [&](t_canvas*) { reinterpret_cast<t_text*>(b)->te_xpix += 15; }, true, "cv2");
    edit("text edit", [&](t_canvas*) { binbuf_text(reinterpret_cast<t_text*>(msg)->te_binbuf, "world", 5); }, true, "cv3");
    edit("paste", [&](t_canvas* c) {
        t_atom at[4];
        SETFLOAT(at, 1200);
        SETFLOAT(at + 1, 160);
        SETSYMBOL(at + 2, gensym("r"));
        SETSYMBOL(at + 3, gensym("kick_t3"));
        pd_typedmess(&c->gl_pd, gensym("obj"), 4, at);
    }, false, "cv4");
    edit("delete", [&](t_canvas* c) { glist_delete(c, pd::Interface::getNewest(c)); }, false, "cv5");
    // Review M2: a hand change of an iemgui's receive name, a disconnect, a retype, an undo and a redo bump it too.
    t_gobj* slider = nullptr;
    host->pdWait([&] {
        t_canvas* c = host->getCanvasBySymbol("main");
        t_atom at[3];
        SETFLOAT(at, 1300);
        SETFLOAT(at + 1, 40);
        SETSYMBOL(at + 2, gensym("hsl"));
        pd_typedmess(&c->gl_pd, gensym("obj"), 3, at);
        slider = pd::Interface::getNewest(c);
        host->identity.bind(c, "cvB", b);
    }, 3000);
    edit("iemgui receive", [&](t_canvas*) {
        t_atom r;
        SETSYMBOL(&r, gensym("p11cv_rcv"));
        pd_typedmess(&slider->g_pd, gensym("receive"), 1, &r);
    }, true, "cv10");
    edit("disconnect", [&](t_canvas*) { obj_disconnect(pd_checkobject(&a->g_pd), 0, pd_checkobject(&b->g_pd), 0); }, true, "cv11");
    {
        auto const before = versions("cv12a");
        auto mv = h.call("/pd/move_batch_id", { "main", "cv12m", "cvB", 1260, 95 }, "/pd/mcp_move_batch_id/reply/cv12m");
        CHECK(mv.has_value());
        sleepMs(300);
        auto const moved = versions("cv12b");
        h.call("/pd/undo", { "main", "cv12u" }, "/pd/undo/reply/cv12u");
        sleepMs(600);
        auto const undone = versions("cv12c");
        h.call("/pd/redo", { "main", "cv12r" }, "/pd/redo/reply/cv12r");
        sleepMs(600);
        auto const redone = versions("cv12d");
        CHECK(moved.second != before.second);
        CHECK(undone.second != moved.second);
        CHECK(redone.second != undone.second);
    }
    edit("retype", [&](t_canvas* c) {
        char text[] = "f 7";
        text_setto(reinterpret_cast<t_text*>(b), c, text, 3);
        b = nullptr;
        for (t_gobj* g = c->gl_list; g; g = g->g_next) // the retyped box (text_setto recreates it)
            if (auto* t = pd_checkobject(&g->g_pd); t && t->te_xpix >= 1200 && t->te_binbuf && binbuf_getnatom(t->te_binbuf) == 2)
                b = g;
    }, false, "cv13");
    // A canvas closed and opened again never gets a version it had before.
    {
        auto subVersion = [&](char const* corr) {
            auto r = h.call("/pd/identity_version", { corr, "pd-p11cvsub" }, std::string("/pd/identity_version/reply/") + corr, 2000);
            return r && r->args.size() >= 3 ? r->num(2) : -1.0;
        };
        auto makeSub = [&] {
            host->pdWait([&] {
                t_canvas* c = host->getCanvasBySymbol("main");
                t_atom at[4];
                SETFLOAT(at, 1400);
                SETFLOAT(at + 1, 40);
                SETSYMBOL(at + 2, gensym("pd"));
                SETSYMBOL(at + 3, gensym("p11cvsub"));
                pd_typedmess(&c->gl_pd, gensym("obj"), 4, at);
            }, 3000);
        };
        auto dropSub = [&] {
            host->pdWait([&] { t_canvas* c = host->getCanvasBySymbol("main"); glist_delete(c, pd::Interface::getNewest(c)); }, 3000);
        };
        makeSub();
        double const first = subVersion("cv14");
        dropSub();
        makeSub();
        double const second = subVersion("cv15");
        dropSub();
        std::printf("  reopened canvas: version %.0f then %.0f\n", first, second);
        CHECK(first >= 0 && second >= 0 && second != first);
    }
    // No edit: no change.
    auto const s1 = versions("cv6");
    auto const s2 = versions("cv7");
    CHECK(s1.second == s2.second);
    // The census header carries the same number.
    auto census = h.call("/pd/census", { "main", "cv8" }, "/pd/census/reply/cv8");
    CHECK(census.has_value() && parseJson(*census).hasProperty("contentVersion"));
    if (census) CHECK(static_cast<double>(parseJson(*census)["contentVersion"]) == s2.second);
    host->pdWait([&] {
        t_canvas* c = host->getCanvasBySymbol("main");
        for (auto* g : { a, b, msg, slider }) if (g) glist_delete(c, g);
    }, 3000);
}

// P11.5 V26: /pd/selection_get <corrId> [canvas] answers the live editor selection {canvasKey, ids, classes, count}
// (empty = nothing selected). Headless there is no editor, so nothing is selected; an unknown canvas is an error.
void runSelectionGetCases(Harness& h)
{
    std::printf("/pd/selection_get (P11.5 V26)\n");
    CHECK(hasCap(h, "selection_get", "sg0"));
    auto any = h.call("/pd/selection_get", { "sg1" }, "/pd/selection_get/reply/sg1", 2000);
    CHECK(any.has_value());
    if (any) {
        auto const json = parseJson(*any);
        CHECK(static_cast<int>(json["count"]) == 0 && json["ids"].isArray() && json["ids"].getArray()->isEmpty());
        CHECK(json["classes"].isArray());
    }
    auto onMain = h.call("/pd/selection_get", { "sg2", "main" }, "/pd/selection_get/reply/sg2", 2000);
    CHECK(onMain.has_value() && parseJson(*onMain)["canvasKey"].toString().isNotEmpty());
    CHECK(onMain.has_value() && static_cast<int>(parseJson(*onMain)["count"]) == 0);
    auto bad = h.call("/pd/selection_get", { "sg3", "pd-p11_no_such_canvas" }, "/pd/selection_get/error/sg3", 2000);
    CHECK(bad.has_value());
}

// P5.1 sink floor S1: /pd/limiter answers the LIVE limiter state and the toolbar threshold
// ({"enabled":bool,"thresholdDb":-12|-6|0|3}), read-only; the capability `limiter` is advertised. The state is set
// here directly (the AI never switches it) and restored after.
// P12c: the canvas approval chips are the bridge's own state, set only by /pd/approval_show and /pd/approval_clear;
// a HUD chapter saying "APPROVE?" shows none (H5); the artist's answer is the event /pd/approval/answer <nonce> yes|no.
void runApprovalCases(Harness& h, PluginProcessor& processor)
{
    std::printf("approval chips (P12c)\n");
    auto& state = processor.getMcp()->session.approvals;
    auto const now = [] { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); };
    for (auto const& n : state.escTargets(now())) state.clear(n);

    // H5: the HUD cannot draw approval chips any more.
    auto hud = h.call("/pd/ai_hud", { "main", "ap0", R"({"active":true,"chapter":"APPROVE?","receipt":"delete everything"})" }, "/pd/ai_hud/reply/ap0");
    CHECK(hud.has_value());
    CHECK(!state.any(now()));
    h.call("/pd/ai_hud", { "main", "ap0b", R"({"active":false})" }, "/pd/ai_hud/reply/ap0b");

    auto a = h.call("/pd/approval_show", { "ap1", R"({"nonce":"n_a","canvas":"pd-main","reason":"load a.pd","targets":["x","y"],"ttlMs":60000})" }, "/pd/approval_show/reply/ap1");
    CHECK(a && a->num(0) == 1.0);
    auto b = h.call("/pd/approval_show", { "ap2", R"({"nonce":"n_b","canvas":"pd-main","reason":"load b.pd","targets":[],"ttlMs":60000})" }, "/pd/approval_show/reply/ap2");
    CHECK(b && b->num(0) == 1.0);
    auto v = state.visible(now());
    CHECK(v.size() == 2 && v[0].nonce == "n_a" && v[1].nonce == "n_b");
    CHECK(v.size() == 2 && v[0].targets.size() == 2 && v[0].reason == "load a.pd");

    auto bad = h.call("/pd/approval_show", { "ap3", R"({"canvas":"pd-main","reason":"no nonce"})" }, "/pd/approval_show/reply/ap3");
    CHECK(bad && bad->num(0) == 0.0);
    auto junk = h.call("/pd/approval_show", { "ap4", "not json" }, "/pd/approval_show/reply/ap4");
    CHECK(junk && junk->num(0) == 0.0);
    CHECK(state.visible(now()).size() == 2);

    // Enter with two pending approves nothing.
    state.markPaintedIfInside("n_a", mcp::ApprovalRect { 0, 0, 10, 10 }, mcp::ApprovalRect { 0, 0, 100, 100 }, now() - 1000);
    state.markPaintedIfInside("n_b", mcp::ApprovalRect { 0, 0, 10, 10 }, mcp::ApprovalRect { 0, 0, 100, 100 }, now() - 1000);
    CHECK(!state.enterTarget(now()).has_value());

    auto c1 = h.call("/pd/approval_clear", { "ap5", "n_a" }, "/pd/approval_clear/reply/ap5");
    CHECK(c1 && c1->num(0) == 1.0);
    auto c2 = h.call("/pd/approval_clear", { "ap6", "n_a" }, "/pd/approval_clear/reply/ap6");
    CHECK(c2 && c2->num(0) == 0.0);
    CHECK(state.visible(now()).size() == 1);
    auto e = state.enterTarget(now());
    CHECK(e.has_value() && *e == "n_b");

    // The answer the canvas sends for exactly one chip.
    h.replies.clear();
    h.bridge.sendApprovalAnswer("n_b", true);
    auto ans = h.replies.waitFor("/pd/approval/answer", 2000);
    CHECK(ans && ans->str(0) == "n_b" && ans->str(1) == "yes");
    h.bridge.sendApprovalAnswer("n_b", false);
    auto no = h.replies.waitFor("/pd/approval/answer", 2000);
    CHECK(no && no->str(1) == "no");

    // Review item 5: line 1 / line 2 as sent; the chip knows its canvas (the outline goes only there).
    state.clear("n_b");
    auto t1 = h.call("/pd/approval_show", { "ap8", R"({"nonce":"n_t","canvas":"pd-main","reason":"load 'x.pd'","title":"LOAD a file \u2014 replaces the whole canvas \u2014 canvas 'main'","detail":"file: x.pd","targets":[],"ttlMs":60000})" }, "/pd/approval_show/reply/ap8");
    CHECK(t1 && t1->num(0) == 1.0);
    auto t2 = h.call("/pd/approval_show", { "ap9", R"({"nonce":"n_u","canvas":"pd-no_such_tab","reason":"r","title":"CLEAR","detail":"","targets":["*"],"ttlMs":60000})" }, "/pd/approval_show/reply/ap9");
    CHECK(t2 && t2->num(0) == 1.0);
    auto t3 = h.call("/pd/approval_show", { "ap10", R"({"nonce":"n_h","canvas":"hardware","reason":"r","targets":[],"ttlMs":60000})" }, "/pd/approval_show/reply/ap10");
    CHECK(t3 && t3->num(0) == 1.0);
    auto const mainId = reinterpret_cast<std::uintptr_t>(processor.getMcp()->getCanvasBySymbol("pd-main"));
    for (auto const& c : state.visible(now())) {
        if (c.nonce == "n_t") {
            CHECK(c.detail == "file: x.pd");
            CHECK(c.title.find("x.pd") == std::string::npos);
            CHECK(c.canvasId != 0 && c.canvasId == mainId);
        }
        if (c.nonce == "n_u") CHECK(c.canvasId == 0);
        if (c.nonce == "n_h") {
            CHECK(c.canvasId == 0);
            // no title sent: fixed words on line 1; the canvas label and the reason on line 2 (round 3 item 4)
            CHECK(c.title == "CONFIRM a change" && c.detail == "hardware \xc2\xb7 r");
        }
    }
    for (auto const* n : { "n_t", "n_u", "n_h" }) state.clear(n);

    // The capability the server checks before it sends a chip.
    auto caps = h.call("/bridge/capabilities", { "ap7" }, "/bridge/capabilities/reply");
    bool cap = false;
    if (caps)
        for (auto const& arg : caps->args)
            if (arg.text == "approval_ui") cap = true;
    CHECK(cap);
    state.clear("n_b");
}

// P12a A1/A2: provenance and the ownership read. An object is the AI's own ONLY when the batch create made it in this
// run (or an MCP retype of such a box); every other way an object appears is the artist's. /pd/ownership answers per id
// {origin, owned, why}, read fresh. All on a scratch subpatch pd-fl_sub of main.
void runFlowProvenanceCases(Harness& h, PluginProcessor& processor)
{
    std::printf("P12a provenance + /pd/ownership\n");
    auto* host = processor.getMcp();
    int corr = 0;
    auto next = [&corr](char const* p) { return juce::String(p) + juce::String(++corr); };
    auto batch = [&](std::initializer_list<A> args, juce::String const& c) {
        auto r = h.call("/pd/batch_atomic", args, ("/pd/batch_atomic/reply/" + c).toStdString());
        return r.has_value();
    };
    // objects[0] of /pd/ownership for one id (void when no reply)
    auto own = [&](char const* canvas, juce::String const& id) -> juce::var {
        auto const c = next("fo");
        auto r = h.call("/pd/ownership", { c, canvas, id }, ("/pd/ownership/reply/" + c).toStdString(), 3000);
        if (!r) return {};
        auto const json = parseJson(*r);
        auto const* arr = json["objects"].getArray();
        return arr && arr->size() == 1 ? arr->getReference(0) : juce::var();
    };
    auto origin = [&](char const* canvas, juce::String const& id) { return own(canvas, id)["origin"].toString().toStdString(); };
    auto why = [&](char const* canvas, juce::String const& id) { return own(canvas, id)["why"].toString().toStdString(); };
    auto sub = [&]() { return host->getCanvasBySymbol("pd-fl_sub"); };
    // A hand-made box on the subpatch, given an id the way the census adopts one.
    auto handBox = [&](char const* cls) {
        juce::String id;
        host->pdWait([&] {
            t_canvas* c = sub();
            if (!c) return;
            t_atom at[3];
            SETFLOAT(at, 300);
            SETFLOAT(at + 1, 300);
            SETSYMBOL(at + 2, gensym(cls));
            pd_typedmess(&c->gl_pd, gensym("obj"), 3, at);
            id = juce::String(host->identity.mint(c, pd::Interface::getNewest(c), "hand"));
        }, 3000);
        return id;
    };

    CHECK(hasCap(h, "ownership", "fo0"));
    auto const c0 = next("fb");
    CHECK(batch({ "main", c0, 0, 0, 0, 1, 0, "fl_sub", 1500, 40, "obj", "pd", 1, "fl_sub", 0, 5, 10, 0 }, c0));
    CHECK(sub() != nullptr);

    // 1. Batch create -> ai, its own leaf; an AI wire into an AI box keeps it owned.
    auto const c1 = next("fb");
    CHECK(batch({ "pd-fl_sub", c1, 0, 0, 0, 2, 1,
        "fl_a", 40, 40, "obj", "f", 0,
        "fl_b", 40, 100, "obj", "f", 0,
        "fl_a", 0, "fl_b", 0, 0, 5, 10, 0 }, c1));
    {
        auto const a = own("pd-fl_sub", "fl_a");
        CHECK(a["origin"].toString() == "ai" && static_cast<bool>(a["owned"]) && a["why"].toString() == "ai_own");
        CHECK(a["found"] && !static_cast<bool>(a["isContainer"]) && !static_cast<bool>(a["isDataHolder"]));
        CHECK(why("pd-fl_sub", "fl_b") == "ai_own");
    }
    // The census row and the identity snapshot carry it too (observability).
    {
        auto const c = next("fc");
        auto census = h.call("/pd/census", { "pd-fl_sub", c }, ("/pd/census/reply/" + c).toStdString());
        bool seen = false;
        if (census) {
            auto const json = parseJson(*census);
            if (auto* arr = json["objects"].getArray())
                for (auto const& o : *arr)
                    if (o["id"].toString() == "fl_a") seen = o["origin"].toString() == "ai";
        }
        CHECK(seen);
        auto const cs = next("fs");
        auto snap = h.call("/pd/identity_snapshot", { "pd-fl_sub", cs }, ("/pd/identity_snapshot/reply/" + cs).toStdString());
        bool snapSeen = false;
        if (snap) {
            auto const json = parseJson(*snap);
            if (auto* arr = json["entries"].getArray())
                for (auto const& o : *arr)
                    if (o["id"].toString() == "fl_a") snapSeen = o["origin"].toString() == "ai";
        }
        CHECK(snapSeen);
    }

    // 2. Hand-made (adopted by the minter) -> unknown. 3. Pasted / created behind the bridge, adopted by a batch's
    // reconcile -> unknown.
    auto const hand = handBox("f");
    CHECK(hand.isNotEmpty() && origin("pd-fl_sub", hand) == "unknown" && why("pd-fl_sub", hand) == "origin_unknown");
    {
        host->pdWait([&] {
            t_canvas* c = sub();
            t_atom at[3];
            SETFLOAT(at, 320);
            SETFLOAT(at + 1, 340);
            SETSYMBOL(at + 2, gensym("osc~"));
            pd_typedmess(&c->gl_pd, gensym("obj"), 3, at); // untracked, as a paste looks to the identity layer
        }, 3000);
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_c", 40, 160, "obj", "f", 0, 0, 5, 10, 0 }, c)); // reconciles first
        juce::String pastedId;
        host->pdWait([&] {
            t_canvas* cv = sub();
            for (t_gobj* g = cv->gl_list; g; g = g->g_next)
                if (auto* t = pd_checkobject(&g->g_pd); t && t->te_xpix == 320 && t->te_ypix == 340)
                    pastedId = juce::String(host->identity.idOf(g));
        }, 3000);
        CHECK(pastedId.isNotEmpty() && origin("pd-fl_sub", pastedId) == "unknown");
        CHECK(origin("pd-fl_sub", "fl_c") == "ai");
    }

    // 4. A wire from an artist box into an AI box: not the AI's own any more.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_w", 40, 220, "obj", "f", 0, 0, 5, 10, 0 }, c));
        host->pdWait([&] {
            t_canvas* cv = sub();
            auto* from = cv->gl_list ? host->identity.resolve(cv, hand.toStdString()) : nullptr;
            auto* to = host->identity.resolve(cv, "fl_w");
            if (from && to) obj_connect(pd_checkobject(&from->g_pd), 0, pd_checkobject(&to->g_pd), 0);
        }, 3000);
        auto const w = own("pd-fl_sub", "fl_w");
        CHECK(w["origin"].toString() == "ai" && !static_cast<bool>(w["owned"]) && w["why"].toString() == "artist_in_wire");
    }

    // Review M1: a wire OUT of an AI box into an artist box asks too.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_o", 120, 220, "obj", "f", 0, 0, 5, 10, 0 }, c));
        CHECK(why("pd-fl_sub", "fl_o") == "ai_own");
        host->pdWait([&] {
            t_canvas* cv = sub();
            auto* from = host->identity.resolve(cv, "fl_o");
            auto* to = host->identity.resolve(cv, hand.toStdString());
            if (from && to) obj_connect(pd_checkobject(&from->g_pd), 0, pd_checkobject(&to->g_pd), 0);
        }, 3000);
        CHECK(why("pd-fl_sub", "fl_o") == "artist_out_wire");
    }
    // Review M1: an artist edit in place (message text, GUI properties) makes it the artist's; a later MCP retype does
    // not launder it back.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 2, 0,
            "fl_m", 120, 460, "msg", "", 1, "hello",
            "fl_s", 120, 520, "obj", "hsl", 0, 0, 5, 10, 0 }, c));
        CHECK(why("pd-fl_sub", "fl_m") == "ai_own");
        CHECK(why("pd-fl_sub", "fl_s") == "ai_own");
        host->pdWait([&] {
            t_canvas* cv = sub();
            if (auto* m = host->identity.resolve(cv, "fl_m")) binbuf_text(reinterpret_cast<t_text*>(m)->te_binbuf, "world", 5);
            if (auto* sl = host->identity.resolve(cv, "fl_s")) {
                t_atom r[2];
                SETFLOAT(r, 20);
                SETFLOAT(r + 1, 20000);
                pd_typedmess(&sl->g_pd, gensym("range"), 2, r); // what the properties dialog changes
            }
        }, 3000);
        auto const m = own("pd-fl_sub", "fl_m");
        CHECK(m["why"].toString() == "edited_in_place" && m["origin"].toString() == "unknown" && !static_cast<bool>(m["owned"]));
        CHECK(why("pd-fl_sub", "fl_s") == "edited_in_place");
        auto const ce = next("fb");
        CHECK(batch({ "pd-fl_sub", ce, 0, 0, 1, 0, 0, "fl_m", "msg", 1, "again", 0, 5, 10, 0 }, ce));
        CHECK(origin("pd-fl_sub", "fl_m") == "unknown");
    }

    // P12a: the AI's own changes through the bridge keep its boxes the AI's (obj_set range, obj_set_batch, obj_relabel);
    // a played value (atom box, GUI) is no edit; only the artist's path (here: the message the properties dialog sends,
    // straight into Pd) marks it edited.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 3, 0,
            "fl_s2", 260, 460, "obj", "hsl", 0,
            "fl_n", 260, 520, "floatatom", "", 0,
            "fl_m2", 260, 580, "msg", "", 1, "hi", 0, 5, 10, 0 }, c));
        CHECK(why("pd-fl_sub", "fl_s2") == "ai_own");
        auto const cs = next("fs");
        auto set = h.call("/pd/obj_set", { "pd-fl_sub", cs, "fl_s2", 0, "range", 2, 20.0, 2000.0 }, ("/pd/obj_set/reply/" + cs).toStdString());
        CHECK(set.has_value() && set->num(0) == 1);
        CHECK(why("pd-fl_sub", "fl_s2") == "ai_own"); // (1) the AI's own range change
        auto const cb = next("fs");
        auto sb = h.call("/pd/obj_set_batch", { "pd-fl_sub", cb, 1, "fl_s2", 0, "label", 1, "s:cutoff" }, ("/pd/obj_set_batch/reply/" + cb).toStdString());
        CHECK(sb.has_value());
        CHECK(why("pd-fl_sub", "fl_s2") == "ai_own");
        auto const cr = next("fs");
        auto rl = h.call("/pd/obj_relabel", { "pd-fl_sub", cr, "fl_m2", "hello there" }, ("/pd/obj_relabel/reply/" + cr).toStdString());
        CHECK(rl.has_value() && rl->num(0) == 1);
        CHECK(why("pd-fl_sub", "fl_m2") == "ai_own");
        // (3) played values: the AI number box, a slider's value, an atom box the artist types into
        juce::String gatomId;
        host->pdWait([&] {
            t_canvas* cv = sub();
            for (char const* id : { "fl_n", "fl_s2" })
                if (auto* g = host->identity.resolve(cv, id)) {
                    t_atom v;
                    SETFLOAT(&v, 7);
                    pd_typedmess(&g->g_pd, gensym("float"), 1, &v);
                }
            t_atom at[2];
            SETFLOAT(at, 400);
            SETFLOAT(at + 1, 600);
            pd_typedmess(&cv->gl_pd, gensym("floatatom"), 2, at);
            t_gobj* g = pd::Interface::getNewest(cv);
            host->identity.bind(cv, "fl_atom", g);
            host->identity.setOriginAi(g, McpHost::editStamp(g)); // as the batch would mark it
            t_atom v;
            SETFLOAT(&v, 42);
            pd_typedmess(&g->g_pd, gensym("float"), 1, &v);
        }, 3000);
        CHECK(why("pd-fl_sub", "fl_n") == "ai_own");
        CHECK(why("pd-fl_sub", "fl_s2") == "ai_own");
        CHECK(why("pd-fl_sub", "fl_atom") == "ai_own");
        // (2) the artist's range change (straight into Pd, as the properties dialog does): edited, and a later AI set
        // does not make it the AI's again
        host->pdWait([&] {
            if (auto* g = host->identity.resolve(sub(), "fl_s2")) {
                t_atom r[2];
                SETFLOAT(r, 0);
                SETFLOAT(r + 1, 1);
                pd_typedmess(&g->g_pd, gensym("range"), 2, r);
            }
        }, 3000);
        CHECK(why("pd-fl_sub", "fl_s2") == "edited_in_place");
        auto const cs2 = next("fs");
        h.call("/pd/obj_set", { "pd-fl_sub", cs2, "fl_s2", 0, "range", 2, 20.0, 2000.0 }, ("/pd/obj_set/reply/" + cs2).toStdString());
        {
            auto const back = own("pd-fl_sub", "fl_s2"); // even back at the AI's old range: the artist's for good
            CHECK(!static_cast<bool>(back["owned"]) && back["origin"].toString() == "unknown");
        }
    }

    // 5. Containers and data holders always ask, whoever made them.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 3, 0,
            "fl_pd", 200, 40, "obj", "pd", 1, "fl_inner",
            "fl_tab", 200, 100, "obj", "table", 2, "fl_tab_t", "16",
            "fl_arr", 200, 160, "obj", "array", 3, "define", "fl_arr_a", "16",
            0, 5, 10, 0 }, c));
        host->pdWait([&] {
            t_canvas* cv = sub();
            t_atom at[5];
            SETFLOAT(at, 200);
            SETFLOAT(at + 1, 220);
            SETSYMBOL(at + 2, gensym("text"));
            SETSYMBOL(at + 3, gensym("define"));
            SETSYMBOL(at + 4, gensym("fl_txt_t"));
            pd_typedmess(&cv->gl_pd, gensym("obj"), 5, at);
            t_gobj* g = pd::Interface::getNewest(cv);
            host->identity.bind(cv, "fl_txt", g);
            host->identity.setOriginAi(g, McpHost::editStamp(g)); // as the batch would mark it
        }, 3000);
        CHECK(why("pd-fl_sub", "fl_pd") == "container");
        auto const tab = why("pd-fl_sub", "fl_tab");
        CHECK(tab == "container" || tab == "data_holder");
        CHECK(why("pd-fl_sub", "fl_arr") == "data_holder");
        CHECK(why("pd-fl_sub", "fl_txt") == "data_holder");
        CHECK(origin("pd-fl_sub", "fl_arr") == "ai");
    }

    // 6. MCP retype carries the origin; of an artist box it stays unknown. 7. GUI retype (the retype hook) -> unknown.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 1, 0, 0, "fl_a", "f", 1, "9", 0, 5, 10, 0 }, c));
        CHECK(origin("pd-fl_sub", "fl_a") == "ai");
        auto const c2 = next("fb");
        CHECK(batch({ "pd-fl_sub", c2, 0, 0, 1, 0, 0, hand, "f", 1, "3", 0, 5, 10, 0 }, c2));
        CHECK(origin("pd-fl_sub", hand) == "unknown");
        auto const c3 = next("fb");
        CHECK(batch({ "pd-fl_sub", c3, 0, 0, 0, 1, 0, "fl_g", 40, 280, "obj", "f", 0, 0, 5, 10, 0 }, c3));
        CHECK(origin("pd-fl_sub", "fl_g") == "ai");
        bool moved = false;
        host->pdWait([&] {
            t_canvas* cv = sub();
            t_gobj* old = host->identity.resolve(cv, "fl_g");
            if (!old) return;
            char text[] = "i 4";
            text_setto(reinterpret_cast<t_text*>(old), cv, text, 3);
            t_gobj* now = pd::Interface::getNewest(cv);
            if (processor.onObjectRetyped) processor.onObjectRetyped(old, now); // what Patch::renameObject calls
            moved = host->identity.resolve(cv, "fl_g") == now;
        }, 3000);
        CHECK(moved);
        CHECK(origin("pd-fl_sub", "fl_g") == "unknown");
    }

    // 8. register_id of an artist box under an AI-looking id -> unknown. 9. A rename keeps it on the object; an artist
    // id renamed onto a freed AI id stays unknown.
    {
        int index = -1;
        host->pdWait([&] {
            t_canvas* cv = sub();
            t_gobj* target = host->identity.resolve(cv, hand.toStdString());
            int i = 0;
            for (t_gobj* g = cv->gl_list; g; g = g->g_next, ++i)
                if (g == target) index = i;
        }, 3000);
        auto const c = next("fr");
        auto r = h.call("/pd/register_id", { "pd-fl_sub", c, index, "fl_ai_like" }, ("/pd/mcp_register_id/reply/" + c).toStdString());
        CHECK(r.has_value() && r->num(0) == 1);
        CHECK(origin("pd-fl_sub", "fl_ai_like") == "unknown");
        auto const cr = next("fn");
        auto rn = h.call("/pd/rename_id", { "pd-fl_sub", cr, "fl_b", "fl_b2" }, ("/pd/rename_id/reply/" + cr).toStdString());
        CHECK(rn.has_value() && rn->num(0) == 1);
        CHECK(origin("pd-fl_sub", "fl_b2") == "ai");
        auto const cr2 = next("fn");
        auto rn2 = h.call("/pd/rename_id", { "pd-fl_sub", cr2, "fl_ai_like", "fl_b" }, ("/pd/rename_id/reply/" + cr2).toStdString());
        CHECK(rn2.has_value() && rn2->num(0) == 1);
        CHECK(origin("pd-fl_sub", "fl_b") == "unknown");
    }

    // 10. A red box is rolled back: nothing to own.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_red", 40, 340, "obj", "p12_no_such_class", 0, 0, 5, 10, 0 }, c));
        CHECK(why("pd-fl_sub", "fl_red") == "not_found");
    }

    // 11. Native undo of an AI delete brings the box back as the artist's.
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_u", 40, 400, "obj", "f", 0, 0, 5, 10, 0 }, c));
        CHECK(origin("pd-fl_sub", "fl_u") == "ai");
        auto const cd = next("fb");
        CHECK(batch({ "pd-fl_sub", cd, 1, 0, 0, 0, 0, "fl_u", 0, 5, 10, 0 }, cd));
        CHECK(why("pd-fl_sub", "fl_u") == "not_found");
        auto const cu = next("fu");
        auto u = h.call("/pd/undo", { "pd-fl_sub", cu }, ("/pd/undo/reply/" + cu).toStdString());
        sleepMs(300);
        CHECK(u.has_value() && u->num(0) == 1);
        auto const back = own("pd-fl_sub", "fl_u");
        std::printf("  undo of an AI delete: found %d, origin %s\n", static_cast<int>(static_cast<bool>(back["found"])), back["origin"].toString().toRawUTF8());
        CHECK(back["origin"].toString() == "unknown" && !static_cast<bool>(back["owned"]));
    }

    // 12. Address reuse: a freed AI box's address taken by a hand-made box never reads as the AI's.
    {
        int reused = 0, wrong = 0;
        host->pdWait([&] {
            t_canvas* cv = sub();
            auto newObj = [cv] {
                t_atom a[3];
                SETFLOAT(a, 600);
                SETFLOAT(a + 1, 40);
                SETSYMBOL(a + 2, gensym("osc~"));
                pd_typedmess(&cv->gl_pd, gensym("obj"), 3, a);
                return pd::Interface::getNewest(cv);
            };
            for (int i = 0; i < 300 && reused < 20; ++i) {
                t_gobj* old = newObj();
                host->identity.bind(cv, "fl_reuse" + std::to_string(i), old);
                host->identity.setOriginAi(old); // as the batch create marks it
                glist_delete(cv, old);
                t_gobj* fresh = newObj();
                if (fresh == old) ++reused;
                host->identity.mint(cv, fresh, "hand");
                if (host->identity.originOf(fresh) == mcp::IdentityService::Origin::Ai) ++wrong;
                glist_delete(cv, fresh);
            }
        }, 20000);
        std::printf("  address reuse: %d reused, %d read as the AI's\n", reused, wrong);
        CHECK(wrong == 0);
    }

    // 13. Encapsulate: the wrapped boxes are the artist's (they leave the AI's leaf set).
    {
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 2, 0,
            "fl_e1", 400, 40, "obj", "f", 0,
            "fl_e2", 400, 100, "obj", "f", 0, 0, 5, 10, 0 }, c));
        CHECK(origin("pd-fl_sub", "fl_e1") == "ai");
        auto const ce = next("fe");
        auto e = h.call("/pd/encapsulate", { "pd-fl_sub", "fl_enc", 2, "fl_e1", "fl_e2", ce }, ("/pd/encapsulate/reply/" + ce).toStdString(), 8000);
        CHECK(e.has_value());
        if (e) std::printf("  encapsulate reply %.0f %s\n", e->num(0), e->str(1).substr(0, 120).c_str());
        sleepMs(300);
        host->pdWait([&] {
            t_canvas* enc = host->getCanvasBySymbol("pd-fl_enc");
            std::printf("  pd-fl_enc %s:", enc ? "found" : "missing");
            for (t_gobj* g = enc ? enc->gl_list : nullptr; g; g = g->g_next)
                std::printf(" [%s]", host->identity.idOf(g).c_str());
            std::printf("\n");
        }, 3000);
        if (e && e->num(0) == 0) {
            // /pd/encapsulate needs an editor view (gui().ensureView); headless it refuses. The rule is code
            // (MCPBridgeCmdEncapsulate.cpp: clearOrigin on every re-attached box) and the A8 live check.
            std::printf("  encapsulate needs an editor: not run headless (live check A8)\n");
            CHECK(origin("pd-fl_sub", "fl_e1") == "ai"); // refused: nothing changed
        } else {
            auto const inner = own("pd-fl_enc", "fl_e1");
            std::printf("  encapsulated: found %d, origin %s\n", static_cast<int>(static_cast<bool>(inner["found"])), inner["origin"].toString().toRawUTF8());
            CHECK(inner["origin"].toString() == "unknown" && !static_cast<bool>(inner["owned"]));
            auto const box = own("pd-fl_sub", "fl_enc");
            CHECK(!static_cast<bool>(box["owned"]));
        }
    }

    // 14. Tombstone rebind (an abstraction reloaded after its save) -> unknown.
    {
        juce::File dir;
        host->pdWait([&] { dir = juce::File(juce::String::fromUTF8(canvas_getdir(sub())->s_name)); }, 3000);
        auto absFile = dir.getChildFile("fl_abs.pd");
        CHECK(absFile.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 inlet;\n"));
        auto const c = next("fb");
        CHECK(batch({ "pd-fl_sub", c, 0, 0, 0, 1, 0, "fl_abs1", 500, 200, "obj", "fl_abs", 0, 0, 5, 10, 0 }, c));
        CHECK(origin("pd-fl_sub", "fl_abs1") == "ai");
        bool rebound = false;
        host->pdWait([&] {
            t_gobj* inst = host->identity.resolve(sub(), "fl_abs1");
            juce::String absName;
            if (!inst || !pd::getAbstractionFileName(inst, absName)) return;
            host->tombstoneAbstractionInstances(absName);
            absFile.replaceWithText("#N canvas 0 50 450 300 12;\n#X obj 10 10 inlet;\n#X obj 10 40 outlet;\n");
            processor.reloadAbstractions(absFile, nullptr);
            host->rebindTombstones();
            t_gobj* after = host->identity.resolve(sub(), "fl_abs1");
            rebound = after && after != inst;
        }, 20000);
        std::printf("  tombstone rebind: %s\n", rebound ? "rebound" : "not rebound");
        CHECK(origin("pd-fl_sub", "fl_abs1") == "unknown");
        absFile.deleteFile();
    }

    // 15. File load with the identity sidecar: the ids come back, as the artist's.
    {
        juce::File dir;
        host->pdWait([&] { dir = juce::File(juce::String::fromUTF8(canvas_getdir(sub())->s_name)); }, 3000);
        auto file = dir.getChildFile("fl_load.pd");
        auto const cs = next("fv");
        auto sp = h.call("/pd/save_patch", { "pd-fl_sub", file.getFullPathName(), cs }, ("/pd/save_patch/reply/" + cs).toStdString(), 5000);
        CHECK(sp.has_value());
        CHECK(origin("pd-fl_sub", "fl_c") == "ai");
        auto const cl = next("fl");
        auto lp = h.call("/pd/load_patch", { "pd-fl_sub", file.getFullPathName(), cl }, ("/pd/load_patch/reply/" + cl).toStdString(), 8000);
        CHECK(lp.has_value());
        sleepMs(300);
        auto const loaded = own("pd-fl_sub", "fl_c");
        std::printf("  loaded: found %d, origin %s\n", static_cast<int>(static_cast<bool>(loaded["found"])), loaded["origin"].toString().toRawUTF8());
        CHECK(static_cast<bool>(loaded["found"]) && loaded["origin"].toString() == "unknown");
        file.deleteFile();
        juce::File(file.getFullPathName() + ".mcpids.json").deleteFile();
    }

    // P11.9 item 1 + coordinator brake: the reply says whether the canvas is on the artist's screen (headless: no
    // editor, so never) and how many objects the canvas holds (the engine's count, for the "more than half" brake).
    {
        // Review M2: comments are no objects for the brake (padding a canvas with comments must not raise the count).
        host->pdWait([&] {
            if (t_canvas* sc = sub()) {
                for (int k = 0; k < 3; ++k) {
                    t_atom at[3];
                    SETFLOAT(at, 600);
                    SETFLOAT(at + 1, 40 + 30 * k);
                    SETSYMBOL(at + 2, gensym("padding"));
                    pd_typedmess(&sc->gl_pd, gensym("text"), 3, at);
                }
            }
        }, 3000);
        auto const c = next("fo");
        auto r = h.call("/pd/ownership", { c, "pd-fl_sub", "fl_a" }, ("/pd/ownership/reply/" + c).toStdString(), 3000);
        CHECK(r.has_value());
        if (r) {
            auto const json = parseJson(*r);
            CHECK(json.hasProperty("visible") && json["visible"].isBool() && !static_cast<bool>(json["visible"]));
            int count = -1;
            host->pdWait([&] {
                if (t_canvas* sc = sub()) {
                    count = 0;
                    for (t_gobj* y = sc->gl_list; y; y = y->g_next) {
                        auto* ob = pd_checkobject(&y->g_pd);
                        if (!(ob && ob->te_type == T_TEXT)) ++count;
                    }
                }
            }, 3000);
            std::printf("  canvasObjects %d (counted %d)\n", static_cast<int>(json["canvasObjects"]), count);
            CHECK(json.hasProperty("canvasObjects") && static_cast<int>(json["canvasObjects"]) == count && count > 0);
        }
        // focus_tab with no editor: no tab, and never "focused".
        auto const cf = next("ft");
        auto f = h.call("/pd/focus_tab", { "fl_sub", cf }, ("/pd/focus_tab/reply/" + cf).toStdString(), 3000);
        CHECK(f.has_value() && f->str(0).rfind("focused", 0) != 0);
    }

    // Unknown canvas: an error, never an empty "nothing is owned".
    {
        auto const c = next("fo");
        auto bad = h.call("/pd/ownership", { c, "pd-p12_no_such_canvas", "x" }, ("/pd/ownership/error/" + c).toStdString(), 2000);
        CHECK(bad.has_value());
    }
    auto const cx = next("fb");
    h.call("/pd/batch_atomic", { "main", cx, 1, 0, 0, 0, 0, "fl_sub", 0, 5, 10, 0 }, ("/pd/batch_atomic/reply/" + cx).toStdString());
}

// P12a A3 / P12a.1: the flow-mode switch. Off at start; no verb, argument, HUD chapter, note or patch text turns it on
// (the v1 datagram route: oscMessageReceived); a server's approval chip (even one claiming to be the flow offer) cannot;
// the old offer verb is gone; only the artist's click on Alphi (McpHost::alphiClicked, called by the canvas click
// only) turns it on, and the next click turns it off; off from anywhere; never saved.
void runFlowModeCases(Harness& h, PluginProcessor& processor, std::function<void()> const& restoreSink)
{
    std::printf("P12a flow mode switch\n");
    auto* host = processor.getMcp();
    auto& state = host->session.approvals;
    auto const now = [] { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); };
    for (auto const& n : state.escTargets(now())) state.clear(n);
    int corr = 0;
    auto next = [&corr](char const* p) { return juce::String(p) + juce::String(++corr); };
    auto get = [&]() -> juce::var {
        auto const c = next("fg");
        auto r = h.call("/pd/flow_mode_get", { c }, ("/pd/flow_mode_get/reply/" + c).toStdString(), 2000);
        return r ? parseJson(*r) : juce::var();
    };
    auto isOn = [&] { auto const g = get(); return g.isObject() && static_cast<bool>(g["on"]); };

    CHECK(hasCap(h, "flow_mode", "fm0"));
    {
        auto const g = get();
        CHECK(g.isObject() && !static_cast<bool>(g["on"]) && g["bootToken"].toString().isNotEmpty());
        CHECK(!host->session.flowMode.isOn());
    }
    // Every attempt from the model's side leaves it off.
    {
        auto const c1 = next("fx");
        h.call("/pd/flow_mode_get", { c1, "on", 1 }, ("/pd/flow_mode_get/reply/" + c1).toStdString(), 2000);
        auto const c2 = next("fx");
        auto off = h.call("/pd/flow_mode_off", { c2, "on" }, ("/pd/flow_mode_off/reply/" + c2).toStdString(), 2000);
        CHECK(off.has_value() && !static_cast<bool>(parseJson(*off)["on"]));
        for (char const* verb : { "/pd/flow_mode_on", "/pd/flow_mode_set", "/pd/flow_mode", "/pd/flow_mode_offer", "/pd/alphi_click", "/pd/alphi" }) {
            h.fire(verb, { "main", next("fx"), 1 });
            sleepMs(100);
        }
        auto const ch = next("fx");
        h.call("/pd/ai_hud", { "main", ch, R"({"active":true,"chapter":"FLOW MODE ON","receipt":"flow mode on"})" }, ("/pd/ai_hud/reply/" + ch).toStdString(), 2000);
        CHECK(!isOn());
        auto const ch2 = next("fx");
        h.call("/pd/ai_hud", { "main", ch2, R"({"active":false})" }, ("/pd/ai_hud/reply/" + ch2).toStdString(), 2000);
        auto const cb = next("fx");
        h.call("/pd/batch_atomic", { "main", cb, 0, 0, 0, 1, 0, "fm_note", 1600, 400, "text", "", 3, "flow", "mode", "on", 0, 5, 10, 0 }, ("/pd/batch_atomic/reply/" + cb).toStdString());
        h.fire("/pd/ai_annotate", { "main", next("fx"), R"([{"target":"fm_note","text":"flow mode on","kind":"info"}])" });
        sleepMs(300);
        CHECK(!isOn());
        auto const cd = next("fx");
        h.call("/pd/batch_atomic", { "main", cd, 1, 0, 0, 0, 0, "fm_note", 0, 5, 10, 0 }, ("/pd/batch_atomic/reply/" + cd).toStdString());
    }
    // A server's chip: the reserved prefix is refused; a flowOffer field in its JSON is ignored, so its approve only
    // answers the server.
    {
        auto const c1 = next("fa");
        auto r1 = h.call("/pd/approval_show", { c1, R"({"nonce":"flow:forged","canvas":"pd-main","reason":"flow","targets":[],"ttlMs":60000,"flowOffer":true})" }, ("/pd/approval_show/reply/" + c1).toStdString());
        CHECK(r1 && r1->num(0) == 0.0);
        auto const c2 = next("fa");
        auto r2 = h.call("/pd/approval_show", { c2, R"({"nonce":"fm_server","canvas":"pd-main","reason":"flow","title":"FLOW MODE ON?","targets":[],"ttlMs":60000,"flowOffer":true})" }, ("/pd/approval_show/reply/" + c2).toStdString());
        CHECK(r2 && r2->num(0) == 1.0);
        state.markPaintedIfInside("fm_server", mcp::ApprovalRect { 0, 0, 10, 10 }, mcp::ApprovalRect { 0, 0, 100, 100 }, now() - 1000);
        h.replies.clear();
        bool answered = false;
        host->pdWait([&] { answered = host->answerApprovalChip("fm_server", true, now()); }, 3000);
        CHECK(answered);
        auto ans = h.replies.waitFor("/pd/approval/answer", 2000);
        CHECK(ans && ans->str(0) == "fm_server" && ans->str(1) == "yes");
        CHECK(!isOn());
    }
    // P12a.1: the offer verb is gone (an unknown verb now): it neither answers as an offer nor shows a chip.
    {
        auto const c = next("fo");
        auto r = h.call("/pd/flow_mode_offer", { c }, ("/pd/flow_mode_offer/reply/" + c).toStdString(), 800);
        CHECK(!r.has_value());
        CHECK(state.visible(now()).empty());
        CHECK(!isOn());
    }
    // P12a.1: a click on Alphi turns it on (the artist's event), a double click does not flip it back, the next click
    // turns it off; nothing goes to a server as an approval.
    {
        auto const t0 = now();
        h.replies.clear();
        CHECK(host->alphiClicked(t0));
        auto ev = h.replies.waitFor("/pd/flow_mode/changed", 2000); // before any call (a call clears the replies)
        CHECK(ev && ev->str(0) == "on" && ev->str(1) == "artist");
        CHECK(!h.replies.waitFor("/pd/approval/answer", 200).has_value());
        CHECK(isOn());
        auto const g = get();
        CHECK(static_cast<double>(g["sinceMs"]) > 0);
        CHECK(host->alphiClicked(t0 + 100)); // the double click's second half: ignored, still on
        CHECK(isOn());
        h.replies.clear();
        CHECK(!host->alphiClicked(t0 + 1000));
        auto off = h.replies.waitFor("/pd/flow_mode/changed", 2000);
        CHECK(off && off->str(0) == "off" && off->str(1) == "alphi");
        CHECK(!isOn());
        CHECK(host->alphiClicked(t0 + 2000));
        CHECK(isOn());
        // The brake (d1 live check 2026-11-05): a click on Alphi while flow is on turns it off through alphiBrake,
        // which never turns it on: off already (another door won the race) stays off, with no event. The 350 ms
        // double-click guard applies to it; the off records its time (the canvas's way ON waits 600 ms after it).
        h.replies.clear();
        auto const beforeBrake = juce::Time::currentTimeMillis();
        CHECK(!host->alphiBrake(t0 + 3000));
        auto brake = h.replies.waitFor("/pd/flow_mode/changed", 2000);
        CHECK(brake && brake->str(0) == "off" && brake->str(1) == "alphi");
        CHECK(!isOn());
        CHECK(host->session.flowMode.lastOffMs() >= beforeBrake);
        CHECK(!host->alphiClicked(t0 + 3100)); // within the 350 ms double-click guard: ignored, still off
        CHECK(!isOn());
        h.replies.clear();
        CHECK(!host->alphiBrake(t0 + 4000)); // off already: stays off, no event
        CHECK(!isOn());
        CHECK(!h.replies.waitFor("/pd/flow_mode/changed", 200).has_value());
        CHECK(host->alphiClicked(t0 + 5000)); // on again, for the cases below
        CHECK(isOn());
        // While on, no verb turns it off-and-on again or toggles it: only flow_mode_off switches (off).
        auto const cg = next("fx");
        h.call("/pd/flow_mode_get", { cg, "off", "toggle" }, ("/pd/flow_mode_get/reply/" + cg).toStdString(), 2000);
        CHECK(isOn());
    }
    // Never saved: the settings file has no trace of it, so a restart starts off (the state is a plain member).
    {
        auto const settings = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Documents/plugdata/settings.json");
        CHECK(!settings.loadFileAsString().containsIgnoreCase("flow_mode"));
    }
    // Off from any door, with its event.
    {
        // fire, not call: the event goes out before the reply, and waiting for the reply drops what came before it
        auto const c = next("ff");
        h.fire("/pd/flow_mode_off", { c, "test" });
        auto ev = h.replies.waitFor("/pd/flow_mode/changed", 2000);
        auto r = h.replies.waitFor(("/pd/flow_mode_off/reply/" + c).toStdString(), 2000);
        CHECK(r && static_cast<bool>(parseJson(*r)["wasOn"]));
        CHECK(ev && ev->str(0) == "off" && ev->str(1) == "test");
        CHECK(!isOn());
    }
    // P12a.1 review L3: Alphi works only while the bridge really runs; stopping the bridge turns flow off, with its
    // event (sent before the listeners close); a click while it is stopped does nothing.
    {
        CHECK(host->bridgeRunning());
        CHECK(host->alphiClicked(now() + 5000));
        CHECK(host->session.flowMode.isOn());
        h.replies.clear();
        host->mcpBridge->stop();
        // The click's own "on" event may land after the clear (the reply thread): take the first "off".
        auto ev = h.replies.waitFor("/pd/flow_mode/changed", 2000);
        if (ev && ev->str(0) == "on")
            ev = h.replies.waitFor("/pd/flow_mode/changed", 2000);
        CHECK(ev && ev->str(0) == "off" && ev->str(1) == "bridge_stop");
        CHECK(!host->session.flowMode.isOn());
        CHECK(!host->bridgeRunning());
        CHECK(!host->alphiClicked(now() + 10000));
        CHECK(!host->session.flowMode.isOn());
        restoreSink();
        CHECK(host->bridgeRunning());
        CHECK(!isOn());
    }
    for (auto const& n : state.escTargets(now())) state.clear(n);
}

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

        // P11.6 (review): the steps above leave async work queued (JUCE notifies Value listeners asynchronously, the
        // host's write-back is a postMessage, the settings save is a 100 ms timer): drain it past the save timer before
        // the panel step, and run the panel step on the message thread like the GUI does. (The 5 failures here were
        // not a race in SettingsFile: the runs used the owner's real settings.json and their running PlugData rewrote
        // it; fixed by the scratch HOME export and the HERMETIC BREACH guard in main.)
        auto drainSettings = [&] {
            for (int i = 0; i < 4; ++i) {
                host->onMessageThread([] { return true; }, 2000, "drain");
                sleepMs(150); // past SettingsFile::saveTimeoutMs (100 ms)
            }
            host->onMessageThread([] { return true; }, 2000, "drain");
        };
        drainSettings();
        // The owner's panel: off first in the host, then the setting: accepted, nothing written back.
        host->onMessageThread([host] {
            host->setOutputCeilingFromGui(false);
            SettingsFile::getInstance()->setProperty("mcp_bridge_ai_ceiling", false);
            return true;
        }, 2000, "test");
        drainSettings();
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
        drainSettings();
        host->onMessageThread([host] { host->setOutputCeilingFromGui(false); return true; }, 2000, "test");
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
    CHECK(guarded == 27 && refused == guarded); // 19 since /pd/midi_devices (minArgs 1); +2 P11 /pd/bound, /pd/selection_get; +2 P12c approval_show/clear (minArgs 2); +3 P12a flow_mode_get/off, ownership (P12a.1: the offer verb is gone); +1 Alphi drop 1 alphi_say (minArgs 1)
}

// Alphi drop 1, step W: /pd/alphi_say <json {text, kind, state, turn, ttlMs}>. No reply on v1 (nothing at all comes
// back), over v2 only the router's final unknown{no_reply_verb}; two slots on the session's board (N4): busy -> the
// activity slot only, a final say -> the line slot and activity cleared, a later busy refresh of that turn dropped;
// cut at 90 code points; dropped after ttlMs; a say that breaks the contract changes nothing.
void runAlphiSayCases(Harness& h, PluginProcessor& processor)
{
    std::printf("Alphi says (alphi drop 1 W)\n");
    auto& board = processor.getMcp()->session.alphiSay;
    auto const now = [] { return mcp::alphiSayNowMs(); };
    auto const say = [&](std::string const& json) { h.fire("/pd/alphi_say", { juce::String(json) }); };
    // The handler runs on the command worker: wait for the board to show what was sent.
    auto const until = [](auto pred, int ms) {
        for (int waited = 0; waited < ms; waited += 10) {
            if (pred()) return true;
            sleepMs(10);
        }
        return pred();
    };
    auto const nothingCameBack = [&] {
        sleepMs(300);
        return h.replies.describe().find("alphi_say") == std::string::npos;
    };

    CHECK(hasCap(h, "alphi_say", "as0"));

    // busy: the activity slot only, and no reply
    say(R"({"text":"placed lop~","kind":"busy","state":"building","turn":"asT1","ttlMs":3000})");
    CHECK(until([&] { return board.activity(now()).has_value(); }, 2000));
    auto a = board.activity(now());
    CHECK(a && a->text == "placed lop~" && a->phase == mcp::AlphiSayPhase::Building && a->turn == "asT1");
    CHECK(!board.line(now()).has_value() || board.line(now())->turn != "asT1");
    CHECK(nothingCameBack());

    // the final line: the line slot, activity cleared
    say(R"({"text":"plays at -18 dB","kind":"plain","state":"idle","turn":"asT1","ttlMs":6000})");
    CHECK(until([&] { auto l = board.line(now()); return l && l->turn == "asT1"; }, 2000));
    CHECK(!board.activity(now()).has_value());
    CHECK(board.line(now())->text == "plays at -18 dB");
    CHECK(nothingCameBack());

    // a late busy refresh of the same turn revives nothing and leaves the line
    say(R"({"text":"placed lop~","kind":"busy","state":"building","turn":"asT1","ttlMs":3000})");
    sleepMs(300);
    CHECK(!board.activity(now()).has_value());
    CHECK(board.line(now()) && board.line(now())->text == "plays at -18 dB");

    // cut at 90 code points (3-byte U+2212 each: never inside one)
    std::string minus;
    for (int i = 0; i < 120; ++i) minus += "\xe2\x88\x92";
    say(R"({"text":")" + minus + R"(","kind":"fail","state":"failed","turn":"asT2","ttlMs":6000})");
    CHECK(until([&] { auto l = board.line(now()); return l && l->turn == "asT2"; }, 2000));
    auto l = board.line(now());
    CHECK(l && l->text.size() == 270 && l->kind == mcp::AlphiSayKind::Fail && l->phase == mcp::AlphiSayPhase::Failed);

    // contract breaks change nothing (and get no reply): bad JSON, kind/state disagree, bad turn, ttl out of range
    for (auto const* bad : { "not json", R"({"text":"x","kind":"busy","state":"idle","turn":"asT3","ttlMs":3000})",
             R"({"text":"x","kind":"plain","state":"idle","turn":"a b","ttlMs":3000})",
             R"({"text":"x","kind":"plain","state":"idle","turn":"asT3","ttlMs":5})",
             R"({"text":"\n","kind":"plain","state":"idle","turn":"asT3","ttlMs":3000})" })
        say(bad);
    CHECK(nothingCameBack());
    l = board.line(now());
    CHECK(l && l->turn == "asT2");

    // ttl: dropped once ttlMs has passed
    say(R"({"text":"short","kind":"plain","state":"idle","turn":"asT4","ttlMs":200})");
    CHECK(until([&] { auto x = board.line(now()); return x && x->turn == "asT4"; }, 2000));
    CHECK(until([&] { return !board.line(now()).has_value(); }, 2000));

    // v2: a keyed fire-and-forget request ends with the router's unknown{no_reply_verb}, nothing else
    auto link = v2Connect();
    link->sendReq("AS1", "/pd/alphi_say", R"([{"t":"s","v":"{\"text\":\"v2 say\",\"kind\":\"plain\",\"state\":\"idle\",\"turn\":\"asT5\",\"ttlMs\":3000}"}])");
    auto const f = finalFor(*link, "AS1", 3000);
    auto const* e = f ? f->find("error") : nullptr;
    CHECK(f && field(*f, "outcome") == "unknown" && e && field(*e, "code") == "no_reply_verb");
    CHECK(until([&] { auto x = board.line(now()); return x && x->text == "v2 say"; }, 2000));
    CHECK(!link->take([](auto const& v) { return field(v, "t") == "rep" && field(v, "id") == "AS1"; }, 300).has_value());
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
// P11.6 review N4: why this scratch HOME is unsafe for a run that writes settings, or "" when it is safe. Paths are
// resolved (realpath) on both sides; the scratch HOME must not be, or contain, the real home of this user
// (getpwuid); no component from the scratch HOME down to Documents/plugdata may be a symlink; the settings folder
// must lie inside the scratch HOME. Only reads paths.
std::string hermeticBreach(std::string const& home, std::string const& appData)
{
    auto real = [](std::string const& p) -> std::string {
        char buf[PATH_MAX];
        return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
    };
    auto inside = [](std::string const& child, std::string const& parent) {
        return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
    };
    std::string const scratch = real(home);
    if (scratch.empty())
        return "the scratch HOME " + home + " does not exist";
    if (scratch == "/")
        return "the scratch HOME is /";
    // The homes to protect: this user's real home (getpwuid), plus MCP_TESTS_GUARD_EXTRA_HOME when set. The extra one
    // only ADDS a home to protect (the self-test plays "the real home" in a temp dir); it never replaces the real one.
    std::vector<std::string> protectedHomes;
    if (auto const* pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
        protectedHomes.push_back(real(pw->pw_dir));
    if (char const* extra = std::getenv("MCP_TESTS_GUARD_EXTRA_HOME"); extra && *extra)
        protectedHomes.push_back(real(extra));
    for (auto const& realHome : protectedHomes)
        if (!realHome.empty() && (scratch == realHome || inside(realHome, scratch)))
            return "the scratch HOME " + scratch + " is, or contains, the protected home " + realHome;
    // Every component below the scratch HOME on the way to the settings folder must be a plain directory, never a link.
    std::string path = home;
    for (char const* part : { "Documents", "plugdata" }) {
        path += "/";
        path += part;
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode))
            return path + " is a symlink";
    }
    // The settings folder itself (or, before the first run, its nearest existing parent) must resolve inside it.
    std::string probe = appData;
    std::string resolved = real(probe);
    while (resolved.empty() && probe.size() > 1) {
        probe = probe.substr(0, probe.find_last_of('/'));
        if (probe.empty()) probe = "/";
        resolved = real(probe);
    }
    if (!(resolved == scratch || inside(resolved, scratch)) || !inside(appData, home.back() == '/' ? home.substr(0, home.size() - 1) : home))
        return "the settings folder " + appData + " (resolved " + resolved + ") is not inside the scratch HOME " + scratch;
    return {};
}

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
    // P11.6 incident: ProjectInfo::appDataDir is a static computed from HOME BEFORE main() runs, so setting HOME here
    // came too late and every run used the owner's real ~/Documents/plugdata/settings.json. The runner exports HOME;
    // this refuses to start (nothing is created before it) unless the scratch HOME is safe (review N4).
    if (auto const why = hermeticBreach(home, ProjectInfo::appDataDir.getFullPathName().toStdString()); !why.empty()) {
        std::printf("mcp_bridge_tests: HERMETIC BREACH: %s. Run through scripts/run-mcp-bridge-tests.sh with a scratch "
                    "MCP_TESTS_HOME. Nothing was run.\n", why.c_str());
        return 3;
    }

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
            runMeterWindowGenCases(harness, processor);
            runMeterTapCases(harness, processor, audioControl);
            runAcTapCases(harness, processor, audioControl);
            runOutputCeilingCases(harness, processor, audioControl);
            runMidiDevicesCases(harness, processor);
            runApprovalCases(harness, processor);
            runAlphiSayCases(harness, processor);
            runLimiterPatchCases(processor);
            runVolumeGuardCases(processor);
            runIndexRefCases(harness, processor);
            runMidiCaptureCases(harness, processor, audioControl);
            runStampCostCases(processor);
            runIdentityValidityCases(processor);
            runSidecarCases(processor);
            runLoadReportCases(harness, processor);
            runBoundCases(harness, processor);
            runSaveCoordsCases(harness, processor);
            runPathCanvasCases(processor);
            runSaveGuardCases(harness, processor);
            runContentVersionCases(harness, processor);
            runSelectionGetCases(harness);
            runFlowProvenanceCases(harness, processor);
            runFlowModeCases(harness, processor, [&] {
                bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });
            });
            runV2HandshakeCases(harness, *bridge, [&] {
                bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });
            });
            startRenderForTeardown(harness); // last: the processor is destroyed with the render running
            casesDone.store(true);
            stopMessageLoop();
        });

        // A hung handler must not hang the run.
        std::thread watchdog([&] {
            for (int waited = 0; waited < 2400 && !casesDone.load(); ++waited)
                sleepMs(100);
            if (!casesDone.load()) {
                std::printf("  FAIL the cases did not finish in 240 s\n");
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
