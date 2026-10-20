// Unit tests for the v2 frame codec and envelope (Plan 4 P4.1 S2): Source/Pd/MCPBridgeV2Json.h,
// Source/Pd/MCPBridgeV2Codec.h. Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeV2Codec.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <random>
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
using mcp::v2schema::ErrorCode;

namespace {

std::vector<std::string> decodeAll(FrameDecoder& d, std::string const& bytes, FrameDecoder::Error* err = nullptr)
{
    std::vector<std::string> out;
    auto const e = d.feed(bytes.data(), bytes.size(), out);
    if (err)
        *err = e;
    return out;
}

DecodeResult inbound(std::string const& json) { return decodeInbound(json); }

bool refused(std::string const& json, ErrorCode code)
{
    auto const r = decodeInbound(json);
    return !r.ok && r.refusal.code == code;
}

std::string hello(char const* token, char const* hash, int protocol = 2)
{
    return std::string(R"({"t":"hello","protocol":)") + std::to_string(protocol) + R"(,"schemaHash":")" + hash
        + R"(","token":")" + token + R"(","client":{"name":"test"}})";
}

} // namespace

int main()
{
    // 1. Frames: header is 4 bytes big-endian; incremental decode at every byte split of a three-frame stream.
    {
        auto const a = encodeFrame(R"({"t":"ping"})");
        CHECK(a.size() == 4 + 12);
        CHECK(a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 12);
        std::string const stream = encodeFrame("{}") + encodeFrame(R"({"t":"bye","reason":"x"})") + encodeFrame(std::string(300, ' ') + "{}");
        bool allSplits = true;
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            FrameDecoder d(1024);
            std::vector<std::string> out;
            d.feed(stream.data(), split, out);
            d.feed(stream.data() + split, stream.size() - split, out);
            if (out.size() != 3 || out[0] != "{}" || out[1] != R"({"t":"bye","reason":"x"})" || out[2].size() != 302)
                allSplits = false;
        }
        CHECK(allSplits);
        // byte by byte
        FrameDecoder d(1024);
        std::vector<std::string> out;
        for (char c : stream)
            d.feed(&c, 1, out);
        CHECK(out.size() == 3);
        CHECK(d.buffered() == 0);
    }

    // 2. Two frames in one read; a 1 MB frame in uneven chunks.
    {
        FrameDecoder d;
        auto const two = decodeAll(d, encodeFrame("[1]") + encodeFrame("[2]"));
        CHECK(two.size() == 2 && two[0] == "[1]" && two[1] == "[2]");
        std::string big(1 << 20, 'x');
        auto const framed = encodeFrame(big);
        std::vector<std::string> out;
        std::mt19937 rng(7);
        std::size_t pos = 0;
        while (pos < framed.size()) {
            std::size_t const n = std::min<std::size_t>(framed.size() - pos, 1 + rng() % 70000);
            d.feed(framed.data() + pos, n, out);
            pos += n;
        }
        CHECK(out.size() == 1 && out[0] == big);
    }

    // 3. Zero length, max, over max (reported on the header, before any body), sticky errors.
    {
        FrameDecoder::Error e {};
        FrameDecoder zero(1024);
        CHECK(decodeAll(zero, std::string("\0\0\0\0", 4), &e).empty());
        CHECK(e == FrameDecoder::Error::Empty);
        CHECK(decodeAll(zero, encodeFrame("{}"), &e).empty()); // sticky
        CHECK(e == FrameDecoder::Error::Empty);

        FrameDecoder atMax(1024);
        auto const okFrames = decodeAll(atMax, encodeFrame(std::string(1024, 'a')), &e);
        CHECK(e == FrameDecoder::Error::None && okFrames.size() == 1);

        FrameDecoder over(1024);
        std::string const header("\0\0\x04\x01", 4); // 1025
        CHECK(decodeAll(over, header, &e).empty());
        CHECK(e == FrameDecoder::Error::TooLarge);

        FrameDecoder defaults;
        CHECK(defaults.maxFrameBytes() == mcp::v2schema::kMaxFrameBytes);
    }

    // 4. An HTTP request line (a browser) and a TLS hello are refused on their first 4 bytes.
    {
        for (char const* line : { "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "POST /mcp HTTP/1.1\r\n", "\x16\x03\x01\x02\x00" }) {
            FrameDecoder d;
            FrameDecoder::Error e {};
            decodeAll(d, line, &e);
            CHECK(e == FrameDecoder::Error::TooLarge);
        }
    }

    // 5. Envelope: hello and a request with typed args.
    {
        auto const h = inbound(hello("tok", "feedfacecafebeef"));
        CHECK(h.ok && h.frame.kind == Kind::Hello);
        CHECK(h.frame.hello.protocol == 2 && h.frame.hello.token == "tok" && h.frame.hello.schemaHash == "feedfacecafebeef");
        CHECK(h.frame.hello.clientName == "test");
        CHECK(!h.frame.hello.telemetry);
        auto const ht = inbound(R"({"t":"hello","protocol":2,"schemaHash":"x","token":"t","telemetry":true})");
        CHECK(ht.ok && ht.frame.hello.telemetry);
        CHECK(refused(R"({"t":"hello","protocol":2,"schemaHash":"x","token":"t","telemetry":"yes"})", ErrorCode::BadFrame));

        auto const r = inbound(R"({"t":"req","id":"V7N42","verb":"/pd/batch_atomic","key":"3f9a0c1d2b4e5f60:42","deadlineMs":10000,)"
                               R"("args":[{"t":"i","v":3},{"t":"f","v":0.5},{"t":"s","v":"osc~ 440"},{"t":"i","v":-2147483648}]})");
        CHECK(r.ok && r.frame.kind == Kind::Req);
        auto const& q = r.frame.req;
        CHECK(q.id == "V7N42" && q.verb == "/pd/batch_atomic" && q.key == "3f9a0c1d2b4e5f60:42");
        CHECK(q.deadlineMs == 10000 && !q.noReply);
        CHECK(q.args.size() == 4);
        if (q.args.size() == 4) {
            CHECK(q.args[0].type == 'i' && q.args[0].i == 3);
            CHECK(q.args[1].type == 'f' && q.args[1].f == 0.5f);
            CHECK(q.args[2].type == 's' && q.args[2].s == "osc~ 440");
            CHECK(q.args[3].type == 'i' && q.args[3].i == INT32_MIN);
        }
        auto const minimal = inbound(R"({"t":"req","id":"V1N1","verb":"/pd/ping","noReply":true})");
        CHECK(minimal.ok && minimal.frame.req.args.empty() && minimal.frame.req.noReply && minimal.frame.req.key.empty());

        CHECK(inbound(R"({"t":"ping"})").ok && inbound(R"({"t":"ping"})").frame.kind == Kind::Ping);
        auto const bye = inbound(R"({"t":"bye","reason":"done"})");
        CHECK(bye.ok && bye.frame.kind == Kind::Bye && bye.frame.reason == "done");
        CHECK(inbound(R"({"t":"ping","future":{"x":[1,2,3]}})").ok); // unknown fields are ignored
    }

    // 6. Bad types and bad shapes are refused as bad_frame; a refused request keeps its id.
    {
        char const* bad[] = {
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"x","v":1}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"i","v":1.5}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"i","v":2147483648}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"f","v":"0.5"}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"f","v":1e39}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"s","v":5}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"s"}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"s","v":"a\u0000b"}]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":["bare"]})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","args":{"t":"s","v":"x"}})",
            R"({"t":"req","id":"V1N1","verb":"pd/x"})",
            R"({"t":"req","id":"V1N1","verb":"/pd/a b"})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","deadlineMs":-1})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","deadlineMs":1.5})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","noReply":1})",
            R"({"t":"req","id":"V1N1","verb":"/pd/x","key":7})",
            R"({"t":"hello","protocol":"2","schemaHash":"h","token":"t"})",
            R"({"t":"hello","protocol":2,"schemaHash":"h"})",
            R"({"t":"nope"})",
            R"({"kind":"ping"})",
            R"([{"t":"ping"}])",
            R"("ping")",
        };
        int n = 0;
        for (auto const* json : bad) {
            bool const r = refused(json, ErrorCode::BadFrame);
            if (!r)
                std::printf("  not refused: %s\n", json);
            n += r ? 1 : 0;
        }
        CHECK(n == static_cast<int>(sizeof bad / sizeof bad[0]));

        auto const r = inbound(R"({"t":"req","id":"V3N9","verb":"/pd/x","args":[{"t":"i","v":"no"}]})");
        CHECK(!r.ok && r.refusal.id == "V3N9");
        CHECK(r.refusal.message.find("args[0]") != std::string::npos);
        // ids are letters, digits, '_' and '-', 1..64 characters; a refused frame with a bad id has no id
        auto const badId = inbound(R"({"t":"req","id":"V1/N1","verb":"/pd/x"})");
        CHECK(!badId.ok && badId.refusal.id.empty());
        CHECK(refused(R"({"t":"req","id":"","verb":"/pd/x"})", ErrorCode::BadFrame));
        CHECK(refused(std::string(R"({"t":"req","id":")") + std::string(65, 'a') + R"(","verb":"/pd/x"})", ErrorCode::BadFrame));
        CHECK(inbound(R"({"t":"req","id":"abc-DEF_1","verb":"/pd/x"})").ok);
    }

    // 7. JSON: garbage, depth, duplicates, numbers, escapes, UTF-8.
    {
        for (char const* g : { "", "not json", "{", "{\"t\":\"ping\"", "{\"t\":\"ping\"} x", "{\"t\":'ping'}", "{t:\"ping\"}",
                               "{\"t\":\"ping\",}", "{\"t\":\"ping\" \"u\":1}", "\xff\xfe" })
            CHECK(refused(g, ErrorCode::BadFrame));
        CHECK(refused(std::string(100000, '['), ErrorCode::BadFrame)); // depth bomb: refused, not a stack overflow
        CHECK(refused(R"({"t":"ping","t":"bye"})", ErrorCode::BadFrame)); // duplicate key
        std::string wide = R"({"t":"ping")";
        for (std::size_t k = 0; k < json::kMaxMembers; ++k)
            wide += ",\"k" + std::to_string(k) + "\":0";
        CHECK(refused(wide + "}", ErrorCode::BadFrame)); // 1025 members: the duplicate scan stays bounded
        std::string err;
        CHECK(!json::parse("01", err) && !json::parse("1.", err) && !json::parse("-", err) && !json::parse(".5", err));
        CHECK(!json::parse("1e", err) && !json::parse("+1", err) && !json::parse("NaN", err) && !json::parse("tru", err));
        auto const nums = json::parse("[-0,1e3,-12.5E-1,2147483647,0.1]", err);
        CHECK(nums && nums->items.size() == 5);
        if (nums && nums->items.size() == 5) {
            CHECK(nums->items[1].number == 1000.0 && nums->items[2].number == -1.25);
            CHECK(nums->items[3].number == 2147483647.0 && nums->items[4].number == 0.1);
        }
        auto const s = json::parse(R"("a\"b\\c\/d\n\t\u00e9\ud83c\udfb9")", err);
        CHECK(s && s->string == "a\"b\\c/d\n\t\xc3\xa9\xf0\x9f\x8e\xb9");
        CHECK(!json::parse(R"("\ud83c")", err));       // lone high surrogate
        CHECK(!json::parse(R"("\udfb9x")", err));      // lone low surrogate
        CHECK(!json::parse(R"("\x")", err));           // bad escape
        CHECK(!json::parse("\"a\nb\"", err));          // raw control character
        CHECK(!json::parse("\"\xc3\"", err));          // truncated UTF-8
        CHECK(!json::parse("\"\xc0\xaf\"", err));      // overlong
        CHECK(!json::parse("\"\xed\xa0\x80\"", err));  // UTF-8 surrogate
        CHECK(json::parse("\"\xe2\x82\xac\"", err));   // the euro sign
        CHECK(json::parse("  {\"a\" : [ true , false , null ] }  ", err));
        // quote() is the inverse of the parser for any valid UTF-8 text, control characters included
        std::string const text = std::string("tab\t nl\n q\" bs\\ ctl\x01 nul") + '\0' + " \xc3\xa9";
        auto const back = json::parse(json::quote(text), err);
        CHECK(back && back->string == text);
        CHECK(json::validUtf8("\xf0\x9f\x8e\xb9") && !json::validUtf8("\xf4\x90\x80\x80")); // > U+10FFFF
    }

    // 8. Random bytes never crash the parser (run under ASan by the sanitizer build) and are refused or decoded.
    {
        std::mt19937 rng(42);
        int decoded = 0;
        std::string const alphabet = "{}[]\":,tv0123456789.eE+-\\u\" nulltruefalse\x80\xc3\xa9";
        for (int i = 0; i < 20000; ++i) {
            std::string junk(rng() % 64, ' ');
            for (auto& c : junk)
                c = alphabet[rng() % alphabet.size()];
            decoded += decodeInbound(junk).ok ? 1 : 0;
        }
        CHECK(decoded >= 0);
    }

    // 9. Hello check: token first (constant time), then protocol, then schema hash.
    {
        Hello h;
        h.protocol = 2;
        h.schemaHash = "feedfacecafebeef";
        h.token = "abcd";
        CHECK(!checkHello(h, "abcd", "feedfacecafebeef").has_value());
        auto bad = h;
        bad.token = "abce";
        auto r = checkHello(bad, "abcd", "feedfacecafebeef");
        CHECK(r && r->code == ErrorCode::Unauthorized);
        bad.token = "abc";
        CHECK(checkHello(bad, "abcd", "feedfacecafebeef")->code == ErrorCode::Unauthorized);
        bad.token = "";
        CHECK(checkHello(bad, "abcd", "feedfacecafebeef")->code == ErrorCode::Unauthorized);
        bad = h;
        bad.protocol = 1;
        CHECK(checkHello(bad, "abcd", "feedfacecafebeef")->code == ErrorCode::ProtocolUnsupported);
        bad = h;
        bad.schemaHash = "0000000000000000";
        r = checkHello(bad, "abcd", "feedfacecafebeef");
        CHECK(r && r->code == ErrorCode::SchemaMismatch && r->message.find("feedfacecafebeef") != std::string::npos);
        bad.token = "zzzz"; // a bad token wins: an unauthorized client learns nothing else
        CHECK(checkHello(bad, "abcd", "feedfacecafebeef")->code == ErrorCode::Unauthorized);
        CHECK(constantTimeEquals("same", "same") && !constantTimeEquals("same", "Same") && !constantTimeEquals("s", "same"));
        CHECK(!constantTimeEquals("", "x") && constantTimeEquals("", ""));
    }

    // 10. Outbound frames parse back with the documented fields.
    {
        Welcome w;
        w.schemaHash = mcp::v2schema::kSchemaHash;
        w.bridgeVersion = "11.18";
        w.bootToken = "123-abc";
        w.sessionId = "V7";
        w.caps = { "census", "protocol_v2", "we\"ird" };
        std::string err;
        auto const v = json::parse(encodeWelcome(w), err);
        CHECK(v && v->find("t") && v->find("t")->string == "welcome");
        CHECK(v && v->find("protocol") && v->find("protocol")->number == 2);
        CHECK(v && v->find("schemaHash")->string == mcp::v2schema::kSchemaHash);
        CHECK(v && v->find("sessionId")->string == "V7" && v->find("bootToken")->string == "123-abc");
        CHECK(v && v->find("bridgeVersion")->string == "11.18");
        CHECK(v && v->find("caps")->items.size() == 3 && v->find("caps")->items[2].string == "we\"ird");
        auto const* limits = v ? v->find("limits") : nullptr;
        CHECK(limits && limits->find("maxFrameBytes") && limits->find("maxFrameBytes")->number == mcp::v2schema::kMaxFrameBytes);
        CHECK(limits && limits->find("helloTimeoutMs")->number == mcp::v2schema::kHelloTimeoutMs);
        CHECK(limits && limits->find("inFlightPerSession")->number == static_cast<double>(mcp::v2schema::kInFlightPerSession));
        CHECK(limits && limits->find("sessionQueueFrames")->number == static_cast<double>(mcp::v2schema::kSessionQueueFrames));
        CHECK(v && !v->find("token")); // the bridge never sends the token

        auto const b = json::parse(encodeBye(ErrorCode::Unauthorized, "bad token"), err);
        CHECK(b && b->find("t")->string == "bye" && b->find("reason")->string == "unauthorized");
        CHECK(b && b->find("error") && b->find("error")->find("code")->string == "unauthorized");
        CHECK(b && b->find("error")->find("message")->string == "bad token");

        auto const p = json::parse(encodePong(), err);
        CHECK(p && p->find("t")->string == "pong");

        auto const rj = json::parse(encodeRejected("V7N1", ErrorCode::ProtocolUnsupported, "later"), err);
        CHECK(rj && rj->find("t")->string == "rep" && rj->find("id")->string == "V7N1");
        CHECK(rj && rj->find("final")->kind == json::Value::Kind::Bool && rj->find("final")->boolean);
        CHECK(rj && rj->find("outcome")->string == "rejected" && rj->find("error")->find("code")->string == "protocol_unsupported");
    }

    // 11. Typed args round trip (float32 exactly, int32, UTF-8 strings); a NaN float travels as null.
    {
        std::vector<Arg> args(4);
        args[0].type = 'f';
        args[0].f = 0.1f;
        args[1].type = 'i';
        args[1].i = -7;
        args[2].type = 's';
        args[2].s = "caf\xc3\xa9 \"x\"";
        args[3].type = 'f';
        args[3].f = 3.4028235e38f;
        std::string const req = R"({"t":"req","id":"V1N1","verb":"/pd/x","args":)" + encodeArgs(args) + "}";
        auto const r = decodeInbound(req);
        CHECK(r.ok && r.frame.req.args.size() == 4);
        if (r.ok && r.frame.req.args.size() == 4) {
            CHECK(r.frame.req.args[0].f == 0.1f && r.frame.req.args[1].i == -7);
            CHECK(r.frame.req.args[2].s == args[2].s && r.frame.req.args[3].f == args[3].f);
        }
        Arg nan;
        nan.type = 'f';
        nan.f = std::nanf("");
        CHECK(encodeArgs({ nan }) == R"([{"t":"f","v":null}])");
        Arg ninf;
        ninf.type = 'f';
        ninf.f = -std::numeric_limits<float>::infinity();
        CHECK(encodeArgs({ ninf }) == R"([{"t":"f","v":"-inf"}])");
        auto const back = decodeInbound(R"({"t":"req","id":"V1N1","verb":"/pd/x","args":)" + encodeArgs({ nan, ninf }) + "}");
        CHECK(back.ok && back.frame.req.args.size() == 2 && std::isnan(back.frame.req.args[0].f) && back.frame.req.args[1].f == ninf.f);
        CHECK(refused(R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[{"t":"f","v":"nan"}]})", ErrorCode::BadFrame));
    }

    // 12. S2 review H1/H2: a node budget bounds memory and time; the duplicate-key check is O(n log n).
    {
        std::string err;
        CHECK(!json::parse("[0,0,0,0]", err, 4) && err.find("too many values") != std::string::npos);
        CHECK(json::parse("[0,0,0]", err, 4).has_value());
        std::string zeros = "[0";
        for (std::size_t i = 0; i < json::kMaxNodes + 10; ++i)
            zeros += ",0";
        CHECK(refused(R"({"t":"req","id":"V1N1","verb":"/pd/x","args":)" + zeros + "]}", ErrorCode::BadFrame));
        // ~2000 objects of 1024 keys each (the review's 7.3 s probe): now refused fast by the budget, and a
        // budget-sized document of wide objects parses in well under a second
        std::string wideObjects = "[";
        for (int o = 0; o < 2000; ++o) {
            wideObjects += o ? ",{" : "{";
            for (int k = 0; k < 1024; ++k)
                wideObjects += (k ? ",\"" : "\"") + std::to_string(k) + "\":0";
            wideObjects += "}";
        }
        wideObjects += "]";
        auto const t0 = std::chrono::steady_clock::now();
        CHECK(!json::parse(wideObjects, err));
        std::string within = "[";
        for (int o = 0; o < 200; ++o) {
            within += o ? ",{" : "{";
            for (int k = 0; k < 1024; ++k)
                within += (k ? ",\"" : "\"") + std::to_string(k) + "\":0";
            within += "}";
        }
        within += "]";
        CHECK(json::parse(within, err).has_value());
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 1500);
        CHECK(!json::parse(R"({"b":1,"a":2,"c":3,"a":4})", err) && err.find("duplicate") != std::string::npos);
    }

    // 13. S2 review M1: truncation never splits a UTF-8 sequence, so refusals and log lines stay valid UTF-8.
    {
        CHECK(json::truncateUtf8("ab\xc3\xa9", 3) == "ab");
        CHECK(json::truncateUtf8("ab\xc3\xa9", 4) == "ab\xc3\xa9");
        CHECK(json::truncateUtf8("\xf0\x9f\x8e\xb9x", 3).empty());
        std::string kind = "zap";
        for (int i = 0; i < 20; ++i)
            kind += "\xc3\xa9";
        auto const r = decodeInbound(R"({"t":")" + kind + "\"}");
        CHECK(!r.ok && json::validUtf8(r.refusal.message));
        std::string err;
        CHECK(json::parse(encodeBye(r.refusal.code, r.refusal.message), err).has_value());
    }

    // 14. Request args are capped; the decoder's limit can be raised after the handshake; welcome limits follow it.
    {
        std::string many = R"({"t":"req","id":"V1N1","verb":"/pd/x","args":[)";
        for (std::size_t i = 0; i <= kMaxArgs; ++i)
            many += i ? R"(,{"t":"i","v":1})" : R"({"t":"i","v":1})";
        CHECK(refused(many + "]}", ErrorCode::BadFrame));
        FrameDecoder d(kPreAuthFrameBytes);
        std::vector<std::string> out;
        auto const big = encodeFrame(std::string(8000, ' '));
        d.setMaxFrameBytes(1 << 20);
        CHECK(d.feed(big.data(), big.size(), out) == FrameDecoder::Error::None && out.size() == 1);
        Welcome w;
        w.maxFrameBytes = 1234;
        std::string err;
        auto const v = json::parse(encodeWelcome(w), err);
        CHECK(v && v->find("limits")->find("maxFrameBytes")->number == 1234);
    }

    if (failures == 0)
        std::printf("v2_codec_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
