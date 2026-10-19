// Unit tests for the v2 listener (Plan 4 P4.1 S2): Source/Pd/MCPBridgeV2Server.{h,cpp} over real loopback
// sockets, discovery file in a temporary directory. Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeV2Server.h"
#include "v2_test_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
using namespace mcp::v2::test;

namespace {

int msSince(Clock::time_point t)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

std::string readFile(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

struct Logs {
    std::mutex m;
    std::vector<std::string> lines;
    std::function<void(std::string const&)> sink()
    {
        return [this](std::string const& line) {
            std::lock_guard<std::mutex> l(m);
            lines.push_back(line);
        };
    }
    bool contains(std::string const& text)
    {
        std::lock_guard<std::mutex> l(m);
        for (auto const& s : lines)
            if (s.find(text) != std::string::npos)
                return true;
        return false;
    }
};

ServerConfig config(std::string const& dir, Logs& logs)
{
    ServerConfig c;
    c.preferredPort = 0;
    c.discoveryPath = dir + "/mcp/bridge-v2.json";
    c.bridgeVersion = "11.18";
    c.bootToken = "boot-1";
    c.caps = { "census", "protocol_v2" };
    c.helloTimeoutMs = 300;
    c.log = logs.sink();
    return c;
}

// The token a client reads from the discovery file.
std::string tokenFrom(std::string const& path)
{
    std::string err;
    auto const v = json::parse(readFile(path), err);
    auto const* t = v ? v->find("token") : nullptr;
    return t ? t->string : std::string();
}

int freePort()
{
    int const s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(s);
    return ntohs(a.sin_port);
}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN); // macOS test client: no MSG_NOSIGNAL
    char dirTemplate[] = "/tmp/v2_server_test_XXXXXX";
    std::string const dir = ::mkdtemp(dirTemplate);
    Logs logs;

    // 1. Token: 32 random bytes, hex; a new one each time.
    {
        std::string a, b;
        CHECK(randomToken(a) && randomToken(b));
        CHECK(a.size() == 64 && a != b);
        CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    }

    // 2. Discovery path: the override, else XDG_CONFIG_HOME (absolute only), else ~/.config.
    {
        ::setenv("HOME", "/home/someone", 1);
        ::unsetenv("PLUGDATA_BRIDGE_DISCOVERY");
        ::unsetenv("XDG_CONFIG_HOME");
        CHECK(defaultDiscoveryPath() == "/home/someone/.config/plugdata/mcp/bridge-v2.json");
        ::setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
        CHECK(defaultDiscoveryPath() == "/x/cfg/plugdata/mcp/bridge-v2.json");
        ::setenv("XDG_CONFIG_HOME", "relative/cfg", 1);
        CHECK(defaultDiscoveryPath() == "/home/someone/.config/plugdata/mcp/bridge-v2.json");
        ::setenv("PLUGDATA_BRIDGE_DISCOVERY", "/tmp/elsewhere.json", 1);
        CHECK(defaultDiscoveryPath() == "/tmp/elsewhere.json");
        ::unsetenv("PLUGDATA_BRIDGE_DISCOVERY");
        ::unsetenv("XDG_CONFIG_HOME");
        ::unsetenv("HOME");
        CHECK(defaultDiscoveryPath().empty()); // no HOME: no file, rather than one in the working directory
    }

    // 3. Start: listener on 127.0.0.1, discovery file 0600 in a 0700 directory with the documented fields.
    {
        Server server;
        std::string reason;
        auto const c = config(dir, logs);
        CHECK(server.start(c, reason));
        CHECK(server.running() && server.port() > 0);
        struct stat st {};
        CHECK(::stat(c.discoveryPath.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(::stat((dir + "/mcp").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        std::string err;
        auto const d = json::parse(readFile(c.discoveryPath), err);
        CHECK(d && d->find("pid") && d->find("pid")->number == ::getpid());
        CHECK(d && d->find("port") && d->find("port")->number == server.port());
        CHECK(d && d->find("token") && d->find("token")->string.size() == 64);
        CHECK(d && d->find("bootToken") && d->find("bootToken")->string == "boot-1");
        CHECK(d && d->find("bridgeVersion") && d->find("bridgeVersion")->string == "11.18");
        CHECK(d && d->find("protocol") && d->find("protocol")->number == 2);
        CHECK(d && d->find("schemaHash") && d->find("schemaHash")->string == mcp::v2schema::kSchemaHash);
        CHECK(d && d->find("startedAt") && d->find("startedAt")->number > 1.7e12);
        auto const token = tokenFrom(c.discoveryPath);

        // Loopback only: 127.0.0.2 is a loopback address too, but the listener is bound to 127.0.0.1 alone.
        Client other(server.port(), "127.0.0.2");
        CHECK(!other.connected);

        // 4. Good hello: welcome, then ping/pong; a request is refused by id until S3 serves it; bye closes.
        {
            Client cl(server.port());
            CHECK(cl.connected);
            CHECK(cl.sendFrame(helloJson(token)));
            auto const w = cl.next();
            CHECK(cl.kindOf(w) == "welcome");
            CHECK(w && w->find("sessionId") && w->find("sessionId")->string.size() >= 2 && w->find("sessionId")->string[0] == 'V');
            CHECK(w && w->find("caps") && w->find("caps")->items.size() == 2 && w->find("caps")->items[1].string == "protocol_v2");
            CHECK(w && w->find("bootToken")->string == "boot-1" && w->find("schemaHash")->string == mcp::v2schema::kSchemaHash);
            CHECK(w && !w->find("token"));
            CHECK(cl.sendFrame(R"({"t":"ping"})"));
            CHECK(cl.kindOf(cl.next()) == "pong");
            CHECK(cl.sendFrame(R"({"t":"req","id":"V1N1","verb":"/pd/ping"})"));
            auto const rep = cl.next();
            CHECK(cl.kindOf(rep) == "rep" && rep->find("id")->string == "V1N1" && rep->find("outcome")->string == "rejected");
            CHECK(errorCode(rep) == "protocol_unsupported");
            CHECK(cl.sendFrame(R"({"t":"req","id":"V1N2","verb":"/pd/ping","args":[{"t":"i","v":"x"}]})"));
            auto const bad = cl.next();
            CHECK(cl.kindOf(bad) == "rep" && bad->find("id")->string == "V1N2" && errorCode(bad) == "bad_frame");
            CHECK(cl.sendFrame(R"({"t":"ping"})")); // a refused request does not end the session
            CHECK(cl.kindOf(cl.next()) == "pong");
            CHECK(cl.sendFrame(R"({"t":"bye","reason":"done"})"));
            CHECK(cl.closedByServer());
        }

        // 5. Refusals: typed bye, then the bridge closes.
        auto const refusedWith = [&](std::string const& first, char const* code) {
            auto const got = refusalOf(server.port(), first);
            if (got != code)
                std::printf("  refusal %s: got %s\n", code, got.c_str());
            return got == code;
        };
        CHECK(refusedWith(encodeFrame(helloJson((token[0] == '0' ? "1" : "0") + token.substr(1))), "unauthorized"));
        CHECK(refusedWith(encodeFrame(helloJson("short")), "unauthorized"));
        CHECK(refusedWith(encodeFrame(helloJson(token, "0000000000000000")), "schema_mismatch"));
        CHECK(refusedWith(encodeFrame(helloJson(token, mcp::v2schema::kSchemaHash, 1)), "protocol_unsupported"));
        CHECK(refusedWith(encodeFrame(R"({"t":"ping"})"), "bad_frame")); // hello must come first
        CHECK(refusedWith(encodeFrame("not json"), "bad_frame"));
        CHECK(refusedWith(std::string("\0\0\0\0", 4), "bad_frame")); // zero length
        CHECK(refusedWith(std::string("\xff\xff\xff\xff", 4), "too_large"));
        // A browser: its request line is refused on the first 4 bytes, and the bye still arrives although the
        // rest of the request is unread (the bridge drains before it closes, so the close is not a reset).
        CHECK(refusedWith("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: x\r\n" + std::string(4000, 'h') + "\r\n\r\n", "too_large"));
        CHECK(!logs.contains(token)); // the token never reaches a log line

        // 6. No hello within the timeout: bye bad_frame, well before 2 s here (the test uses 300 ms).
        {
            Client cl(server.port());
            auto const t0 = Clock::now();
            auto const f = cl.next(3000);
            CHECK(cl.kindOf(f) == "bye" && errorCode(f) == "bad_frame");
            CHECK(cl.closedByServer());
            CHECK(msSince(t0) >= 250 && msSince(t0) < 1500);
        }

        // 7. After welcome: a second hello, bad JSON or an unknown kind ends the session.
        for (char const* second : { "HELLO", R"({"t":"zap"})", "{" }) {
            Client cl(server.port());
            cl.sendFrame(helloJson(token));
            CHECK(cl.kindOf(cl.next()) == "welcome");
            cl.sendFrame(std::string(second) == "HELLO" ? helloJson(token) : std::string(second));
            auto const f = cl.next();
            CHECK(cl.kindOf(f) == "bye" && errorCode(f) == "bad_frame");
            CHECK(cl.closedByServer());
        }

        // 8. Eight clients at once, 200 pings each: every pong arrives; sessions are counted and reaped.
        {
            std::atomic<int> pongs { 0 };
            std::vector<std::thread> threads;
            for (int k = 0; k < 8; ++k)
                threads.emplace_back([&] {
                    Client cl(server.port());
                    cl.sendFrame(helloJson(token));
                    if (cl.kindOf(cl.next()) != "welcome")
                        return;
                    std::string burst;
                    for (int i = 0; i < 200; ++i)
                        burst += encodeFrame(R"({"t":"ping"})");
                    cl.sendRaw(burst);
                    for (int i = 0; i < 200; ++i)
                        if (cl.kindOf(cl.next()) == "pong")
                            ++pongs;
                });
            for (auto& t : threads)
                t.join();
            CHECK(pongs.load() == 1600);
            auto const t0 = Clock::now();
            while (server.sessionCount() > 0 && msSince(t0) < 2000)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(server.sessionCount() == 0);
        }

        // 9. Session ids are distinct per connection.
        {
            Client a(server.port()), b(server.port());
            a.sendFrame(helloJson(token));
            b.sendFrame(helloJson(token));
            auto const wa = a.next(), wb = b.next();
            CHECK(wa && wb && wa->find("sessionId")->string != wb->find("sessionId")->string);
        }

        // 10. One listener per process: a second server refuses to start while this one runs.
        {
            Server second;
            std::string why;
            auto c2 = config(dir + "/second", logs);
            CHECK(!second.start(c2, why));
            CHECK(why.find("another") != std::string::npos);
            CHECK(!second.running());
        }

        // 11. stop() with a client connected: bye shutting_down, the connection closes, the file is removed.
        {
            Client cl(server.port());
            cl.sendFrame(helloJson(token));
            CHECK(cl.kindOf(cl.next()) == "welcome");
            auto const t0 = Clock::now();
            server.stop();
            CHECK(msSince(t0) < 1500);
            auto const f = cl.next(1000);
            CHECK(cl.kindOf(f) == "bye" && errorCode(f) == "shutting_down");
            CHECK(cl.closedByServer(1000));
            CHECK(!server.running());
            struct stat gone {};
            CHECK(::stat(c.discoveryPath.c_str(), &gone) != 0);
            server.stop(); // idempotent
        }
    }

    // 12. The preferred port when free; an ephemeral one when it is taken. The file is removed only if it is ours.
    {
        int const want = freePort();
        Server s;
        std::string reason;
        auto c = config(dir, logs);
        c.preferredPort = want;
        CHECK(s.start(c, reason));
        CHECK(s.port() == want);
        s.stop();

        int const holder = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(want));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(holder, 4) == 0);
        CHECK(s.start(c, reason));
        CHECK(s.port() > 0 && s.port() != want);
        CHECK(logs.contains("in use"));
        ::close(holder);

        // Another process wrote its own file over ours: stop() leaves it alone.
        {
            std::ofstream out(c.discoveryPath, std::ios::trunc);
            out << R"({"pid":1,"port":1,"token":"theirs"})";
        }
        s.stop();
        CHECK(readFile(c.discoveryPath).find("theirs") != std::string::npos);
        ::unlink(c.discoveryPath.c_str());
    }

    // 13. A client that never reads: its queue passes the cap and the bridge drops that session, nobody else.
    {
        Server s;
        std::string reason;
        auto c = config(dir, logs);
        c.sessionQueueFrames = 64;
        CHECK(s.start(c, reason));
        auto const token = tokenFrom(c.discoveryPath);
        Client slow(s.port());
        int small = 4096;
        ::setsockopt(slow.fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
        slow.sendFrame(helloJson(token));
        CHECK(slow.kindOf(slow.next()) == "welcome");
        std::string burst;
        for (int i = 0; i < 20000; ++i)
            burst += encodeFrame(R"({"t":"ping"})");
        std::thread writer([&] {
            for (int round = 0; round < 50 && slow.sendRaw(burst); ++round) { }
        });
        auto const t0 = Clock::now();
        while (!logs.contains("queue") && msSince(t0) < 10000)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(logs.contains("queue"));
        Client fine(s.port());
        fine.sendFrame(helloJson(token));
        CHECK(fine.kindOf(fine.next()) == "welcome");
        ::shutdown(slow.fd, SHUT_RDWR);
        writer.join();
        s.stop();
    }

    // 14. S2 review: pre-auth bounds. No discovery path, no listener; frames before welcome are at most 4 KiB; a
    // fifth silent connection evicts the oldest; a seventeenth authenticated one gets bye{busy}; logs stay UTF-8.
    {
        Server none;
        std::string why;
        auto c0 = config(dir, logs);
        c0.discoveryPath.clear();
        CHECK(!none.start(c0, why) && why.find("discovery") != std::string::npos);

        Server s;
        auto c = config(dir, logs);
        c.helloTimeoutMs = 4000;
        CHECK(s.start(c, why));
        auto const token = tokenFrom(c.discoveryPath);
        CHECK(refusalOf(s.port(), encodeFrame(helloJson(token).substr(0, 10) + std::string(5000, ' '))) == "too_large");

        std::vector<std::unique_ptr<Client>> silent;
        for (std::size_t i = 0; i < kMaxPendingSessions; ++i)
            silent.push_back(std::make_unique<Client>(s.port()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // all pending and young: the newcomer is refused (it may retry), nobody is evicted
        CHECK(refusalOf(s.port(), encodeFrame(helloJson(token))) == "busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(kEvictAfterMs));
        Client fifth(s.port()); // now the oldest silent one gives way
        auto const evicted = silent[0]->next(2000);
        CHECK(Client::kindOf(evicted) == "bye" && errorCode(evicted) == "busy");
        CHECK(fifth.sendFrame(helloJson(token)) && Client::kindOf(fifth.next()) == "welcome");
        silent.clear();
        auto const settle = [&](std::size_t n) {
            auto const t0 = Clock::now();
            while (s.sessionCount() > n && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 3000)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
        settle(1);

        std::vector<std::unique_ptr<Client>> authed;
        for (std::size_t i = 1; i < kMaxSessions; ++i) { // `fifth` is the first
            authed.push_back(std::make_unique<Client>(s.port()));
            authed.back()->sendFrame(helloJson(token));
            CHECK(Client::kindOf(authed.back()->next()) == "welcome");
        }
        CHECK(refusalOf(s.port(), encodeFrame(helloJson(token))) == "busy");
        authed.clear();
        settle(1);

        std::string name;
        for (int i = 0; i < 40; ++i)
            name += "\xc3\xa9";
        Client named(s.port());
        named.sendFrame(R"({"t":"hello","protocol":2,"schemaHash":")" + std::string(mcp::v2schema::kSchemaHash) + R"(","token":")" + token
            + R"(","client":{"name":")" + name + "\x7f\"}}");
        CHECK(Client::kindOf(named.next()) == "welcome");
        named.sendFrame(R"({"t":"zap)" + name + "\"}");
        auto const bye = named.next();
        CHECK(Client::kindOf(bye) == "bye" && errorCode(bye) == "bad_frame"); // parsed: the bridge sent valid UTF-8
        s.stop();
        std::lock_guard<std::mutex> l(logs.m);
        bool allValid = true;
        for (auto const& line : logs.lines)
            allValid = allValid && json::validUtf8(line) && line.find('\x7f') == std::string::npos;
        CHECK(allValid);
    }

    // 15. S3 callbacks: a request goes to onRequest (its refusal is sent back), the session is reported opened with
    // its telemetry flag and closed when it ends, send() reaches a session by number, onTick runs.
    {
        Server s;
        std::string why;
        auto c = config(dir, logs);
        std::mutex m;
        std::vector<std::string> seen;
        std::vector<std::pair<int, bool>> opened;
        std::vector<int> closed;
        std::atomic<int> ticks { 0 };
        c.onRequest = [&](int session, Request r) -> std::optional<RequestAnswer> {
            std::lock_guard<std::mutex> l(m);
            seen.push_back(std::to_string(session) + " " + r.id + " " + r.verb);
            if (r.verb == "/pd/refuse")
                return RequestAnswer { encodeRejected(r.id, ErrorCode::UnknownVerb, "no"), false };
            if (r.verb == "/pd/close_me")
                return RequestAnswer { encodeBye(ErrorCode::BadFrame, "reused id"), true };
            return std::nullopt;
        };
        c.onSessionOpened = [&](int session, bool telemetry, std::string const&) { std::lock_guard<std::mutex> l(m); opened.emplace_back(session, telemetry); };
        c.onSessionClosed = [&](int session) { std::lock_guard<std::mutex> l(m); closed.push_back(session); };
        c.onTick = [&] { ++ticks; };
        CHECK(s.start(c, why));
        auto const token = tokenFrom(c.discoveryPath);
        int sessionNo = 0;
        {
            Client cl(s.port());
            cl.sendFrame(R"({"t":"hello","protocol":2,"schemaHash":")" + std::string(mcp::v2schema::kSchemaHash) + R"(","token":")" + token
                + R"(","telemetry":true})");
            auto const w = cl.next();
            CHECK(Client::kindOf(w) == "welcome");
            sessionNo = w ? std::atoi(w->find("sessionId")->string.c_str() + 1) : 0;
            cl.sendFrame(R"({"t":"req","id":"V1N1","verb":"/pd/census","args":[{"t":"s","v":"main"}]})");
            cl.sendFrame(R"({"t":"req","id":"V1N2","verb":"/pd/refuse"})");
            auto const r = cl.next();
            CHECK(Client::kindOf(r) == "rep" && r->find("id")->string == "V1N2" && errorCode(r) == "unknown_verb");
            s.send(sessionNo, R"({"t":"evt","class":"telemetry","address":"/pd/log","args":[]})");
            CHECK(Client::kindOf(cl.next()) == "evt");
            s.send(sessionNo + 1000, R"({"t":"pong"})"); // no such session: dropped
            cl.sendFrame(R"({"t":"req","id":"V1N3","verb":"/pd/close_me"})");
            auto const bye = cl.next();
            CHECK(Client::kindOf(bye) == "bye" && errorCode(bye) == "bad_frame");
            CHECK(cl.closedByServer());
        }
        auto const t0 = Clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 2000) {
            {
                std::lock_guard<std::mutex> l(m);
                if (!closed.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        s.stop();
        std::lock_guard<std::mutex> l(m);
        CHECK(seen.size() == 3 && seen[0] == std::to_string(sessionNo) + " V1N1 /pd/census");
        CHECK(opened.size() == 1 && opened[0].first == sessionNo && opened[0].second);
        CHECK(closed.size() == 1 && closed[0] == sessionNo);
        CHECK(ticks.load() > 0);
    }

    std::string const cleanup = "rm -rf '" + dir + "'";
    if (std::system(cleanup.c_str()) != 0)
        std::printf("  (could not remove %s)\n", dir.c_str());
    if (failures == 0)
        std::printf("v2_server_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
