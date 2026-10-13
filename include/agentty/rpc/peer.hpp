#pragma once
// agentty::rpc::Peer — one JSON-RPC connection, driven by agentty on maya.
//
// jsonrpc-cpp's Engine is a pure state machine: frames and time in, effects
// out, no thread anywhere. This is the runtime around it, and the only place
// in agentty that is (docs/PROTOCOL_LIBRARIES.md §6):
//
//   * ONE owner. The engine lives in a maya::guarded; every transition runs
//     under it. Reads from the wire, our own requests, deadlines and close
//     all go through step(), so they are serialised by construction.
//   * a reader task (maya::pool, isolated) reads the inbound byte channel,
//     splits lines (jsonrpc::LineSplitter) and steps Received for each;
//   * a deadline task sleeps on maya::delay_for until next_deadline() and
//     steps Tick;
//   * a writer task drains an ordered outbox, so frames never interleave
//     and no lock is held across a write.
//
// Outbound requests are blocking-style for the caller (`call<M>(params)`
// returns the typed result), because both of agentty's integrations already
// call from a worker thread that may wait: the caller sleeps until its own
// completion is parked, never holding the engine.
//
// The peer's calls and notifications go to a dispatch callback the owner
// supplies; it runs on the reader thread, outside the engine lock, so a
// handler may itself call<> the peer without deadlocking.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <maya/runtime.hpp>

#include <jsonrpc/jsonrpc.hpp>

namespace agentty::rpc {

// ── Lock order ─────────────────────────────────────────────────────────────
//
// A Peer holds two LEAF locks (its engine and its outbox) and never holds one
// across I/O. So, by capability (agentty/util/lock_levels.hpp):
//
//   below<leaf>   write-side calls that only queue a frame or step the
//                 engine: notify, notify_raw, reply, cancel, Batch,
//                 set_request_meta, closed. Callable from any non-leaf
//                 lock body.
//   unlocked      calls that WAIT for another thread: call/call_raw (waits
//                 for the reply), wait_closed, stop (joins the tasks).
//
// The dispatch callbacks run on the Peer's reader thread, between frames,
// holding nothing, so they are handed that thread's `unlocked`: a handler can
// make a nested call() or take any lock in rank order.

// A byte channel to the peer. Both functions block; read returns 0 at EOF
// (or on error), write returns false when the channel is gone.
//
// read runs on the Peer's reader thread and write on its writer thread;
// each holds nothing and passes its `unlocked` (an in-memory channel waits
// on a lock inside read). interrupt is called by stop(), also `unlocked`.
struct Channel {
    std::function<std::size_t(const maya::unlocked&, char* buf, std::size_t cap)> read;
    std::function<bool(const maya::unlocked&, std::string_view bytes)>            write;
    // Wake a read blocked in `read` so the reader can exit (close the fd,
    // shut the socket). Called once, at stop.
    std::function<void(const maya::unlocked&)>                                    interrupt;
};

// Raw read(2)/write(2) on two fds, e.g. our own stdin/stdout (0, 1).
[[nodiscard]] Channel fd_channel(int in_fd, int out_fd);

// A child's pipes as streams (util::ChildProcess's out()/in()). The
// owner keeps the streams alive for the peer's life.
[[nodiscard]] Channel stream_channel(std::istream& in, std::ostream& out);

// Two in-memory channels wired to each other: what one writes, the other
// reads. Interrupting either end closes both. For tests and loopback.
[[nodiscard]] std::pair<Channel, Channel> channel_pair();

// How a call may end early. A stop on `cancel` runs `on_cancel` (say, to
// tell the peer) and fails the call `cancel_grace` later if the peer hasn't
// answered by then; zero grace fails it at once. `timeout` of zero means no
// deadline; unset means the peer's default.
//
// The stop on `cancel` may be requested from any thread, holding anything,
// so the stop callback only raises a flag. The CALLER -- the thread waiting
// in call(), which holds nothing -- notices it within a few milliseconds and
// then runs on_cancel, sends the cancel notification and settles the call.
struct CallOptions {
    std::optional<std::chrono::milliseconds> timeout;
    std::stop_token                          cancel;
    // Runs on the calling thread, which holds nothing.
    std::function<void(const maya::unlocked&)> on_cancel;
    std::chrono::milliseconds                cancel_grace{0};
    // Also send $/cancel_request with this call's id, so the peer can drop
    // the request (a permission dialog, a form).
    bool                                     notify_peer = false;
};

// What the owner does with the peer's calls and notifications. Return the
// reply frame for a call to have it written now, or Nothing to answer later
// with Peer::reply (a deferred method).
struct Dispatch {
    // Called on the reader thread, which holds nothing: they get its
    // `unlocked`. A handler may make nested calls or take locks.
    std::function<jsonrpc::Maybe<std::string>(const maya::unlocked&, const jsonrpc::Call&)>
        on_call;
    std::function<void(const maya::unlocked&, const jsonrpc::Notification&)> on_notification;
    // Every frame in and out, for a wire trace. Optional.
    std::function<void(bool inbound, std::string_view frame)>        trace;
};

} // namespace agentty::rpc

// The peer calls a Channel's and a Dispatch's functions from its own reader,
// writer and ticker threads, read and write at the same time. Handing one to
// a Peer is the claim that this is safe; jaal can't look inside a closure to
// check it. Every channel in rpc/peer.cpp keeps it: read and write touch
// separate ends, and interrupt only closes them.
MAYA_SYNC(agentty::rpc::Channel);
MAYA_SYNC(agentty::rpc::Dispatch);

namespace agentty::rpc {

class Peer {
public:
    using Clock = jsonrpc::Clock;

    Peer(Channel ch, Dispatch d, std::string_view name);
    // Stops and joins, like stop(). A destructor can't be handed `unlocked`;
    // it may wait anyway because a Peer is marked joins_on_destroy, so no
    // maya::guarded<T> can own one and it is never destroyed inside a lock
    // body. Prefer calling stop(cap) explicitly first.
    ~Peer();
    Peer(const Peer&)            = delete;
    Peer& operator=(const Peer&) = delete;

    // Start reading. Separate from construction so the owner can finish
    // wiring before the first frame arrives.
    void start();

    // Stop reading, fail every waiting call with ConnectionLost, join.
    // Joins: `unlocked`.
    void stop(const maya::unlocked&) noexcept;

    // Block until the reader has seen EOF. For a server whose life is the
    // connection's. Waits: `unlocked`.
    void wait_closed(const maya::unlocked&);

    // Deadline for calls that pass none. Zero means no deadline.
    void set_default_timeout(std::chrono::milliseconds d) { default_timeout_ = d; }
    void set_request_meta(const maya::below<maya::leaf>&, jsonrpc::Json meta);

    // ── to the peer ─────────────────────────────────────────────────────

    // A typed request, waited for. Errors (the peer's, a timeout, a closed
    // connection, a cancel, a result that doesn't decode) come back as
    // RpcError. Waits for the reply: `unlocked`.
    template <jsonrpc::IsMethod M>
    std::expected<typename M::result, jsonrpc::RpcError>
    call(const maya::unlocked& cap, const typename M::params& p, CallOptions o = {}) {
        auto c = call_raw(cap, M::name, jsonrpc::to_json(p), std::move(o));
        return jsonrpc::result<M>(c);
    }

    // The untyped form: the completion as the engine returned it.
    jsonrpc::Completed call_raw(const maya::unlocked&, std::string_view method,
                                jsonrpc::Json params, CallOptions o = {});

    template <jsonrpc::IsNote N>
    void notify(const maya::below<maya::leaf>& cap, const typename N::params& p) {
        write(cap, jsonrpc::notify<N>(p));
    }
    void notify_raw(const maya::below<maya::leaf>& cap, std::string_view method,
                    jsonrpc::Json params) {
        write(cap, jsonrpc::notify_raw(method, std::move(params)));
    }

    // Answer a call the dispatch deferred.
    void reply(const maya::below<maya::leaf>& cap, const jsonrpc::Id& id,
               std::expected<jsonrpc::Json, jsonrpc::RpcError> outcome) {
        write(cap, jsonrpc::reply(id, std::move(outcome)));
    }
    template <jsonrpc::IsMethod M>
    void reply(const maya::below<maya::leaf>& cap, const jsonrpc::Id& id,
               const typename M::result& r) {
        write(cap, jsonrpc::reply<M>(id, r));
    }

private:
    struct Core;   // shared with the tasks, so an abandoned reader outlives us
    using core_t = maya::co_owned<Core>;
    static void write_core(const maya::below<maya::leaf>& cap, const core_t& c, std::string frame);

public:

    // A deferred call's answer, sent at most once. The first ok()/error()
    // wins; later ones are dropped. It co-owns the peer's core, so a worker
    // holding one can outlive the Peer: the answer is just dropped then.
    template <jsonrpc::IsMethod M>
    class Deferred {
    public:
        Deferred(Peer& p, jsonrpc::Id id) : c_(p.core_), id_(std::move(id)) {}
        Deferred(Deferred&&) noexcept            = default;
        Deferred& operator=(Deferred&&) noexcept = default;

        [[nodiscard]] const jsonrpc::Id& id() const { return id_; }
        void ok(const maya::below<maya::leaf>& cap, const typename M::result& r) {
            if (std::exchange(done_, true)) return;
            write_core(cap, c_, jsonrpc::reply<M>(id_, r));
        }
        void error(const maya::below<maya::leaf>& cap, int code, std::string msg) {
            if (std::exchange(done_, true)) return;
            write_core(cap, c_,
                       jsonrpc::reply(id_, std::unexpected(jsonrpc::RpcError(code, std::move(msg)))));
        }
    private:
        maya::co_owned<Core> c_;
        jsonrpc::Id id_;
        bool        done_ = false;
    };

    // Fail one of our calls now (locally), e.g. on user cancel.
    void cancel(const maya::unlocked&, jsonrpc::RequestId id);

    // Coalesce every frame written by THIS thread inside `body` into one
    // write. For a burst of notifications (replaying a thread). Scoped by a
    // call, not a free-standing RAII object, so the end of the batch runs
    // with the same capability the beginning had. Nested batches flatten.
    template <class F>
    decltype(auto) batch(const maya::below<maya::leaf>& cap, F&& body) {
        begin_batch(cap);
        struct End {
            Peer& p;
            const maya::below<maya::leaf>& c;
            ~End() { p.end_batch(c); }
        } end{*this, cap};
        return std::forward<F>(body)();
    }

    [[nodiscard]] bool closed(const maya::below<maya::leaf>&) const;

private:
    void write(const maya::below<maya::leaf>&, std::string frame);
    void begin_batch(const maya::below<maya::leaf>&);
    void end_batch(const maya::below<maya::leaf>&) noexcept;

    core_t                    core_;
    std::chrono::milliseconds default_timeout_{0};
    maya::pool                pool_{3};   // reader, writer, ticker
};

}  // namespace agentty::rpc

// A Peer joins its reader/writer/ticker when destroyed: never inside a
// maya::guarded<T> (jaal refuses the type), so its destructor may wait.
MAYA_JOINS_ON_DESTROY(agentty::rpc::Peer);

// A deferred reply is a handle the dispatch hands to whoever answers later.
// A Deferred owns a handle to the peer's core (co_owned), the call's id and a
// flag: moving it to the worker that will answer shares nothing.
template <jsonrpc::IsMethod M>
MAYA_SENDABLE_T(agentty::rpc::Peer::Deferred<M>);
