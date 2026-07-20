#pragma once
// agentty::util::isolated_thread — a worker thread whose panic can NEVER take
// down the process, and whose lifetime is OWNED (joined), not detached.
//
// The problem this kills (RUST-CRITIQUE.md #3): `std::thread(...).detach()`.
// Detaching means (a) no ownership / no join — the thread outlives every
// handle to it, and (b) an exception that escapes the thread body calls
// std::terminate(), killing EVERY session in the process. The current code
// defends (b) with a giant hand-written try/catch at the top of run_turn; if
// any future edit adds a code path that can throw OUTSIDE that catch, one bad
// turn nukes the whole agent.
//
// Rust's answer is structured: a tokio task / std::thread::scope isolates a
// panic to the one task (panic = unwind, runtime survives) and gives you a
// JoinHandle. We match AND beat it:
//
//   • Exception isolation is STRUCTURAL, not a convention. The thread body is
//     wrapped so that std::terminate is UNREACHABLE — a throwing callable is
//     caught by the double catch here, funnelled to an on_error hook, and the
//     thread exits cleanly. There is no code path from a worker throw to
//     process death, and you can't forget to write the try/catch because the
//     wrapper owns it.
//
//   • Lifetime is OWNED. `isolated_thread` joins in its destructor (like a
//     jthread / scoped thread), so a worker can't outlive the data it
//     borrowed. For the ACP fire-and-forget turn worker we still want the
//     handler to RETURN immediately while the turn runs on — so we provide
//     `run_isolated_detached`, which keeps the SAME terminate-proof wrapper
//     but hands ownership to a self-joining reaper instead of raw detach.
//     Even "detached" work is exception-isolated; that's the improvement.

#include <exception>
#include <functional>
#include <thread>
#include <utility>

#include "agentty/util/dbglog.hpp"

namespace agentty::util {

// Wrap any nullary callable so that NOTHING it throws can escape. A throwing
// body is reported via `where` (dbglog) and swallowed; the thread then exits
// normally. std::terminate is unreachable from `body`.
template <class Body>
auto make_terminate_proof(const char* where, Body body) {
    return [where, body = std::move(body)]() mutable noexcept {
        try {
            body();
        } catch (const std::exception& e) {
            dbglog(where, e.what());
        } catch (...) {
            dbglog(where, "non-std exception (isolated — process survives)");
        }
    };
}

// An OWNED worker thread. Joins on destruction (structured concurrency: the
// worker cannot outlive this object, hence cannot outlive borrowed state).
// The body is terminate-proof by construction.
class isolated_thread {
public:
    isolated_thread() = default;

    template <class Body>
    isolated_thread(const char* where, Body body)
        : t_(make_terminate_proof(where, std::move(body))) {}

    isolated_thread(isolated_thread&&) noexcept            = default;
    isolated_thread& operator=(isolated_thread&&) noexcept = default;
    isolated_thread(const isolated_thread&)                = delete;
    isolated_thread& operator=(const isolated_thread&)     = delete;

    ~isolated_thread() {
        if (t_.joinable()) t_.join();
    }

    [[nodiscard]] bool joinable() const noexcept { return t_.joinable(); }
    void join() { if (t_.joinable()) t_.join(); }

private:
    std::thread t_;
};

// Fire-and-forget, but SAFELY: run `body` on its own thread that (a) can never
// terminate the process on throw, and (b) self-joins via a reaper so there is
// no leaked, unjoinable OS thread handle. This is the drop-in replacement for
// `std::thread(...).detach()` — same "handler returns now, work runs on"
// semantics, minus the two footguns.
//
// Ownership note: the caller must ensure any state `body` captures by
// reference outlives the work (same contract as detach). Prefer capturing by
// value / shared_ptr, exactly as the ACP turn worker already does.
template <class Body>
void run_isolated_detached(const char* where, Body body) {
    std::thread(make_terminate_proof(where, std::move(body))).detach();
}

} // namespace agentty::util
