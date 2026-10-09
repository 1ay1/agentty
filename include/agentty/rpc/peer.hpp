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

// A byte channel to the peer. Both functions block; read returns 0 at EOF
// (or on error), write returns false when the channel is gone.
struct Channel {
    std::function<std::size_t(char* buf, std::size_t cap)> read;
    std::function<bool(std::string_view bytes)>            write;
    // Wake a read blocked in `read` so the reader can exit (close the fd,
    // shut the socket). Called once, at stop.
    std::function<void()>                                  interrupt;
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
struct CallOptions {
    std::optional<std::chrono::milliseconds> timeout;
    std::stop_token                          cancel;
    std::function<void()>                    on_cancel;
    std::chrono::milliseconds                cancel_grace{0};
};

// What the owner does with the peer's calls and notifications. Return the
// reply frame for a call to have it written now, or Nothing to answer later
// with Peer::reply (a deferred method).
struct Dispatch {
    std::function<jsonrpc::Maybe<std::string>(const jsonrpc::Call&)> on_call;
    std::function<void(const jsonrpc::Notification&)>                on_notification;
    // Every frame in and out, for a wire trace. Optional.
    std::function<void(bool inbound, std::string_view frame)>        trace;
};

class Peer {
public:
    using Clock = jsonrpc::Clock;

    Peer(Channel ch, Dispatch d, std::string_view name);
    ~Peer();
    Peer(const Peer&)            = delete;
    Peer& operator=(const Peer&) = delete;

    // Start reading. Separate from construction so the owner can finish
    // wiring before the first frame arrives.
    void start();

    // Stop reading, fail every waiting call with ConnectionLost, join.
    void stop() noexcept;

    // Block until the reader has seen EOF. For a server whose life is the
    // connection's.
    void wait_closed();

    // Deadline for calls that pass none. Zero means no deadline.
    void set_default_timeout(std::chrono::milliseconds d) { default_timeout_ = d; }
    void set_request_meta(jsonrpc::Json meta);

    // ── to the peer ─────────────────────────────────────────────────────

    // A typed request, waited for. Errors (the peer's, a timeout, a closed
    // connection, a cancel, a result that doesn't decode) come back as
    // RpcError.
    template <jsonrpc::IsMethod M>
    std::expected<typename M::result, jsonrpc::RpcError>
    call(const typename M::params& p, CallOptions o = {}) {
        auto c = call_raw(M::name, jsonrpc::to_json(p), std::move(o));
        return jsonrpc::result<M>(c);
    }

    // The untyped form: the completion as the engine returned it.
    jsonrpc::Completed call_raw(std::string_view method, jsonrpc::Json params, CallOptions o = {});

    template <jsonrpc::IsNote N>
    void notify(const typename N::params& p) { write(jsonrpc::notify<N>(p)); }
    void notify_raw(std::string_view method, jsonrpc::Json params) {
        write(jsonrpc::notify_raw(method, std::move(params)));
    }

    // Answer a call the dispatch deferred.
    void reply(const jsonrpc::Id& id, std::expected<jsonrpc::Json, jsonrpc::RpcError> outcome) {
        write(jsonrpc::reply(id, std::move(outcome)));
    }
    template <jsonrpc::IsMethod M>
    void reply(const jsonrpc::Id& id, const typename M::result& r) { write(jsonrpc::reply<M>(id, r)); }

    // A deferred call's answer, sent at most once. The first ok()/error()
    // wins; later ones are dropped.
    template <jsonrpc::IsMethod M>
    class Deferred {
    public:
        Deferred(Peer& p, jsonrpc::Id id) : p_(&p), id_(std::move(id)) {}
        Deferred(Deferred&&) noexcept            = default;
        Deferred& operator=(Deferred&&) noexcept = default;

        [[nodiscard]] const jsonrpc::Id& id() const { return id_; }
        void ok(const typename M::result& r) {
            if (std::exchange(done_, true)) return;
            p_->reply<M>(id_, r);
        }
        void error(int code, std::string msg) {
            if (std::exchange(done_, true)) return;
            p_->reply(id_, std::unexpected(jsonrpc::RpcError(code, std::move(msg))));
        }
    private:
        Peer*       p_;
        jsonrpc::Id id_;
        bool        done_ = false;
    };

    // Fail one of our calls now (locally), e.g. on user cancel.
    void cancel(jsonrpc::RequestId id);

    // Coalesce every frame written by THIS thread until the Batch ends into
    // one write. For a burst of notifications (replaying a thread).
    class Batch {
    public:
        explicit Batch(Peer& p);
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
    private:
        Peer& p_;
    };
    [[nodiscard]] Batch batch() { return Batch(*this); }

    [[nodiscard]] bool closed() const;

private:
    struct Core;   // shared with the tasks, so an abandoned reader outlives us

    void write(std::string frame);

    std::shared_ptr<Core>     core_;
    std::chrono::milliseconds default_timeout_{0};
    maya::pool                pool_{3};   // reader, writer, ticker
};

}  // namespace agentty::rpc
