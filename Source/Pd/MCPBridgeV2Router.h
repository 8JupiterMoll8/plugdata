/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// v2 reply routing (Plan 4 P4.1 S3; docs/plans/p4.1-steps.md "Reply routing"). JUCE-free, unit-tested by
// Tests/mcp/v2_router_test.cpp.
//
// A request admitted from a session becomes a WORK token (Work): the command worker holds it while the handler
// runs, and every hop that carries work to another thread copies it (captureWork + WorkScope: runInPdContext,
// postMessage, runJob, McpHost::onMessageThread/postMessage/enqueuePd). The token's last release is "done": the
// handler and everything it posted have finished, so every reply they sent is already in the reply queue ahead of
// the done marker. Each message the bridge sends carries the ORIGIN of the work running on its thread, and on the
// reply thread route() decides:
//   1. its last address segment is a pending request's corr token: that request's reply (final if the schema's
//      pattern says so; a token match with no declared pattern is a side reply); a token that finished recently is
//      dropped (a late reply never leaks to v1);
//   2. else, with an origin, the address is one of the origin verb's constant patterns: the origin's reply
//      (rule 2b: an `uncorrelated` pattern is final only for a request sent without an id); any other non-event
//      message with an origin belonged to v2 and is dropped;
//   3. else it is an event if the schema lists it: telemetry to every subscribed session and to UDP, artist to the
//      editor only (S6: the session that changed the patch last, else the newest session; never UDP while a v2
//      session is connected); anything else is a v1 reply: UDP only.
// Session ownership (S6, O1-O2): a request whose verb needs the edit lease (effect patch, live or file) makes its
// session the editor. The session it displaced gets one owner_changed{by} frame; nothing is sent when the editor
// does not change. Reads and ui/session verbs never change the editor.
// Finals the handlers never send are synthesized: rejected{unknown_verb | busy | bad_frame | deadline |
// shutting_down} at admission, start or while still queued; unknown{no_reply_verb} when a sync or fire-and-forget
// request is done without one; unknown{timeout} for an async request past its deadline after it started;
// unknown{too_large} for a reply over the frame limit.
//
// Threads: admit() on a V2Read thread, begin() on the command worker, route()/handlerDone()/tick() on the reply
// thread, sessionOpened()/sessionClosed() on V2Read threads. One mutex; the sink is called outside it.
#pragma once

#include "MCPBridgeV2Codec.h"
#include "MCPBridgeV2Store.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcp::v2 {

inline constexpr std::size_t kInFlightBytesPerSession = 64u << 20; // admitted request bytes not yet done
inline constexpr std::int64_t kMaxDeadlineMs = 600000;              // the store's TTL: a longer one is clamped
inline constexpr std::size_t kRecentTokens = 1024;                  // finished tokens whose late replies are dropped
inline constexpr std::size_t kMaxNameBytes = 64;                    // a session's client name in the status line

// Where frames go: the server's sessions.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void send(int session, std::string const& payload) = 0;
};

// The v2 request a piece of code is running for.
struct Origin {
    int session = 0;
    std::string id;
    std::string corrToken; // what the handler puts as the last address segment; "" = none
    v2schema::VerbMeta const* verb = nullptr;
    bool noReply = false;
    std::int64_t deadlineMs = 0; // effective: the request's, else the verb's, clamped; 0 = none
    std::int64_t startBy = 0;    // ms on the router's clock: not started by then = rejected{deadline}; 0 = none
    std::size_t bytes = 0;       // counted against the session's in-flight budget until done
    std::string key;             // S4: idempotency key ("" = none); its final goes into the store
};
using OriginPtr = std::shared_ptr<Origin const>;

// One admitted request while its handler or anything it posted may still reply. The destructor reports "done".
class Work {
public:
    Work(OriginPtr o, std::function<void(OriginPtr const&)> done)
        : origin(std::move(o))
        , onDone(std::move(done))
    {
    }
    ~Work()
    {
        if (onDone)
            onDone(origin);
    }
    Work(Work const&) = delete;
    Work& operator=(Work const&) = delete;

    // The request was answered another way (it could not be queued): report nothing.
    void disarm() { onDone = nullptr; }

    OriginPtr const origin;

private:
    std::function<void(OriginPtr const&)> onDone;
};
using WorkPtr = std::shared_ptr<Work>;

// The work of the code running on this thread: a plain pointer into the innermost WorkScope (trivially
// destructible, so no TLS destructor is registered on host threads).
inline thread_local WorkPtr const* currentWork = nullptr;

inline WorkPtr captureWork() { return currentWork ? *currentWork : nullptr; }
inline OriginPtr currentOrigin() { return currentWork && *currentWork ? (*currentWork)->origin : nullptr; }

// Runs code for `work` (null = for no request) on this thread; restores the outer work at the end.
class WorkScope {
public:
    explicit WorkScope(WorkPtr work)
        : held(std::move(work))
        , previous(currentWork)
    {
        currentWork = &held;
    }
    ~WorkScope() { currentWork = previous; } // `held` is released after: possibly the last reference
    WorkScope(WorkScope const&) = delete;
    WorkScope& operator=(WorkScope const&) = delete;

private:
    WorkPtr held;
    WorkPtr const* previous;
};

// An outgoing bridge message, converted from juce::OSCMessage.
struct OutMessage {
    std::string address;
    std::vector<Arg> args;
};

inline std::string encodeRep(std::string_view id, bool final, std::string_view outcome, std::string_view address,
    std::vector<Arg> const& args, std::optional<std::pair<ErrorCode, std::string>> const& error = std::nullopt)
{
    std::string out = std::string(R"({"t":"rep","id":)") + json::quote(id) + ",\"final\":" + (final ? "true" : "false");
    if (final)
        out += ",\"outcome\":" + json::quote(outcome);
    out += ",\"address\":" + json::quote(address) + ",\"args\":" + encodeArgs(args);
    if (error)
        out += ",\"error\":" + encodeError(error->first, error->second);
    return out + "}";
}

// A final with no handler reply behind it.
inline std::string encodeSynth(std::string_view id, v2schema::Outcome outcome, ErrorCode code, std::string_view message)
{
    return std::string(R"({"t":"rep","id":)") + json::quote(id) + R"(,"final":true,"outcome":)"
        + json::quote(v2schema::kOutcomeNames[static_cast<int>(outcome)]) + ",\"error\":" + encodeError(code, message) + "}";
}

inline std::string encodeEvt(bool artist, std::string_view address, std::vector<Arg> const& args)
{
    return std::string(R"({"t":"evt","class":)") + (artist ? "\"artist\"" : "\"telemetry\"") + ",\"address\":"
        + json::quote(address) + ",\"args\":" + encodeArgs(args) + "}";
}

// An envelope event the bridge itself sends to one session (S6: owner_changed); listed in the schema's sessionEvents.
inline std::string encodeSessionEvt(std::string_view address, std::vector<Arg> const& args)
{
    return std::string(R"({"t":"evt","class":"session","address":)") + json::quote(address) + ",\"args\":" + encodeArgs(args) + "}";
}

// The editor changed: `to` (the displaced editor) is told who now changes the patch.
struct OwnerChange {
    int to = 0;
    std::string by;
};

class Router {
public:
    explicit Router(Sink& out, std::size_t maxFrame = v2schema::kMaxFrameBytes)
        : sink(out)
        , maxFrameBytes(maxFrame)
    {
    }

    struct Admission {
        OriginPtr origin;                 // set: run the request with this origin (wrap it in a Work)
        std::optional<std::string> reply; // set: send this to the session now (a refusal), do not run
        bool closeSession = false;        // the reply is a bye: the client broke the protocol
    };

    // A `req` from `session` (V2Read thread). `verb` is the schema row the bridge resolved, nullptr = unknown.
    Admission admit(int session, Request const& req, v2schema::VerbMeta const* verb, std::int64_t now)
    {
        if (!verb)
            return refusal(req.id, ErrorCode::UnknownVerb, "no verb " + json::truncateUtf8(req.verb, 128));
        auto origin = std::make_shared<Origin>(); // filled here, frozen (OriginPtr) once admitted
        origin->session = session;
        origin->id = req.id;
        origin->corrToken = corrTokenOf(req, *verb);
        origin->verb = verb;
        origin->noReply = req.noReply;
        auto const deadlineMs = std::min(req.deadlineMs > 0 ? req.deadlineMs : static_cast<std::int64_t>(verb->deadlineMs), kMaxDeadlineMs);
        origin->deadlineMs = deadlineMs;
        origin->startBy = deadlineMs > 0 ? now + deadlineMs : 0;
        origin->bytes = req.verb.size() + 16;
        for (auto const& a : req.args)
            origin->bytes += a.s.size() + 8;
        std::uint64_t const fingerprint = verb->keyRequired ? fingerprintOf(req.verb, req.args) : 0;

        FinalPtr replay;
        std::optional<OwnerChange> owner;
        Admission out;
        {
            std::lock_guard<std::mutex> const l(lock);
            out = admitLocked(session, req, std::move(origin), fingerprint, now, replay, owner);
        }
        if (replay)
            out.reply = encodeFinal(req.id, *replay, true); // ran before: not again (encoded outside the lock)
        if (owner)
            sink.send(owner->to, encodeSessionEvt("owner_changed", { textArg(owner->by) }));
        return out;
    }

    // The worker is about to run the request: false (and the final sent, if one is owed) when it is too late.
    bool begin(OriginPtr const& origin, std::int64_t now)
    {
        if (!origin)
            return true;
        bool const late = origin->startBy > 0 && now > origin->startBy;
        {
            std::lock_guard<std::mutex> const l(lock);
            if (notToRun.erase(origin.get()))
                return false; // tick() answered rejected{deadline} and removed it
            auto const it = mine(*origin);
            if (it != entries.end()) {
                if (it->second.claimed)
                    return false; // tick() already answered rejected{deadline}: it must not run now
                if (!late) {
                    it->second.started = true;
                    it->second.doneBy = origin->deadlineMs > 0 ? now + origin->deadlineMs : 0;
                    return true;
                }
                it->second.claimed = true;
            } else if (!late) {
                return true; // a closed session's request runs (S4's store will keep its result)
            }
        }
        finish(*origin, synth("rejected", ErrorCode::Deadline, "not started within its deadline: nothing was applied"), false, now);
        return false;
    }

    // True when the message should also (or only) go to the v1 UDP target.
    bool route(OutMessage const& m, Origin const* origin, std::int64_t now)
    {
        // Rule 1
        auto const slash = m.address.find_last_of('/');
        auto const token = slash == std::string::npos ? std::string() : m.address.substr(slash + 1);
        if (!token.empty()) {
            std::unique_lock<std::mutex> l(lock);
            auto const found = byToken.find(token);
            if (found != byToken.end()) {
                auto const it = entries.find(found->second);
                if (it == entries.end()) {
                    byToken.erase(found); // cannot happen (indexes are erased together); never dereference
                } else {
                    auto const o = it->second.origin;
                    l.unlock();
                    deliver(*o, patternFinal(*o->verb, m.address, token, false).value_or(false), m, now);
                    return false;
                }
            } else if (recentSet.count(token)) {
                return false; // a late reply to a v2 request that already has its final
            }
        }
        // Rule 2 (and 2b)
        if (origin) {
            if (auto const final = patternFinal(*origin->verb, m.address, {}, origin->corrToken.empty())) {
                bool known = false;
                if (!origin->noReply) {
                    std::lock_guard<std::mutex> const l(lock);
                    known = mine(*origin) != entries.end();
                }
                if (known)
                    deliver(*origin, *final, m, now);
                return false;
            }
        }
        // Rule 3
        auto const* event = eventOf(m.address);
        if (!event)
            return origin == nullptr; // with an origin it was this v2 request's message: never v1's
        std::vector<int> to;
        bool udp = true;
        {
            std::lock_guard<std::mutex> const l(lock);
            if (event->artist) {
                if (auto const target = artistTargetLocked()) {
                    to.push_back(*target); // O2: the editor, else the newest session
                    udp = false;
                }
            } else {
                for (auto const& s : sessions)
                    if (s.telemetry)
                        to.push_back(s.no);
            }
        }
        if (!to.empty()) {
            auto const frame = encodeEvt(event->artist, m.address, m.args);
            if (frame.size() <= maxFrameBytes)
                for (int s : to)
                    sink.send(s, frame);
        }
        return udp;
    }

    // The work is done (its last reference went; queued behind every reply it sent). Releases its in-flight share;
    // a sync or fire-and-forget request without a final gets one now, an async one starts waiting for its deadline.
    void handlerDone(OriginPtr const& origin, std::int64_t now)
    {
        if (!origin)
            return;
        bool stillPending = false;
        {
            std::lock_guard<std::mutex> const l(lock);
            release(*origin);
            auto const it = mine(*origin);
            if (it != entries.end()) {
                stillPending = true;
                if (origin->verb->replyMode == v2schema::ReplyMode::Async) {
                    it->second.done = true;
                    if (it->second.doneBy == 0 && origin->deadlineMs > 0)
                        it->second.doneBy = now + origin->deadlineMs;
                    return;
                }
            }
        }
        if (!stillPending)
            return;
        finish(*origin, synth("unknown", ErrorCode::NoReplyVerb,
            origin->verb->replyMode == v2schema::ReplyMode::None ? "this verb sends no reply: it ran, its effect is not confirmed"
                                                                 : "the handler finished without a final reply"),
            true, now);
    }

    // Requests still queued past their start deadline end rejected{deadline} (begin() then skips them); async
    // requests whose work is done and whose deadline passed since they started end unknown{timeout}. A started
    // request whose work is still held (a handler or hop still running, or a hop stuck behind a wedged message
    // thread) has no router deadline: it owes its final until the work ends or its session closes.
    void tick(std::int64_t now)
    {
        std::vector<OriginPtr> notStarted, expired;
        {
            std::lock_guard<std::mutex> const l(lock);
            for (auto& [key, e] : entries) {
                if (!e.started && !e.claimed && e.origin->startBy > 0 && now > e.origin->startBy) {
                    e.claimed = true; // begin() sees it (or notToRun once the entry is gone) and does not run it
                    notToRun.insert(e.origin.get());
                    notStarted.push_back(e.origin);
                } else if (e.done && e.doneBy > 0 && now > e.doneBy) {
                    expired.push_back(e.origin);
                }
            }
        }
        for (auto const& o : notStarted)
            finish(*o, synth("rejected", ErrorCode::Deadline, "not started within its deadline: nothing was applied"), false, now);
        for (auto const& o : expired)
            finish(*o, synth("unknown", ErrorCode::Timeout, "no final reply within the deadline"), true, now);
    }

    void sessionOpened(int session, bool telemetry, std::string name = {})
    {
        std::lock_guard<std::mutex> const l(lock);
        sessions.push_back({ session, telemetry, json::truncateUtf8(name, kMaxNameBytes) });
        open.fetch_add(1);
    }

    void sessionClosed(int session)
    {
        std::lock_guard<std::mutex> const l(lock);
        for (auto it = sessions.begin(); it != sessions.end(); ++it)
            if (it->no == session) {
                sessions.erase(it);
                open.fetch_sub(1);
                break;
            }
        if (editor == session)
            editor = 0; // the next patch change makes its session the editor; nobody is told
        for (auto it = entries.begin(); it != entries.end();) {
            // S4: a keyed request completes into the store although nobody listens: a reconnect asks `status`
            if (it->second.origin->session == session && it->second.origin->key.empty()) {
                eraseIndexes(*it->second.origin);
                remember(it->second.origin->corrToken); // a later timer reply on it is v2's: dropped, not v1's
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    // `status{id, key}` (V2Read thread): the stored final under `id` (replayed), else a status frame: in_flight (the
    // asker joins: the final comes to `id` too) or unknown_key (never seen, refused before it ran, or expired).
    std::string status(int session, std::string const& id, std::string const& key, std::int64_t now)
    {
        if (key.empty())
            return statusFrame(id, key, "unknown_key");
        FinalPtr done;
        {
            std::lock_guard<std::mutex> const l(lock);
            auto const st = store.status(key, now);
            if (st.state == Store::State::InFlight) {
                // S6 follow-up: the session that owns the live request already gets its final under this id
                auto const own = entries.find(entryKey(session, id));
                bool const owns = own != entries.end() && own->second.origin->key == key;
                if (!owns)
                    store.join(key, { session, id }); // past the waiter cap it is only told; it may ask again
                return statusFrame(id, key, "in_flight");
            }
            if (st.state != Store::State::Done)
                return statusFrame(id, key, "unknown_key");
            done = st.final;
        }
        return encodeFinal(id, *done, true); // encoded outside the lock (a final can be MBs)
    }

    // S6: "alpha (editing), beta (watching)" for the status line; "" with no session. Newest last.
    std::string sessionsSummary() const
    {
        std::lock_guard<std::mutex> const l(lock);
        std::string out;
        for (auto const& s : sessions) {
            if (!out.empty())
                out += ", ";
            out += (s.name.empty() ? std::string("unnamed") : s.name) + (s.no == editor ? " (editing)" : " (watching)");
        }
        return out;
    }

    // The request could not be queued after admission: forget it (the caller answers it). Its Work is never made.
    void drop(OriginPtr const& origin)
    {
        if (!origin)
            return;
        std::vector<Waiter> waiters;
        {
            std::lock_guard<std::mutex> const l(lock);
            release(*origin);
            if (!origin->key.empty())
                waiters = store.abandon(origin->key);
            auto const it = mine(*origin);
            if (it != entries.end()) {
                eraseIndexes(*origin);
                entries.erase(it);
            }
        }
        for (auto const& w : waiters) // they joined between admission and the failed post: nothing ran
            sink.send(w.session, encodeSynth(w.id, v2schema::Outcome::Rejected, ErrorCode::ShuttingDown, "the bridge is not running commands"));
    }

    // The bridge is stopping: pending requests are dropped (their sessions get bye{shutting_down}); new ones refused.
    void close(std::int64_t now)
    {
        std::lock_guard<std::mutex> const l(lock);
        closed = true;
        notToRun.clear();
        for (auto const& [key, e] : entries) {
            remember(e.origin->corrToken);
            if (!e.origin->key.empty())
                store.complete(e.origin->key, std::make_shared<StoredFinal const>(synth("unknown", ErrorCode::ShuttingDown, "the bridge stopped before this request finished")), now);
        }
        entries.clear();
        byToken.clear();
        inFlight.clear();
        pending.store(0);
    }

    void reopen()
    {
        std::lock_guard<std::mutex> const l(lock);
        closed = false;
    }

    // Cheap check on every outgoing message: anything for v2 to do?
    bool active() const { return open.load() > 0 || pending.load() > 0; }
    bool hasPending() const { return pending.load() > 0; }
    std::size_t pendingCount() const { return static_cast<std::size_t>(pending.load()); }

private:
    static Admission refusal(std::string const& id, ErrorCode code, std::string const& message)
    {
        return Admission { nullptr, encodeSynth(id, v2schema::Outcome::Rejected, code, message) };
    }

    static std::string statusFrame(std::string const& id, std::string const& key, char const* state)
    {
        return std::string(R"({"t":"status","id":)") + json::quote(id) + ",\"key\":" + json::quote(key) + ",\"state\":\"" + state + "\"}";
    }

    // Caller holds `lock`. `replay` is set when the answer is a stored final (encoded by the caller, unlocked);
    // `owner` when this request changed the editor (S6: the displaced session is told by the caller, unlocked).
    Admission admitLocked(int session, Request const& req, std::shared_ptr<Origin> origin, std::uint64_t fingerprint, std::int64_t now, FinalPtr& replay,
        std::optional<OwnerChange>& owner)
    {
        auto const* verb = origin->verb;
        if (closed)
            return refusal(req.id, ErrorCode::ShuttingDown, "the bridge is stopping");
        // A verb that changes the patch always owes a final and needs a key; noReply is the realtime lane only.
        if (verb->keyRequired && req.noReply)
            return refusal(req.id, ErrorCode::BadFrame, "noReply is for realtime verbs: this verb needs a key and gets a final");
        bool const keyed = verb->keyRequired;
        if (keyed && req.key.empty())
            return refusal(req.id, ErrorCode::KeyRequired, "this verb changes something: send an idempotency key");
        // A second request with the id (or corr token) of one still in flight breaks the protocol, unless it is the
        // same keyed request again. Answering it by that id would settle the LIVE request for the client, so the
        // session is closed instead (S3 verification review M2).
        auto const protocolBreak = [&](std::string const& what) {
            return Admission { nullptr, encodeBye(ErrorCode::BadFrame, "a request with " + what + " is in flight"), true };
        };
        auto const live = entries.find(keyOf(*origin));
        // S4 first: a resend under a known key is not a duplicate (it carries the same corr token by design).
        if (keyed) {
            auto const seen = store.peek(req.key, fingerprint, now);
            switch (seen.state) {
            case Store::State::Done:
                if (live != entries.end())
                    return protocolBreak("id " + json::truncateUtf8(req.id, 64)); // that id is taken by a live request
                replay = seen.final;
                return Admission {};
            case Store::State::InFlight:
                if (live != entries.end()) {
                    if (live->second.origin->key != req.key)
                        return protocolBreak("id " + json::truncateUtf8(req.id, 64));
                    return Admission { nullptr, statusFrame(req.id, req.key, "in_flight") }; // this id gets the final anyway
                }
                if (!store.join(req.key, { session, req.id }))
                    return Admission { nullptr, statusFrame(req.id, req.key, "in_flight") }; // enough askers: ask status later
                return Admission {}; // joined: nothing to run, the running request's final comes to this id too
            case Store::State::Conflict:
                return refusal(req.id, ErrorCode::KeyConflict, "this key was used for another request");
            case Store::State::New:
                break;
            }
        }
        if (!req.noReply) {
            if (live != entries.end())
                return protocolBreak("id " + json::truncateUtf8(req.id, 64));
            if (!origin->corrToken.empty() && byToken.count(origin->corrToken))
                return protocolBreak("corr id " + json::truncateUtf8(origin->corrToken, 64));
        }
        auto const known = inFlight.find(session);
        Load const now0 = known != inFlight.end() ? known->second : Load {};
        if (now0.count >= v2schema::kInFlightPerSession)
            return refusal(req.id, ErrorCode::Busy, "more than " + std::to_string(v2schema::kInFlightPerSession) + " requests in flight");
        if (now0.bytes + origin->bytes > kInFlightBytesPerSession)
            return refusal(req.id, ErrorCode::Busy, "more than " + std::to_string(kInFlightBytesPerSession >> 20) + " MiB of requests in flight");
        if (keyed) {
            store.begin(req.key, fingerprint, now); // every check passed: the key is in flight from now
            origin->key = req.key;                  // not shared with anyone yet
        }
        auto& load = inFlight[session];
        if (!req.noReply) {
            entries.emplace(keyOf(*origin), Entry { origin });
            if (!origin->corrToken.empty())
                byToken.emplace(origin->corrToken, keyOf(*origin));
            pending.fetch_add(1);
        }
        ++load.count;
        load.bytes += origin->bytes;
        if (verb->needsLease)
            owner = takeEditorLocked(session);
        return Admission { std::move(origin), std::nullopt };
    }

    // Caller holds `lock`. The session that changes the patch becomes the editor; the one it displaces is returned
    // (only if it is still connected) so that it can be told.
    std::optional<OwnerChange> takeEditorLocked(int session)
    {
        if (editor == session)
            return std::nullopt;
        std::optional<OwnerChange> displaced;
        auto const newcomer = std::find_if(sessions.begin(), sessions.end(), [session](SessionRow const& s) { return s.no == session; });
        if (editor != 0 && hasSessionLocked(editor) && newcomer != sessions.end())
            displaced = OwnerChange { editor, newcomer->name.empty() ? std::string("unnamed") : newcomer->name };
        editor = session;
        return displaced;
    }

    // Caller holds `lock`. The artist's notes go to the editor while it is connected, else to the newest session.
    std::optional<int> artistTargetLocked() const
    {
        if (sessions.empty())
            return std::nullopt;
        if (editor != 0 && hasSessionLocked(editor))
            return editor;
        return sessions.back().no;
    }

    bool hasSessionLocked(int session) const
    {
        return std::any_of(sessions.begin(), sessions.end(), [session](SessionRow const& s) { return s.no == session; });
    }

    static Arg textArg(std::string text)
    {
        Arg a;
        a.type = 's';
        a.s = std::move(text);
        return a;
    }

    static std::string entryKey(int session, std::string const& id) { return std::to_string(session) + ":" + id; }

    struct Entry {
        OriginPtr origin;
        bool started = false;
        bool claimed = false; // its final is being sent by tick()/begin(): never run, never answered twice
        bool done = false;
        std::int64_t doneBy = 0;
    };
    struct SessionRow {
        int no;
        bool telemetry;
        std::string name; // the client's name from hello (printable, capped); "" = none
    };
    struct Load {
        std::size_t count = 0;
        std::size_t bytes = 0;
    };

    static std::string keyOf(Origin const& o) { return entryKey(o.session, o.id); }

    // Caller holds `lock`. The entry of THIS request: an id may be reused once its final went out, and a late done
    // or final of the earlier request must not settle the newer one (S3 verification review M1).
    std::unordered_map<std::string, Entry>::iterator mine(Origin const& o)
    {
        auto const it = entries.find(keyOf(o));
        return it != entries.end() && it->second.origin.get() == &o ? it : entries.end();
    }

    static bool hasArg(Request const& req, std::string const& text)
    {
        for (auto const& a : req.args)
            if (a.type == 's' && a.s == text)
                return true;
        return false;
    }

    static std::string argText(Arg const& a)
    {
        if (a.type == 's')
            return a.s;
        if (a.type == 'i')
            return std::to_string(a.i);
        return {};
    }

    // The request id when an argument carries it (the TS v2 contract: id = v1 corrId), else the verb's corr arg.
    static std::string corrTokenOf(Request const& req, v2schema::VerbMeta const& verb)
    {
        if (verb.corrArg == v2schema::kCorrNone)
            return {};
        std::string token;
        if (hasArg(req, req.id))
            token = req.id;
        else if (verb.corrArg >= 0 && static_cast<std::size_t>(verb.corrArg) < req.args.size())
            token = argText(req.args[static_cast<std::size_t>(verb.corrArg)]);
        else if (verb.corrArg == v2schema::kCorrLast && !req.args.empty())
            token = argText(req.args.back());
        return token == "0" ? std::string() : token; // handlers read "0" as "no id"
    }

    // Final flag of the verb's pattern matching `address` (with `token` for "{id}"), or nullopt. `noIdRequest`
    // makes an uncorrelated pattern final (rule 2b). Rule 1 passes the token; rule 2 matches constant patterns.
    static std::optional<bool> patternFinal(v2schema::VerbMeta const& verb, std::string_view address, std::string_view token, bool noIdRequest)
    {
        for (std::size_t i = 0; i < verb.replyCount; ++i) {
            auto const& p = v2schema::kReplies[verb.firstReply + i];
            std::string_view pattern = p.pattern;
            constexpr std::string_view idMark = "{id}";
            bool const correlated = pattern.size() > idMark.size() && pattern.substr(pattern.size() - idMark.size()) == idMark;
            if (correlated) {
                if (token.empty())
                    continue;
                auto const prefix = pattern.substr(0, pattern.size() - idMark.size());
                if (address.size() == prefix.size() + token.size() && address.substr(0, prefix.size()) == prefix
                    && address.substr(prefix.size()) == token)
                    return p.final;
            } else if (token.empty() && address == pattern) {
                return p.final && (!p.uncorrelated || noIdRequest);
            }
        }
        return token.empty() ? std::nullopt : std::optional<bool>(false); // a token match is at least a side reply
    }

    static v2schema::EventMeta const* eventOf(std::string_view address)
    {
        for (auto const& e : v2schema::kEvents)
            if (address == e.address)
                return &e;
        return nullptr;
    }

    static std::string outcomeOf(std::string_view address)
    {
        return (address.find("/error/") != std::string_view::npos || (address.size() >= 6 && address.substr(address.size() - 6) == "/error"))
            ? "rejected"
            : "applied";
    }

    static StoredFinal synth(std::string outcome, ErrorCode code, std::string message)
    {
        StoredFinal f;
        f.outcome = std::move(outcome);
        f.error = std::make_pair(code, std::move(message));
        return f;
    }

    void deliver(Origin const& o, bool final, OutMessage const& m, std::int64_t now)
    {
        auto const outcome = outcomeOf(m.address);
        if (!final) {
            auto const frame = encodeRep(o.id, false, outcome, m.address, m.args);
            if (frame.size() <= maxFrameBytes)
                sink.send(o.session, frame); // a side reply too big for a frame is dropped
            return;
        }
        StoredFinal f;
        f.outcome = outcome;
        f.address = m.address;
        f.args = m.args;
        if (outcome == "rejected") {
            std::string text;
            for (auto const& a : m.args)
                if (a.type == 's') {
                    text = a.s;
                    break;
                }
            f.error = std::make_pair(ErrorCode::HandlerError, text);
        }
        auto const size = encodeFinal(o.id, f, true).size();
        if (size > maxFrameBytes)
            f = synth("unknown", ErrorCode::TooLarge, "the reply (" + std::to_string(size) + " bytes) is over the frame limit; the request ran");
        finish(o, f, true, now);
    }

    // Sends the request's final, once (a second final for the same request is dropped). A keyed request's final
    // goes into the store when the request ran (or may have), else its key is forgotten so a resend runs; requests
    // that joined it by key get the same final under their own ids, marked replayed.
    void finish(Origin const& o, StoredFinal const& f, bool ran, std::int64_t now)
    {
        std::vector<Waiter> waiters;
        auto const shared = std::make_shared<StoredFinal const>(f); // built before the lock: stored without a copy
        {
            std::lock_guard<std::mutex> const l(lock);
            auto const it = mine(o);
            if (it == entries.end())
                return;
            eraseIndexes(o);
            entries.erase(it);
            remember(o.corrToken);
            if (!o.key.empty())
                waiters = ran ? store.complete(o.key, shared, now) : store.abandon(o.key);
        }
        sink.send(o.session, encodeFinal(o.id, f, false));
        for (auto const& w : waiters)
            sink.send(w.session, encodeFinal(w.id, f, true));
    }

    // Caller holds `lock`. The admitted share of a request whose work is done (or that never got a Work).
    void release(Origin const& o)
    {
        auto const load = inFlight.find(o.session);
        if (load == inFlight.end())
            return;
        load->second.count -= std::min<std::size_t>(load->second.count, 1);
        load->second.bytes -= std::min(load->second.bytes, o.bytes);
        if (load->second.count == 0)
            inFlight.erase(load);
    }

    // Caller holds `lock`.
    void remember(std::string const& token)
    {
        if (token.empty() || !recentSet.insert(token).second)
            return;
        recent.push_back(token);
        if (recent.size() > kRecentTokens) {
            recentSet.erase(recent.front());
            recent.pop_front();
        }
    }

    // Caller holds `lock`; the entry is erased by the caller.
    void eraseIndexes(Origin const& o)
    {
        if (!o.corrToken.empty())
            byToken.erase(o.corrToken);
        pending.fetch_sub(1);
    }

    Sink& sink;
    std::size_t const maxFrameBytes;
    mutable std::mutex lock;
    std::unordered_map<std::string, Entry> entries;       // "<session>:<id>": requests that owe a final
    std::unordered_map<std::string, std::string> byToken; // corr token -> entry key
    std::unordered_map<int, Load> inFlight;               // per session: admitted work not yet done
    std::vector<SessionRow> sessions;                     // in welcome order: back() is the newest
    int editor = 0;                                       // S6: the session that changed the patch last; 0 = none
    std::unordered_set<Origin const*> notToRun;           // answered rejected{deadline} by tick(): begin() refuses
    std::deque<std::string> recent;                       // finished corr tokens, oldest first
    std::unordered_set<std::string> recentSet;
    Store store;                                          // S4: one per boot (the router lives as long as the bridge)
    std::atomic<int> open { 0 };
    std::atomic<long> pending { 0 };
    bool closed = false;
};

} // namespace mcp::v2
