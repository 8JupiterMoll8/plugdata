/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The v2 listener (Plan 4 P4.1 S2): loopback TCP, length-prefixed JSON frames (MCPBridgeV2Codec.h), a token
// handshake, and a discovery file. JUCE-free POSIX sockets, so teardown can shut a socket down (waking its threads)
// and close it only after they are joined, and so Tests/mcp/v2_server_test.cpp runs it with plain g++.
//
//   threads   V2Accept  accepts, reaps finished sessions
//             V2Read    one per session: decode frames, answer envelope frames, never wait on the bridge
//             V2Write   one per session (mcp::SerialWorker): the only writer of that socket
//   handshake the first frame must be hello{protocol, schemaHash, token} within helloTimeoutMs; the answer is
//             welcome{...} or bye{reason, error{code, message}}, after which the bridge closes
//   file      <discoveryPath> (0600, temp + rename): {pid, port, token, bootToken, bridgeVersion, protocol,
//             schemaHash, startedAt}; removed at stop only while it still holds this server's pid and token
//
// One listener per process (a DAW may host several plugdata bridges): a second start() is refused.
// The server answers hello, ping and bye itself. A `req` goes to ServerConfig::onRequest (S3: the bridge admits it
// and posts it to its command worker), a `status` to ServerConfig::onStatus (S4: the idempotency store); without
// those callbacks the answer is rep{rejected, protocol_unsupported}.
// Not on Windows yet (start() returns false; v1 UDP is unaffected).
#pragma once

#include "MCPBridgeV2Codec.h"
#include "MCPBridgeV2Router.h" // Sink

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcp::v2 {

inline constexpr std::size_t kMaxSessions = 16;       // authenticated; more are answered bye{busy} and closed
inline constexpr std::size_t kMaxPendingSessions = 8; // waiting for hello; then the oldest (if older than
inline constexpr int kEvictAfterMs = 250;              // kEvictAfterMs) gets bye{busy}, else the newcomer does
inline constexpr std::size_t kSessionQueueBytes = 64u << 20; // per session write queue, with sessionQueueFrames

// What the bridge answers a `req` with at once (a refusal). closeSession: `frame` is a bye and the session ends.
struct RequestAnswer {
    std::string frame;
    bool closeSession = false;
};

struct ServerConfig {
    int preferredPort = 0;     // tried first; taken or 0 = an ephemeral port
    std::string discoveryPath; // required (a listener whose token nobody can read is refused); see defaultDiscoveryPath()
    std::string bridgeVersion;
    std::string bootToken;
    std::vector<std::string> caps;
    int helloTimeoutMs = v2schema::kHelloTimeoutMs;
    std::uint32_t maxFrameBytes = v2schema::kMaxFrameBytes;
    std::size_t sessionQueueFrames = v2schema::kSessionQueueFrames;
    std::size_t sessionQueueBytes = kSessionQueueBytes;
    std::function<void(std::string const&)> log; // any thread; never sees the token
    // V2Read thread. Returns a frame to send at once (a refusal), or nullopt when the request was taken. Must not
    // wait for the bridge (it only posts).
    std::function<std::optional<RequestAnswer>(int session, Request request)> onRequest;
    // V2Read thread, S4: the answer to `status{id, key}` (a frame); answered here, never queued behind a handler.
    std::function<std::string(int session, Request request)> onStatus;
    std::function<void(int session, bool telemetry)> onSessionOpened; // after welcome
    std::function<void(int session)> onSessionClosed;                 // a welcomed session ended
    std::function<void()> onTick;                                     // V2Accept, about every 50 ms
};

class Session;
struct ServerShared; // config + token, shared with the sessions

class Server : public Sink {
public:
    Server();
    ~Server() override;
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;

    // False with `reason` set when there is no listener: another bridge of this process holds it, no random
    // source, no socket, no discovery path, or the discovery file could not be written.
    // start() and stop() are called from one thread at a time (the bridge's owner); sessions run on their own.
    bool start(ServerConfig config, std::string& reason);
    // Sends bye{shutting_down} to every session, closes them, removes the discovery file. Idempotent.
    void stop();

    bool running() const { return isRunning.load(); }
    int port() const { return boundPort.load(); }
    std::size_t sessionCount() const;

    // Queues a frame on session `session` (the number in its id "V<n>"); dropped when it is gone. Any thread.
    void send(int session, std::string const& payload) override;

private:
    void acceptLoop();
    void reapFinished();
    void log(std::string const& line) const;

    std::shared_ptr<ServerShared const> shared;
    int listenFd = -1;
    std::atomic<int> boundPort { 0 };
    std::atomic<bool> isRunning { false };
    std::atomic<bool> stopping { false };
    bool ownsProcessSlot = false;
    std::thread acceptThread;
    mutable std::mutex sessionsLock;
    std::vector<std::unique_ptr<Session>> sessions;
};

// 32 bytes from the OS random source, as 64 lowercase hex digits. False when there is no source.
bool randomToken(std::string& hex);

// $PLUGDATA_BRIDGE_DISCOVERY, else (Linux) $XDG_CONFIG_HOME (absolute) or ~/.config, (macOS)
// ~/Library/Application Support, + /plugdata/mcp/bridge-v2.json; "" without HOME. Read at call time, so a test
// that moves HOME first gets its own file.
std::string defaultDiscoveryPath();

} // namespace mcp::v2
