#pragma once
// agentty::session — per-turn state for a single in-flight LLM request.
// See docs/design/streaming.md for the full design rationale.

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <maya/widget/spinner.hpp>

namespace agentty::http { class CancelToken; }

namespace agentty {

// ── Retry / stall watchdog state machine ──────────────────────
// Used to be two parallel bools (`stall_dispatched`, `retry_pending`)
// that together encoded a 3-state machine; the invariants were comment-
// only and hand-maintained across ~6 sites in the reducer. Now a proper
// sum type:
//
//   Fresh        — stream alive, watchdog armed.
//   StallFired   — watchdog tripped the cancel token, synthetic
//                  StreamError is in flight. The worker thread's late
//                  StreamError("cancelled") must be re-classified as
//                  Transient, not user-cancel.
//   Scheduled    — StreamError handler scheduled a RetryStream via
//                  Cmd::after. A second StreamError arriving during the
//                  wait must NOT schedule another retry.
//
// Transitions:
//   Fresh       → StallFired  (Tick watchdog detects dead stream)
//   Fresh       → Scheduled   (StreamError fires directly, non-stall)
//   StallFired  → Scheduled   (the synthetic StreamError schedules retry)
//   Scheduled   → Fresh       (RetryStream fires, fresh stream begins)
//   any         → Fresh       (CancelStream, StreamStarted reset)
namespace retry {
struct Fresh      {};
struct StallFired {};
struct Scheduled  {};
} // namespace retry
using RetryState = std::variant<retry::Fresh, retry::StallFired, retry::Scheduled>;

namespace phase {

// Per-turn streaming context: alive whenever the request lifecycle is
// in flight (Streaming → AwaitingPermission → ExecutingTool → Streaming
// → … → Idle). Embedded inside every non-Idle phase variant alternative
// so the fields below DO NOT EXIST when the FSM is Idle. Reading them
// from Idle is now a type error rather than a logic bug masked by
// default-zero values.
//
// The context flows across legal transitions: Streaming → Awaiting-
// Permission preserves the same `cancel` token, the same `started`
// stamp, the same retry counters. The transition functions below take
// the source by `&&` and re-wrap its `ctx` inside the destination
// variant — there's no slot in C++ for "an optional Active that
// follows whichever phase happens to be active right now," so the FSM
// itself carries it.
struct Active {
    std::shared_ptr<agentty::http::CancelToken> cancel;

    // Turn start (set on Idle → Streaming) and event-time-of-last-
    // observed-activity (bumped on every SSE event). Together they
    // drive the elapsed-time chip and the 120-s stall watchdog.
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_event_at{};

    // Per-turn retry counters. truncation_retries: silent re-launches
    // when the stream EOFs mid-tool-args. transient_retries: 5xx /
    // network / overloaded / 429. Independent budgets.
    int truncation_retries = 0;
    int transient_retries  = 0;

    // Live tok/s speedometer — bytes of text/json delta, not the rare
    // usage field. first_delta_at excludes TTFT from the rate divisor.
    // Reset at every StreamStarted (sub-turn) but accumulated across
    // a single sub-turn's deltas.
    std::size_t live_delta_bytes = 0;
    std::chrono::steady_clock::time_point first_delta_at{};
    std::chrono::steady_clock::time_point rate_last_sample_at{};
    std::size_t rate_last_sample_bytes = 0;

    // Retry/stall machine state — see RetryState above.
    RetryState retry = retry::Fresh{};
};

// ── Phase types ──────────────────────────────────────────────────────
// Idle holds nothing — there's no in-flight request to carry context
// for. Streaming / AwaitingPermission / ExecutingTool each carry one
// Active block; the only difference between them is the tag.
//
// Why all three need the SAME context (rather than each peeling off
// just the fields it cares about):
//   • cancel       — Esc must work in every active phase. Even
//                    AwaitingPermission needs the token live so a
//                    user Esc cancels the underlying SSE stream
//                    (the request is still open, waiting for the
//                    next tool args).
//   • last_event_at — the stall watchdog runs in every active phase;
//                    a stream that goes silent during ExecutingTool
//                    is still a stalled stream.
//   • retry counters / retry_state — error retries can fire from any
//                    active phase (StreamError can land while we're
//                    in AwaitingPermission, e.g. server killed the
//                    request while the user is reading a permission
//                    prompt) and need to preserve attempt counts.
struct Idle               {};
struct Streaming          { Active ctx; };
struct AwaitingPermission { Active ctx; };
struct ExecutingTool      { Active ctx; };

} // namespace phase

using Phase = std::variant<phase::Idle, phase::Streaming,
                           phase::AwaitingPermission, phase::ExecutingTool>;

// ── Active-context accessors ─────────────────────────────────────────
// The 60-odd reader sites that used to touch `m.s.cancel` /
// `m.s.last_event_at` / etc. on a flat StreamState now go through
// `active_ctx(m.s.phase)`. Returns nullptr when the phase is Idle —
// readers that only run during active phases can dereference
// unconditionally; readers that may run from Idle (the Tick watchdog,
// status-bar widgets) check for null first. The single visit replaces
// what would otherwise be ~60 hand-written `std::get_if` chains.
[[nodiscard]] inline phase::Active* active_ctx(Phase& p) noexcept {
    return std::visit([](auto& v) -> phase::Active* {
        if constexpr (requires { v.ctx; }) return &v.ctx;
        else                               return nullptr;
    }, p);
}
[[nodiscard]] inline const phase::Active* active_ctx(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> const phase::Active* {
        if constexpr (requires { v.ctx; }) return &v.ctx;
        else                               return nullptr;
    }, p);
}

// Consume the source phase's ctx; returns nullopt if Idle. Used by the
// transition sites where ctx flows from old to new phase: instead of
// hand-rolling a 4-arm visit at every site, callers do
//
//     auto ctx = take_active_ctx(std::move(m.s.phase));
//     m.s.phase = phase::ExecutingTool{std::move(ctx).value()};
//
// The .value() asserts "we expected an active source here" — bugs
// from Idle leaking into a Streaming-only transition site abort
// rather than silently corrupt a default-constructed ctx.
[[nodiscard]] inline std::optional<phase::Active> take_active_ctx(Phase&& p) noexcept {
    return std::visit([](auto&& v) -> std::optional<phase::Active> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, phase::Idle>) return std::nullopt;
        else                                        return std::move(v.ctx);
    }, std::move(p));
}

[[nodiscard]] constexpr std::string_view to_string(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "idle";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "streaming";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "permission";
        else                                                           return "working";
    }, p);
}

struct StreamState {
    Phase phase = phase::Idle{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in   = 0;
    int tokens_out  = 0;
    int context_max = 200000;
    // True while a compaction round is in flight: the synthetic
    // "summarise per spec" user message has been appended and the
    // assistant is producing the summary. StreamFinished's compaction
    // branch reads this flag, replaces messages with a single
    // compacted-summary user message, and resets it. Invariant: only
    // ever true when phase != Idle (the transition is paired with the
    // launch of the compaction stream); StreamError on a compaction
    // turn clears it without applying.
    bool compacting = false;
    // True between `init()` kicking off the background OAuth refresh
    // and `TokenRefreshed` landing. While set, `submit_message` queues
    // the user's text into `composer.queued` instead of dispatching a
    // stream — Deps still holds the pre-refresh (expired) auth header,
    // and a request fired now would 401. The TokenRefreshed handler
    // clears the flag and drains the queue once new creds are live.
    bool oauth_refresh_in_flight = false;
    // True while the background thread-history load kicked off from
    // `init()` is still running. The thread picker view consults this
    // to render a "loading…" placeholder instead of an empty list.
    // Cleared by the `ThreadsLoaded` handler.
    bool threads_loading = false;
    std::string status;
    // Optional expiry for `status`. When set, the status bar hides the
    // banner once now() passes this point and the reducer treats the
    // field as empty. Used for toast-style transient messages
    // (retrying, cancelled, checkpoint-restore-not-implemented, …) so
    // they don't linger forever. A default-constructed time_point
    // (epoch=0) means "no expiry" — the status stays until something
    // else writes over it. Lives on StreamState (not phase::Active)
    // because terminal-error and cancellation handlers set it WHILE
    // transitioning to Idle, so the toast must outlive the ctx.
    std::chrono::steady_clock::time_point status_until{};

    // True iff `status` is set AND either has no expiry or hasn't expired yet.
    [[nodiscard]] bool status_active(std::chrono::steady_clock::time_point now) const noexcept {
        if (status.empty()) return false;
        if (status_until.time_since_epoch().count() == 0) return true;
        return now < status_until;
    }
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};

    // Sparkline ring buffer for the status-bar trend glyphs. NOT
    // reset between sub-turns or across the active→Idle boundary —
    // a user-visible trace of generation rate over the whole session.
    static constexpr std::size_t kRateSamples = 12;
    std::array<float, kRateSamples> rate_history{};
    std::size_t rate_history_pos = 0;
    bool rate_history_full = false;

    // ── Phase predicates ─────────────────────────────────────────────
    [[nodiscard]] bool is_idle()                const noexcept { return std::holds_alternative<phase::Idle>(phase); }
    [[nodiscard]] bool is_streaming()           const noexcept { return std::holds_alternative<phase::Streaming>(phase); }
    [[nodiscard]] bool is_awaiting_permission() const noexcept { return std::holds_alternative<phase::AwaitingPermission>(phase); }
    [[nodiscard]] bool is_executing_tool()      const noexcept { return std::holds_alternative<phase::ExecutingTool>(phase); }

    // Derived: "is anything actively in flight?" — true whenever the
    // session is in any non-Idle phase. Used to be a parallel `bool
    // active` field that callers had to keep in lock-step with phase;
    // deriving it eliminates the invariant ("active iff phase != Idle")
    // that the type system couldn't enforce.
    [[nodiscard]] bool active() const noexcept { return !is_idle(); }

    // Retry-state predicates — read through active_ctx() so they're
    // safe to call from Idle (return values are equivalent to "no
    // retry pending," which is what callers expect from Idle anyway:
    // the watchdog gate `if (in_fresh()) check_for_stall()` correctly
    // skips when there's no stream to stall).
    [[nodiscard]] bool in_fresh() const noexcept {
        auto* c = active_ctx(phase);
        return !c || std::holds_alternative<retry::Fresh>(c->retry);
    }
    [[nodiscard]] bool in_stall_fired() const noexcept {
        auto* c = active_ctx(phase);
        return c && std::holds_alternative<retry::StallFired>(c->retry);
    }
    [[nodiscard]] bool in_scheduled() const noexcept {
        auto* c = active_ctx(phase);
        return c && std::holds_alternative<retry::Scheduled>(c->retry);
    }
};

} // namespace agentty
