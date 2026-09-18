#pragma once
// agentty::util::teardown — the process-exit join registry.
//
// ── The design flaw this closes ──────────────────────────────────────────
//
// Several subsystems own a background thread. Each must be joined before the
// CRT runs static destructors. Today that obligation is discharged by a
// hand-written list at the bottom of main():
//
//     join_workspace_prewarm();
//     join_workspace_symbols_prewarm();
//     persistence::flush_pending_saves();
//     provider::release_acp_agents();
//     ...
//
// A list like that is correct exactly until someone adds a subsystem and
// forgets to append to it — which is precisely what happened to
// settings_cache: it wrote `shutdown()`, documented it as "called during
// teardown", and no call was ever added. The worker stayed joinable inside a
// function-local static and ~thread fired std::terminate at exit.
//
// The flaw is that main() is asked to KNOW about every threaded subsystem.
// That is backwards: main() is the one place with no knowledge of what the
// subsystems are. Inverting it — each subsystem registers its own shutdown
// when it first starts a thread — makes the list self-maintaining, and makes
// "I added a thread but forgot to join it" impossible for anything built on
// a subsystem that owns a thread.
//
// Ordering: LIFO, like static destruction. A subsystem registered later may
// depend on one registered earlier, so it is torn down first.
//
// `run()` is idempotent and safe to call when nothing registered.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace agentty::util::teardown {

// Register a shutdown action under `name` (used only for diagnostics).
// Actions must be noexcept in spirit: any exception is swallowed by run(),
// because a throw during teardown is worse than the failure it reports.
//
// Returns a TOKEN identifying the registration. An owner whose lifetime is
// shorter than the process must pass it to `cancel()` in its destructor —
// otherwise run() would invoke a callback bound to freed memory. Callers that
// genuinely live forever (function-local statics, the common case) can ignore
// the token.
using Token = std::uint64_t;

Token on_shutdown(std::string name, std::function<void()> action);

// Remove a registration. Idempotent, and safe with a token whose action has
// already run. This is what lets a registrant with a bounded lifetime stay
// safe: the registry never holds a callback into a dead object.
void cancel(Token token) noexcept;

// Run every registered action, most-recently-registered first, then clear the
// registry. Idempotent: a second call is a no-op unless new actions were
// registered in between. Never throws.
void run() noexcept;

// How many actions are pending. For tests and diagnostics.
[[nodiscard]] std::size_t pending() noexcept;

} // namespace agentty::util::teardown
