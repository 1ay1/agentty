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

/// How the child was started, and what has to be serviced while it runs.
struct started {
    pf::posix_process    proc;
    int                  supervisor_fd = -1;
    std::function<bool()> service_broker;
};

/// Start `argv`, inside the sandbox when one is active.
///
/// Two ways in, one way to watch. claybin clones with namespace flags,
/// installs a seccomp filter and enters a cgroup -- none of it expressible
/// as "fork then exec" -- so it spawns, and jaal adopts the result. After
/// that line the two paths are the same object with the same conformance
/// suite behind it.
[[nodiscard]] std::expected<started, std::string>
start_child(const std::vector<std::string>& argv,
      const std::string& cwd,
      const std::vector<std::pair<std::string, std::string>>& env) {
    namespace sb = agentty::tools::util::sandbox;

#if defined(__linux__)
    if (sb::is_active() && sb::detected_backend() == sb::Backend::Claybin) {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0)
            return std::unexpected(std::string{"pipe: "} + std::strerror(errno));

        // One pipe for both streams: the interleaving a terminal would show.
        // cwd goes to the sandbox as a real chdir, not as a shell prefix.
        auto r = sb::spawn_in_sandbox(argv, cwd, fds[1]);
        ::close(fds[1]);
        if (!r.started) {
            ::close(fds[0]);
            return std::unexpected(r.start_error);
        }

        pf::posix_process::adopted_child c;
        c.pid       = r.pid;
        c.pidfd     = r.pidfd;     // claybin already opened one
        c.stdout_fd = fds[0];
        c.merged    = true;
        auto adopted = pf::posix_process::adopt(c);
        if (!adopted)
            return std::unexpected(std::string{adopted.error().what});

        return started{std::move(*adopted), r.supervisor_fd,
                       std::move(r.service_broker)};
    }
#endif

    pf::process_spec spec;
    spec.argv         = argv;
    spec.cwd          = cwd;
    spec.env          = env;
    spec.merge_stderr = true;
    spec.new_session  = true;
    auto p = pf::posix_process::spawn(spec);
    if (!p) return std::unexpected(std::string{p.error().what});
    return started{std::move(*p), -1, nullptr};
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
        const auto deadline = clock_t_::now() + wait;
        while (st_.read([](const Shared& s) { return s.pending.empty() && s.alive; })
               && clock_t_::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{20});

        // One exclusive section that both drains and decides. Returning a
        // struct by value is the only way out -- nothing can hand back a
        // reference into the guarded state.
        auto u = st_.with([](Shared& s) {
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
        u.uptime = std::chrono::duration_cast<std::chrono::seconds>(clock_t_::now() - started_);
        return u;
    }

    void stop() override {
        if (!st_.read([](const Shared& s) { return s.alive; })) return;
        (void)proc_.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
        stop_at_ = clock_t_::now() + std::chrono::seconds{2};
        st_.with([](Shared& s) { s.stopping = true; });
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
            if (st_.read([](const Shared& s) { return s.stopping; }) && !exited
                && clock_t_::now() >= stop_at_) {
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
        mt::ExecOutcome outcome   = mt::Exited{0};
    };

    pf::posix_process    proc_;
    std::size_t          cap_;
    clock_t_::time_point started_;
    maya::guarded<Shared> st_;
    clock_t_::time_point stop_at_{};
    // Last member: destroyed (joined) first, before anything the pump uses.
    ::agentty::util::WorkerGroup drain_{"exec.session.drain"};
};

class JaalExec final : public mt::Exec {
  public:
    explicit JaalExec(ExecDefaults d) : d_(d) {}

    [[nodiscard]] mt::ExecResult run(const mt::ExecRequest& req) override {
        mt::ExecResult out;

        // ── budgets ─────────────────────────────────────────────────────
        const auto idle = req.budgets.idle.value_or(d_.idle);
        const auto wall = [&] {
            if (const auto o = env_wall_override()) return *o;
            if (req.budgets.wall) return *req.budgets.wall;
            return std::max(idle * d_.wall_multiple, d_.wall_floor);
        }();
        const std::size_t cap = req.max_output_bytes.value_or(d_.max_output_bytes);

        // ── spawn ───────────────────────────────────────────────────────
        std::vector<std::string> argv;
        argv.reserve(req.program.args.size() + 1);
        argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) argv.push_back(a);

        auto st = start_child(argv, req.cwd.value_or(std::string{}), req.env);
        if (!st) {
            out.outcome = mt::StartFailed{st.error()};
            return out;
        }
        auto& proc = st->proc;

        auto reactor = pf::poll_reactor::create();
        if (!reactor) {
            out.outcome = mt::StartFailed{"could not create a reactor"};
            return out;
        }

        auto exit_reg = reactor->watch(proc.exit_handle().get(),
                                       pf::interest::read, kExitToken);
        std::optional<pf::poll_reactor::registration> out_reg;
        if (auto h = proc.stdout_handle()) {
            if (auto r = reactor->watch(h->get(), pf::interest::read, kOutToken))
                out_reg = std::move(*r);
        }
        // The seccomp broker, when the policy delegates syscalls. It MUST be
        // serviced: the kernel blocks the guest thread until someone answers,
        // so an unpolled listener is a hang, not a weaker wall. Watching it
        // here rather than on its own thread is the point of having a
        // reactor -- one wait covers output, exit and brokered calls.
        std::optional<pf::poll_reactor::registration> broker_reg;
        if (st->supervisor_fd >= 0 && st->service_broker) {
            if (auto r = reactor->watch(st->supervisor_fd, pf::interest::read,
                                        kBrokerToken))
                broker_reg = std::move(*r);
        }

        // ── the two clocks ──────────────────────────────────────────────
        const auto started  = clock_t_::now();
        const auto wall_at  = started + wall;
        auto       idle_at  = started + idle;
        const bool has_wall = wall > std::chrono::seconds::zero();

        bool                       stopping = false;   // SIGTERM sent
        std::optional<clock_t_::time_point> kill_at;
        std::optional<mt::ExecOutcome>      why;       // why we stopped it
        bool                       exited = false;
        bool                       stopped_early = false;

        for (;;) {
            const auto now = clock_t_::now();

            // Next thing to wake for. The reactor takes the timeout, so
            // there is no sleep-and-poll: we wake for output, for the exit,
            // or for a deadline, and nothing else.
            auto next = idle_at;
            if (has_wall && wall_at < next) next = wall_at;
            if (kill_at && *kill_at < next) next = *kill_at;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                next > now ? next - now : std::chrono::milliseconds::zero());

            auto res = reactor->wait(ms);
            if (!res) break;

            bool saw_output = false;
            for (std::uint8_t i = 0; i < res->count; ++i) {
                const auto& r = res->ready[i];
                if (r.token == kOutToken && (r.readable || r.hangup)) {
                    if (auto h = proc.stdout_handle())
                        if (drain_into(h->get(), out.output, cap, out.truncated) > 0)
                            saw_output = true;
                } else if (r.token == kExitToken && (r.readable || r.hangup)) {
                    exited = true;
                } else if (r.token == kBrokerToken && r.readable) {
                    // Answer the brokered syscall. Returns false when the
                    // listener is done; stop watching then rather than
                    // spinning on a dead fd.
                    if (st->service_broker && !st->service_broker())
                        broker_reg.reset();
                }
            }
            // Output resets the idle clock and ONLY the idle clock. That
            // asymmetry is the whole design: a chatty runaway keeps this
            // line running forever and the wall clock below is what ends it.
            if (saw_output) idle_at = clock_t_::now() + idle;

            // The caller had enough. Only it can know that -- "enough" is a
            // property of what it asked for, not of the process.
            if (saw_output && req.stop_when && req.stop_when(out.output)) {
                stopped_early = true;
                (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
                break;
            }

            if (exited) break;

            const auto t = clock_t_::now();
            if (!stopping && t >= idle_at) {
                why = mt::TimedOut{mt::TimedOut::budget::idle};
            } else if (!stopping && has_wall && t >= wall_at) {
                why = mt::TimedOut{mt::TimedOut::budget::wall};
            }
            if (why && !stopping) {
                // Ask first. A shell given SIGTERM reaps its own children;
                // SIGKILL leaves them for us to find.
                (void)proc.stop(pf::stop_mode::graceful, pf::stop_scope::tree);
                stopping = true;
                kill_at  = t + d_.kill_grace;
                continue;
            }
            if (stopping && kill_at && t >= *kill_at) {
                (void)proc.stop(pf::stop_mode::forceful, pf::stop_scope::tree);
                kill_at.reset();
            }
        }

        // The exit says the bytes are done, not that we have them: output
        // written before the exit is still in the pipe. jaal's conformance
        // suite pins that rule; honouring it is one more drain.
        if (auto h = proc.stdout_handle())
            drain_into(h->get(), out.output, cap, out.truncated);

        // ExecResult says "UTF-8 valid", so make it so. A child writes
        // whatever it likes, and a cap can cut a multi-byte codepoint in
        // half; handing that to a JSON encoder is a failure two layers
        // further on with nothing pointing back here.
        out.output = ::mcp::tools::util::to_valid_utf8(std::move(out.output));

        // ── outcome ─────────────────────────────────────────────────────
        if (stopped_early) {
            out.outcome = mt::StoppedEarly{};
        } else if (why) {
            out.outcome = *why;            // a budget ended it; say which
        } else if (auto st = proc.reap()) {
            out.outcome = (st->how == pf::exit_status::kind::exited)
                              ? mt::ExecOutcome{mt::Exited{st->code}}
                              : mt::ExecOutcome{mt::Signalled{st->code}};
        } else {
            out.outcome = mt::Signalled{0};   // gone, and nothing to tell us how
        }
        out.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - started);
        return out;
    }

    [[nodiscard]] std::expected<std::shared_ptr<mt::Session>, std::string>
    start(const mt::ExecRequest& req) override {
        std::vector<std::string> argv;
        argv.reserve(req.program.args.size() + 1);
        argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) argv.push_back(a);

        auto st = start_child(argv, req.cwd.value_or(std::string{}), req.env);
        if (!st) return std::unexpected(st.error());
        // The broker is not serviced for a session. A posture that delegates
        // syscalls would hang its guest on the first brokered call, so a
        // session is started OUTSIDE that posture rather than inside one it
        // cannot answer -- see start_child, which only brokers for run().
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
