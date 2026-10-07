#pragma once
// agentty::util::background — fire-and-forget work that is still OWNED.
//
// THE PRIMITIVE IS JAAL'S. THE POLICY IS HERE.
//
// jaal::kernel::pool is the capability: a job gets its own thread when it may
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
//   work that must finish before this frame returns   jaal::scope (a nursery;
//                                                     [&] captures are safe,
//                                                     every helper joined)
//   work that outlives the frame, result goes to the  Cmd::task / task_isolated
//     reducer                                         (stays on the Msg seam)
//   work that outlives the frame with no result       run_isolated_detached,
//                                                     below
//
// The OWNED, join-on-destruction thread this header used to export is gone:
// it had no callers, and jaal::scope covers that shape properly.

#include <concepts>
#include <exception>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <jaal/kernel/pool.hpp>

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
inline std::string spawn_site(const std::source_location& loc) {
    std::string f = loc.file_name();
    // Keep just the basename — full build paths are noise in a log line.
    if (auto slash = f.find_last_of("/\\"); slash != std::string::npos)
        f.erase(0, slash + 1);
    return f + ":" + std::to_string(loc.line()) + " " + loc.function_name();
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
inline jaal::kernel::pool& background_pool() {
    static jaal::kernel::pool* const p = [] {
        static jaal::kernel::pool inst{
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

// Fire-and-forget, but SAFELY: `body` runs on a thread of its own, cannot
// take the process down by throwing, is asked to stop at shutdown, and is
// waited for inside the shutdown grace before being abandoned.
//
// Isolated (a dedicated thread) rather than queued, because every caller here
// is a job that MAY NEVER RETURN — an orphaned connect waiter, a tool reaper
// blocked on a future, an ACP turn. Those must not occupy a shared worker.
//
// `body` may take a std::stop_token to honour cancellation; a nullary body
// still works and simply ignores it.
//
// Ownership note: the caller must ensure any state `body` captures by
// reference outlives the work. Prefer capturing by value / shared_ptr,
// exactly as the ACP turn worker already does.
template <WorkerBody Body>
void run_isolated_detached(std::string_view where, Body body,
                           std::source_location loc = std::source_location::current()) {
    std::string tag = std::string(where) + " @ " + spawn_site(loc);
    background_pool().post_isolated(
        [tag = std::move(tag), body = std::move(body)](
            std::stop_token st) mutable noexcept {
            try {
                if constexpr (std::invocable<Body, std::stop_token>)
                    body(std::move(st));
                else
                    body();
            } catch (const std::exception& e) {
                dbglog(tag, e.what());
            } catch (...) {
                dbglog(tag, "non-std exception (isolated — process survives)");
            }
        });
}

} // namespace agentty::util
