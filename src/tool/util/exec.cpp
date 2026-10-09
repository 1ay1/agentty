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
#include <cstdio>
#include <string>
#include <vector>

#include <maya/runtime.hpp>
#include "agentty/util/background.hpp"   // util::WorkerGroup

#include <mcp/tools/util/utf8.hpp>

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/sandbox_claybin.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace agentty::tools::util {
namespace {

namespace pf = maya::platform;
namespace mt = ::mcp::tools;

using clock_t_ = std::chrono::steady_clock;

constexpr std::uint64_t kExitToken = 1;
constexpr std::uint64_t kOutToken  = 2;
constexpr std::uint64_t kBrokerToken = 3;
constexpr std::uint64_t kInToken   = 4;


/// The sandbox's way in, or empty when no sandbox is active.
///
/// claybin clones with namespace flags, installs a seccomp filter and enters
/// a cgroup -- none of it expressible as "fork then exec" -- so it spawns,
/// and run_child adopts the result. After that line the two paths are the
/// same object with the same conformance suite behind it.
[[nodiscard]] std::function<AdoptedChild(int)>
sandbox_spawner(const std::vector<std::string>& argv, const std::string& cwd) {
#if defined(__linux__)
    namespace sb = agentty::tools::util::sandbox;
    if (sb::is_active() && sb::detected_backend() == sb::Backend::Claybin) {
        return [argv, cwd](int out_fd) {
            auto r = sb::spawn_in_sandbox(argv, cwd, out_fd);
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
#else
    (void)argv; (void)cwd;
#endif
    return {};
}

struct started {
    pf::posix_process    proc;
    int                  supervisor_fd = -1;
    std::function<bool()> service_broker;
    std::function<bool()> kill_tree;
};

/// Spawn, or let the sandbox spawn and adopt. stdout and stderr share one
/// pipe: the interleaving a terminal would show.
[[nodiscard]] std::expected<started, std::string>
start_child(const ChildRun& run) {
    if (run.argv.empty()) return std::unexpected(std::string{"empty command"});
    if (run.spawn_adopted) {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0)
            return std::unexpected(std::string{"pipe: "} + std::strerror(errno));
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

    pf::process_spec spec;
    spec.argv         = run.argv;
    spec.cwd          = run.cwd;
    spec.env          = run.env;
    spec.merge_stderr = true;
    spec.new_session  = true;
    if (!run.stdin_data.empty()) spec.stdin_from = pf::stream_to::pipe;
    auto p = pf::posix_process::spawn(spec);
    if (!p) return std::unexpected(std::string{p.error().what} + ": " + std::strerror(p.error().native));
    return started{std::move(*p), -1, nullptr, nullptr};
}

/// Read everything currently available. Never blocks: the handles are
/// non-blocking and we only get here because the reactor said ready.
std::size_t drain_into(int fd, std::string& out, std::size_t cap, bool& truncated) {
    char buf[64 * 1024];
    std::size_t got = 0;
    for (;;) {
        const auto n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            // Past the cap we keep READING and discard. Stopping the drain
            // would leave the child blocked on a full pipe, which is how a
            // command that produced too much output became a command that
            // never finished.
            if (out.size() < cap) {
                const auto room = cap - out.size();
                const auto take = std::min(room, static_cast<std::size_t>(n));
                out.append(buf, take);
                if (take < static_cast<std::size_t>(n)) truncated = true;
            } else {
                truncated = true;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;   // EAGAIN (nothing left) or 0 (EOF)
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

/// A started program, polled for later.
///
/// The drain runs on its own worker, and that is deliberate rather than
/// reluctant: a pipe nobody reads fills at 64 KiB and then BLOCKS the child.
/// Polls are seconds apart by nature -- a caller checks a dev server when it
/// wants to, not on a schedule -- so draining only inside poll() would stall
/// exactly the chatty servers this tool exists for. It is a jaal worker
/// (util::WorkerGroup): stop_token to wind it up, joined on destruction.
class JaalSession final : public mt::Session {
  public:
    JaalSession(pf::posix_process p, std::size_t cap)
        : proc_(std::move(p)), cap_(cap), started_(clock_t_::now()) {
        drain_.post([this](std::stop_token st) { pump(st); });
    }
    ~JaalSession() override {
        stop();
        drain_.stop();   // requests stop on the pump's token, then joins
    }

    [[nodiscard]] Update poll(std::chrono::milliseconds wait) override {
        // Wait for something to report rather than returning empty
        // immediately: a caller that polls in a loop should not spin, and one
        // that asks for 5s of news should get 5s of news.
        // Wakes the instant the pump writes news or the child ends. One
        // exclusive section that both drains and decides; returning a struct
        // by value is the only way out of the guarded state.
        auto [u, news] = st_.wait_with_for(wait,
            [](const Shared& s) { return !s.pending.empty() || !s.alive; },
            [](Shared& s) {
            Update u;
            u.output    = std::exchange(s.pending, {});
            u.truncated = std::exchange(s.truncated, false);
            u.running   = s.alive;
            if (!s.alive && !s.reported) {
                s.reported = true;
                u.outcome  = s.outcome;
            }
            return u;
        });
        (void)news;
        u.uptime = std::chrono::duration_cast<std::chrono::seconds>(clock_t_::now() - started_);
        return u;
    }

    void stop() override {
        if (!st_.read([](const Shared& s) { return s.alive; })) return;
        (void)proc_.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
        st_.with([](Shared& s, clock_t_::time_point at) { s.stopping = true; s.stop_at = at; },
                 clock_t_::now() + std::chrono::seconds{2});
    }

  private:
    void pump(std::stop_token st) {
        auto reactor = pf::poll_reactor::create();
        if (!reactor) return;
        auto exit_reg = reactor->watch(proc_.exit_handle().get(),
                                       pf::interest::read, kExitToken);
        std::optional<pf::poll_reactor::registration> out_reg;
        if (auto h = proc_.stdout_handle())
            if (auto r = reactor->watch(h->get(), pf::interest::read, kOutToken))
                out_reg = std::move(*r);

        bool exited = false;
        while (!st.stop_requested()) {
            auto res = reactor->wait(std::chrono::milliseconds{100});
            if (!res) break;
            for (std::uint8_t i = 0; i < res->count; ++i) {
                const auto& r = res->ready[i];
                if (r.token == kOutToken && (r.readable || r.hangup)) {
                    std::string chunk;
                    bool        lost = false;
                    if (auto h = proc_.stdout_handle())
                        drain_into(h->get(), chunk, cap_, lost);
                    if (!chunk.empty() || lost)
                        st_.with([](Shared& s, std::string c, bool l,
                                    std::size_t cap) {
                            if (s.pending.size() < cap) s.pending += c;
                            else                        s.truncated = true;
                            if (l) s.truncated = true;
                        }, std::move(chunk), lost, cap_);
                } else if (r.token == kExitToken && (r.readable || r.hangup)) {
                    exited = true;
                }
            }
            if (!exited && st_.read([](const Shared& s, clock_t_::time_point now) {
                    return s.stopping && now >= s.stop_at;
                }, clock_t_::now())) {
                (void)proc_.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            }
            if (exited) break;
        }

        // Whatever was written before the end is still in the pipe.
        std::string tail;
        bool        lost = false;
        if (auto h = proc_.stdout_handle())
            drain_into(h->get(), tail, cap_, lost);

        mt::ExecOutcome oc = mt::Signalled{0};
        if (auto s = proc_.reap())
            oc = (s->how == pf::exit_status::kind::exited)
                     ? mt::ExecOutcome{mt::Exited{s->code}}
                     : mt::ExecOutcome{mt::Signalled{s->code}};

        st_.with([](Shared& s, std::string t, bool l, mt::ExecOutcome o) {
            if (!t.empty()) s.pending += t;
            if (l) s.truncated = true;
            s.outcome = o;
            s.alive   = false;
        }, std::move(tail), lost, oc);
    }

    /// Everything two threads touch, in one place, reachable only while the
    /// lock is held. guarded<T> makes the unsafe forms unwriteable rather
    /// than merely wrong: there is no get(), nothing escapes (results must
    /// be Sendable), and no second lock can be NAMED inside with() because
    /// the function is captureless. agentty had 44 raw std::mutex and zero
    /// of these; this is the first, and a drain thread feeding a poller is
    /// exactly the shape it is for.
    struct Shared {
        std::string     pending;
        bool            truncated = false;
        bool            reported  = false;
        bool            alive     = true;
        bool            stopping  = false;
        clock_t_::time_point stop_at{};   // SIGKILL after this, once stopping
        mt::ExecOutcome outcome   = mt::Exited{0};
    };

    pf::posix_process    proc_;
    std::size_t          cap_;
    clock_t_::time_point started_;
    maya::guarded<Shared> st_;
    // Last member: destroyed (joined) first, before anything the pump uses.
    ::agentty::util::WorkerGroup drain_{"exec.session.drain"};
};

class JaalExec final : public mt::Exec {
  public:
    explicit JaalExec(ExecDefaults d) : d_(d) {}

    [[nodiscard]] mt::ExecResult run(const mt::ExecRequest& req) override {
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
        run.spawn_adopted = sandbox_spawner(run.argv, run.cwd);

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
    start(const mt::ExecRequest& req) override {
        ChildRun run;
        run.argv.reserve(req.program.args.size() + 1);
        run.argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) run.argv.push_back(a);
        run.cwd = req.cwd.value_or(std::string{});
        run.env = req.env;
        run.spawn_adopted = sandbox_spawner(run.argv, run.cwd);

        auto st = start_child(run);
        if (!st) return std::unexpected(st.error());
        // The broker is not serviced for a session: nothing polls the
        // listener between the caller's polls.
        return std::static_pointer_cast<mt::Session>(
            std::make_shared<JaalSession>(std::move(st->proc),
                                          req.max_output_bytes.value_or(
                                              d_.max_output_bytes)));
    }

    [[nodiscard]] bool stops_whole_tree() const noexcept override {
        // Honest: a process group is not a tree. A descendant that calls
        // setsid() leaves it and survives. A Linux host with a delegated
        // cgroup can do better, and when that backend lands this answer
        // comes from it rather than from here.
        return false;
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

    auto reactor = pf::poll_reactor::create();
    if (!reactor) {
        (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
        r.start_error = "reactor: " + std::string{reactor.error().what};
        stamp();
        return r;
    }
    r.started = true;

    auto exit_reg = reactor->watch(proc.exit_handle().get(), pf::interest::read, kExitToken);
    std::optional<pf::poll_reactor::registration> out_reg, in_reg, broker_reg;
    if (auto h = proc.stdout_handle())
        if (auto reg = reactor->watch(h->get(), pf::interest::read, kOutToken))
            out_reg = std::move(*reg);
    // The seccomp broker, when the policy delegates syscalls. It MUST be
    // serviced: the kernel blocks the guest until someone answers.
    if (st->supervisor_fd >= 0 && st->service_broker)
        if (auto reg = reactor->watch(st->supervisor_fd, pf::interest::read, kBrokerToken))
            broker_reg = std::move(*reg);

    // stdin is fed from this loop, a chunk per wake, so a child that writes
    // before it reads cannot deadlock against us.
    std::size_t fed = 0;
    auto feed = [&] {
        const auto h = proc.stdin_handle();
        if (!h) return;
        while (fed < run.stdin_data.size()) {
            const auto n = ::write(h->get(), run.stdin_data.data() + fed,
                                   run.stdin_data.size() - fed);
            if (n > 0) { fed += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            break;   // EPIPE: it closed its stdin, nothing more to give
        }
        in_reg.reset();
        proc.close_stdin();
    };
    if (auto h = proc.stdin_handle()) {
        ::fcntl(h->get(), F_SETFL, ::fcntl(h->get(), F_GETFL) | O_NONBLOCK);
        if (auto reg = reactor->watch(h->get(), pf::interest::write, kInToken))
            in_reg = std::move(*reg);
        feed();
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
    bool exited = false;

    for (;;) {
        const auto now = clock_t_::now();
        auto next = now + std::chrono::hours{24};
        if (has_idle && !stopping) next = std::min(next, idle_at);
        if (has_wall && !stopping) next = std::min(next, wall_at);
        if (kill_at) next = std::min(next, *kill_at);
        if (run.stop_requested && !stopping) next = std::min(next, now + kCancelTick);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            next > now ? next - now : std::chrono::milliseconds::zero());

        auto res = reactor->wait(ms);
        if (!res) break;

        bool saw_output = false;
        for (std::uint8_t i = 0; i < res->count; ++i) {
            const auto& e = res->ready[i];
            if (e.token == kOutToken && (e.readable || e.hangup)) {
                if (auto h = proc.stdout_handle())
                    if (drain_into(h->get(), r.output, run.max_output_bytes, r.truncated) > 0)
                        saw_output = true;
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
                // Ask first: a shell given SIGTERM reaps its own children.
                (void)proc.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
                stopping = true;
                kill_at  = t + run.kill_grace;
                continue;
            }
        }
        if (kill_at && t >= *kill_at) {
            // The whole tree. kill_tree is claybin's cgroup.kill, which a
            // setsid() in a grandchild cannot escape; the group signal is
            // the fallback.
            if (!(st->kill_tree && st->kill_tree()))
                (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
            kill_at.reset();
        }
    }

    // The exit says the bytes are done, not that we have them.
    if (auto h = proc.stdout_handle())
        drain_into(h->get(), r.output, run.max_output_bytes, r.truncated);
    progress(true);

    if (auto status = proc.reap()) {
        if (status->how == pf::exit_status::kind::exited) { r.exited = true; r.exit_code = status->code; }
        else                                              { r.signalled = true; r.signal = status->code; }
    } else {
        r.signalled = true;   // gone, and nothing to tell us how
    }
    stamp();
    return r;
}

void scope_fan_out(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (n <= 1) { if (n) fn(0); return; }
    maya::scope([&](maya::nursery& nur) {
        for (std::size_t i = 1; i < n; ++i)
            nur.spawn([&fn, i] { fn(i); });
        fn(0);   // the caller runs a share too
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
