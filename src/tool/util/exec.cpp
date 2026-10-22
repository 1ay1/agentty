// src/tool/util/exec.cpp — one loop, over jaal.
//
// The shape is a reactor wait with two deadlines. What makes it short is
// that none of the hard parts are here: spawning, making an exit watchable,
// and the handle lifetimes are jaal's, and they are covered by a conformance
// suite that runs on every backend. What is left is the policy, and policy
// is the part that should be readable.

#include "agentty/tool/util/exec.hpp"

#include <algorithm>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <maya/runtime.hpp>
#include "agentty/util/background.hpp"   // util::WorkerGroup, run_isolated_detached

#include <mcp/tools/util/utf8.hpp>
#include "agentty/tool/util/utf8.hpp"   // progress_tail

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/host_call.hpp"   // the dispatcher's per-call data

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace agentty::tools::util {
namespace {

namespace pf = maya::platform;
namespace mt = ::mcp::tools;
using process_t = pf::native_process;
using reactor_t = pf::native_reactor;

using clock_t_ = std::chrono::steady_clock;

constexpr std::uint64_t kExitToken = 1;
constexpr std::uint64_t kOutToken  = 2;
constexpr std::uint64_t kBrokerToken = 3;
constexpr std::uint64_t kInToken   = 4;

// AGENTTY_STOP_TRACE=1 logs the exec stop sequence to stderr, each line
// prefixed with ms since `since`.
bool stop_trace_on() {
    static const bool on = [] {
        const char* v = std::getenv("AGENTTY_STOP_TRACE");
        return v && v[0] == '1';
    }();
    return on;
}

[[gnu::format(printf, 2, 3)]]
void stop_trace(clock_t_::time_point since, const char* fmt, ...) {
    if (!stop_trace_on()) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_t_::now() - since).count();
    std::fprintf(stderr, "[exec] t=%lldms ", static_cast<long long>(ms));
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}


#if defined(__linux__) || defined(_WIN32)
/// The sandbox's way in, or empty when no sandbox is active. Linux and
/// Windows only: adopting a foreign child needs a pidfd or a process handle.
/// Darwin confines by wrapping the argv instead (see JaalExec::confine).
///
/// claybin clones with namespace flags, installs a seccomp filter and enters
/// a cgroup -- none of it expressible as "fork then exec" -- so it spawns,
/// and run_child adopts the result. After that line the two paths are the
/// same object with the same conformance suite behind it.
[[nodiscard]] std::function<AdoptedChild(int)>
sandbox_spawner(const std::vector<std::string>& argv, const std::string& cwd,
                std::vector<std::string> extra_roots = {},
                const handoff::TrustedSnapshot* trusted = nullptr,
                std::vector<std::pair<std::string, std::string>> env = {}) {
    namespace sb = agentty::tools::util::sandbox;
    if (sb::is_active() && sb::detected_backend() == sb::Backend::Claybin) {
        // `trusted` is the dispatcher's, alive for the whole call; the spawn
        // runs inside that call (start_child), never later.
        return [argv, cwd, extra_roots, trusted, env = std::move(env)](int out_fd) {
            auto r = sb::spawn_in_sandbox(argv, cwd, out_fd, extra_roots, trusted, env);
            AdoptedChild c;
            if (!r.started) { c.error = r.start_error; return c; }
            c.pid            = r.pid;
            c.pidfd          = r.pidfd;
            c.supervisor_fd  = r.supervisor_fd;
            c.service_broker = std::move(r.service_broker);
            c.kill_tree      = std::move(r.kill_tree);
            return c;
        };
    }
    return {};
}
#endif

struct started {
    process_t            proc;
    int                  supervisor_fd = -1;
    std::function<bool()> service_broker;
    std::function<bool()> kill_tree;
};

/// Spawn, or let the sandbox spawn and adopt. stdout and stderr share one
/// pipe: the interleaving a terminal would show.
[[nodiscard]] std::expected<started, std::string>
start_child(const ChildRun& run) {
    if (run.argv.empty()) return std::unexpected(std::string{"empty command"});
#if !defined(_WIN32)
    if (run.spawn_adopted) {
        int fds[2];
#if defined(__linux__)
        if (::pipe2(fds, O_CLOEXEC) != 0)
            return std::unexpected(std::string{"pipe: "} + std::strerror(errno));
#else
        // No pipe2 on macOS/BSD: set CLOEXEC after the fact. The race with a
        // concurrent fork is the same one every non-Linux spawner accepts;
        // jaal closes every fd above stderr in the child anyway.
        if (::pipe(fds) != 0)
            return std::unexpected(std::string{"pipe: "} + std::strerror(errno));
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
        auto a = run.spawn_adopted(fds[1]);
        ::close(fds[1]);
        if (!a.error.empty() || a.pid <= 0) {
            ::close(fds[0]);
            return std::unexpected(a.error.empty() ? std::string{"sandbox did not start"} : a.error);
        }
        pf::posix_process::adopted_child c;
        c.pid             = a.pid;
        c.pidfd           = a.pidfd;
        c.stdout_fd       = fds[0];
        c.merged          = true;
        c.leads_own_group = a.leads_own_group;
        auto adopted = pf::posix_process::adopt(c);
        if (!adopted) return std::unexpected(std::string{adopted.error().what});
        return started{std::move(*adopted), a.supervisor_fd,
                       std::move(a.service_broker), std::move(a.kill_tree)};
    }
#endif

#if defined(_WIN32)
    if (run.spawn_adopted) {
        // One anonymous pipe; the sandbox gets the write end duplicated
        // inheritable, we keep the read end. Same shape as the posix arm --
        // one merged stream, the interleaving a terminal would show -- but
        // with HANDLEs, because that is what every windows API takes.
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out_r = nullptr, out_w = nullptr;
        if (!::CreatePipe(&out_r, &out_w, &sa, 0))
            return std::unexpected(std::string{"pipe: "} +
                                   std::to_string(::GetLastError()));
        auto a = run.spawn_adopted(
            static_cast<int>(reinterpret_cast<intptr_t>(out_w)));
        // the spawner duplicated what it needed; OUR write end goes now, or
        // the reader below never sees EOF.
        ::CloseHandle(out_w);
        if (a.error.empty() && a.pid <= 0) a.error = "sandbox did not start";
        if (!a.error.empty()) {
            ::CloseHandle(out_r);
            return std::unexpected(a.error);
        }
        pf::windows_process::adopted_child c;
        c.pid         = a.pid;
        c.process     = reinterpret_cast<void*>(static_cast<intptr_t>(a.pidfd));
        c.stdout_pipe = out_r;
        auto adopted = pf::windows_process::adopt(std::move(c));
        if (!adopted) {
            ::CloseHandle(out_r);
            return std::unexpected(std::string{adopted.error().what});
        }
        return started{std::move(*adopted), -1 /*no seccomp on windows*/,
                       std::move(a.service_broker), std::move(a.kill_tree)};
    }
#endif

    pf::process_spec spec;
    spec.argv         = run.argv;
    spec.windows_command_line = run.windows_command_line;
    spec.cwd          = run.cwd;
    spec.env          = run.env;
    spec.merge_stderr = true;
    spec.new_session  = true;
    if (!run.stdin_data.empty()) spec.stdin_from = pf::stream_to::pipe;
    auto p = process_t::spawn(spec);
    if (!p) return std::unexpected(std::string{p.error().what} + " (" +
                                   std::to_string(p.error().native) + ")");
    return started{std::move(*p), -1, nullptr, nullptr};
}

/// Read up to `cap` bytes without blocking. 0 with `eof` = the writer closed.
std::size_t read_ready(pf::borrowed_handle h, char* buf, std::size_t cap, bool& eof) {
#if defined(_WIN32)
    return pf::read_some(h, buf, cap, eof);
#else
    eof = false;
    for (;;) {
        const auto n = ::read(h.get(), buf, cap);
        if (n > 0) return static_cast<std::size_t>(n);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) eof = true;
        return 0;   // EAGAIN: nothing left
    }
#endif
}

/// Write without blocking past what the pipe takes. `closed` = reader gone.
std::size_t write_ready(pf::borrowed_handle h, const char* buf, std::size_t len, bool& closed) {
#if defined(_WIN32)
    return pf::write_some(h, buf, len, closed);
#else
    closed = false;
    for (;;) {
        const auto n = ::write(h.get(), buf, len);
        if (n > 0) return static_cast<std::size_t>(n);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        closed = true;   // EPIPE
        return 0;
    }
#endif
}

/// Read everything currently available. Never blocks: the handles are
/// non-blocking and we only get here because the reactor said ready.
/// `closed` (optional) is set when the writer has closed the pipe.
std::size_t drain_into(pf::borrowed_handle h, std::string& out, std::size_t cap,
                       bool& truncated, bool* closed = nullptr) {
    char buf[64 * 1024];
    std::size_t got = 0;
    for (;;) {
        bool eof = false;
        const auto n = read_ready(h, buf, sizeof buf, eof);
        if (eof && closed) *closed = true;
        if (n > 0) {
            got += n;
            // Past the cap we keep READING and discard. Stopping the drain
            // would leave the child blocked on a full pipe, which is how a
            // command that produced too much output became a command that
            // never finished.
            if (out.size() < cap) {
                const auto room = cap - out.size();
                const auto take = std::min(room, n);
                out.append(buf, take);
                if (take < n) truncated = true;
            } else {
                truncated = true;
            }
            continue;
        }
        break;   // nothing left, or EOF
    }
    return got;
}

std::optional<std::chrono::seconds> env_wall_override() {
    const char* e = std::getenv("AGENTTY_TOOL_HARD_TIMEOUT_SECS");
    if (!e || !e[0]) return std::nullopt;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || v < 0) return std::nullopt;
    return std::chrono::seconds{v};
}

class JaalSession;   // below

namespace session_detail {
// What the sandbox handed back with the child: the seccomp listener the pump
// must answer, and cgroup.kill. Empty when there's no sandbox.
struct SandboxHooks {
    int                   supervisor_fd = -1;
    std::function<bool()> service_broker;   // answer one notification
    std::function<bool()> kill_tree;        // false = fall back to the group
};
}  // namespace session_detail
}  // namespace
}  // namespace agentty::tools::util
// Moved once into the pump, never shared: the closures own what they
// capture (the listener and the cgroup, by shared_ptr only they hold).
MAYA_SENDABLE(agentty::tools::util::session_detail::SandboxHooks);
namespace agentty::tools::util {
namespace {

namespace session_detail {
// A background session's reactor, split by who may touch what:
//   * adopt(): once, by the session's constructor, before the pump exists
//     (so before any other thread can see it). Needs a PumpKey too.
//   * wake(): any thread, any time (the Reactor concept makes it
//     thread-safe and async-signal-safe)
//   * drive(): needs a PumpKey, and only JaalSession can make one, which
//     it does in pump() alone. So registering and waiting happen on the
//     pump's thread and nowhere else.
// That split is what makes it Sync (the opt-in below the namespace).
class PumpKey {
    PumpKey() = default;
    friend class ::agentty::tools::util::JaalSession;
};
class PumpReactor {
  public:
    PumpReactor() = default;
    // Never copied or reseated.
    PumpReactor(const PumpReactor&)            = delete;
    PumpReactor& operator=(const PumpReactor&) = delete;
    void adopt(PumpKey, reactor_t r) { r_.emplace(std::move(r)); }
    void wake() const noexcept { if (r_) r_->waker().wake(); }
    [[nodiscard]] bool valid() const noexcept { return r_.has_value(); }
    reactor_t& drive(PumpKey) noexcept { return *r_; }
  private:
    std::optional<reactor_t> r_;
};
}  // namespace session_detail
}  // namespace
}  // namespace agentty::tools::util
// wake() is thread-safe; drive() is reachable only from the pump (PumpKey).
MAYA_SYNC(agentty::tools::util::session_detail::PumpReactor);
namespace agentty::tools::util {
namespace {

/// A started program, polled for later: an actor.
///
/// ONE thread, the pump, owns the child, the pipe and the reactor. It is the
/// only writer of the output. The handle (this class) and the pump share one
/// `Core`: a guarded Stream (the only copy of the output, plus the read
/// cursor and the end state), a stop flag, and the reactor's waker. Core is
/// co_owned and Sync, so the handle and the pump can each outlive the other
/// and nothing is freed under either.
///
///   * read(): one exclusive section takes what's new and moves the cursor,
///     so concurrent reads get disjoint bytes.
///   * stop(): sets the flag and wakes the pump through the waker. Never
///     blocks. The pump sends SIGTERM, then SIGKILL after a grace.
///   * destruction: stop(), and that's all. The pump finishes the kill, keeps
///     draining to EOF, reaps, and exits on its own. It runs as an isolated
///     background job, so it never ties up a pool worker, and agentty's
///     teardown bounds it at exit.
///
/// Lock order: the Stream is a LEAF whose bodies only move bytes and flags.
/// read() is an mcp-cpp virtual whose signature the library fixes, so it
/// re-states `unlocked` with thread_root at that one entry (listed in
/// tests/lint/allowlist.txt); sound because mcp-cpp calls it only from a
/// tool body, outside every agentty lock. stop() takes no capability at all:
/// a leaf lock is the only thing it touches.
class JaalSession final : public mt::Session {
  public:
    JaalSession(process_t p, std::size_t window, session_detail::SandboxHooks hooks = {}) {
        // Core owns the reactor: the handle and the pump both co-own Core,
        // so the reactor (and the eventfd behind its waker) lives until the
        // last of them is gone. stop() can never wake a closed fd. It is
        // handed in here, before the pump exists, so nothing races it.
        if (auto r = reactor_t::create()) core_->reactor.adopt(PumpKey{}, std::move(*r));
        if (!core_->reactor.valid()) {
            // No reactor: nothing can watch the child. Kill it and report
            // the end straight away, so a caller isn't left polling forever.
            (void)p.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            mt::ExecOutcome oc = mt::StartFailed{"no reactor"};
            if (auto st = p.reap()) oc = mt::Signalled{st->code};
            maya::thread_root([&](const maya::unlocked& c) {
                core_->s.with(c, [](Stream& s, mt::ExecOutcome o) {
                    s.end(std::move(o));
                    s.eof = true;
                }, std::move(oc));
            });
            return;
        }
        ::agentty::util::run_isolated_detached("exec.session.pump",
            [](const maya::unlocked& c, std::stop_token st, process_t proc,
               std::size_t win, core_t core, session_detail::SandboxHooks hk) {
                pump(c, std::move(st), std::move(proc), win, std::move(core), std::move(hk));
            },
            std::move(p), window, core_, std::move(hooks));
    }
    ~JaalSession() override { stop(); }

    [[nodiscard]] Read read(std::chrono::milliseconds wait, std::size_t max) override {
        return maya::thread_root([&](const maya::unlocked& c) {
            auto [t, _] = core_->s.wait_with_for(c, wait,
                [](const Stream& s, std::size_t) { return s.fresh() || s.ended; },
                [](Stream& s, std::size_t m) { return s.take(m); }, max);
            Read r;
            r.output  = std::move(t.output);
            r.dropped = t.dropped;
            r.running = t.running;
            r.eof     = t.eof;
            r.outcome = std::move(t.outcome);
            r.uptime  = std::chrono::duration_cast<std::chrono::seconds>(
                (t.running ? clock_t_::now() : t.ended_at) - core_->started);
            return r;
        });
    }

    void stop() noexcept override {
        // One atomic flag and one eventfd write: both are safe from any
        // thread, under any lock, at any time. That's why stop and the
        // destructor never block.
        if (core_->stop.exchange(true)) return;
        core_->reactor.wake();
    }

  private:
    // The pump's reactor: see PumpReactor above the class.
    using PumpKey = session_detail::PumpKey;
    using PumpReactor = session_detail::PumpReactor;

    // The output, the cursor, and the end: everything read() and the pump
    // both touch. guarded<T>: reachable only inside a lock body, nothing
    // escapes but owned values.
    struct Stream {
        std::string  window;            // the newest `cap` bytes
        std::size_t  base = 0;          // absolute offset of window[0]
        std::size_t  seen = 0;          // absolute offset read() has handed out
        std::size_t  cap  = 128 * 1024;
        bool         eof   = false;     // pipe closed: no more output
        bool         ended = false;     // child reaped
        mt::ExecOutcome outcome = mt::Exited{0};
        clock_t_::time_point ended_at{};

        [[nodiscard]] bool fresh() const noexcept { return base + window.size() > seen; }

        void append(std::string_view bytes) {
            window.append(bytes);
            if (window.size() > cap) {
                const auto cut = window.size() - cap;
                window.erase(0, cut);
                base += cut;
            }
        }
        void end(mt::ExecOutcome o) {
            ended    = true;
            outcome  = std::move(o);
            ended_at = clock_t_::now();
        }

        // What read() takes: plain owned values.
        struct Taken {
            std::string  output;
            std::size_t  dropped = 0;
            bool         running = true;
            bool         eof = false;
            std::optional<mt::ExecOutcome> outcome;
            clock_t_::time_point ended_at{};
        };

        Taken take(std::size_t max) {
            Taken r;
            const auto end = base + window.size();
            auto from = std::max(seen, base);
            r.dropped = from - seen;                      // scrolled out unseen
            if (end - from > max) { r.dropped += (end - from) - max; from = end - max; }
            r.output  = window.substr(from - base, end - from);
            seen      = end;
            r.running = !ended;
            r.eof     = eof;
            if (ended) r.outcome = outcome;
            r.ended_at = ended_at;
            return r;
        }
    };

    // What the handle and the pump share, co_owned by both, and Sync:
    //   s        the output stream: guarded, a leaf
    //   stop     set by the handle, read by the pump
    //   reactor  the pump's reactor, owned HERE so the eventfd behind its
    //            waker lives as long as anyone can call stop()
    // An aggregate, so jaal walks it and checks every field is Sync.
    struct Core {
        maya::guarded<Stream>              s;
        std::atomic<bool>                  stop{false};
        const clock_t_::time_point         started = clock_t_::now();
        PumpReactor                        reactor;
    };
    using core_t = maya::co_owned<Core>;

    static void pump(const maya::unlocked& cap, std::stop_token pool_stop, process_t proc,
                     std::size_t window, core_t core, session_detail::SandboxHooks hooks) {
        // The pump is the only thread that waits on or registers with the
        // reactor; Core owns it so the waker stays valid for stop().
        auto& reactor = core->reactor.drive(PumpKey{});
        core->s.with(cap, [](Stream& s, std::size_t w) { s.cap = w; }, window);
        auto exit_reg = reactor.watch(proc.exit_handle().get(), pf::interest::read, kExitToken);
        if (!exit_reg) {
            // Same rule as run_child: an exit nobody watches is a session
            // that never ends. Stop it and end the stream now.
            (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            core->stop.store(true);
        }
        std::optional<reactor_t::registration> out_reg, broker_reg;
        if (auto h = proc.stdout_handle())
            if (auto r = reactor.watch(h->get(), pf::interest::read, kOutToken))
                out_reg = std::move(*r);
        // The seccomp listener. The kernel parks a guest thread on every
        // brokered syscall until it's answered, so this has to be watched
        // for as long as the child lives, same as run_child does. A POSIX fd:
        // Windows has no broker, and its reactor watches HANDLEs, not ints.
#if !defined(_WIN32)
        if (hooks.supervisor_fd >= 0 && hooks.service_broker)
            if (auto r = reactor.watch(hooks.supervisor_fd, pf::interest::read, kBrokerToken))
                broker_reg = std::move(*r);
#endif
        // SIGKILL for the whole tree: cgroup.kill when the sandbox gave us
        // one (a setsid() descendant can't leave it), else the group.
        auto kill_all = [&] {
            if (!(hooks.kill_tree && hooks.kill_tree()))
                (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
        };

        constexpr auto kGrace = std::chrono::seconds{2};
        bool exited = false, eof = !out_reg;
        std::optional<clock_t_::time_point> kill_at;
        // After a stop, the pipe can stay open forever if a descendant left
        // the process group. Stop waiting for EOF this long after the exit.
        std::optional<clock_t_::time_point> give_up_eof;

        while (!(exited && eof)) {
            // Stop: asked by the handle, or the process is shutting down.
            const bool stopping = core->stop.load() || pool_stop.stop_requested();
            if (stopping && !kill_at && !exited) {
#if defined(_WIN32)
                // A graceful stop only ends the leader here, and a surviving
                // grandchild holds stdout open, so kill the job first.
                if (hooks.kill_tree) (void)hooks.kill_tree();
#endif
                (void)proc.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
                kill_at = clock_t_::now() + kGrace;
            }
            if (kill_at && !exited && clock_t_::now() >= *kill_at) {
                kill_all();
                kill_at = clock_t_::time_point::max();   // once
            }
            if (exited && stopping && !give_up_eof)
                give_up_eof = clock_t_::now() + std::chrono::milliseconds{500};
            if (give_up_eof && clock_t_::now() >= *give_up_eof) break;

            std::optional<std::chrono::milliseconds> wait;   // none: until an event
            if (kill_at && *kill_at != clock_t_::time_point::max())
                wait = std::chrono::ceil<std::chrono::milliseconds>(*kill_at - clock_t_::now());
            if (give_up_eof)
                wait = std::chrono::ceil<std::chrono::milliseconds>(*give_up_eof - clock_t_::now());
            if (wait && wait->count() < 0) wait = std::chrono::milliseconds{0};

            auto res = reactor.wait(wait);
            if (!res) break;
            for (std::uint8_t i = 0; i < res->count; ++i) {
                const auto& r = res->ready[i];
                if (r.token == kOutToken && (r.readable || r.hangup)) {
                    std::string chunk;
                    bool lost = false, closed = false;
                    if (auto h = proc.stdout_handle())
                        drain_into(*h, chunk, std::numeric_limits<std::size_t>::max(), lost, &closed);
                    if (!chunk.empty())
                        core->s.with(cap, [](Stream& s, std::string c) { s.append(c); },
                                     std::move(chunk));
                    // Drained and closed: stop watching, or the reactor
                    // reports the hangup on every wait and the loop spins.
                    if (closed) { out_reg.reset(); eof = true; }
                } else if (r.token == kExitToken && (r.readable || r.hangup)) {
                    exited = true;
                    exit_reg = {};
                } else if (r.token == kBrokerToken && (r.readable || r.hangup)) {
                    // false: the listener is done (guest gone). Stop watching
                    // it, or a hung-up fd wakes this loop forever.
                    if (!hooks.service_broker()) broker_reg.reset();
                }
            }
            if (exited && !eof) {
                // Publish the end now; output may keep coming from a child
                // that inherited the pipe, and read() reports both.
                publish_exit(cap, proc, core);
            }
        }
        if (!exited) {
            // Reactor failure, or the EOF give-up with the child alive: make
            // sure it's dead, then give the kernel a moment to make it
            // reapable (reap() refuses rather than block).
            kill_all();
            for (int i = 0; i < 100; ++i) {
                publish_exit(cap, proc, core);
                if (core->s.read(cap, [](const Stream& s) { return s.ended; })) break;
                (void)maya::delay_for(cap, std::stop_token{}, std::chrono::milliseconds{20});
            }
        }
        publish_exit(cap, proc, core);
        core->s.with(cap, [](Stream& s) {
            s.eof = true;
            // Still not reapable after 2 s of SIGKILL: report it ended
            // anyway, so no caller waits on it forever.
            if (!s.ended) s.end(mt::ExecOutcome{mt::Signalled{9}});
        });
    }

    // Reap once and publish the outcome. Idempotent.
    static void publish_exit(const maya::unlocked& cap, process_t& proc, const core_t& core) {
        if (core->s.read(cap, [](const Stream& s) { return s.ended; })) return;
        mt::ExecOutcome oc = mt::Signalled{0};
        if (auto s = proc.reap())
            oc = (s->how == pf::exit_status::kind::exited)
                     ? mt::ExecOutcome{mt::Exited{s->code}}
                     : mt::ExecOutcome{mt::Signalled{s->code}};
        else
            return;   // not reapable yet
        core->s.with(cap, [](Stream& s, mt::ExecOutcome o) { s.end(std::move(o)); },
                     std::move(oc));
    }

    core_t core_ = core_t::make();
};

class JaalExec final : public mt::Exec {
  public:
    explicit JaalExec(ExecDefaults d) : d_(d) {}

    // Put the sandbox on a child about to start, the same way for run and
    // start. The call's extra roots (an editor session's folders) are
    // writable inside the sandbox too. On darwin jaal can't adopt a foreign
    // child (no pidfd), so the sandbox goes into the argv instead
    // (sandbox-exec, same posture). Returns why it couldn't: fail closed, a
    // profile we can't build must not become an unconfined run.
    [[nodiscard]] static std::optional<std::string> confine(const mt::Call& call, ChildRun& run) {
        std::vector<std::string> roots;
        for (const auto& r : call.scope_extra_roots) roots.push_back(r.string());
#if defined(__linux__) || defined(_WIN32)
        run.spawn_adopted = sandbox_spawner(run.argv, run.cwd, roots,
                                            ::agentty::tools::host_call(call.host).trusted,
                                            run.env);
#elif defined(__APPLE__)
        namespace sb = agentty::tools::util::sandbox;
        if (sb::is_active()) {
            auto wrapped = sb::wrap_argv_for_exec(run.argv, run.cwd, roots);
            if (!wrapped) return "sandbox: " + wrapped.error();
            run.argv = std::move(*wrapped);
        }
#endif
        return std::nullopt;
    }

    [[nodiscard]] mt::ExecResult run(const mt::Call& call, const mt::ExecRequest& req) override {
        ChildRun run;
        run.argv.reserve(req.program.args.size() + 1);
        run.argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) run.argv.push_back(a);
        run.cwd = req.cwd.value_or(std::string{});
        run.env = req.env;

        const std::chrono::seconds idle = req.budgets.idle.value_or(d_.idle);
        run.idle = idle;
        run.wall = [&]() -> std::chrono::seconds {
            if (const auto o = env_wall_override()) return *o;
            if (req.budgets.wall) return *req.budgets.wall;
            return std::max(idle * d_.wall_multiple, d_.wall_floor);
        }();
        run.kill_grace       = d_.kill_grace;
        run.max_output_bytes = req.max_output_bytes.value_or(d_.max_output_bytes);
        if (req.stop_when)
            run.stop_when = [&req](std::string_view so_far) { return req.stop_when(so_far); };
        if (auto bad = confine(call, run)) {
            mt::ExecResult er;
            er.outcome = mt::StartFailed{std::move(*bad)};
            return er;
        }
        // The tool call's live output and cancel: where its output goes and
        // whether its user hit Esc. Only the tail a card shows is copied and
        // checked, so a long command doesn't re-scan megabytes every tick.
        if (call.progress)
            run.on_progress = [&call](std::string_view raw) {
                call.progress(::mcp::tools::util::to_valid_utf8(
                    std::string{progress_tail(raw)}));
            };
        if (call.stop.stop_possible())
            run.stop_requested = [st = call.stop] { return st.stop_requested(); };

        auto r = run_child(run);

        mt::ExecResult out;
        out.output    = ::mcp::tools::util::to_valid_utf8(std::move(r.output));
        out.truncated = r.truncated;
        out.elapsed   = r.elapsed;
        if (!r.started)            out.outcome = mt::StartFailed{r.start_error};
        else if (r.timed_out_idle) out.outcome = mt::TimedOut{mt::TimedOut::budget::idle};
        else if (r.timed_out_wall) out.outcome = mt::TimedOut{mt::TimedOut::budget::wall};
        else if (r.stopped_early)  out.outcome = mt::StoppedEarly{};
        else if (r.cancelled)      out.outcome = mt::Cancelled{};
        else if (r.exited)         out.outcome = mt::Exited{r.exit_code};
        else                       out.outcome = mt::Signalled{r.signal};
        return out;
    }

    [[nodiscard]] std::expected<std::shared_ptr<mt::Session>, std::string>
    start(const mt::Call& call, const mt::ExecRequest& req) override {
        ChildRun run;
        run.argv.reserve(req.program.args.size() + 1);
        run.argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) run.argv.push_back(a);
        run.cwd = req.cwd.value_or(std::string{});
        run.env = req.env;
        if (auto bad = confine(call, run)) return std::unexpected(std::move(*bad));

        auto st = start_child(run);
        if (!st) return std::unexpected(st.error());
        // The pump answers the broker and owns cgroup.kill, as run_child does.
        session_detail::SandboxHooks hooks{st->supervisor_fd, std::move(st->service_broker),
                                           std::move(st->kill_tree)};
        return std::static_pointer_cast<mt::Session>(
            std::make_shared<JaalSession>(std::move(st->proc),
                                          req.max_output_bytes.value_or(
                                              d_.max_output_bytes),
                                          std::move(hooks)));
    }

    [[nodiscard]] bool stops_whole_tree() const noexcept override {
        // Windows: a job object holds the tree. POSIX: a process group is not
        // a tree, since a descendant that calls setsid() leaves it.
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    }

  private:
    ExecDefaults d_;
};

}  // namespace

std::shared_ptr<mt::Exec> make_exec(ExecDefaults defaults) {
    return std::make_shared<JaalExec>(defaults);
}

ChildResult run_child(const ChildRun& run) {
    ChildResult r;
    const auto started_at = clock_t_::now();
    auto stamp = [&] {
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - started_at);
    };

    auto st = start_child(run);
    if (!st) { r.start_error = st.error(); stamp(); return r; }
    auto& proc = st->proc;

    auto reactor = reactor_t::create();
    if (!reactor) {
        (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
        r.start_error = "reactor: " + std::string{reactor.error().what};
        stamp();
        return r;
    }
    r.started = true;

    // The exit MUST be watched: the loop below only learns of it through
    // this registration, so without it every run would sit until its
    // deadline. jaal never returns a process whose exit can't fire, so this
    // failing means the reactor itself refused; say so and stop the child.
    auto exit_reg = reactor->watch(proc.exit_handle().get(), pf::interest::read, kExitToken);
    if (!exit_reg) {
        (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
        r.started = false;
        r.start_error = "cannot watch the child's exit: " + std::string{exit_reg.error().what};
        stamp();
        return r;
    }
    std::optional<reactor_t::registration> out_reg, in_reg, broker_reg;
#if defined(_WIN32)
    ::agentty::util::WorkerGroup feeder{"exec.stdin_feed"};   // destroyed after the reap
    bool feeder_started = false;   // a feeder job is running, so there's something to join
#endif
    if (auto h = proc.stdout_handle())
        if (auto reg = reactor->watch(h->get(), pf::interest::read, kOutToken))
            out_reg = std::move(*reg);
#if !defined(_WIN32)
    // The seccomp broker, when the policy delegates syscalls. It MUST be
    // serviced: the kernel blocks the guest until someone answers.
    if (st->supervisor_fd >= 0 && st->service_broker)
        if (auto reg = reactor->watch(st->supervisor_fd, pf::interest::read, kBrokerToken))
            broker_reg = std::move(*reg);
#endif

    // stdin is fed from this loop, a chunk per wake, so a child that writes
    // before it reads cannot deadlock against us.
    std::size_t fed = 0;
    auto feed = [&] {
        const auto h = proc.stdin_handle();
        if (!h) return;
        while (fed < run.stdin_data.size()) {
            bool closed = false;
            const auto n = write_ready(*h, run.stdin_data.data() + fed,
                                       run.stdin_data.size() - fed, closed);
            if (n > 0) { fed += n; continue; }
            if (!closed) return;   // full: wait to be writable again
            break;                 // it closed its stdin, nothing more to give
        }
        in_reg.reset();
        proc.close_stdin();
    };
    if (auto h = proc.stdin_handle()) {
#if defined(_WIN32)
        // No writable readiness for an anonymous pipe, so the feed runs as a
        // job with its own copy of the handle. The loop keeps draining output
        // meanwhile; once the child ends, the write fails and the job ends.
        // feeder.stop() after the reap is the join.
        if (auto dup = maya::platform::duplicate_handle(h->get())) {
            proc.close_stdin();
            feeder_started = true;
            feeder.post([](const maya::unlocked&, std::stop_token, maya::platform::owned_handle own,
                           std::string data) {
                std::size_t off = 0;
                while (off < data.size()) {
                    bool closed = false;
                    const auto n = maya::platform::write_some(
                        maya::platform::borrowed_handle{own.get()},
                        data.data() + off, data.size() - off, closed);
                    if (closed || n == 0) break;
                    off += n;
                }
            }, maya::platform::owned_handle{*dup}, run.stdin_data);   // `own` closes at the end: EOF for the child
        } else {
            proc.close_stdin();
        }
#else
        ::fcntl(h->get(), F_SETFL, ::fcntl(h->get(), F_GETFL) | O_NONBLOCK);
        if (auto reg = reactor->watch(h->get(), pf::interest::write, kInToken))
            in_reg = std::move(*reg);
        feed();
#endif
    }

    auto last_progress = clock_t_::time_point{};
    auto progress = [&](bool force) {
        if (!run.on_progress) return;
        const auto now = clock_t_::now();
        if (!force && now - last_progress < std::chrono::milliseconds{80}) return;
        last_progress = now;
        run.on_progress(r.output);
    };

    // ── the two clocks ──────────────────────────────────────────────────
    const bool has_idle = run.idle.count() > 0;
    const bool has_wall = run.wall.count() > 0;
    const auto wall_at  = started_at + run.wall;
    auto       idle_at  = started_at + run.idle;
    // Cancel has no wake source of its own, so wake at least this often to
    // ask; without a stop_requested the deadlines alone decide.
    constexpr std::chrono::milliseconds kCancelTick{100};

    bool stopping = false;   // SIGTERM sent
    std::optional<clock_t_::time_point> kill_at;
    std::optional<clock_t_::time_point> give_up_at;   // after SIGKILL
    constexpr std::chrono::seconds kAfterKill{5};
    bool exited = false;

    for (;;) {
        const auto now = clock_t_::now();
        auto next = now + std::chrono::hours{24};
        if (has_idle && !stopping) next = std::min(next, idle_at);
        if (has_wall && !stopping) next = std::min(next, wall_at);
        if (kill_at) next = std::min(next, *kill_at);
        if (give_up_at) next = std::min(next, *give_up_at);
        if (run.stop_requested && !stopping) next = std::min(next, now + kCancelTick);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            next > now ? next - now : std::chrono::milliseconds::zero());

        auto res = reactor->wait(ms);
        if (!res) break;

        bool saw_output = false;
        for (std::uint8_t i = 0; i < res->count; ++i) {
            const auto& e = res->ready[i];
            if (e.token == kOutToken && (e.readable || e.hangup)) {
                std::size_t got = 0;
                if (auto h = proc.stdout_handle())
                    got = drain_into(*h, r.output, run.max_output_bytes, r.truncated);
                if (got > 0) saw_output = true;
                // Hung up and drained: stop watching, or the reactor reports
                // the hangup on every wait and this loop spins until exit.
                else if (e.hangup) out_reg.reset();
            } else if (e.token == kExitToken && (e.readable || e.hangup)) {
                exited = true;
            } else if (e.token == kInToken && (e.writable || e.hangup)) {
                feed();
            } else if (e.token == kBrokerToken && e.readable) {
                if (st->service_broker && !st->service_broker()) broker_reg.reset();
            }
        }
        // Output resets the idle clock and only the idle clock: a chatty
        // runaway keeps that one moving forever, so the wall clock ends it.
        if (saw_output) {
            idle_at = clock_t_::now() + run.idle;
            progress(false);
        }
        if (saw_output && !stopping && run.stop_when && run.stop_when(r.output)) {
            r.stopped_early = true;
            (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            break;
        }
        if (exited) break;

        const auto t = clock_t_::now();
        if (!stopping) {
            if (run.stop_requested && run.stop_requested()) r.cancelled = true;
            else if (has_idle && t >= idle_at)              r.timed_out_idle = true;
            else if (has_wall && t >= wall_at)              r.timed_out_wall = true;
            if (r.cancelled || r.timed_out_idle || r.timed_out_wall) {
                // A graceful stop only ends the leader on Windows, and a
                // surviving grandchild holds stdout open, so kill the job first.
                bool tree_killed = false;
#if defined(_WIN32)
                tree_killed = st->kill_tree && st->kill_tree();
#endif
                // Ask first: a shell given SIGTERM reaps its own children.
                (void)proc.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
                stopping = true;
                kill_at  = t + run.kill_grace;
                stop_trace(started_at, "stop cancelled=%d idle=%d wall=%d tree_kill=%d",
                           int(r.cancelled), int(r.timed_out_idle), int(r.timed_out_wall),
                           int(tree_killed));
                continue;
            }
        }
        if (kill_at && t >= *kill_at) {
            // The whole tree. kill_tree is claybin's cgroup.kill, which a
            // setsid() in a grandchild cannot escape; the group signal is
            // the fallback.
            const bool kt = st->kill_tree && st->kill_tree();
            stop_trace(started_at, "force tree_kill=%d", int(kt));
            if (!kt)
                (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            kill_at.reset();
            // SIGKILL is sent; the exit should follow at once. If it doesn't
            // (a task in D state, a kill that didn't land), stop waiting
            // rather than sleep in the reactor with no deadline at all.
            give_up_at = t + kAfterKill;
        }
        if (give_up_at && t >= *give_up_at) {
            (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            break;
        }
    }

    // The exit says the bytes are done, not that we have them.
    stop_trace(started_at, "loop ended");
    if (auto h = proc.stdout_handle())
        drain_into(*h, r.output, run.max_output_bytes, r.truncated);
    stop_trace(started_at, "drained");
    progress(true);

    if (auto status = proc.reap()) {
        if (status->how == pf::exit_status::kind::exited) { r.exited = true; r.exit_code = status->code; }
        else                                              { r.signalled = true; r.signal = status->code; }
    } else {
        r.signalled = true;   // gone, and nothing to tell us how
    }
    stop_trace(started_at, "reaped");
#if defined(_WIN32)
    // A grandchild can still hold the stdin pipe's read end, leaving the
    // feeder blocked in WriteFile. Ending the job closes every copy, so the
    // write fails and the barrier below can't hang.
    (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
    // run_child is a blocking leaf helper (it locks nothing of ours). Its
    // callers are tool bodies, mcp-cpp's Exec, the keystore/clipboard CLI
    // probes, and the RAG docs converter. That last one runs inside the RAG
    // index lock, where a join can't be re-stated (#112), but it passes no
    // stdin, so there is no feeder to join. Only a run that started one joins
    // it, and those all come from outside every agentty lock.
    if (feeder_started) {
        maya::thread_root([&](const maya::unlocked& c) { feeder.stop(c); });
        stop_trace(started_at, "feeder joined");
    }
#endif
    stamp();
    return r;
}

// The scan fan-out mcp-cpp's and rag-cpp's search tools call through their
// Splitter (a std::function whose signature those libraries fix). Joining
// helpers needs `unlocked`; it is re-stated here, the one reviewed root for
// the splitter (tests/lint/allowlist.txt). Sound: the libraries call it from
// a tool body, never from inside an agentty lock body, and `fn` (their
// per-share scan) takes no agentty lock.
//
// Never hand this splitter to an object that runs inside a lock body: the
// RAG index builds and searches holding rag_index, so its engines get an
// inline Splitter{} (rag/adapter.cpp make_engine_config, #112).
void scope_fan_out(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (n <= 1) { if (n) fn(0); return; }
    maya::thread_root([&](const maya::unlocked& cap) {
        maya::parallel_for(cap, n, n, fn);   // joins every share before returning
    });
}

std::size_t fan_out_width() noexcept {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4 : hc;
}

mt::Splitter make_splitter() {
    return mt::Splitter{scope_fan_out, fan_out_width()};
}

}  // namespace agentty::tools::util
