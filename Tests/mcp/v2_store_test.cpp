// Unit tests for the v2 idempotency store (Plan 4 P4.1 S4): Source/Pd/MCPBridgeV2Store.h.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeV2Store.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace mcp::v2;

namespace {

StoredFinal applied(std::string address, std::string text = "ok")
{
    StoredFinal f;
    f.outcome = "applied";
    f.address = std::move(address);
    Arg a;
    a.s = std::move(text);
    f.args.push_back(a);
    return f;
}

FinalPtr fin(StoredFinal f) { return std::make_shared<StoredFinal const>(std::move(f)); }

} // namespace

int main()
{
    // 1. New, then in flight: a second sight of the key joins; done: the stored final is replayed.
    {
        Store s;
        CHECK(s.begin("c:1", 11, 0).state == Store::State::New);
        auto again = s.begin("c:1", 11, 1);
        CHECK(again.state == Store::State::InFlight);
        s.join("c:1", { 2, "V2N1" });
        auto waiters = s.complete("c:1", fin(applied("/pd/x/reply/V1N1")), 5);
        CHECK(waiters.size() == 1 && waiters[0].session == 2 && waiters[0].id == "V2N1");
        auto done = s.begin("c:1", 11, 6);
        CHECK(done.state == Store::State::Done && done.final && done.final->address == "/pd/x/reply/V1N1");
        CHECK(s.complete("c:1", fin(applied("/other")), 7).empty()); // a second completion changes nothing
        CHECK(s.begin("c:1", 11, 8).final->address == "/pd/x/reply/V1N1");
    }

    // 2. The same key with another fingerprint is a conflict, in flight and done.
    {
        Store s;
        s.begin("c:2", 1, 0);
        CHECK(s.begin("c:2", 2, 0).state == Store::State::Conflict);
        s.complete("c:2", fin(applied("/a")), 1);
        CHECK(s.begin("c:2", 2, 2).state == Store::State::Conflict);
        CHECK(s.begin("c:2", 1, 2).state == Store::State::Done);
    }

    // 3. Abandon (refused before it ran): the key is forgotten, so a resend runs; joined waiters are returned.
    {
        Store s;
        s.begin("c:3", 1, 0);
        s.join("c:3", { 4, "V4N1" });
        auto w = s.abandon("c:3");
        CHECK(w.size() == 1 && w[0].id == "V4N1");
        CHECK(s.begin("c:3", 1, 1).state == Store::State::New);
    }

    // 4. Status: done -> the final, in flight -> in flight, unknown otherwise.
    {
        Store s;
        CHECK(s.status("nope", 0).state == Store::State::New);
        s.begin("c:4", 1, 0);
        CHECK(s.status("c:4", 0).state == Store::State::InFlight);
        s.complete("c:4", fin(applied("/b")), 1);
        auto st = s.status("c:4", 0);
        CHECK(st.state == Store::State::Done && st.final && st.final->address == "/b");
    }

    // 5. TTL: a done entry older than the TTL is gone (its key runs again); in-flight entries also expire.
    {
        Store s(Store::Limits { 16, 1000, 1 << 20 });
        s.begin("c:5", 1, 0);
        s.complete("c:5", fin(applied("/c")), 10);
        CHECK(s.begin("c:5", 1, 1009).state == Store::State::Done);
        CHECK(s.begin("c:5", 1, 1011).state == Store::State::New);
        s.begin("c:6", 1, 2000);
        CHECK(s.begin("c:7", 1, 9000).state == Store::State::New);
        CHECK(s.status("c:6", 9000).state == Store::State::InFlight); // in flight never expires: the router ends it
        s.complete("c:6", fin(applied("/f")), 9000);
        CHECK(s.status("c:6", 9999).state == Store::State::Done);
        CHECK(s.status("c:6", 10001).state == Store::State::New); // status honours the TTL too
    }

    // 6. Bounds: the oldest done entries go first, by count and by bytes; in-flight entries are never evicted.
    {
        Store s(Store::Limits { 3, 600000, 1 << 20 });
        s.begin("in", 1, 0); // stays in flight
        for (int k = 0; k < 4; ++k) {
            auto const key = "d" + std::to_string(k);
            s.begin(key, 1, k);
            s.complete(key, fin(applied("/d")), k);
        }
        CHECK(s.status("in", 10).state == Store::State::InFlight);
        CHECK(s.status("d0", 10).state == Store::State::New && s.status("d1", 10).state == Store::State::New);
        CHECK(s.status("d3", 10).state == Store::State::Done && s.size() == 3);

        Store b(Store::Limits { 100, 600000, 3000 });
        for (int k = 0; k < 5; ++k) {
            auto const key = "b" + std::to_string(k);
            b.begin(key, 1, k);
            b.complete(key, fin(applied("/e", std::string(1000, 'x'))), k);
        }
        CHECK(b.bytes() <= 3000 && b.status("b4", 10).state == Store::State::Done && b.status("b0", 10).state == Store::State::New);
    }

    // 7. Encoded finals: the id is the asker's, `replayed` marks a copy, a synthesized final has no address.
    {
        std::string err;
        auto v = json::parse(encodeFinal("V9N1", applied("/pd/x/reply/V1N1"), true), err);
        CHECK(v && v->find("id")->string == "V9N1" && v->find("replayed") && v->find("replayed")->boolean);
        CHECK(v && v->find("final")->boolean && v->find("address")->string == "/pd/x/reply/V1N1");
        StoredFinal synth;
        synth.outcome = "unknown";
        synth.error = std::make_pair(ErrorCode::NoReplyVerb, std::string("ran"));
        auto u = json::parse(encodeFinal("V1N2", synth, false), err);
        CHECK(u && !u->find("address") && !u->find("replayed") && u->find("error")->find("code")->string == "no_reply_verb");
        CHECK(fingerprintOf("/pd/x", {}) != fingerprintOf("/pd/y", {}));
        std::vector<Arg> a(1), b(1);
        a[0].s = "1";
        b[0].type = 'i';
        b[0].i = 1;
        CHECK(fingerprintOf("/pd/x", a) != fingerprintOf("/pd/x", b)); // "1" and 1 are different requests
    }

    // 8. At most kMaxWaiters askers per key; the same (session, id) joins once.
    {
        Store s;
        s.begin("w", 1, 0);
        CHECK(s.join("w", { 1, "a" }) && s.join("w", { 1, "a" }));
        int joined = 1;
        for (int k = 0; k < 20; ++k)
            joined += s.join("w", { 2, "x" + std::to_string(k) }) ? 1 : 0;
        CHECK(joined == static_cast<int>(kMaxWaiters));
        CHECK(s.complete("w", fin(applied("/w")), 1).size() == kMaxWaiters);
        CHECK(!s.join("w", { 3, "late" })); // done: nothing to join
    }

    if (failures == 0)
        std::printf("v2_store_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
