// src/rpc/peer.cpp — a jsonrpc Engine, driven on maya. See peer.hpp.
//
// Three tasks around two owners, no lock ever held across I/O:
//
//   reader  reads the channel, splits lines, steps Received, hands calls and
//           notifications to the dispatch
//   writer  drains the outbox to the channel, in order
//   ticker  sleeps until next_deadline() and steps Tick
//
//   engine  guarded<Shared>: the Engine, completions not yet collected, the
//           closed flag. Every transition happens under it.
//   outbox  guarded<Outbox>: frames to write, in order.
//
// Lock order: both are LEAVES. No body below takes another lock or does I/O;
// effects (frames to send, calls to dispatch) are returned out of the body
// and performed after it. Each task starts with the fresh thread's
// `unlocked`, which is what lets it wait (writer, ticker) and hand the
// dispatch a capability.
//
// The tasks co-own Core, so a reader stuck in a blocking read can be
// abandoned at shutdown without touching a dead Peer.

#include "agentty/rpc/peer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <istream>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "agentty/util/dbglog.hpp"
#include "agentty/util/sendable.hpp"   // nlohmann::json

// jsonrpc's values own everything they hold. jaal can't see past private
// members or std::expected, so they are opted in here.
MAYA_SENDABLE(jsonrpc::Engine);
MAYA_SENDABLE(jsonrpc::RpcError);
MAYA_SENDABLE(jsonrpc::Completed);
MAYA_SENDABLE(jsonrpc::Effects);
MAYA_SENDABLE(jsonrpc::Event);

namespace agentty::rpc {

namespace {
using namespace std::chrono_literals;

std::uint64_t thread_key() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

jsonrpc::Completed lost(std::string_view method) {
    return {jsonrpc::RequestId{0}, std::string(method),
            std::unexpected(jsonrpc::RpcError(jsonrpc::errc::ConnectionLost, "connection closed"))};
}
}  // namespace

// ── channels ───────────────────────────────────────────────────────────────

Channel fd_channel(int in_fd, int out_fd) {
    Channel ch;
    ch.read = [in_fd](const maya::unlocked&, char* buf, std::size_t cap) -> std::size_t {
        for (;;) {
#if defined(_WIN32)
            const int n = ::_read(in_fd, buf, static_cast<unsigned>(cap));
#else
            const auto n = ::read(in_fd, buf, cap);
            if (n < 0 && errno == EINTR) continue;
#endif
            return n > 0 ? static_cast<std::size_t>(n) : 0;
        }
    };
    ch.write = [out_fd](const maya::unlocked&, std::string_view bytes) {
        while (!bytes.empty()) {
#if defined(_WIN32)
            const int n = ::_write(out_fd, bytes.data(), static_cast<unsigned>(bytes.size()));
#else
            const auto n = ::write(out_fd, bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) continue;
#endif
            if (n <= 0) return false;
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    };
    return ch;
}

Channel stream_channel(std::istream& in, std::ostream& out) {
    Channel ch;
    // Block for one byte, then take whatever else is already buffered.
    ch.read = [&in](const maya::unlocked&, char* buf, std::size_t cap) -> std::size_t {
        auto* sb = in.rdbuf();
        if (!sb || cap == 0) return 0;
        const auto c = sb->sbumpc();
        if (c == std::char_traits<char>::eof()) return 0;
        buf[0] = static_cast<char>(c);
        std::size_t n = 1;
        while (n < cap && sb->in_avail() > 0) {
            const auto more = sb->sgetn(buf + n, std::min<std::streamsize>(
                sb->in_avail(), static_cast<std::streamsize>(cap - n)));
            if (more <= 0) break;
            n += static_cast<std::size_t>(more);
        }
        return n;
    };
    ch.write = [&out](const maya::unlocked&, std::string_view bytes) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return out.good();
    };
    return ch;
}

namespace {
// One direction of an in-memory pipe. A leaf: every body is a few lines over
// the buffer.
struct Pipe {
    std::string bytes;
    bool        closed = false;
};
using PipeLock = maya::guarded<Pipe>;
using PipePtr  = std::shared_ptr<PipeLock>;

std::size_t pipe_read(const maya::unlocked& cap, PipeLock& p, char* buf, std::size_t n) {
    const auto got = p.wait_with(cap,
        [](const Pipe& s, std::size_t) { return !s.bytes.empty() || s.closed; },
        [](Pipe& s, std::size_t k) {
            k = std::min(k, s.bytes.size());
            std::string out = s.bytes.substr(0, k);
            s.bytes.erase(0, k);
            return out;
        }, n);
    std::copy(got.begin(), got.end(), buf);
    return got.size();
}

bool pipe_write(const maya::below<maya::leaf>& cap, PipeLock& p, std::string_view b) {
    return p.with(cap, [](Pipe& s, std::string chunk) {
        if (s.closed) return false;
        s.bytes += chunk;
        return true;
    }, std::string(b));
}

void pipe_close(const maya::below<maya::leaf>& cap, PipeLock& p) {
    p.with(cap, [](Pipe& s) { s.closed = true; });
}
}  // namespace

std::pair<Channel, Channel> channel_pair() {
    auto ab = std::make_shared<PipeLock>();   // a writes, b reads
    auto ba = std::make_shared<PipeLock>();   // b writes, a reads
    auto end = [](PipePtr in, PipePtr out) {
        Channel ch;
        ch.read = [in](const maya::unlocked& c, char* buf, std::size_t n) {
            return pipe_read(c, *in, buf, n);
        };
        ch.write = [out](const maya::unlocked& c, std::string_view b) {
            return pipe_write(c, *out, b);
        };
        ch.interrupt = [in, out](const maya::unlocked& c) {
            pipe_close(c, *in);
            pipe_close(c, *out);
        };
        return ch;
    };
    return {end(ba, ab), end(ab, ba)};
}

struct Peer::Core {
    struct Shared {
        jsonrpc::Engine engine;
        // Completions not yet collected by their caller, by request id.
        std::unordered_map<std::int64_t, jsonrpc::Completed> done;
        // Cancelled calls still waiting for the peer, and when to give up.
        std::unordered_map<std::int64_t, Clock::time_point> give_up;
        bool closed = false;
        bool stopped = false;   // stop() ran
    };
    struct Outbox {
        std::vector<std::string> frames;   // ready to write, in order
        // Bytes held by an open batch, per thread, and its nesting depth.
        std::unordered_map<std::uint64_t, std::string> held;
        std::unordered_map<std::uint64_t, int>         depth;
        bool closed = false;
    };

    // Set once at construction, then only read (and the channel's functions
    // are safe to call from the reader and writer at once: read and write
    // are separate ends, interrupt is for stop()).
    const Channel     ch;
    const Dispatch    dispatch;
    const std::string name;
    maya::guarded<Shared> s;
    maya::guarded<Outbox> out;

    void write(const maya::below<maya::leaf>& cap, std::string frame);
    void perform(const maya::unlocked& cap, jsonrpc::Effects fx);
    jsonrpc::Effects step(const maya::below<maya::leaf>& cap, jsonrpc::Event ev);
    void close(const maya::unlocked& cap, std::string reason);
    void reader(const maya::unlocked& cap, std::stop_token st);
    void writer(const maya::unlocked& cap);
    void ticker(const maya::unlocked& cap, std::stop_token st);
};

Peer::Peer(Channel ch, Dispatch d, std::string_view name)
    : core_(core_t::make(std::move(ch), std::move(d), std::string(name))) {}

// A Peer is joins_on_destroy (peer.hpp), so no guarded<T> can own one and
// this destructor never runs inside a lock body: the thread holds nothing,
// and may join. Reviewed capability root (tests/lint/allowlist.txt).
Peer::~Peer() {
    maya::thread_root([this](const maya::unlocked& cap) { stop(cap); });
}

void Peer::start() {
    pool_.post_isolated(
        [](const maya::unlocked& cap, std::stop_token st, core_t c) { c->reader(cap, st); }, core_);
    pool_.post_isolated(
        [](const maya::unlocked& cap, std::stop_token, core_t c) { c->writer(cap); }, core_);
    pool_.post_isolated(
        [](const maya::unlocked& cap, std::stop_token st, core_t c) { c->ticker(cap, st); }, core_);
}

void Peer::stop(const maya::unlocked& cap) noexcept {
    const bool again =
        core_->s.with(cap, [](Core::Shared& s) { return std::exchange(s.stopped, true); });
    if (again) return;
    try { core_->close(cap, "stopped"); } catch (...) {}
    if (core_->ch.interrupt) {
        try { core_->ch.interrupt(cap); } catch (...) {}
    }
    // A reader wedged in read() past the grace is abandoned; it co-owns Core.
    (void)pool_.shutdown(cap);
}

void Peer::wait_closed(const maya::unlocked& cap) {
    core_->s.wait_with(cap, [](const Core::Shared& s) { return s.closed; }, [](Core::Shared&) {});
}

void Peer::set_request_meta(const maya::below<maya::leaf>& cap, jsonrpc::Json meta) {
    core_->s.with(cap,
                  [](Core::Shared& s, jsonrpc::Json m) { s.engine.set_request_meta(std::move(m)); },
                  std::move(meta));
}

bool Peer::closed(const maya::below<maya::leaf>& cap) const {
    return core_->s.read(cap, [](const Core::Shared& s) { return s.closed; });
}

void Peer::write(const maya::below<maya::leaf>& cap, std::string frame) {
    core_->write(cap, std::move(frame));
}
void Peer::write_core(const maya::below<maya::leaf>& cap, const core_t& c, std::string frame) {
    c->write(cap, std::move(frame));
}

// ── the outbox ──────────────────────────────────────────────────────────────

void Peer::Core::write(const maya::below<maya::leaf>& cap, std::string frame) {
    if (dispatch.trace) dispatch.trace(false, frame);
    frame.push_back('\n');
    out.with(cap, [](Outbox& o, std::uint64_t key, std::string f) {
        if (o.closed) return;
        if (auto it = o.depth.find(key); it != o.depth.end() && it->second > 0)
            o.held[key] += f;               // this thread is batching
        else
            o.frames.push_back(std::move(f));
    }, thread_key(), std::move(frame));
}

void Peer::begin_batch(const maya::below<maya::leaf>& cap) {
    core_->out.with(cap, [](Core::Outbox& o, std::uint64_t key) { ++o.depth[key]; }, thread_key());
}

void Peer::end_batch(const maya::below<maya::leaf>& cap) noexcept {
    try {
        core_->out.with(cap, [](Core::Outbox& o, std::uint64_t key) {
            if (--o.depth[key] > 0) return;
            o.depth.erase(key);
            if (auto it = o.held.find(key); it != o.held.end()) {
                if (!o.closed && !it->second.empty()) o.frames.push_back(std::move(it->second));
                o.held.erase(it);
            }
        }, thread_key());
    } catch (...) {}
}

void Peer::Core::writer(const maya::unlocked& cap) {
    for (;;) {
        // Sleep until there is something to write, or the peer is closing.
        // Frames queued before close still go out.
        auto batch = out.wait_with(cap,
            [](const Outbox& o) { return !o.frames.empty() || o.closed; },
            [](Outbox& o) { return std::exchange(o.frames, {}); });
        if (batch.empty()) return;   // closed with nothing left
        std::string bytes;
        for (auto& f : batch) bytes += f;
        if (!ch.write(cap, bytes)) {
            util::dbglog("rpc.write", name + ": channel gone");
            out.with(cap, [](Outbox& o) { o.closed = true; o.frames.clear(); });
            return;
        }
    }
}

// ── transitions and effects ───────────────────────────────────────────────

// One transition under the owner. Completions are parked in `done` (which
// wakes their callers); the rest is performed by the caller, outside the lock.
jsonrpc::Effects Peer::Core::step(const maya::below<maya::leaf>& cap, jsonrpc::Event ev) {
    return s.with(cap, [](Shared& sh, jsonrpc::Event e) {
        auto fx = jsonrpc::step(sh.engine, std::move(e));
        for (auto& c : fx.completed) sh.done.insert_or_assign(c.id.get(), std::move(c));
        fx.completed.clear();
        return fx;
    }, std::move(ev));
}

// Effects run outside every lock: the dispatch is handed the thread's
// `unlocked`, so a handler can make a nested call or take any lock.
void Peer::Core::perform(const maya::unlocked& cap, jsonrpc::Effects fx) {
    for (auto& f : fx.send) write(cap, std::move(f));
    for (auto& n : fx.notifications) {
        if (!dispatch.on_notification) continue;
        try { dispatch.on_notification(cap, n); }
        catch (const std::exception& e) { util::dbglog("rpc.notification", e.what()); }
        catch (...) {}
    }
    for (auto& c : fx.calls) {
        jsonrpc::Maybe<std::string> frame;
        try {
            if (dispatch.on_call) frame = dispatch.on_call(cap, c);
            else frame = jsonrpc::reply(c.id, std::unexpected(jsonrpc::RpcError(
                jsonrpc::errc::MethodNotFound, "Method not found: " + c.method)));
        } catch (const std::exception& e) {
            frame = jsonrpc::reply(c.id, std::unexpected(
                jsonrpc::RpcError(jsonrpc::errc::InternalError, e.what())));
        }
        if (frame) write(cap, std::move(*frame));
    }
}

// The transport ended (EOF, or stop()). Every waiting call fails; queued
// frames still drain, then the writer leaves.
void Peer::Core::close(const maya::unlocked& cap, std::string reason) {
    perform(cap, s.with(cap, [](Shared& sh, std::string r) {
        jsonrpc::Effects fx;
        if (sh.closed) return fx;
        sh.closed = true;
        fx = jsonrpc::step(sh.engine, jsonrpc::Closed{std::move(r)});
        for (auto& c : fx.completed) sh.done.insert_or_assign(c.id.get(), std::move(c));
        fx.completed.clear();
        return fx;
    }, std::move(reason)));
    out.with(cap, [](Outbox& o) { o.closed = true; });
}

// ── the reader ─────────────────────────────────────────────────────────────

void Peer::Core::reader(const maya::unlocked& cap, std::stop_token st) {
    jsonrpc::LineSplitter lines;
    std::string buf(64 * 1024, '\0');
    // One frame that throws while being handled must not take the reader
    // with it: a dead reader leaves the peer deaf for good, with no error.
    auto take = [this, &cap](std::string line) {
        try {
            if (dispatch.trace) dispatch.trace(true, line);
            perform(cap, step(cap, jsonrpc::Received{std::move(line)}));
        } catch (const std::exception& e) {
            util::dbglog("rpc.reader", name + ": dropped a frame: " + e.what());
        } catch (...) {
            util::dbglog("rpc.reader", name + ": dropped a frame");
        }
    };
    while (!st.stop_requested()) {
        const std::size_t n = ch.read(cap, buf.data(), buf.size());
        if (n == 0) break;   // EOF or error
        for (auto& line : lines.feed(std::string_view{buf.data(), n})) take(std::move(line));
    }
    if (!st.stop_requested())
        if (auto last = lines.finish()) take(std::move(*last));
    close(cap, "eof");
}

// ── the ticker ─────────────────────────────────────────────────────────────
//
// Steps Tick when the nearest deadline passes, and Cancel when a cancelled
// call's grace runs out. It sleeps in short slices so a nearer time armed
// meanwhile is honoured promptly, and parks on the engine when there is
// nothing to time (any with() wakes it to re-check).

namespace {
jsonrpc::Deadline next_due(const jsonrpc::Engine& e,
                           const std::unordered_map<std::int64_t, jsonrpc::Clock::time_point>& give_up) {
    auto next = e.next_deadline();
    for (const auto& [_, at] : give_up)
        if (!next || at < *next) next = at;
    return next;
}
}  // namespace

void Peer::Core::ticker(const maya::unlocked& cap, std::stop_token st) {
    while (!st.stop_requested()) {
        struct Next { jsonrpc::Deadline at; bool closed; };
        const auto next = s.read(cap, [](const Shared& sh) {
            return Next{next_due(sh.engine, sh.give_up), sh.closed};
        });
        if (next.closed) return;
        if (!next.at) {
            s.wait_with(cap,
                [](const Shared& sh) { return sh.closed || next_due(sh.engine, sh.give_up).has_value(); },
                [](Shared&) {});
            continue;
        }
        const auto now = Clock::now();
        if (*next.at > now) {
            const auto wait = std::min<Clock::duration>(*next.at - now, 50ms);
            if (maya::delay_for(cap, st, std::chrono::ceil<std::chrono::milliseconds>(wait))) return;
            continue;
        }
        perform(cap, s.with(cap, [](Shared& sh, Clock::time_point t) {
            auto fx = jsonrpc::step(sh.engine, jsonrpc::Tick{t});
            for (auto it = sh.give_up.begin(); it != sh.give_up.end();) {
                if (it->second > t) { ++it; continue; }
                fx += jsonrpc::step(sh.engine, jsonrpc::Cancel{jsonrpc::RequestId{it->first}});
                it = sh.give_up.erase(it);
            }
            for (auto& c : fx.completed) sh.done.insert_or_assign(c.id.get(), std::move(c));
            fx.completed.clear();
            return fx;
        }, now));
    }
}

// ── calls ──────────────────────────────────────────────────────────────────

jsonrpc::Completed Peer::call_raw(const maya::unlocked& cap, std::string_view method,
                                  jsonrpc::Json params, CallOptions o) {
    const auto t = o.timeout.value_or(default_timeout_);
    const jsonrpc::Deadline deadline =
        t.count() > 0 ? jsonrpc::Deadline{Clock::now() + t} : jsonrpc::Deadline{};

    // Start it under the owner; a closed peer fails at once.
    struct Started { std::int64_t id; jsonrpc::Effects fx; bool closed; };
    auto started = core_->s.with(cap, [](Core::Shared& s, std::string m, jsonrpc::Json p,
                                         jsonrpc::Deadline d) -> Started {
        if (s.closed) return {0, {}, true};
        auto [id, fx] = jsonrpc::request_raw(s.engine, m, std::move(p), d);
        return {id.get(), std::move(fx), false};
    }, std::string(method), std::move(params), deadline);

    if (started.closed) return lost(method);
    core_->perform(cap, std::move(started.fx));

    // A stop on `cancel` may be requested from any thread, holding any lock,
    // so its callback only raises a flag. This thread -- which holds nothing
    // -- acts on it: run on_cancel, tell the peer, and fail the call now or
    // once the grace runs out. Either way it settles as Cancelled.
    const auto requested = std::make_shared<std::atomic<bool>>(false);
    std::optional<std::stop_callback<std::function<void()>>> on_stop;
    if (o.cancel.stop_possible())
        on_stop.emplace(o.cancel, std::function<void()>{[requested] {
            requested->store(true, std::memory_order_release);
        }});
    bool cancel_handled = false;

    // Wait for this id's completion: a reply, a Tick past its deadline, a
    // cancel, or close. Then take it, and drop any grace still armed. The
    // wait is sliced so a cancel request is acted on within a few ms.
    for (;;) {
        auto [got, ready] = core_->s.wait_with_for(cap, 10ms,
            [](const Core::Shared& s, std::int64_t id) { return s.done.contains(id); },
            [](Core::Shared& s, std::int64_t id) -> std::optional<jsonrpc::Completed> {
                auto it = s.done.find(id);
                if (it == s.done.end()) return std::nullopt;
                s.give_up.erase(id);
                auto node = s.done.extract(it);
                return std::move(node.mapped());
            }, started.id);
        (void)ready;
        if (got) return std::move(*got);

        if (cancel_handled || !requested->load(std::memory_order_acquire)) continue;
        cancel_handled = true;
        if (o.on_cancel) {
            try { o.on_cancel(cap); } catch (...) {}
        }
        if (o.notify_peer) {
            try { notify_raw(cap, "$/cancel_request", jsonrpc::Json{{"id", started.id}}); }
            catch (...) {}
        }
        if (o.cancel_grace.count() <= 0) {
            core_->perform(cap, core_->step(cap, jsonrpc::Cancel{jsonrpc::RequestId{started.id}}));
        } else {
            core_->s.with(cap, [](Core::Shared& s, std::int64_t i, Clock::time_point at) {
                if (s.engine.is_pending(jsonrpc::RequestId{i})) s.give_up.emplace(i, at);
            }, started.id, Clock::now() + o.cancel_grace);
        }
    }
}

void Peer::cancel(const maya::unlocked& cap, jsonrpc::RequestId id) {
    core_->perform(cap, core_->step(cap, jsonrpc::Cancel{id}));
}

}  // namespace agentty::rpc
