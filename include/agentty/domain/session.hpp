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

#include <maya/core/animation.hpp>
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
    //
    // transient_retries is NOT purely monotonic per turn: it resets to
    // 0 whenever the wire proves healthy — first content delta OR a
    // heartbeat (SSE ping / thinking_delta). A stream that connects,
    // pings, then stalls before any content used to climb the ladder
    // every attempt and hit kMaxRetries with the session dead; the
    // heartbeat reset gives each healthy-then-stalled attempt a fresh
    // budget so long sessions recover instead of latching terminal.
    int truncation_retries = 0;
    int transient_retries  = 0;

    // Mid-stream failure count — increments every time a retry is
    // scheduled for a failure that happened AFTER the wire proved itself
    // alive this turn (stall watchdog fired, or a content delta had
    // already arrived). Unlike `transient_retries`, this is NEVER reset by
    // a heartbeat or first-delta: a connection that keeps delivering one
    // byte/ping and then dying is a real outage, and resetting the budget
    // on that single byte would let it retry forever. The mid-stream retry
    // cap (provider::max_retries_for(klass, mid_stream=true)) is checked
    // against THIS counter so the "only N attempts once we've seen bytes"
    // guarantee actually latches terminal instead of looping on a flapping
    // wire. Connect-time (pre-delta, pre-stall) failures don't touch it —
    // they're governed by the full transient_retries budget.
    int mid_stream_failures = 0;

    // Wall-clock stamp of the last transient/stall failure. The retry
    // decision decays the budget: if the previous failure was more
    // than kRetryDecayWindow ago the connection has been healthy in
    // the interim, so transient_retries resets before counting this
    // one. Stops a multi-hour session from accumulating unrelated
    // brown-out failures into a permanent budget exhaustion.
    std::chrono::steady_clock::time_point last_failure_at{};

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

// ── Typed phase transitions ───────────────────────────────────────
// Each function below names a SPECIFIC legal transition between two
// phases. The signature consumes the source by value-move and returns
// the destination; illegal transitions (Idle → ExecutingTool, etc.)
// simply don't have a function defined, so any reducer site that uses
// the typed API can't call an illegal one — the function lookup fails
// to compile.
//
// The legal transition graph (see phase::PhaseKind below for the
// closed set):
//
//                  start()                land_perm()         exec_tool()
//   Idle   ───────────▶ Streaming ─────────▶ AwaitingPermission ─────────▶ ExecutingTool
//   ▲                       │                       │                              │
//   │ finish()              │                       │ reject()                     │ done_tool()
//   │                       │                       ▼                              ▼
//   └──────────────────────────────────── Idle ←────────────────────────────────────
//                                                                                  │
//                  resume_stream() (after tool done)                                │
//   Streaming  ◀────────────────────────────────────────────────────────────────────────┘
//
// abort() is special — collapses ANY active phase back to Idle. Used by
// Esc / cancel / terminal error handlers.
namespace phase {

// Closed set of phase identities. Pairs 1:1 with the Phase variant
// alternatives; the legality matrix below is indexed on this enum.
enum class PhaseKind : std::uint8_t {
    Idle,
    Streaming,
    AwaitingPermission,
    ExecutingTool,
};

[[nodiscard]] constexpr PhaseKind kind_of(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> PhaseKind {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, Idle>)               return PhaseKind::Idle;
        else if constexpr (std::same_as<T, Streaming>)          return PhaseKind::Streaming;
        else if constexpr (std::same_as<T, AwaitingPermission>) return PhaseKind::AwaitingPermission;
        else                                                    return PhaseKind::ExecutingTool;
    }, p);
}

// True iff a (from, to) transition is part of the legal graph above.
// abort() is encoded as "any → Idle" (every from-phase can reach Idle).
[[nodiscard]] constexpr bool is_legal_transition(PhaseKind from, PhaseKind to) noexcept {
    using K = PhaseKind;
    // Every phase can be aborted to Idle.
    if (to == K::Idle) return true;
    // Idle → Streaming: start a new turn.
    if (from == K::Idle               && to == K::Streaming)          return true;
    // Streaming → AwaitingPermission: model emitted tool_use, awaiting user.
    if (from == K::Streaming          && to == K::AwaitingPermission) return true;
    // AwaitingPermission → ExecutingTool: user approved.
    if (from == K::AwaitingPermission && to == K::ExecutingTool)      return true;
    // ExecutingTool → Streaming: tool done, resume the open stream.
    if (from == K::ExecutingTool      && to == K::Streaming)          return true;
    return false;
}

// ── Transition functions (one per legal edge) ───────────────────────
// Each takes the source by value-move and returns the destination.
// Adopting these incrementally at reducer sites means an illegal
// transition (e.g. Idle → ExecutingTool) is a compile error — no
// function with that signature exists.

// Idle → Streaming: brand-new turn, fresh Active context.
[[nodiscard]] inline Streaming start(Idle&&, Active a) noexcept {
    return Streaming{std::move(a)};
}

// Streaming → AwaitingPermission: tool_use arrived, ask the user.
// Ctx flows unchanged (cancel, retry, timestamps preserve).
[[nodiscard]] inline AwaitingPermission land_perm(Streaming&& s) noexcept {
    return AwaitingPermission{std::move(s.ctx)};
}

// AwaitingPermission → ExecutingTool: user approved.
[[nodiscard]] inline ExecutingTool exec_tool(AwaitingPermission&& p) noexcept {
    return ExecutingTool{std::move(p.ctx)};
}

// ExecutingTool → Streaming: tool finished, resume the open stream.
[[nodiscard]] inline Streaming resume_stream(ExecutingTool&& e) noexcept {
    return Streaming{std::move(e.ctx)};
}

// Streaming → Idle: natural finish (StreamFinished) or terminal error.
[[nodiscard]] inline Idle finish(Streaming&&) noexcept { return Idle{}; }

// AwaitingPermission → Idle: user rejected the tool prompt.
[[nodiscard]] inline Idle reject(AwaitingPermission&&) noexcept { return Idle{}; }

// ExecutingTool → Idle: tool failed terminally and no resumption is
// possible (very rare — the reducer prefers reject → Streaming → Idle
// so the model gets a tool_result with is_error=true). Provided for
// completeness so the abort path has the right name.
[[nodiscard]] inline Idle done_tool(ExecutingTool&&) noexcept { return Idle{}; }

// abort — collapses any active phase back to Idle. Used by Esc /
// CancelStream / terminal-error handlers that don't care which phase
// they're aborting from.
[[nodiscard]] inline Idle abort(Streaming&&)          noexcept { return Idle{}; }
[[nodiscard]] inline Idle abort(AwaitingPermission&&) noexcept { return Idle{}; }
[[nodiscard]] inline Idle abort(ExecutingTool&&)      noexcept { return Idle{}; }

// ── Compile-time proofs of the transition graph ────────────────────
namespace proofs {

// Every transition function above has the right signature: source by
// value-move (rvalue ref), destination by value. This is the contract
// that makes the typestate enforcement work — without an rvalue source,
// the old phase value sticks around and the typestate "erases" the
// pre-transition state only by convention.
static_assert(std::is_same_v<decltype(start(std::declval<Idle&&>(),
                                            std::declval<Active>())),
                             Streaming>);
static_assert(std::is_same_v<decltype(land_perm(std::declval<Streaming&&>())),
                             AwaitingPermission>);
static_assert(std::is_same_v<decltype(exec_tool(std::declval<AwaitingPermission&&>())),
                             ExecutingTool>);
static_assert(std::is_same_v<decltype(resume_stream(std::declval<ExecutingTool&&>())),
                             Streaming>);
static_assert(std::is_same_v<decltype(finish(std::declval<Streaming&&>())),
                             Idle>);
static_assert(std::is_same_v<decltype(reject(std::declval<AwaitingPermission&&>())),
                             Idle>);
static_assert(std::is_same_v<decltype(done_tool(std::declval<ExecutingTool&&>())),
                             Idle>);

// Exhaustive sweep: for every (from, to) pair, the legality matrix
// is_legal_transition() agrees with the existence of a transition
// function. The matrix is the spec; the functions are the
// implementation. Drift between them = bug.
//
// We can't directly ask "does a function exist from X to Y" via SFINAE
// without naming each one, so the proof is a parallel re-statement of
// the legal edges and a static_assert that it equals the matrix on
// every cell. Adding a new phase requires updating BOTH this proof
// and is_legal_transition, mirroring the spec::policy proof in
// tool/policy.hpp.
consteval bool transition_graph_matches_spec() {
    using K = PhaseKind;
    constexpr K kAll[] = {K::Idle, K::Streaming, K::AwaitingPermission, K::ExecutingTool};

    // Hand-rolled spec: the same edges encoded in the transition
    // functions above. Listed positively (legal) — every other cell
    // is illegal except for the universal "X → Idle" abort edges.
    auto spec = [](K f, K t) {
        if (t == K::Idle) return true;
        if (f == K::Idle               && t == K::Streaming)          return true;
        if (f == K::Streaming          && t == K::AwaitingPermission) return true;
        if (f == K::AwaitingPermission && t == K::ExecutingTool)      return true;
        if (f == K::ExecutingTool      && t == K::Streaming)          return true;
        return false;
    };

    for (auto f : kAll)
        for (auto t : kAll)
            if (is_legal_transition(f, t) != spec(f, t)) return false;
    return true;
}
static_assert(transition_graph_matches_spec(),
              "is_legal_transition disagrees with the transition-function "
              "graph above — update both together when changing phases");

// Pin the closed-set size so adding a phase requires updating every
// cell-by-cell proof site below.
static_assert(std::variant_size_v<Phase> == 4,
              "Phase variant size changed — update PhaseKind, the "
              "transition functions, and is_legal_transition together");

// Named-edge spot checks: read better than the loop above and pin the
// human-meaningful invariants in case the matrix ever loses its mind.
static_assert( is_legal_transition(PhaseKind::Idle, PhaseKind::Streaming));
static_assert(!is_legal_transition(PhaseKind::Idle, PhaseKind::ExecutingTool),
              "Idle → ExecutingTool must skip through Streaming; the user "
              "hasn't sent a turn yet");
static_assert(!is_legal_transition(PhaseKind::Idle, PhaseKind::AwaitingPermission),
              "Idle → AwaitingPermission is impossible — there's no tool "
              "to authorise without a model turn first");
static_assert( is_legal_transition(PhaseKind::Streaming, PhaseKind::AwaitingPermission));
static_assert( is_legal_transition(PhaseKind::AwaitingPermission, PhaseKind::ExecutingTool));
static_assert( is_legal_transition(PhaseKind::ExecutingTool, PhaseKind::Streaming));
static_assert(!is_legal_transition(PhaseKind::ExecutingTool, PhaseKind::AwaitingPermission),
              "ExecutingTool → AwaitingPermission is impossible — a tool "
              "that needs a follow-up permission goes through Streaming first");
static_assert( is_legal_transition(PhaseKind::Streaming,          PhaseKind::Idle));
static_assert( is_legal_transition(PhaseKind::AwaitingPermission, PhaseKind::Idle));
static_assert( is_legal_transition(PhaseKind::ExecutingTool,      PhaseKind::Idle));

} // namespace proofs

} // namespace phase

struct StreamState {
    Phase phase = phase::Idle{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in   = 0;
    int tokens_out  = 0;
    int context_max = 200000;
    // True while a compaction round is in flight: the request that
    // includes the synthesised "summarise per spec" prompt has been
    // dispatched and the assistant is streaming its summary into the
    // off-transcript `compaction_buffer` below. StreamFinished's
    // compaction branch reads this flag, lifts the summary out of the
    // buffer, pushes a Thread::CompactionRecord describing the
    // [0, compaction_target_index) prefix, and resets the flag.
    // StreamError during compaction clears the flag and discards the
    // buffer; `messages` is untouched in EITHER outcome. Invariant:
    // only ever true when phase != Idle (the transition is paired
    // with the launch of the compaction stream).
    bool compacting = false;
    // Snapshot of `messages.size()` taken at CompactContext kickoff.
    // The compaction-finalize handler stamps this onto the new
    // Thread::CompactionRecord as `up_to_index` so wire payloads for
    // future requests know exactly which prefix the summary replaces.
    // Reset to 0 on completion (success OR error).
    //
    // This used to be called `compact_pre_synth_count` back when the
    // synthetic summarisation prompt + assistant placeholder were
    // appended to `messages` and the finalizer needed to recover the
    // pre-synth slice. Compaction no longer touches `messages`, so the
    // field's only remaining role is to remember the boundary; the
    // name now reflects that.
    std::size_t compaction_target_index = 0;
    // Off-transcript sink for the compaction stream's text deltas.
    // Stream handlers redirect `StreamTextDelta` into this buffer
    // instead of `messages.back().pending_stream` whenever
    // `compacting` is true, so the summarisation reply never
    // contaminates the user-visible transcript. Drained + cleared
    // at StreamFinished (on success: parsed into the new
    // CompactionRecord) or StreamError (on failure: discarded).
    std::string compaction_buffer;
    // Rapid-refill breaker. When auto-compaction fires within
    // `kRapidRefillTurns` assistant turns of the previous one,
    // `recent_compacts` increments; on a quieter cycle it resets.
    // Crossing `kRapidRefillCount` flips `autocompact_disabled` so
    // we stop thrashing — the user sees a status toast suggesting
    // they reduce a too-large tool output. Mirrors Claude Code's
    // `LC7=3 / qP8=3 / VC7=3` triple (binary near offset 112623088).
    int recent_compacts = 0;
    int turns_since_last_compact = 1000000;   // effectively ∞ at startup
    bool autocompact_disabled = false;
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
    // True while a background single-thread JSON load is in flight
    // (kicked off by `ThreadListSelect`). The status bar shows a
    // "loading thread…" chip; the previous thread stays visible
    // until `ThreadLoaded` lands and swaps `m.d.current`. Suppresses
    // additional ThreadListSelect dispatches so a mashed Enter can't
    // queue multiple loads behind each other.
    bool thread_loading = false;
    // True while a background model-list fetch is in flight (kicked off
    // by init(), OpenModelPicker, a provider switch, or a login). The
    // model picker view consults this to render "Loading models…" vs.
    // "No models available" — without it, a fetch that throws or
    // returns an empty list would leave the picker stuck on the
    // spinner forever (the available_models list stays empty). ALWAYS
    // cleared by the `ModelsLoaded` handler, which fetch_models()
    // dispatches on BOTH success and failure.
    bool models_loading = false;
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
    // 64 samples × 500 ms cadence ≈ 32 s of history: enough to fill
    // the ADAPTIVE status-bar sparkline (which stretches across the
    // activity row's free space, clamped to 64 cells) on a wide
    // terminal. Narrow terminals just show the newest suffix — the
    // widget right-pins and takes the last N samples it can fit.
    static constexpr std::size_t kRateSamples = 64;
    std::array<float, kRateSamples> rate_history{};
    std::size_t rate_history_pos = 0;
    bool rate_history_full = false;

    // Spring-smoothed version of the BIG displayed tok/s readout. The raw
    // instantaneous rate (live_delta_bytes/4 ÷ elapsed) is recomputed every
    // frame and jitters hard — the number visibly flickers. We retarget this
    // spring at the raw value each frame and display its settled value, so
    // the readout glides instead of strobing. Ticked in the Tick handler
    // alongside the spinner; reset to 0 when no stream is in flight.
    maya::anim::Spring<double> disp_rate_spring{0.0, maya::anim::spring_presets::gentle};

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
