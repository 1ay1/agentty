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
// The tasks co-own Core, so a reader stuck in a blocking read can be
// abandoned at shutdown without touching a dead Peer.

#include "agentty/rpc/peer.hpp"

#include <algorithm>
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
    ch.read = [in_fd](char* buf, std::size_t cap) -> std::size_t {
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
    ch.write = [out_fd](std::string_view bytes) {
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
    ch.read = [&in](char* buf, std::size_t cap) -> std::size_t {
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
    ch.write = [&out](std::string_view bytes) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return out.good();
    };
    return ch;
}

namespace {
// One direction of an in-memory pipe.
struct Pipe {
    std::string bytes;
    bool        closed = false;
};
using PipePtr = std::shared_ptr<maya::guarded<Pipe>>;

std::size_t pipe_read(maya::guarded<Pipe>& p, char* buf, std::size_t cap) {
    const auto got = p.wait_with(
        [](const Pipe& s, std::size_t) { return !s.bytes.empty() || s.closed; },
        [](Pipe& s, std::size_t n) {
            n = std::min(n, s.bytes.size());
            std::string out = s.bytes.substr(0, n);
            s.bytes.erase(0, n);
            return out;
        }, cap);
    std::copy(got.begin(), got.end(), buf);
    return got.size();
}

bool pipe_write(maya::guarded<Pipe>& p, std::string_view b) {
    return p.with([](Pipe& s, std::string chunk) {
        if (s.closed) return false;
        s.bytes += chunk;
        return true;
    }, std::string(b));
}

void pipe_close(maya::guarded<Pipe>& p) {
    p.with([](Pipe& s) { s.closed = true; });
}
}  // namespace

std::pair<Channel, Channel> channel_pair() {
    auto ab = std::make_shared<maya::guarded<Pipe>>();   // a writes, b reads
    auto ba = std::make_shared<maya::guarded<Pipe>>();   // b writes, a reads
    auto end = [](PipePtr in, PipePtr out) {
        Channel ch;
        ch.read      = [in](char* buf, std::size_t cap) { return pipe_read(*in, buf, cap); };
        ch.write     = [out](std::string_view b) { return pipe_write(*out, b); };
        ch.interrupt = [in, out] { pipe_close(*in); pipe_close(*out); };
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
        // Bytes held by an open Batch, per thread, and its nesting depth.
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

    void write(std::string frame);
    void perform(jsonrpc::Effects fx);
    jsonrpc::Effects step(jsonrpc::Event ev);
    void close(std::string reason);
    void reader(std::stop_token st);
    void writer();
    void ticker(std::stop_token st);
};

Peer::Peer(Channel ch, Dispatch d, std::string_view name)
    : core_(core_t::make(std::move(ch), std::move(d), std::string(name))) {}

Peer::~Peer() { stop(); }

void Peer::start() {
    pool_.post_isolated([](std::stop_token st, core_t c) { c->reader(st); }, core_);
    pool_.post_isolated([](std::stop_token, core_t c) { c->writer(); }, core_);
    pool_.post_isolated([](std::stop_token st, core_t c) { c->ticker(st); }, core_);
}

void Peer::stop() noexcept {
    const bool again = core_->s.with([](Core::Shared& s) { return std::exchange(s.stopped, true); });
    if (again) return;
    core_->close("stopped");
    if (core_->ch.interrupt) {
        try { core_->ch.interrupt(); } catch (...) {}
    }
    // A reader wedged in read() past the grace is abandoned; it co-owns Core.
    pool_.shutdown();
}

void Peer::wait_closed() {
    core_->s.wait_with([](const Core::Shared& s) { return s.closed; }, [](Core::Shared&) {});
}

void Peer::set_request_meta(jsonrpc::Json meta) {
    core_->s.with([](Core::Shared& s, jsonrpc::Json m) { s.engine.set_request_meta(std::move(m)); },
                  std::move(meta));
}

bool Peer::closed() const {
    return core_->s.read([](const Core::Shared& s) { return s.closed; });
}

void Peer::write(std::string frame) { core_->write(std::move(frame)); }
void Peer::write_core(const core_t& c, std::string frame) { c->write(std::move(frame)); }

// ── the outbox ──────────────────────────────────────────────────────────────

void Peer::Core::write(std::string frame) {
    if (dispatch.trace) dispatch.trace(false, frame);
    frame.push_back('\n');
    out.with([](Outbox& o, std::uint64_t key, std::string f) {
        if (o.closed) return;
        if (auto it = o.depth.find(key); it != o.depth.end() && it->second > 0)
            o.held[key] += f;               // this thread is batching
        else
            o.frames.push_back(std::move(f));
    }, thread_key(), std::move(frame));
}

Peer::Batch::Batch(Peer& p) : p_(p) {
    p_.core_->out.with([](Core::Outbox& o, std::uint64_t key) { ++o.depth[key]; }, thread_key());
}

Peer::Batch::~Batch() {
    p_.core_->out.with([](Core::Outbox& o, std::uint64_t key) {
        if (--o.depth[key] > 0) return;
        o.depth.erase(key);
        if (auto it = o.held.find(key); it != o.held.end()) {
            if (!o.closed && !it->second.empty()) o.frames.push_back(std::move(it->second));
            o.held.erase(it);
        }
    }, thread_key());
}

void Peer::Core::writer() {
    for (;;) {
        // Sleep until there is something to write, or the peer is closing.
        // Frames queued before close still go out.
        auto batch = out.wait_with(
            [](const Outbox& o) { return !o.frames.empty() || o.closed; },
            [](Outbox& o) { return std::exchange(o.frames, {}); });
        if (batch.empty()) return;   // closed with nothing left
        std::string bytes;
        for (auto& f : batch) bytes += f;
        if (!ch.write(bytes)) {
            util::dbglog("rpc.write", name + ": channel gone");
            out.with([](Outbox& o) { o.closed = true; o.frames.clear(); });
            return;
        }
    }
}

// ── transitions and effects ───────────────────────────────────────────────

// One transition under the owner. Completions are parked in `done` (which
// wakes their callers); the rest is performed by the caller, outside the lock.
jsonrpc::Effects Peer::Core::step(jsonrpc::Event ev) {
    return s.with([](Shared& sh, jsonrpc::Event e) {
        auto fx = jsonrpc::step(sh.engine, std::move(e));
        for (auto& c : fx.completed) sh.done.insert_or_assign(c.id.get(), std::move(c));
        fx.completed.clear();
        return fx;
    }, std::move(ev));
}

void Peer::Core::perform(jsonrpc::Effects fx) {
    for (auto& f : fx.send) write(std::move(f));
    for (auto& n : fx.notifications) {
        if (!dispatch.on_notification) continue;
        try { dispatch.on_notification(n); }
        catch (const std::exception& e) { util::dbglog("rpc.notification", e.what()); }
        catch (...) {}
    }
    for (auto& c : fx.calls) {
        jsonrpc::Maybe<std::string> frame;
        try {
            if (dispatch.on_call) frame = dispatch.on_call(c);
            else frame = jsonrpc::reply(c.id, std::unexpected(jsonrpc::RpcError(
                jsonrpc::errc::MethodNotFound, "Method not found: " + c.method)));
        } catch (const std::exception& e) {
            frame = jsonrpc::reply(c.id, std::unexpected(
                jsonrpc::RpcError(jsonrpc::errc::InternalError, e.what())));
        }
        if (frame) write(std::move(*frame));
    }
}

// The transport ended (EOF, or stop()). Every waiting call fails; queued
// frames still drain, then the writer leaves.
void Peer::Core::close(std::string reason) {
    perform(s.with([](Shared& sh, std::string r) {
        jsonrpc::Effects fx;
        if (sh.closed) return fx;
        sh.closed = true;
        fx = jsonrpc::step(sh.engine, jsonrpc::Closed{std::move(r)});
        for (auto& c : fx.completed) sh.done.insert_or_assign(c.id.get(), std::move(c));
        fx.completed.clear();
        return fx;
    }, std::move(reason)));
    out.with([](Outbox& o) { o.closed = true; });
}

// ── the reader ─────────────────────────────────────────────────────────────

void Peer::Core::reader(std::stop_token st) {
    jsonrpc::LineSplitter lines;
    std::string buf(64 * 1024, '\0');
    while (!st.stop_requested()) {
        const std::size_t n = ch.read(buf.data(), buf.size());
        if (n == 0) break;   // EOF or error
        for (auto& line : lines.feed(std::string_view{buf.data(), n})) {
            if (dispatch.trace) dispatch.trace(true, line);
            perform(step(jsonrpc::Received{std::move(line)}));
        }
    }
    if (!st.stop_requested())
        if (auto last = lines.finish()) perform(step(jsonrpc::Received{std::move(*last)}));
    close("eof");
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

void Peer::Core::ticker(std::stop_token st) {
    while (!st.stop_requested()) {
        struct Next { jsonrpc::Deadline at; bool closed; };
        const auto next = s.read([](const Shared& sh) {
            return Next{next_due(sh.engine, sh.give_up), sh.closed};
        });
        if (next.closed) return;
        if (!next.at) {
            s.wait_with(
                [](const Shared& sh) { return sh.closed || next_due(sh.engine, sh.give_up).has_value(); },
                [](Shared&) {});
            continue;
        }
        const auto now = Clock::now();
        if (*next.at > now) {
            const auto wait = std::min<Clock::duration>(*next.at - now, 50ms);
            if (maya::delay_for(st, std::chrono::ceil<std::chrono::milliseconds>(wait))) return;
            continue;
        }
        perform(s.with([](Shared& sh, Clock::time_point t) {
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

jsonrpc::Completed Peer::call_raw(std::string_view method, jsonrpc::Json params,
                                  CallOptions o) {
    const auto t = o.timeout.value_or(default_timeout_);
    const jsonrpc::Deadline deadline =
        t.count() > 0 ? jsonrpc::Deadline{Clock::now() + t} : jsonrpc::Deadline{};

    // Start it under the owner; a closed peer fails at once.
    struct Started { std::int64_t id; jsonrpc::Effects fx; bool closed; };
    auto started = core_->s.with([](Core::Shared& s, std::string m, jsonrpc::Json p,
                                    jsonrpc::Deadline d) -> Started {
        if (s.closed) return {0, {}, true};
        auto [id, fx] = jsonrpc::request_raw(s.engine, m, std::move(p), d);
        return {id.get(), std::move(fx), false};
    }, std::string(method), std::move(params), deadline);

    if (started.closed) return lost(method);
    core_->perform(std::move(started.fx));

    // A stop on `cancel`: tell whoever asked, then fail the call now or once
    // the grace runs out. Either way it settles as Cancelled, which wakes us.
    std::stop_callback on_cancel(o.cancel, [c = &core_.get(), id = started.id, &o] {
        if (o.on_cancel) {
            try { o.on_cancel(); } catch (...) {}
        }
        if (o.cancel_grace.count() <= 0) {
            c->perform(c->step(jsonrpc::Cancel{jsonrpc::RequestId{id}}));
            return;
        }
        c->s.with([](Core::Shared& s, std::int64_t i, Clock::time_point at) {
            if (s.engine.is_pending(jsonrpc::RequestId{i})) s.give_up.emplace(i, at);
        }, id, Clock::now() + o.cancel_grace);
    });

    // Wait for this id's completion: a reply, a Tick past its deadline, a
    // cancel, or close. Then take it, and drop any grace still armed.
    return core_->s.wait_with(
        [](const Core::Shared& s, std::int64_t id) { return s.done.contains(id); },
        [](Core::Shared& s, std::int64_t id) {
            s.give_up.erase(id);
            auto node = s.done.extract(id);
            return std::move(node.mapped());
        }, started.id);
}

void Peer::cancel(jsonrpc::RequestId id) {
    core_->perform(core_->step(jsonrpc::Cancel{id}));
}

}  // namespace agentty::rpc
