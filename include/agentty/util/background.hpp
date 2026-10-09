#pragma once
// agentty::util::background — fire-and-forget work that is still OWNED.
//
// THE PRIMITIVE IS JAAL'S. THE POLICY IS HERE.
//
// maya::pool is the capability: a job gets its own thread when it may
// never return, every body is wrapped so an exception cannot reach
// std::terminate, every job gets a std::stop_token, and shutdown is BOUNDED —
// isolated jobs are counted before their thread exists, waited for inside the
// grace, then abandoned safely (the pool's state is co-owned by every worker,
// so an abandoned job that finally returns touches only memory it still owns).
//
// This header adds the two things jaal cannot know: that agentty has ONE
// background pool, joined through agentty's teardown registry; and the
// per-job `where` label that makes a reported throw traceable.
//
// ── What this replaces, and the claim it was making ──────────────────────
//
// This file used to hand-roll the whole thing, and its own comment was the
// bug report: run_isolated_detached promised the body "self-joins via a
// reaper so there is no leaked, unjoinable OS thread handle", and the
// implementation was `std::thread(...).detach()`. There was no reaper. What
// the old code actually had was exception isolation (real, and kept below)
// and nothing else: no stop_token, so a worker could not be asked to stop;
// no join, so at exit these threads were still running while the CRT
// destroyed the statics underneath them; no bound, so "shutdown" was a race
// nobody was timing.
//
// The pool keeps the isolation and supplies the three missing halves.
//
// ── Choosing a shape ────────────────────────────────────────────────────
//
//   work that must finish before this frame returns   maya::scope (a nursery;
//                                                     [&] captures are safe,
//                                                     every helper joined)
//   work that outlives the frame, result goes to the  Cmd::task / task_isolated
//     reducer                                         (stays on the Msg seam)
//   work that outlives the frame with no result       run_isolated_detached,
//                                                     below
//
// The OWNED, join-on-destruction thread this header used to export is gone:
// it had no callers, and maya::scope covers that shape properly.
//
// ── The body rule ──────────────────────────────────────────────────────────────
//
// jaal's: a job body is CAPTURELESS, `body(stop_token, args...)`, and every
// argument is Sendable. State the job shares with its owner is passed in as
// a maya::co_owned<T>, which requires T to be Sync. So a job can't borrow
// `this` or a local and outlive it; that used to be a comment here.

#include <chrono>
#include <concepts>
#include <exception>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <maya/runtime.hpp>

#include "agentty/util/dbglog.hpp"
#include "agentty/util/teardown.hpp"

namespace agentty::util {

// The one background pool, created on first use and registered for shutdown
// in the same breath — the rule teardown.hpp states: register the join in the
// function that CREATES the thread, not in main().
//
// Process-lifetime, so no teardown::cancel() is needed (the registry's
// documented exemption for function-local statics).
//
// The pool-level error hook is the LAST-RESORT net. Each job below carries
// its own try/catch so the report names the job; anything that gets
// past that is reported here without one, which is still better than the
// process dying.
inline maya::pool& background_pool() {
    static maya::pool* const p = [] {
        static maya::pool inst{
            /*max_workers=*/0u,
            [](std::exception_ptr) {
                dbglog("util.background_pool",
                       "a job escaped its own handler (isolated — process "
                       "survives)");
            }};
        teardown::on_shutdown("util.background_pool", [] {
            // Bounded by the pool's grace. Anything still stuck is abandoned,
            // which is safe by construction: a worker co-owns the state it
            // touches, so it cannot outlive it.
            if (const std::size_t stuck = inst.shutdown(); stuck > 0)
                dbglog("util.background_pool",
                       "abandoned " + std::to_string(stuck)
                           + " job(s) still running at the grace deadline");
        });
        return &inst;
    }();
    return *p;
}

namespace detail {

// The job's label, for the report when it throws. A string_view, NOT
// copied: every call site passes a literal.
struct Where { std::string_view name; };

// Run the body, and let nothing out. Body is a captureless lambda type, so
// Body{} is the same function.
template <class Body, class... Args>
void run_guarded(std::stop_token st, Where where, Args... args) noexcept {
    try {
        (void)Body{}(std::move(st), std::move(args)...);
    } catch (const std::exception& e) {
        dbglog(where.name, e.what());
    } catch (...) {
        dbglog(where.name, "non-std exception (isolated \xe2\x80\x94 process survives)");
    }
}

}  // namespace detail
} // namespace agentty::util

// A view onto a string literal: static storage, nothing that dies.
MAYA_SENDABLE(agentty::util::detail::Where);

namespace agentty::util {

// A job body: captureless, `body(stop_token, args...)`, args Sendable.
template <class Body, class... Args>
concept JobBody = maya::CheckedJob<Body, Args...>;

// Fire-and-forget on a WARM worker. For bounded work that will finish:
// a filesystem walk, a parse, a cache refresh that cannot block forever.
//
// Prefer this over run_isolated_detached when the body is guaranteed to
// return. It reuses a pooled thread, so a burst costs a queue push and a
// notify rather than a thread spawn each.
//
// The cost of being wrong: a body that blocks forever here OCCUPIES one of
// the pool's workers permanently. If it might wedge, it is isolated work.
template <class Body, class... Args>
void run_background(std::string_view where, Body, Args... args) {
    maya::require_job<Body, Args...>();
    background_pool().post(&detail::run_guarded<Body, Args...>,
                           detail::Where{where}, std::move(args)...);
}

// Fire-and-forget, but SAFELY: `body` runs on a thread of its own, cannot
// take the process down by throwing, is asked to stop at shutdown, and is
// waited for inside the shutdown grace before being abandoned.
//
// Isolated (a dedicated thread) rather than queued, because the callers here
// are jobs that MAY NEVER RETURN: an ACP turn, a network dial. Those must not
// occupy a shared worker. If the body is bounded, use run_background above.
// Use maya::delay_for on the token rather than sleeping through a shutdown.
template <class Body, class... Args>
void run_isolated_detached(std::string_view where, Body, Args... args) {
    maya::require_job<Body, Args...>();
    background_pool().post_isolated(&detail::run_guarded<Body, Args...>,
                                    detail::Where{where}, std::move(args)...);
}

// ── WorkerGroup ─ background work tied to ONE object's lifetime ────────
//
// maya::worker_group (jaal) is the primitive: a long-lived object's jobs,
// and a stop() that is a hard barrier with no grace. This adds only the
// `where` label when a job throws.
class WorkerGroup {
  public:
    /// `where` labels the group in logs. Static storage, same contract as the
    /// spawn functions above.
    explicit WorkerGroup(std::string_view where) : where_(where) {}

    /// Run `body(stop_token, args...)` on a thread of its own. Dropped once
    /// the group is stopped.
    template <class Body, class... Args>
    void post(Body, Args... args) {
        maya::require_job<Body, Args...>();
        group_.post(&detail::run_guarded<Body, Args...>,
                    detail::Where{where_}, std::move(args)...);
    }

    /// Barrier: when this returns, no job posted here is running. No
    /// deadline, on purpose, so a job that hangs hangs this. Give jobs their
    /// own timeouts. Idempotent; the destructor calls it too.
    void stop() noexcept { group_.stop(); }

  private:
    std::string_view   where_;
    maya::worker_group group_;
};

} // namespace agentty::util
