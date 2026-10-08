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
// per-job `where` + spawn-site breadcrumb that makes a reported throw
// traceable to the line that started the work.
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

#include <chrono>
#include <concepts>
#include <exception>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <maya/runtime.hpp>

#include "agentty/util/dbglog.hpp"
#include "agentty/util/teardown.hpp"

namespace agentty::util {

// A worker body is callable with no arguments, or with the std::stop_token
// the pool hands every job. Naming it turns "you passed a non-callable /
// wrong-arity thing" into a one-line concept error at the spawn call instead
// of a template-depth error inside the pool.
template <class Body>
concept WorkerBody =
    std::invocable<Body> || std::invocable<Body, std::stop_token>;

// Format a source_location into a compact "file:line function" breadcrumb so a
// worker panic reports WHERE it was spawned, auto-captured, never mislabeled.
//
// Called ONLY from the failure path. Building this eagerly cost a string
// allocation (two, with the concatenation) on every spawn to produce a label
// that is discarded unread in the overwhelmingly common case where the body
// returns normally. The spawn now carries the string_view and the
// source_location — both trivially copyable, both free — and pays for the
// formatting only when there is actually something to report.
inline std::string spawn_site(std::string_view where,
                              const std::source_location& loc) {
    std::string f = loc.file_name();
    // Keep just the basename — full build paths are noise in a log line.
    if (auto slash = f.find_last_of("/\\"); slash != std::string::npos)
        f.erase(0, slash + 1);
    std::string out;
    out.reserve(where.size() + f.size() + 32);
    out.append(where).append(" @ ").append(f).append(":")
       .append(std::to_string(loc.line())).append(" ")
       .append(loc.function_name());
    return out;
}

// The one background pool, created on first use and registered for shutdown
// in the same breath — the rule teardown.hpp states: register the join in the
// function that CREATES the thread, not in main().
//
// Process-lifetime, so no teardown::cancel() is needed (the registry's
// documented exemption for function-local statics).
//
// The pool-level error hook is the LAST-RESORT net. Each job below carries
// its own try/catch so the report names the spawn site; anything that gets
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

// The one wrapper both spawns share: run the body, and let nothing out.
//
// `where` is a std::string_view and is NOT copied — same contract as
// jaal::error::what. Every call site passes a string literal, so the
// pointee outlives the job for free; a caller handing this a temporary
// std::string would dangle, which is why the parameter is documented rather
// than quietly made a std::string.
template <class Body>
auto guarded_body(std::string_view where, std::source_location loc, Body body) {
    return [where, loc, body = std::move(body)](
               std::stop_token st) mutable noexcept {
        try {
            if constexpr (std::invocable<Body, std::stop_token>)
                body(std::move(st));
            else
                body();
        } catch (const std::exception& e) {
            dbglog(spawn_site(where, loc), e.what());
        } catch (...) {
            dbglog(spawn_site(where, loc),
                   "non-std exception (isolated \xe2\x80\x94 process survives)");
        }
    };
}

}  // namespace detail

// Fire-and-forget on a WARM worker. For bounded work that will finish:
// a filesystem walk, a parse, a cache refresh that cannot block forever.
//
// Prefer this over run_isolated_detached when the body is guaranteed to
// return. It reuses a pooled thread, so a burst costs a queue push and a
// notify rather than a thread spawn each — which is the difference that
// matters for anything that can fire more than once in a session.
//
// The cost of being wrong: a body that blocks forever here OCCUPIES one of
// the pool's workers permanently. If it might wedge, it is isolated work.
template <WorkerBody Body>
void run_background(std::string_view where, Body body,
                    std::source_location loc = std::source_location::current()) {
    background_pool().post(
        detail::guarded_body(where, loc, std::move(body)));
}

// Fire-and-forget, but SAFELY: `body` runs on a thread of its own, cannot
// take the process down by throwing, is asked to stop at shutdown, and is
// waited for inside the shutdown grace before being abandoned.
//
// Isolated (a dedicated thread) rather than queued, because the callers here
// are jobs that MAY NEVER RETURN — an orphaned connect waiter, a tool reaper
// blocked on a future, an ACP turn, a network dial. Those must not occupy a
// shared worker. If the body is bounded, use run_background above.
//
// `body` may take a std::stop_token to honour cancellation; a nullary body
// still works and simply ignores it. Use maya::delay_for to wait on
// that token rather than sleeping through a shutdown.
//
// Ownership note: the caller must ensure any state `body` captures by
// reference outlives the work. Prefer capturing by value / shared_ptr,
// exactly as the ACP turn worker already does.
template <WorkerBody Body>
void run_isolated_detached(std::string_view where, Body body,
                           std::source_location loc = std::source_location::current()) {
    background_pool().post_isolated(
        detail::guarded_body(where, loc, std::move(body)));
}

// ── WorkerGroup ─ background work tied to ONE object's lifetime ────────
//
// The third shape, and the one the table above was missing. Not "finishes
// before this frame returns" (maya::scope) and not "outlives the frame, nobody
// waits" (run_isolated_detached), but: a long-lived OBJECT owns workers that
// touch its collaborators, and its stop() is a HARD BARRIER — when stop()
// returns, no job is running, so the caller may safely destroy things the jobs
// were using.
//
// WHY THIS IS NOT run_isolated_detached
//
// jaal's pool shutdown is deliberately BOUNDED: it asks, waits out a grace,
// then abandons. Abandoning is safe for anything the worker CO-OWNS — that is
// why the pool keeps its own state in a shared core. It is not safe for
// something a THIRD party owns and is about to free.
//
// agentty's MCP HTTP transport is exactly that case: its POST workers push
// responses into the pipe its peer reads, and the connection drops the
// transport right after stop() returns. Making
// the worker co-own the transport does not help, because the dangling thing
// is the ENGINE, which the transport cannot co-own. So "wait as long as it
// takes" is a real requirement and not timidity.
//
// Each such site used to hand-roll it: a mutex, a condition variable, an
// inflight counter, and an alive flag that had to flip under the SAME lock as
// the counter so a stop() could not slip between "alive check passed" and
// "counter incremented". That coupling is subtle, it was commented at length,
// and it is the kind of thing that should exist once. The pool already does
// the hard half: post_isolated counts a job under its own lock BEFORE the
// thread exists, which is precisely that check-then-increment atomicity.
//
// So this adds exactly one thing to the pool — a barrier grace instead of a
// teardown grace — and takes the lifetime bookkeeping off the call site.
class WorkerGroup {
  public:
    /// `where` labels the group in logs. Static storage, same contract as the
    /// spawn functions above.
    explicit WorkerGroup(std::string_view where) : where_(where) {}

    WorkerGroup(const WorkerGroup&)            = delete;
    WorkerGroup& operator=(const WorkerGroup&) = delete;

    /// Joins on destruction, so an object that forgets to call stop() is
    /// still safe — it just blocks later than it meant to.
    ~WorkerGroup() { stop(); }

    /// Run `body` on a thread of its own. Dropped if the group is already
    /// stopped, which is the admission half of the old alive_/inflight_
    /// coupling and is handled inside the pool under its own lock.
    ///
    /// Isolated rather than queued because these jobs block on network IO by
    /// nature; a queued one would occupy a shared worker for its duration.
    template <WorkerBody Body>
    void post(Body body,
              std::source_location loc = std::source_location::current()) {
        pool_.post_isolated(detail::guarded_body(where_, loc, std::move(body)));
    }

    /// Barrier: when this returns, no job posted to this group is running.
    /// Idempotent — the pool's shutdown is, and stop() is reached from both
    /// an explicit teardown path and the destructor.
    ///
    /// NO DEADLINE, and that is the contract rather than an oversight. A
    /// group with a grace would be a barrier that sometimes isn't: it would
    /// return while a job was still running, having told the caller it was
    /// safe to destroy what that job writes into. Bounded-and-abandon is the
    /// right default everywhere a worker only touches what it co-owns, which
    /// is why jaal's pool defaults to it — it is the wrong answer HERE, where
    /// the whole reason this type exists is that the jobs touch a
    /// collaborator the caller is about to free.
    ///
    /// So a job that hangs forever hangs this. That turns silent corruption
    /// at exit into a visibly stuck shutdown with a thread to look at, which
    /// is the trade this type is for. Post work that carries its own timeout
    /// — as the MCP transport's does.
    void stop() noexcept {
        (void)pool_.shutdown(maya::pool::no_deadline);
    }

  private:
    std::string_view   where_;
    // One worker: these groups serialize their own calls (the MCP provider
    // holds a call mutex), so a second would never be used.
    maya::pool pool_{/*max_workers=*/1};
};

} // namespace agentty::util
