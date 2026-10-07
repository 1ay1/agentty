// src/tool/util/exec.cpp — one loop, over jaal.
//
// The shape is a reactor wait with two deadlines. What makes it short is
// that none of the hard parts are here: spawning, making an exit watchable,
// and the handle lifetimes are jaal's, and they are covered by a conformance
// suite that runs on every backend. What is left is the policy, and policy
// is the part that should be readable.

#include "agentty/tool/util/exec.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <jaal/platform/posix/process.hpp>
#include <jaal/platform/posix/poll_reactor.hpp>

#include <unistd.h>

namespace agentty::tools::util {
namespace {

namespace pf = jaal::platform;
namespace mt = ::mcp::tools;

using clock_t_ = std::chrono::steady_clock;

constexpr std::uint64_t kExitToken = 1;
constexpr std::uint64_t kOutToken  = 2;

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
        pf::process_spec spec;
        spec.argv.reserve(req.program.args.size() + 1);
        spec.argv.push_back(req.program.exe);
        for (const auto& a : req.program.args) spec.argv.push_back(a);
        if (req.cwd) spec.cwd = *req.cwd;
        spec.env          = req.env;
        spec.merge_stderr = true;   // one stream: the interleaving a terminal shows
        spec.new_session  = true;   // its own group, so stop() can reach the tree

        auto spawned = pf::posix_process::spawn(spec);
        if (!spawned) {
            out.outcome = mt::StartFailed{std::string{spawned.error().what}};
            return out;
        }
        auto proc = std::move(*spawned);

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

        // ── the two clocks ──────────────────────────────────────────────
        const auto started  = clock_t_::now();
        const auto wall_at  = started + wall;
        auto       idle_at  = started + idle;
        const bool has_wall = wall > std::chrono::seconds::zero();

        bool                       stopping = false;   // SIGTERM sent
        std::optional<clock_t_::time_point> kill_at;
        std::optional<mt::ExecOutcome>      why;       // why we stopped it
        bool                       exited = false;

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
                }
            }
            // Output resets the idle clock and ONLY the idle clock. That
            // asymmetry is the whole design: a chatty runaway keeps this
            // line running forever and the wall clock below is what ends it.
            if (saw_output) idle_at = clock_t_::now() + idle;

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

        // ── outcome ─────────────────────────────────────────────────────
        if (why) {
            out.outcome = *why;            // a budget ended it; say which
        } else if (auto st = proc.reap()) {
            out.outcome = (st->how == pf::exit_status::kind::exited)
                              ? mt::ExecOutcome{mt::Exited{st->code}}
                              : mt::ExecOutcome{mt::Signalled{st->code}};
        } else {
            out.outcome = mt::Signalled{0};   // gone, and nothing to tell us how
        }
        return out;
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

}  // namespace agentty::tools::util
