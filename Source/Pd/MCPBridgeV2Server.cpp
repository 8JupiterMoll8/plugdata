/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The v2 listener (Plan 4 P4.1 S2). See MCPBridgeV2Server.h; threads in docs/mcp-bridge/THREADING.md.
//
// Before the token check any local process (or a web page through the browser) can talk to this code, so the
// pre-auth path is bounded: 4 KiB frames, 8 pending connections (then one is refused), the hello deadline,
// rate-limited log lines. An exception in a session (e.g. bad_alloc) ends that session only: it never reaches
// std::terminate, which would take the host (a DAW) down.
#include "MCPBridgeV2Server.h"

#if defined(_WIN32)

namespace mcp::v2 {

struct ServerShared { };
class Session { };

Server::Server() = default;
Server::~Server() = default;
bool Server::start(ServerConfig, std::string& reason)
{
    reason = "the v2 listener is not available on Windows yet";
    return false;
}
void Server::stop() { }
std::size_t Server::sessionCount() const { return 0; }
void Server::acceptLoop() { }
void Server::reapFinished() { }
void Server::log(std::string const&) const { }
void Server::send(int, std::string const&) { }
bool randomToken(std::string&) { return false; }
std::string defaultDiscoveryPath() { return {}; }

} // namespace mcp::v2

#else

#include "MCPBridgeWorker.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mcp::v2 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollMs = 50;           // how often a blocked reader or the acceptor looks at its stop flag
constexpr int kDrainAfterByeMs = 500; // after our bye: read and discard until the peer closes, at most this
constexpr int kDrainOnStopMs = 200;   // the same during stop(), which the app's quit waits for
constexpr int kAcceptBackoffMs = 100; // out of descriptors: wait instead of spinning on a readable listener
constexpr std::size_t kMaxRefusing = 4;      // sessions only draining a bye{busy}; beyond: closed at once
constexpr int kPreAuthLogsPerMinute = 10;

std::atomic<bool> processSlotTaken { false };
std::atomic<int> nextSessionNo { 1 };

// Control characters and DEL out, length capped on a UTF-8 boundary: client-supplied text goes into log lines.
std::string printable(std::string const& s)
{
    std::string out = json::truncateUtf8(s, 64);
    for (auto& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            c = '?';
    return out;
}

void setCloseOnExec(int fd) { ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC); }

void setBlocking(int fd, bool blocking)
{
    int const flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A non-blocking listening socket on 127.0.0.1:port, close-on-exec from birth where the OS allows; -1 with errno.
int listenOn(int port)
{
#ifdef SOCK_CLOEXEC
    int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (fd < 0)
        return -1;
    setCloseOnExec(fd);
#if !defined(__APPLE__)
    // Linux: lets a restart bind past TIME_WAIT; a live listener on the port is still refused. Not on macOS, where it
    // would let us bind 127.0.0.1:port next to another service's *:port and take its loopback traffic.
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) {
        int const e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    setBlocking(fd, false); // a connection reset between poll and accept must not block accept (BSD)
    return fd;
}

// The next connection, blocking and close-on-exec, with TCP_NODELAY; -1 with errno.
int acceptOne(int listenFd)
{
#if defined(__linux__)
    int const fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    int const fd = ::accept(listenFd, nullptr, nullptr);
#endif
    if (fd < 0)
        return -1;
    setCloseOnExec(fd);
    setBlocking(fd, true); // BSD: an accepted socket inherits O_NONBLOCK
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); // macOS: no MSG_NOSIGNAL
#endif
    return fd;
}

int localPort(int fd)
{
    sockaddr_in a {};
    socklen_t len = sizeof a;
    return ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0 ? ntohs(a.sin_port) : 0;
}

bool writeAll(int fd, char const* data, std::size_t size)
{
    while (size > 0) {
        auto const n = ::send(fd, data, size, kSendFlags);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool makeDirs(std::string const& dir)
{
    if (dir.empty())
        return true;
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    auto const slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !makeDirs(dir.substr(0, slash)))
        return false;
    return ::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
}

// Mode 0600, written to a fresh temporary name and renamed, so a reader never sees half a file.
bool writePrivateFile(std::string const& path, std::string const& content, std::string& error)
{
    auto const slash = path.find_last_of('/');
    if (slash != std::string::npos && !makeDirs(path.substr(0, slash))) {
        error = "cannot create the directory of " + path + ": " + std::strerror(errno);
        return false;
    }
    auto const tmp = path + ".tmp." + std::to_string(::getpid());
    ::unlink(tmp.c_str()); // left by a crash of an earlier process with the same pid
    int const fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    for (std::size_t done = 0; ok && done < content.size();) {
        auto const n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        ok = n > 0;
        done += ok ? static_cast<std::size_t>(n) : 0;
    }
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot write " + path + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string readSmallFile(std::string const& path)
{
    int const fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return {};
    std::string out;
    char buf[4096];
    for (;;) {
        auto const n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || out.size() > 65536)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

} // namespace

struct ServerShared {
    ServerConfig config;
    std::string token;

    void log(std::string const& line) const
    {
        if (config.log)
            config.log(line);
    }

    // Lines a client can cause before it proved the token: at most kPreAuthLogsPerMinute, then one note.
    void logPreAuth(std::string const& line) const
    {
        bool emit = false, note = false;
        {
            std::lock_guard<std::mutex> const l(limiterLock);
            auto const now = Clock::now();
            if (now - windowStart > std::chrono::minutes(1)) {
                windowStart = now;
                inWindow = 0;
            }
            ++inWindow;
            emit = inWindow <= kPreAuthLogsPerMinute;
            note = inWindow == kPreAuthLogsPerMinute + 1;
        }
        if (emit)
            log(line);
        else if (note)
            log("MCP bridge v2: more refused connections this minute are not logged");
    }

    mutable std::mutex limiterLock;
    mutable Clock::time_point windowStart {};
    mutable int inWindow = 0;
};

// One connection: its reader thread (this object's loop) and its writer.
class Session {
public:
    // With `refusal` set the session only sends that bye and closes (the busy path, drained like any other).
    Session(int socketFd, std::shared_ptr<ServerShared const> s, std::optional<Refusal> refusal = std::nullopt)
        : fd(socketFd)
        , shared(std::move(s))
        , sessionNo(nextSessionNo.fetch_add(1))
        , sessionId("V" + std::to_string(sessionNo))
        , decoder(std::min(kPreAuthFrameBytes, shared->config.maxFrameBytes))
        , refuseWith(std::move(refusal))
    {
    }

    ~Session() { join(); }

    void start()
    {
        writer.start([this](Out out) { write(std::move(out)); });
        reader = std::thread([this] { run(); });
    }

    void requestStop() { stopRequested.store(true); }
    void evict() { evicted.store(true); }
    bool finished() const { return done.load(); }
    bool authenticated() const { return authed.load(); }
    bool refusing() const { return refuseWith.has_value(); }
    bool isClosing() const { return closing.load(); }
    int number() const { return sessionNo; }

    // Any thread. Nothing is queued once the session is closing (the bye and the FIN are the last frames).
    void post(std::string const& payload)
    {
        if (closing.load())
            return;
        enqueue(encodeFrame(payload));
    }
    void postFrame(std::string frame) // already encoded
    {
        if (closing.load())
            return;
        enqueue(std::move(frame));
    }
    Clock::time_point acceptedAt() const { return accepted; }
    void join()
    {
        if (reader.joinable())
            reader.join();
    }

private:
    struct Out {
        std::string bytes;
        bool shutdownWrite = false;
    };

    void write(Out out)
    {
        if (out.shutdownWrite) {
            ::shutdown(fd, SHUT_WR); // FIN after everything queued before it
            return;
        }
        queuedBytes.fetch_sub(out.bytes.size());
        if (!writeAll(fd, out.bytes.data(), out.bytes.size()))
            aborted.store(true); // the peer is gone; the reader ends the session
    }

    void enqueue(std::string frame)
    {
        auto const size = frame.size();
        auto const bytes = queuedBytes.fetch_add(size) + size;
        if (writer.pending() >= shared->config.sessionQueueFrames || bytes > shared->config.sessionQueueBytes) {
            queuedBytes.fetch_sub(size);
            if (!aborted.exchange(true))
                shared->log("MCP bridge v2: session " + sessionId + " write queue over "
                    + std::to_string(shared->config.sessionQueueFrames) + " frames or "
                    + std::to_string(shared->config.sessionQueueBytes) + " bytes (client not reading): closed");
            return;
        }
        if (!writer.post(Out { std::move(frame), false }))
            queuedBytes.fetch_sub(size); // not running: nothing queued
    }

    void beginDrain(int drainMs)
    {
        closing.store(true);
        writer.post(Out { {}, true });
        draining = true;
        drainUntil = Clock::now() + std::chrono::milliseconds(drainMs);
    }

    // The last frame: bye with a typed error, then FIN, then drain until the peer closes.
    void fail(ErrorCode code, std::string const& message, int drainMs = kDrainAfterByeMs)
    {
        if (code != ErrorCode::ShuttingDown) {
            auto const line = "MCP bridge v2: session " + sessionId + " closed: " + codeName(code) + " (" + printable(message) + ")";
            welcomed ? shared->log(line) : shared->logPreAuth(line);
        }
        if (!closing.load())
            enqueue(encodeFrame(encodeBye(code, message)));
        beginDrain(drainMs);
    }

    void welcome(Hello const& hello)
    {
        welcomed = true;
        authed.store(true);
        decoder.setMaxFrameBytes(shared->config.maxFrameBytes);
        clientName = printable(hello.clientName);
        Welcome w;
        w.schemaHash = v2schema::kSchemaHash;
        w.bridgeVersion = shared->config.bridgeVersion;
        w.bootToken = shared->config.bootToken;
        w.sessionId = sessionId;
        w.caps = shared->config.caps;
        w.maxFrameBytes = shared->config.maxFrameBytes;
        w.helloTimeoutMs = shared->config.helloTimeoutMs;
        w.sessionQueueFrames = shared->config.sessionQueueFrames;
        post(encodeWelcome(w));
        if (shared->config.onSessionOpened)
            shared->config.onSessionOpened(sessionNo, hello.telemetry);
        shared->log("MCP bridge v2: session " + sessionId + " connected (" + (clientName.empty() ? "unnamed" : clientName) + ")");
    }

    void handle(std::string const& payload)
    {
        auto const r = decodeInbound(payload);
        if (!r.ok) {
            if (welcomed && !r.refusal.id.empty())
                post(encodeRejected(r.refusal.id, r.refusal.code, r.refusal.message)); // answered by id, session goes on
            else
                fail(r.refusal.code, r.refusal.message);
            return;
        }
        auto const& f = r.frame;
        if (!welcomed) {
            if (f.kind != Kind::Hello)
                return fail(ErrorCode::BadFrame, "the first frame must be hello");
            if (auto const refusal = checkHello(f.hello, shared->token, v2schema::kSchemaHash))
                return fail(refusal->code, refusal->message);
            return welcome(f.hello);
        }
        switch (f.kind) {
        case Kind::Hello:
            return fail(ErrorCode::BadFrame, "hello after welcome");
        case Kind::Ping:
            return post(encodePong());
        case Kind::Bye:
            shared->log("MCP bridge v2: session " + sessionId + " said bye (" + printable(f.reason) + ")");
            return beginDrain(kDrainAfterByeMs);
        case Kind::Req:
            if (shared->config.onRequest) {
                if (auto const answer = shared->config.onRequest(sessionNo, f.req)) {
                    if (!answer->closeSession)
                        return post(answer->frame);
                    shared->log("MCP bridge v2: session " + sessionId + " closed: bad_frame (a request id reused while in flight)");
                    post(answer->frame); // the bye
                    beginDrain(kDrainAfterByeMs);
                }
                return;
            }
            [[fallthrough]];
        case Kind::Status:
            return post(encodeRejected(f.req.id, ErrorCode::ProtocolUnsupported, "this bridge does not serve v2 requests yet"));
        case Kind::Claim:
        case Kind::Release:
            return; // the edit lease comes with S6
        }
    }

    // One pass of the reader: false when the session is over.
    bool step(Clock::time_point helloDeadline, std::vector<char>& buf, std::vector<std::string>& frames)
    {
        auto const now = Clock::now();
        if (draining) {
            if (now >= drainUntil)
                return false;
        } else if (stopRequested.load()) {
            fail(ErrorCode::ShuttingDown, "the bridge is stopping", kDrainOnStopMs);
        } else if (evicted.load()) {
            fail(ErrorCode::Busy, "too many connections are waiting for hello");
        } else if (!welcomed && now >= helloDeadline) {
            fail(ErrorCode::BadFrame, "no hello within " + std::to_string(shared->config.helloTimeoutMs) + " ms");
        }
        pollfd p { fd, POLLIN, 0 };
        int const ready = ::poll(&p, 1, kPollMs);
        if (ready < 0)
            return errno == EINTR;
        if (ready == 0)
            return true;
        auto const n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n == 0)
            return false; // the peer closed
        if (n < 0)
            return errno == EINTR || errno == EAGAIN;
        if (draining)
            return true; // discarded: closing with unread input would reset the connection and lose our bye
        frames.clear();
        auto const error = decoder.feed(buf.data(), static_cast<std::size_t>(n), frames);
        for (auto const& payload : frames) {
            handle(payload);
            if (draining || aborted.load())
                return true;
        }
        if (error != FrameDecoder::Error::None)
            fail(error == FrameDecoder::Error::TooLarge ? ErrorCode::TooLarge : ErrorCode::BadFrame,
                error == FrameDecoder::Error::TooLarge
                    ? "a frame is at most " + std::to_string(decoder.maxFrameBytes()) + " bytes here"
                    : std::string("a frame is never empty"));
        return true;
    }

    void run()
    {
        try {
            if (refuseWith)
                fail(refuseWith->code, refuseWith->message);
            auto const helloDeadline = Clock::now() + std::chrono::milliseconds(shared->config.helloTimeoutMs);
            std::vector<char> buf(65536);
            std::vector<std::string> frames;
            while (!aborted.load() && step(helloDeadline, buf, frames)) { }
        } catch (std::exception const& e) {
            shared->log("MCP bridge v2: session " + sessionId + " ended by an internal error: " + e.what());
        } catch (...) {
            shared->log("MCP bridge v2: session " + sessionId + " ended by an internal error");
        }
        closing.store(true); // late replies from the bridge are dropped from here on
        if (authed.load() && shared->config.onSessionClosed)
            shared->config.onSessionClosed(sessionNo);
        ::shutdown(fd, SHUT_RDWR); // wakes a writer blocked on a full socket
        writer.stop();
        ::close(fd);
        done.store(true);
    }

    int const fd;
    std::shared_ptr<ServerShared const> shared;
    int const sessionNo;
    std::string const sessionId;
    Clock::time_point const accepted = Clock::now();
    FrameDecoder decoder;
    std::optional<Refusal> const refuseWith;
    std::string clientName;
    bool welcomed = false; // reader thread only
    bool draining = false; // reader thread only
    Clock::time_point drainUntil;
    std::atomic<bool> authed { false };
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> evicted { false };
    std::atomic<bool> closing { false };
    std::atomic<bool> aborted { false };
    std::atomic<bool> done { false };
    std::atomic<std::size_t> queuedBytes { 0 };
    std::thread reader;
    mcp::SerialWorker<Out> writer; // last: stopped (joined) by the reader before fd closes
};

Server::Server() = default;

Server::~Server() { stop(); }

void Server::log(std::string const& line) const
{
    if (shared)
        shared->log(line);
}

bool Server::start(ServerConfig config, std::string& reason)
{
    if (isRunning.load()) {
        reason = "already running";
        return false;
    }
    if (config.discoveryPath.empty()) {
        reason = "no discovery file path (HOME unset?): nobody could read the token";
        return false;
    }
    bool expected = false;
    if (!processSlotTaken.compare_exchange_strong(expected, true)) {
        reason = "another bridge in this process holds the v2 listener";
        return false;
    }
    int fd = -1;
    // Everything below either completes or gives the slot (and the socket) back.
    auto const giveUp = [&](std::string why) {
        if (fd >= 0)
            ::close(fd);
        processSlotTaken.store(false);
        reason = std::move(why);
        return false;
    };
    try {
        auto s = std::make_shared<ServerShared>();
        s->config = std::move(config);
        if (!randomToken(s->token))
            return giveUp("no OS random source for the token");
        int const preferred = s->config.preferredPort;
        fd = preferred > 0 ? listenOn(preferred) : -1;
        if (fd < 0 && preferred > 0)
            s->log("MCP bridge v2: TCP 127.0.0.1:" + std::to_string(preferred) + " in use (" + std::strerror(errno) + "), using an ephemeral port");
        if (fd < 0)
            fd = listenOn(0);
        if (fd < 0)
            return giveUp(std::string("cannot listen on 127.0.0.1: ") + std::strerror(errno));
        int const port = localPort(fd);

        auto const startedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string const body = "{\"pid\":" + std::to_string(::getpid()) + ",\"port\":" + std::to_string(port)
            + ",\"token\":" + json::quote(s->token) + ",\"bootToken\":" + json::quote(s->config.bootToken)
            + ",\"bridgeVersion\":" + json::quote(s->config.bridgeVersion) + ",\"protocol\":" + std::to_string(v2schema::kProtocol)
            + ",\"schemaHash\":" + json::quote(v2schema::kSchemaHash) + ",\"startedAt\":" + std::to_string(startedAt) + "}\n";
        std::string error;
        if (!writePrivateFile(s->config.discoveryPath, body, error))
            return giveUp("discovery file: " + error);

        shared = s;
        listenFd = fd;
        boundPort.store(port);
        stopping.store(false);
        acceptThread = std::thread([this] { acceptLoop(); });
    } catch (std::exception const& e) {
        return giveUp(std::string("v2 listener: ") + e.what());
    }
    ownsProcessSlot = true;
    isRunning.store(true);
    log("MCP bridge v2 listening on TCP 127.0.0.1:" + std::to_string(boundPort.load()));
    return true;
}

void Server::stop()
{
    if (!isRunning.exchange(false))
        return;
    stopping.store(true);
    if (acceptThread.joinable())
        acceptThread.join();
    ::close(listenFd);
    listenFd = -1;

    std::vector<std::unique_ptr<Session>> closing;
    {
        std::lock_guard<std::mutex> const l(sessionsLock);
        closing.swap(sessions);
    }
    for (auto& session : closing)
        session->requestStop(); // all at once: each drains for at most kDrainOnStopMs
    closing.clear();             // joins

    // Another process may have written its own file since: remove it only while it is still ours.
    auto const& path = shared->config.discoveryPath;
    std::string error;
    auto const doc = json::parse(readSmallFile(path), error);
    auto const* pid = doc ? doc->find("pid") : nullptr;
    auto const* tok = doc ? doc->find("token") : nullptr;
    if (pid && tok && pid->number == ::getpid() && tok->is(json::Value::Kind::String) && constantTimeEquals(tok->string, shared->token))
        ::unlink(path.c_str());
    boundPort.store(0);
    if (ownsProcessSlot) {
        ownsProcessSlot = false;
        processSlotTaken.store(false);
    }
    log("MCP bridge v2 stopped");
}

std::size_t Server::sessionCount() const
{
    std::lock_guard<std::mutex> const l(sessionsLock);
    std::size_t n = 0;
    for (auto const& s : sessions)
        n += s->finished() ? 0 : 1;
    return n;
}

void Server::send(int session, std::string const& payload)
{
    auto frame = encodeFrame(payload); // the copy is made outside the lock
    std::lock_guard<std::mutex> const l(sessionsLock);
    for (auto const& s : sessions)
        if (s->number() == session && !s->finished()) {
            s->postFrame(std::move(frame)); // only queues: never waits on the socket
            return;
        }
}

void Server::reapFinished()
{
    std::vector<std::unique_ptr<Session>> finished;
    {
        std::lock_guard<std::mutex> const l(sessionsLock);
        for (auto it = sessions.begin(); it != sessions.end();) {
            if ((*it)->finished()) {
                finished.push_back(std::move(*it));
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }
    // destroyed (joined) here, outside the lock
}

void Server::acceptLoop()
{
    while (!stopping.load()) {
        try {
            pollfd p { listenFd, POLLIN, 0 };
            int const ready = ::poll(&p, 1, kPollMs);
            reapFinished();
            if (shared->config.onTick)
                shared->config.onTick();
            if (ready <= 0)
                continue;
            int const fd = acceptOne(listenFd);
            if (fd < 0) {
                if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
                    std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptBackoffMs));
                continue;
            }
            std::size_t authed = 0, pending = 0, refusing = 0;
            Session* oldestPending = nullptr;
            {
                std::lock_guard<std::mutex> const l(sessionsLock);
                for (auto const& s : sessions) {
                    if (s->finished() || s->isClosing())
                        continue; // a closing session only drains; it holds no slot
                    if (s->refusing()) {
                        ++refusing;
                    } else if (s->authenticated()) {
                        ++authed;
                    } else {
                        ++pending;
                        if (!oldestPending || s->acceptedAt() < oldestPending->acceptedAt())
                            oldestPending = s.get();
                    }
                }
            }
            // A real client sends hello at once; one still silent after kEvictAfterMs gives way to the newcomer. If
            // all are younger (a burst of connects), the newcomer is refused instead and may retry.
            bool const pendingFull = pending >= kMaxPendingSessions;
            bool const evictOldest = pendingFull && oldestPending
                && Clock::now() - oldestPending->acceptedAt() >= std::chrono::milliseconds(kEvictAfterMs);
            if (evictOldest)
                oldestPending->evict();
            std::optional<Refusal> refusal;
            if (pendingFull && !evictOldest) {
                if (refusing >= kMaxRefusing) {
                    ::close(fd);
                    continue;
                }
                refusal = Refusal { ErrorCode::Busy, "too many connections are waiting for hello", {} };
            } else if (authed >= kMaxSessions) {
                if (refusing >= kMaxRefusing) {
                    ::close(fd);
                    continue;
                }
                refusal = Refusal { ErrorCode::Busy, "too many v2 sessions (" + std::to_string(kMaxSessions) + ")", {} };
                shared->logPreAuth("MCP bridge v2: refused a connection: " + std::to_string(kMaxSessions) + " sessions open");
            }
            std::unique_ptr<Session> session;
            try {
                session = std::make_unique<Session>(fd, shared, std::move(refusal));
                session->start();
            } catch (...) {
                session.reset(); // start() threw before its reader ran, so nothing else uses fd
                ::close(fd);
                throw;
            }
            std::lock_guard<std::mutex> const l(sessionsLock);
            sessions.push_back(std::move(session));
        } catch (std::exception const& e) {
            log(std::string("MCP bridge v2: accept: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptBackoffMs));
        }
    }
}

bool randomToken(std::string& hex)
{
    unsigned char bytes[32];
    int const fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::size_t got = 0;
    while (got < sizeof bytes) {
        auto const n = ::read(fd, bytes + got, sizeof bytes - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != sizeof bytes)
        return false;
    static constexpr char digits[] = "0123456789abcdef";
    hex.clear();
    for (auto b : bytes) {
        hex += digits[b >> 4];
        hex += digits[b & 0xF];
    }
    return true;
}

std::string defaultDiscoveryPath()
{
    if (char const* given = std::getenv("PLUGDATA_BRIDGE_DISCOVERY"); given && *given)
        return given;
    char const* home = std::getenv("HOME");
    if (!home || !*home)
        return {};
#if defined(__APPLE__)
    std::string const base = std::string(home) + "/Library/Application Support";
#else
    char const* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string const base = (xdg && xdg[0] == '/') ? std::string(xdg) : std::string(home) + "/.config";
#endif
    return base + "/plugdata/mcp/bridge-v2.json";
}

} // namespace mcp::v2

#endif
