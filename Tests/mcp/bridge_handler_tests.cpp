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
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
#include "v2_test_client.h" // P4.1 S2: loopback v2 client

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
    CHECK(guarded == 16 && refused == guarded);
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
            runV2IdempotencyCases(harness);
            runV2OwnershipCases(harness);
            runZeroArgCases(harness);
            runV2WithoutUdpCases(*bridge, [&] {
                bridge->setReplySinkForTests([&replies](juce::OSCMessage const& msg) { replies.push(msg); });
            });
            runPdContextCases(processor, audioControl);
            runNativeUndoCases(processor);
            runSharedUndoCases(harness, processor);
            runAudioStallCases(harness, audioControl);
            runMidiCaptureCases(processor, audioControl);
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
