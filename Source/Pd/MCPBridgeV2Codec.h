/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The v2 frame codec and envelope (Plan 4 P4.1 S2; docs/protocol/v2-schema.json `frames`). JUCE-free, unit-tested
// by Tests/mcp/v2_codec_test.cpp.
//
//   frame    = 4-byte big-endian length, then that many bytes of UTF-8 JSON (one object)
//   arg      = {"t":"i","v":3} | {"t":"f","v":0.5} | {"t":"s","v":"osc~"}   (f is float32 on arrival, as in OSC;
//              NaN is {"t":"f","v":null}, infinity {"t":"f","v":"inf"} or "-inf")
//
// FrameDecoder splits a byte stream into frame payloads, at any read boundary; a zero length or a length over the
// maximum is a sticky error (the connection is closed). An HTTP request line or a TLS hello fails on its first 4
// bytes ("GET " reads as 1.2 GB). decodeInbound checks one payload against the client-to-bridge kinds; anything it
// cannot use is a Refusal{bad_frame}, which keeps the request id when the id itself was readable.
#pragma once

#include "MCPBridgeV2Json.h"
#include "MCPBridgeV2Schema.generated.h"

#include <algorithm>
#include <cfloat>
#include <limits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::v2 {

using v2schema::ErrorCode;

inline char const* codeName(ErrorCode code) { return v2schema::kErrorCodeNames[static_cast<int>(code)]; }

inline constexpr std::size_t kLengthBytes = 4;
inline constexpr std::size_t kMaxIdChars = 64;
inline constexpr std::size_t kMaxVerbChars = 256;
inline constexpr std::size_t kMaxKeyChars = 128;
inline constexpr std::size_t kMaxTextChars = 256; // hello fields, bye reason
inline constexpr std::size_t kMaxArgs = 16384;     // per request (write_bulk sends 1,024)
inline constexpr std::uint32_t kPreAuthFrameBytes = 4096; // until welcome: a hello is well under 1 KiB

inline std::string encodeFrame(std::string_view payload)
{
    auto const n = static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.reserve(kLengthBytes + payload.size());
    out += static_cast<char>((n >> 24) & 0xFF);
    out += static_cast<char>((n >> 16) & 0xFF);
    out += static_cast<char>((n >> 8) & 0xFF);
    out += static_cast<char>(n & 0xFF);
    out.append(payload);
    return out;
}

class FrameDecoder {
public:
    enum class Error { None, Empty, TooLarge };

    explicit FrameDecoder(std::uint32_t maxFrameBytes = v2schema::kMaxFrameBytes)
        : max(maxFrameBytes)
    {
    }

    // Appends every frame completed by these bytes to `out`. After an error nothing more is decoded.
    Error feed(char const* data, std::size_t size, std::vector<std::string>& out)
    {
        std::size_t i = 0;
        while (error == Error::None && i < size) {
            if (header.size() < kLengthBytes) {
                header += data[i++];
                if (header.size() < kLengthBytes)
                    continue;
                expected = (std::uint32_t(static_cast<unsigned char>(header[0])) << 24)
                    | (std::uint32_t(static_cast<unsigned char>(header[1])) << 16)
                    | (std::uint32_t(static_cast<unsigned char>(header[2])) << 8)
                    | std::uint32_t(static_cast<unsigned char>(header[3]));
                if (expected == 0)
                    error = Error::Empty;
                else if (expected > max)
                    error = Error::TooLarge;
                continue; // the body grows as bytes arrive: a bare header reserves nothing (S2 review H1)
            }
            auto const take = std::min<std::size_t>(size - i, expected - body.size());
            body.append(data + i, take);
            i += take;
            if (body.size() == expected) {
                out.push_back(std::move(body));
                body.clear();
                header.clear();
                expected = 0;
            }
        }
        return error;
    }

    // The session raises it from kPreAuthFrameBytes to maxFrameBytes after welcome.
    void setMaxFrameBytes(std::uint32_t newMax) { max = newMax; }
    Error lastError() const { return error; }
    std::uint32_t maxFrameBytes() const { return max; }
    std::size_t buffered() const { return header.size() + body.size(); }

private:
    std::uint32_t max;
    std::string header;
    std::string body;
    std::uint32_t expected = 0;
    Error error = Error::None;
};

struct Arg {
    char type = 's'; // 'i', 'f' or 's'
    std::int32_t i = 0;
    float f = 0;
    std::string s;
};

enum class Kind { Hello, Req, Status, Claim, Release, Ping, Bye };

struct Hello {
    int protocol = 0;
    std::string schemaHash;
    std::string token;
    std::string clientName;
    bool telemetry = false; // S3: logs and telemetry events wanted (artist events need no subscription)
};

struct Request {
    std::string id;
    std::string verb;
    std::string key;
    std::vector<Arg> args;
    std::int64_t deadlineMs = 0; // 0 = none given
    bool noReply = false;
};

struct Inbound {
    Kind kind = Kind::Ping;
    Hello hello;   // Kind::Hello
    Request req;   // Kind::Req; Kind::Status uses id and key
    bool force = false; // Kind::Claim
    std::string reason; // Kind::Bye
};

struct Refusal {
    ErrorCode code = ErrorCode::BadFrame;
    std::string message;
    std::string id; // the request id, when it was readable
};

struct DecodeResult {
    bool ok = false;
    Inbound frame;
    Refusal refusal;
};

namespace detail {

using json::Value;

inline bool idChars(std::string_view s)
{
    if (s.empty() || s.size() > kMaxIdChars)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

inline bool verbChars(std::string_view s)
{
    if (s.size() < 2 || s.size() > kMaxVerbChars || s[0] != '/')
        return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F)
            return false;
    return true;
}

// Optional string member: absent is fine, present must be a string of at most `maxChars`.
inline bool optionalString(Value const& v, char const* key, std::size_t maxChars, std::string& out)
{
    auto const* m = v.find(key);
    if (!m)
        return true;
    if (!m->is(Value::Kind::String) || m->string.size() > maxChars)
        return false;
    out = m->string;
    return true;
}

inline bool integral(Value const& v, double lo, double hi)
{
    return v.is(Value::Kind::Number) && std::trunc(v.number) == v.number && v.number >= lo && v.number <= hi;
}

// The smallest double that rounds to float infinity: FLT_MAX plus half its unit in the last place.
inline double const kFloatLimit = static_cast<double>(FLT_MAX) + std::ldexp(1.0, 103);

inline std::optional<std::string> readArg(Value const& a, Arg& out)
{
    if (!a.is(Value::Kind::Object))
        return "is not an object";
    auto const* t = a.find("t");
    auto const* v = a.find("v");
    if (!t || !t->is(Value::Kind::String) || t->string.size() != 1)
        return "has no type";
    if (!v)
        return "has no value";
    out.type = t->string[0];
    switch (out.type) {
    case 'i':
        if (!integral(*v, INT32_MIN, INT32_MAX))
            return "is not an int32";
        out.i = static_cast<std::int32_t>(v->number);
        return std::nullopt;
    case 'f':
        if (v->is(Value::Kind::Null)) {
            out.f = std::numeric_limits<float>::quiet_NaN(); // as encodeArgs writes them
            return std::nullopt;
        }
        if (v->is(Value::Kind::String) && (v->string == "inf" || v->string == "-inf")) {
            out.f = v->string == "inf" ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
            return std::nullopt;
        }
        if (!v->is(Value::Kind::Number) || std::fabs(v->number) >= kFloatLimit)
            return "is not a float32";
        out.f = static_cast<float>(v->number);
        return std::nullopt;
    case 's':
        if (!v->is(Value::Kind::String))
            return "is not a string";
        if (v->string.find('\0') != std::string::npos)
            return "contains NUL"; // an OSC string ends at NUL
        out.s = v->string;
        return std::nullopt;
    default:
        return "has an unknown type";
    }
}

inline DecodeResult refuse(std::string message, std::string id = {})
{
    DecodeResult r;
    r.refusal.code = ErrorCode::BadFrame;
    r.refusal.message = std::move(message);
    r.refusal.id = std::move(id);
    return r;
}

inline DecodeResult readRequest(Value const& v, Kind kind)
{
    DecodeResult r;
    r.frame.kind = kind;
    auto& q = r.frame.req;
    auto const* id = v.find("id");
    if (!id || !id->is(Value::Kind::String) || !idChars(id->string))
        return refuse("id must be 1-64 letters, digits, '_' or '-'");
    q.id = id->string;
    if (!optionalString(v, "key", kMaxKeyChars, q.key))
        return refuse("key must be a string of at most 128 bytes", q.id);
    if (kind == Kind::Status) {
        r.ok = true;
        return r;
    }
    auto const* verb = v.find("verb");
    if (!verb || !verb->is(Value::Kind::String) || !verbChars(verb->string))
        return refuse("verb must be an OSC address", q.id);
    q.verb = verb->string;
    if (auto const* d = v.find("deadlineMs")) {
        if (!integral(*d, 0, 86400000.0))
            return refuse("deadlineMs must be a whole number of milliseconds", q.id);
        q.deadlineMs = static_cast<std::int64_t>(d->number);
    }
    if (auto const* n = v.find("noReply")) {
        if (!n->is(Value::Kind::Bool))
            return refuse("noReply must be a boolean", q.id);
        q.noReply = n->boolean;
    }
    if (auto const* args = v.find("args")) {
        if (!args->is(Value::Kind::Array))
            return refuse("args must be an array", q.id);
        if (args->items.size() > kMaxArgs)
            return refuse("at most " + std::to_string(kMaxArgs) + " args", q.id);
        q.args.resize(args->items.size());
        for (std::size_t i = 0; i < args->items.size(); ++i)
            if (auto const why = readArg(args->items[i], q.args[i]))
                return refuse("args[" + std::to_string(i) + "] " + *why, q.id);
    }
    r.ok = true;
    return r;
}

inline DecodeResult readHello(Value const& v)
{
    DecodeResult r;
    r.frame.kind = Kind::Hello;
    auto& h = r.frame.hello;
    auto const* protocol = v.find("protocol");
    auto const* hash = v.find("schemaHash");
    auto const* token = v.find("token");
    if (!protocol || !integral(*protocol, 0, 1000000))
        return refuse("hello.protocol must be a number");
    if (!hash || !hash->is(Value::Kind::String) || hash->string.size() > kMaxTextChars)
        return refuse("hello.schemaHash must be a string");
    if (!token || !token->is(Value::Kind::String) || token->string.size() > kMaxTextChars)
        return refuse("hello.token must be a string");
    h.protocol = static_cast<int>(protocol->number);
    h.schemaHash = hash->string;
    h.token = token->string;
    if (auto const* client = v.find("client"); client && client->is(Value::Kind::Object))
        optionalString(*client, "name", kMaxTextChars, h.clientName);
    if (auto const* t = v.find("telemetry")) {
        if (!t->is(Value::Kind::Bool))
            return refuse("hello.telemetry must be a boolean");
        h.telemetry = t->boolean;
    }
    r.ok = true;
    return r;
}

} // namespace detail

// One client-to-bridge frame: hello, req, status, claim, release, ping or bye. Unknown members are ignored.
inline DecodeResult decodeInbound(std::string_view payload)
{
    using json::Value;
    std::string error;
    auto const doc = json::parse(payload, error);
    if (!doc)
        return detail::refuse("not JSON: " + error);
    if (!doc->is(Value::Kind::Object))
        return detail::refuse("a frame is a JSON object");
    auto const* t = doc->find("t");
    if (!t || !t->is(Value::Kind::String))
        return detail::refuse("a frame needs a kind in \"t\"");
    auto const& kind = t->string;
    if (kind == "hello")
        return detail::readHello(*doc);
    if (kind == "req")
        return detail::readRequest(*doc, Kind::Req);
    if (kind == "status")
        return detail::readRequest(*doc, Kind::Status);
    DecodeResult r;
    r.ok = true;
    if (kind == "ping") {
        r.frame.kind = Kind::Ping;
    } else if (kind == "release") {
        r.frame.kind = Kind::Release;
    } else if (kind == "claim") {
        r.frame.kind = Kind::Claim;
        if (auto const* f = doc->find("force")) {
            if (!f->is(Value::Kind::Bool))
                return detail::refuse("claim.force must be a boolean");
            r.frame.force = f->boolean;
        }
    } else if (kind == "bye") {
        r.frame.kind = Kind::Bye;
        if (!detail::optionalString(*doc, "reason", kMaxTextChars, r.frame.reason))
            return detail::refuse("bye.reason must be a string");
    } else {
        return detail::refuse("unknown frame kind \"" + json::truncateUtf8(kind, 32) + "\"");
    }
    return r;
}

// Equal length and equal bytes, in time that depends only on the lengths (the token check).
inline bool constantTimeEquals(std::string_view a, std::string_view b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    std::size_t const n = a.size() > b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto const x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        auto const y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(x ^ y);
    }
    return diff == 0;
}

// Why a hello is refused, or nullopt. The token comes first, so a client without it learns nothing else.
inline std::optional<Refusal> checkHello(Hello const& h, std::string_view token, std::string_view schemaHash)
{
    if (token.empty() || !constantTimeEquals(h.token, token))
        return Refusal { ErrorCode::Unauthorized, "bad token", {} };
    if (h.protocol != v2schema::kProtocol)
        return Refusal { ErrorCode::ProtocolUnsupported,
            "this bridge speaks protocol " + std::to_string(v2schema::kProtocol) + ", not " + std::to_string(h.protocol), {} };
    if (h.schemaHash != schemaHash)
        return Refusal { ErrorCode::SchemaMismatch,
            "bridge schema " + std::string(schemaHash) + ", client schema " + json::truncateUtf8(h.schemaHash, 32), {} };
    return std::nullopt;
}

struct Welcome {
    std::string schemaHash;
    std::string bridgeVersion;
    std::string bootToken;
    std::string sessionId;
    std::vector<std::string> caps;
    std::uint32_t maxFrameBytes = v2schema::kMaxFrameBytes;
    int helloTimeoutMs = v2schema::kHelloTimeoutMs;
    std::size_t sessionQueueFrames = v2schema::kSessionQueueFrames;
    std::size_t inFlightPerSession = v2schema::kInFlightPerSession;
};

inline std::string encodeWelcome(Welcome const& w)
{
    using json::quote;
    std::string caps = "[";
    for (std::size_t i = 0; i < w.caps.size(); ++i)
        caps += (i ? "," : "") + quote(w.caps[i]);
    caps += "]";
    return std::string(R"({"t":"welcome","protocol":)") + std::to_string(v2schema::kProtocol)
        + ",\"schemaHash\":" + quote(w.schemaHash) + ",\"bridgeVersion\":" + quote(w.bridgeVersion)
        + ",\"bootToken\":" + quote(w.bootToken) + ",\"sessionId\":" + quote(w.sessionId) + ",\"caps\":" + caps
        + ",\"limits\":{\"maxFrameBytes\":" + std::to_string(w.maxFrameBytes)
        + ",\"helloTimeoutMs\":" + std::to_string(w.helloTimeoutMs)
        + ",\"sessionQueueFrames\":" + std::to_string(w.sessionQueueFrames)
        + ",\"inFlightPerSession\":" + std::to_string(w.inFlightPerSession) + "}}";
}

inline std::string encodeError(ErrorCode code, std::string_view message)
{
    return std::string("{\"code\":") + json::quote(codeName(code)) + ",\"message\":" + json::quote(message) + "}";
}

// The last frame before the bridge closes a connection.
inline std::string encodeBye(ErrorCode code, std::string_view message)
{
    return std::string(R"({"t":"bye","reason":)") + json::quote(codeName(code)) + ",\"error\":" + encodeError(code, message) + "}";
}

inline std::string encodePong() { return R"({"t":"pong"})"; }

// The final reply of a request the bridge refused before any handler ran.
inline std::string encodeRejected(std::string_view id, ErrorCode code, std::string_view message)
{
    return std::string(R"({"t":"rep","id":)") + json::quote(id) + R"(,"final":true,"outcome":"rejected","error":)"
        + encodeError(code, message) + "}";
}

inline std::string encodeArgs(std::vector<Arg> const& args)
{
    std::string out = "[";
    for (std::size_t k = 0; k < args.size(); ++k) {
        auto const& a = args[k];
        out += k ? ",{\"t\":\"" : "{\"t\":\"";
        out += a.type;
        out += "\",\"v\":";
        if (a.type == 'i')
            out += std::to_string(a.i);
        else if (a.type == 'f')
            out += json::floatText(a.f);
        else
            out += json::quote(a.s);
        out += "}";
    }
    return out + "]";
}

} // namespace mcp::v2
