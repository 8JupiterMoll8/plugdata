// Unit tests for the v2 reply router (Plan 4 P4.1 S3): Source/Pd/MCPBridgeV2Router.h. Real schema rows, a fake
// sink, an injected clock. Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeV2Router.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace mcp::v2;
using mcp::v2schema::findVerb;

namespace {

struct FakeSink : Sink {
    std::map<int, std::vector<json::Value>> frames;
    void send(int session, std::string const& payload) override
    {
        std::string err;
        auto v = json::parse(payload, err);
        if (!v)
            std::printf("  sink got bad JSON: %s\n", err.c_str());
        frames[session].push_back(v ? *v : json::Value {});
    }
    std::vector<json::Value> take(int session)
    {
        auto out = std::move(frames[session]);
        frames[session].clear();
        return out;
    }
};

std::string str(json::Value const& v, char const* key)
{
    auto const* m = v.find(key);
    return m && m->is(json::Value::Kind::String) ? m->string : std::string("<none>");
}
bool isFinal(json::Value const& v)
{
    auto const* f = v.find("final");
    return f && f->is(json::Value::Kind::Bool) && f->boolean;
}
std::string code(json::Value const& v)
{
    auto const* e = v.find("error");
    return e ? str(*e, "code") : std::string("<none>");
}

Request req(std::string id, std::string verb, std::vector<std::string> sargs = {})
{
    Request r;
    r.id = std::move(id);
    r.verb = std::move(verb);
    for (auto& s : sargs) {
        Arg a;
        a.type = 's';
        a.s = std::move(s);
        r.args.push_back(std::move(a));
    }
    return r;
}

OutMessage msg(std::string address, std::vector<std::string> sargs = {})
{
    OutMessage m;
    m.address = std::move(address);
    for (auto& s : sargs) {
        Arg a;
        a.type = 's';
        a.s = std::move(s);
        m.args.push_back(std::move(a));
    }
    return m;
}

} // namespace

int main()
{
    // 1. Rule 1: the last segment is a pending request's corr token. Sync final, then the handler's end adds nothing.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("V1N1", "/pd/census", { "main", "V1N1" }), findVerb("pd", "census"), 0);
        CHECK(a.origin && !a.reply);
        CHECK(a.origin && a.origin->corrToken == "V1N1");
        CHECK(!r.route(msg("/pd/census/reply/V1N1", { "{}" }), nullptr, 1)); // not to UDP
        auto f = sink.take(1);
        CHECK(f.size() == 1 && str(f[0], "t") == "rep" && str(f[0], "id") == "V1N1" && isFinal(f[0]));
        CHECK(f.size() == 1 && str(f[0], "outcome") == "applied" && str(f[0], "address") == "/pd/census/reply/V1N1");
        CHECK(f.size() == 1 && f[0].find("args") && f[0].find("args")->items.size() == 1);
        r.handlerDone(a.origin, 2);
        CHECK(sink.take(1).empty());
        CHECK(r.pendingCount() == 0);
        // the same address again: a late duplicate for a finished v2 request, dropped (never to v1)
        CHECK(!r.route(msg("/pd/census/reply/V1N1"), nullptr, 3));
        CHECK(sink.take(1).empty());
    }

    // 2. The corr token is the request id when an argument carries it, else the verb's corr argument, "0" = none.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("H7", "/array", { "stats", "arr", "44100", "a2" }), findVerb("array", "stats"), 0);
        CHECK(a.origin && a.origin->corrToken == "a2"); // id not in the args: args[corrArg]
        auto b = r.admit(1, req("V1N2", "/pd/clear", { "main", "0" }), findVerb("pd", "clear"), 0);
        CHECK(b.origin && b.origin->corrToken.empty());
        auto c = r.admit(1, req("V1N3", "/meter/start", { "main", "x", "V1N3" }), findVerb("meter", "start"), 0);
        CHECK(c.origin && c.origin->corrToken == "V1N3");
        // a second request with a token already in flight breaks the protocol: the session is closed (a reply by
        // that id would settle the live request for the client)
        auto d = r.admit(1, req("V1N4", "/pd/census", { "main", "a2" }), findVerb("pd", "census"), 0);
        CHECK(!d.origin && d.reply && d.closeSession);
        std::string err;
        auto const dv = d.reply ? json::parse(*d.reply, err) : std::nullopt;
        CHECK(dv && str(*dv, "t") == "bye" && code(*dv) == "bad_frame");
    }

    // 3. Rule 2: a constant-address reply sent while its request is current (origin) is that request's final.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("V1N1", "/bridge/capabilities"), findVerb("bridge", "capabilities"), 0);
        CHECK(!r.route(msg("/bridge/capabilities/reply", { "11.18", "census" }), a.origin.get(), 1));
        auto f = sink.take(1);
        CHECK(f.size() == 1 && isFinal(f[0]) && str(f[0], "id") == "V1N1");
        // the same address without an origin (a v1 request's reply) goes to UDP and to nobody else
        CHECK(r.route(msg("/bridge/capabilities/reply"), nullptr, 2));
        CHECK(sink.take(1).empty());
    }

    // 4. Rule 2b: /pd/cleared is the final of a clear sent without an id, a side reply of one with an id.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto noId = r.admit(1, req("V1N1", "/pd/clear", { "main" }), findVerb("pd", "clear"), 0);
        r.handlerDone(noId.origin, 1); // async: nothing yet
        CHECK(sink.take(1).empty());
        CHECK(!r.route(msg("/pd/cleared"), noId.origin.get(), 2)); // posted later, origin carried by postMessage
        auto f = sink.take(1);
        CHECK(f.size() == 1 && isFinal(f[0]) && str(f[0], "id") == "V1N1");

        auto withId = r.admit(1, req("V1N2", "/pd/clear", { "main", "V1N2" }), findVerb("pd", "clear"), 3);
        r.handlerDone(withId.origin, 4);
        CHECK(!r.route(msg("/pd/cleared"), withId.origin.get(), 5));
        CHECK(!r.route(msg("/pd/mcp_clear_ids/reply/0", { "1" }), withId.origin.get(), 5));
        CHECK(!r.route(msg("/pd/clear/reply/V1N2", { "1", "1" }), withId.origin.get(), 6));
        f = sink.take(1);
        CHECK(f.size() == 3);
        if (f.size() == 3) {
            CHECK(!isFinal(f[0]) && str(f[0], "address") == "/pd/cleared");
            CHECK(!isFinal(f[1]) && str(f[1], "address") == "/pd/mcp_clear_ids/reply/0");
            CHECK(isFinal(f[2]) && str(f[2], "address") == "/pd/clear/reply/V1N2");
        }
        // a v1 clear's /pd/cleared (no origin) never reaches a pending v2 clear
        auto third = r.admit(1, req("V1N3", "/pd/clear", { "main" }), findVerb("pd", "clear"), 7);
        CHECK(r.route(msg("/pd/cleared"), nullptr, 8));
        CHECK(sink.take(1).empty());
        (void)third;
    }

    // 5. Async: an ack is not final; the final arrives later from a timer (no origin); errors are rejected.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto m = r.admit(1, req("V1N1", "/meter/start", { "main", "osc1", "V1N1" }), findVerb("meter", "start"), 0);
        CHECK(!r.route(msg("/meter/start/reply/V1N1", { "7" }), m.origin.get(), 1));
        r.handlerDone(m.origin, 2);
        CHECK(!r.route(msg("/meter/result/V1N1", { "{}" }), nullptr, 500));
        auto f = sink.take(1);
        CHECK(f.size() == 2 && !isFinal(f[0]) && isFinal(f[1]) && str(f[1], "outcome") == "applied");

        auto e = r.admit(1, req("V1N2", "/pd/render", { "main", "x.wav", "1", "V1N2" }), findVerb("pd", "render"), 600);
        r.handlerDone(e.origin, 601);
        CHECK(!r.route(msg("/pd/render/error/V1N2", { "busy rendering" }), nullptr, 700));
        f = sink.take(1);
        CHECK(f.size() == 1 && isFinal(f[0]) && str(f[0], "outcome") == "rejected" && code(f[0]) == "handler_error");
        CHECK(f.size() == 1 && f[0].find("error") && str(*f[0].find("error"), "message") == "busy rendering");
    }

    // 6. Synthesized finals: a fire-and-forget verb, a sync verb whose handler sent no final, an async deadline,
    // an unknown verb, a request that could not start before its deadline, the in-flight cap, shutting down.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto none = r.admit(1, req("V1N1", "/pd/clear_selection", { "main" }), findVerb("pd", "clear_selection"), 0);
        r.handlerDone(none.origin, 1);
        auto f = sink.take(1);
        CHECK(f.size() == 1 && isFinal(f[0]) && str(f[0], "outcome") == "unknown" && code(f[0]) == "no_reply_verb");

        auto rt = r.admit(1, [] { auto q = req("V1N2", "/trigger/note", { "60" }); q.noReply = true; return q; }(), findVerb("trigger", ""), 0);
        r.handlerDone(rt.origin, 1);
        CHECK(sink.take(1).empty()); // the realtime lane: nothing, ever

        auto silent = r.admit(1, req("V1N3", "/pd/census", { "main", "V1N3" }), findVerb("pd", "census"), 0);
        r.handlerDone(silent.origin, 1);
        f = sink.take(1);
        CHECK(f.size() == 1 && str(f[0], "outcome") == "unknown" && code(f[0]) == "no_reply_verb");

        auto late = r.admit(1, req("V1N4", "/meter/start", { "main", "o", "V1N4" }), findVerb("meter", "start"), 1000);
        CHECK(r.begin(late.origin, 1001));
        r.handlerDone(late.origin, 1001);
        r.tick(1001 + 9999);
        CHECK(sink.take(1).empty());
        r.tick(1001 + 10001); // the async clock runs from the start (here: the end of its work)
        f = sink.take(1);
        CHECK(f.size() == 1 && str(f[0], "id") == "V1N4" && str(f[0], "outcome") == "unknown" && code(f[0]) == "timeout");
        CHECK(!r.route(msg("/meter/result/V1N4"), nullptr, 20000)); // a late reply to a finished v2 request: dropped
        CHECK(sink.take(1).empty());

        auto unknown = r.admit(1, req("V1N5", "/pd/no_such_verb"), nullptr, 0);
        std::string err;
        auto uv = unknown.reply ? json::parse(*unknown.reply, err) : std::nullopt;
        CHECK(!unknown.origin && uv && str(*uv, "outcome") == "rejected" && code(*uv) == "unknown_verb");

        auto q = req("V1N6", "/pd/census", { "main", "V1N6" });
        q.deadlineMs = 100;
        auto stale = r.admit(1, q, findVerb("pd", "census"), 0);
        CHECK(!r.begin(stale.origin, 150)); // the worker reached it too late: not run
        r.handlerDone(stale.origin, 150);   // its Work goes away all the same
        f = sink.take(1);
        CHECK(f.size() == 1 && str(f[0], "outcome") == "rejected" && code(f[0]) == "deadline");
        auto fresh = r.admit(1, req("V1N7", "/pd/census", { "main", "V1N7" }), findVerb("pd", "census"), 0);
        CHECK(r.begin(fresh.origin, 2999)); // the class deadline (read, 3 s)
        r.handlerDone(fresh.origin, 3000);
        sink.take(1);

        int busy = 0;
        for (std::size_t i = 0; i < mcp::v2schema::kInFlightPerSession + 1; ++i) {
            auto const id = "V2N" + std::to_string(i);
            auto x = r.admit(1, req(id, "/meter/start", { "m", "o", id }), findVerb("meter", "start"), 0);
            if (!x.origin && x.reply && x.reply->find("\"busy\"") != std::string::npos)
                ++busy;
        }
        CHECK(busy == 1);
        r.close();
        auto down = r.admit(1, req("V9N1", "/pd/census", { "main", "V9N1" }), findVerb("pd", "census"), 0);
        CHECK(!down.origin && down.reply && down.reply->find("shutting_down") != std::string::npos);
        CHECK(r.pendingCount() == 0);
    }

    // 7. Rule 3, events: telemetry to every subscribed session AND UDP; artist events to the newest session only;
    // with no v2 session they go to UDP as before. Unlisted addresses are v1 replies: UDP only.
    {
        FakeSink sink;
        Router r(sink);
        CHECK(!r.active());
        CHECK(r.route(msg("/pd/ui/prompt", { "make it warmer" }), nullptr, 0)); // no v2 session: UDP
        r.sessionOpened(1, true);
        r.sessionOpened(2, false);
        r.sessionOpened(3, true);
        CHECK(r.active());
        CHECK(r.route(msg("/pd/log", { "hello" }), nullptr, 1));
        CHECK(sink.take(1).size() == 1 && sink.take(2).empty());
        auto t3 = sink.take(3);
        CHECK(t3.size() == 1 && str(t3[0], "t") == "evt" && str(t3[0], "class") == "telemetry" && str(t3[0], "address") == "/pd/log");
        CHECK(!r.route(msg("/pd/ui/prompt", { "make it warmer" }), nullptr, 2));
        auto a3 = sink.take(3);
        CHECK(sink.take(1).empty() && sink.take(2).empty() && a3.size() == 1 && str(a3[0], "class") == "artist");
        r.sessionClosed(3);
        CHECK(!r.route(msg("/pd/ui/prompt"), nullptr, 3));
        CHECK(sink.take(2).size() == 1); // now the newest
        CHECK(r.route(msg("/pd/something/reply/k1"), nullptr, 4));
        CHECK(sink.take(1).empty() && sink.take(2).empty());
    }

    // 8. A closed session's requests are dropped; replies to them go nowhere (not UDP: they were v2's).
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("V1N1", "/meter/start", { "m", "o", "V1N1" }), findVerb("meter", "start"), 0);
        r.handlerDone(a.origin, 1);
        r.sessionClosed(1);
        CHECK(r.pendingCount() == 0);
        CHECK(!r.route(msg("/meter/result/V1N1"), nullptr, 2)); // the closed request's token: v2's, dropped (not v1's)
        CHECK(sink.take(1).empty());
    }

    // 9. A reply carries typed args; a reply on a corr token that matches no declared pattern is a side reply.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("V1N1", "/pd/census", { "main", "V1N1" }), findVerb("pd", "census"), 0);
        OutMessage m;
        m.address = "/pd/census/reply/V1N1";
        Arg i;
        i.type = 'i';
        i.i = 7;
        Arg f;
        f.type = 'f';
        f.f = 0.25f;
        m.args = { i, f };
        CHECK(!r.route(msg("/pd/log/V1N1", { "side" }), nullptr, 1));
        CHECK(!r.route(m, a.origin.get(), 1));
        auto got = sink.take(1);
        CHECK(got.size() == 2 && !isFinal(got[0]) && isFinal(got[1]));
        if (got.size() == 2) {
            auto const* args = got[1].find("args");
            CHECK(args && args->items.size() == 2 && str(args->items[0], "t") == "i" && args->items[0].find("v")->number == 7);
            CHECK(args && str(args->items[1], "t") == "f" && args->items[1].find("v")->number == 0.25);
        }
    }

    // 10. S3 review CRITICAL: a duplicate request id is refused (with another token, and with none), so no index is
    // left dangling; the first request still completes and the session's count goes back to zero.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto first = r.admit(1, req("X", "/pd/census", { "main", "T1" }), findVerb("pd", "census"), 0);
        auto again = r.admit(1, req("X", "/pd/census", { "main", "T2" }), findVerb("pd", "census"), 0);
        CHECK(first.origin && !again.origin && again.closeSession && again.reply->find("bad_frame") != std::string::npos);
        CHECK(!r.route(msg("/pd/census/reply/T1", { "{}" }), nullptr, 1));
        CHECK(r.route(msg("/pd/census/reply/T2"), nullptr, 2)); // nobody owns T2: v1 rules, no crash
        r.handlerDone(first.origin, 3);
        auto p1 = r.admit(1, req("P", "/pd/ping"), findVerb("pd", "ping"), 0);
        auto p2 = r.admit(1, req("P", "/pd/ping"), findVerb("pd", "ping"), 0);
        CHECK(p1.origin && !p2.origin);
        r.handlerDone(p1.origin, 4);
        CHECK(r.pendingCount() == 0);
    }

    // 11. S3 review MEDIUM: noReply requests count against the in-flight cap and the byte budget; deadlines are clamped.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        std::vector<OriginPtr> held;
        for (std::size_t k = 0; k < mcp::v2schema::kInFlightPerSession; ++k) {
            auto q = req("N" + std::to_string(k), "/trigger/note", { "60" });
            q.noReply = true;
            auto a = r.admit(1, q, findVerb("trigger", ""), 0);
            if (a.origin)
                held.push_back(a.origin);
        }
        CHECK(held.size() == mcp::v2schema::kInFlightPerSession);
        auto q = req("N-over", "/trigger/note", { "60" });
        q.noReply = true;
        auto over = r.admit(1, q, findVerb("trigger", ""), 0);
        CHECK(!over.origin && over.reply && over.reply->find("busy") != std::string::npos);
        for (auto const& o : held)
            r.handlerDone(o, 1);
        CHECK(r.admit(1, q, findVerb("trigger", ""), 0).origin != nullptr);

        Router big(sink);
        auto huge = req("B1", "/pd/census", { "main", std::string(kInFlightBytesPerSession / 2, 'x') });
        CHECK(big.admit(2, huge, findVerb("pd", "census"), 0).origin);
        huge.id = "B2";
        huge.args[1].s = std::string(kInFlightBytesPerSession / 2, 'z'); // another corr token, the same size
        auto refused = big.admit(2, huge, findVerb("pd", "census"), 0);
        CHECK(!refused.origin && refused.reply && refused.reply->find("MiB") != std::string::npos);

        auto longDeadline = req("D1", "/pd/census", { "main", "D1" });
        longDeadline.deadlineMs = 86400000;
        auto d = r.admit(1, longDeadline, findVerb("pd", "census"), 0);
        CHECK(d.origin && d.origin->deadlineMs == kMaxDeadlineMs);
    }

    // 12. S3 review MEDIUM: a request still queued past its deadline is answered by tick(), and then not run.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto q = req("Q1", "/pd/census", { "main", "Q1" });
        q.deadlineMs = 100;
        auto a = r.admit(1, q, findVerb("pd", "census"), 0);
        r.tick(50);
        CHECK(sink.take(1).empty());
        r.tick(101);
        auto f = sink.take(1);
        CHECK(f.size() == 1 && code(f[0]) == "deadline" && str(f[0], "outcome") == "rejected");
        CHECK(!r.begin(a.origin, 5000));
        CHECK(sink.take(1).empty()); // answered once
        // a started async request is not cut by the start deadline, only by its own deadline from the start
        auto m = r.admit(1, req("Q2", "/meter/start", { "m", "o", "Q2" }), findVerb("meter", "start"), 0);
        CHECK(r.begin(m.origin, 9000));
        r.handlerDone(m.origin, 9001);
        r.tick(15000);
        CHECK(sink.take(1).empty());
        r.tick(19001);
        f = sink.take(1);
        CHECK(f.size() == 1 && code(f[0]) == "timeout");
    }

    // 13. S3 review MEDIUM: a non-event message sent for a v2 request that matches nothing is dropped, not sent to
    // v1 (a noReply request on a correlated verb, an undeclared address).
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto q = req("Z1", "/pd/census", { "main", "Z1" });
        q.noReply = true;
        auto a = r.admit(1, q, findVerb("pd", "census"), 0);
        CHECK(!r.route(msg("/pd/census/reply/Z1"), a.origin.get(), 1));
        auto b = r.admit(1, req("Z2", "/pd/census", { "main", "Z2" }), findVerb("pd", "census"), 0);
        CHECK(!r.route(msg("/pd/some_undeclared/reply"), b.origin.get(), 1));
        CHECK(r.route(msg("/pd/log", { "x" }), b.origin.get(), 1)); // an event stays an event
        CHECK(sink.take(1).empty());
    }

    // 14. A reply over the frame limit becomes unknown{too_large}; an oversized side reply is dropped.
    {
        FakeSink sink;
        Router r(sink, 4096);
        r.sessionOpened(1, false);
        auto a = r.admit(1, req("L1", "/pd/census", { "main", "L1" }), findVerb("pd", "census"), 0);
        CHECK(!r.route(msg("/pd/log/L1", { std::string(5000, 'y') }), nullptr, 1));
        CHECK(!r.route(msg("/pd/census/reply/L1", { std::string(5000, 'y') }), nullptr, 1));
        auto f = sink.take(1);
        CHECK(f.size() == 1 && str(f[0], "outcome") == "unknown" && code(f[0]) == "too_large");
        (void)a;
    }

    // 15. Work tokens: done is reported when the last copy goes (the handler AND every hop it posted), and a scope
    // restores the outer work.
    {
        std::vector<std::string> done;
        auto origin = std::make_shared<Origin>();
        origin->id = "W1";
        WorkPtr hop;
        {
            WorkScope scope(std::make_shared<Work>(origin, [&](OriginPtr const& o) { done.push_back(o->id); }));
            CHECK(currentOrigin() && currentOrigin()->id == "W1");
            hop = captureWork(); // what postMessage does
            {
                WorkScope none(nullptr);
                CHECK(!currentOrigin());
            }
            CHECK(currentOrigin() && currentOrigin()->id == "W1");
        }
        CHECK(!currentOrigin());
        CHECK(done.empty()); // the posted hop still holds it
        {
            WorkScope run(std::move(hop)); // the hop runs on another thread
        }
        CHECK(done.size() == 1 && done[0] == "W1");
    }

    // 16. S3 verification review M1: an id reused after its final went out; the EARLIER request's late done and late
    // final must not settle the newer one.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto first = r.admit(1, req("R", "/pd/census", { "main", "R" }), findVerb("pd", "census"), 0);
        CHECK(r.begin(first.origin, 1));
        CHECK(!r.route(msg("/pd/census/reply/R", { "{}" }), first.origin.get(), 2)); // its final
        sink.take(1);
        auto second = r.admit(1, req("R", "/pd/census", { "main", "R" }), findVerb("pd", "census"), 3);
        CHECK(second.origin != nullptr);
        r.handlerDone(first.origin, 4); // the first request's work ends late (a hop was still queued)
        CHECK(sink.take(1).empty());    // the second is not settled by it
        CHECK(r.begin(second.origin, 5));
        CHECK(!r.route(msg("/pd/census/reply/R", { "{}" }), second.origin.get(), 6));
        auto f = sink.take(1);
        CHECK(f.size() == 1 && isFinal(f[0]) && str(f[0], "outcome") == "applied");
        r.handlerDone(second.origin, 7);
        CHECK(sink.take(1).empty() && r.pendingCount() == 0);
    }

    // 17. S3 verification review L3: once tick() answered rejected{deadline}, begin() never runs the request.
    {
        FakeSink sink;
        Router r(sink);
        r.sessionOpened(1, false);
        auto q = req("T", "/pd/census", { "main", "T" });
        q.deadlineMs = 10;
        auto a = r.admit(1, q, findVerb("pd", "census"), 0);
        r.tick(11);
        CHECK(sink.take(1).size() == 1);
        CHECK(!r.begin(a.origin, 10)); // even with a clock read at the deadline itself
    }

    if (failures == 0)
        std::printf("v2_router_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
