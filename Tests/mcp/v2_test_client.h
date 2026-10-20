// A blocking v2 test client (Plan 4 P4.1 S2): real loopback TCP, frames out, frames in with a timeout.
// Used by v2_server_test.cpp (g++) and bridge_handler_tests.cpp (mcp_bridge_tests). POSIX only.
#pragma once

#include "../../Source/Pd/MCPBridgeV2Codec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcp::v2::test {

#ifdef MSG_NOSIGNAL
inline constexpr int kClientSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kClientSendFlags = 0; // macOS: a dead peer raises SIGPIPE; the tests ignore it in main()
#endif

using Clock = std::chrono::steady_clock;

class Client {
public:
    explicit Client(int port, char const* host = "127.0.0.1")
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, host, &a.sin_addr);
        connected = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    }
    ~Client()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Client(Client const&) = delete;
    Client& operator=(Client const&) = delete;

    bool sendRaw(std::string const& bytes)
    {
        std::size_t done = 0;
        while (done < bytes.size()) {
            auto const n = ::send(fd, bytes.data() + done, bytes.size() - done, kClientSendFlags);
            if (n <= 0)
                return false;
            done += static_cast<std::size_t>(n);
        }
        return true;
    }
    bool sendFrame(std::string const& json) { return sendRaw(encodeFrame(json)); }

    // The next frame, or nullopt on EOF (then `eof` is set) or timeout.
    std::optional<json::Value> next(int timeoutMs = 3000)
    {
        auto const until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (!pending.empty()) {
                auto const payload = pending.front();
                pending.erase(pending.begin());
                std::string err;
                return json::parse(payload, err);
            }
            int const left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
            if (left <= 0)
                return std::nullopt;
            pollfd p { fd, POLLIN, 0 };
            if (::poll(&p, 1, left) <= 0)
                return std::nullopt;
            char buf[65536];
            auto const n = ::recv(fd, buf, sizeof buf, 0);
            if (n <= 0) {
                eof = true;
                return std::nullopt;
            }
            decoder.feed(buf, static_cast<std::size_t>(n), pending);
        }
    }

    // True when the server closed the connection within the timeout (after any frames still queued).
    bool closedByServer(int timeoutMs = 3000)
    {
        auto const until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < until && !eof)
            next(50);
        return eof;
    }

    static std::string kindOf(std::optional<json::Value> const& v)
    {
        auto const* t = v ? v->find("t") : nullptr;
        return t ? t->string : std::string("<none>");
    }

    int fd = -1;
    bool connected = false;
    bool eof = false;

private:
    FrameDecoder decoder;
    std::vector<std::string> pending;
};

inline std::string helloJson(std::string const& token, std::string const& hash = v2schema::kSchemaHash, int protocol = 2)
{
    return R"({"t":"hello","protocol":)" + std::to_string(protocol) + R"(,"schemaHash":")" + hash + R"(","token":")" + token
        + R"(","client":{"name":"v2-test"}})";
}

inline std::string errorCode(std::optional<json::Value> const& v)
{
    auto const* e = v ? v->find("error") : nullptr;
    auto const* c = e ? e->find("code") : nullptr;
    return c ? c->string : std::string("<none>");
}

// The bye a refused first chunk of bytes gets, and whether the bridge then closed: "<code>" or "<kind>/<code>/open".
inline std::string refusalOf(int port, std::string const& firstBytes)
{
    Client cl(port);
    cl.sendRaw(firstBytes);
    auto const f = cl.next();
    auto const code = errorCode(f);
    if (Client::kindOf(f) == "bye" && cl.closedByServer())
        return code;
    return Client::kindOf(f) + "/" + code + "/open";
}

} // namespace mcp::v2::test
