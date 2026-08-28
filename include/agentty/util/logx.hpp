#pragma once
// agentty::logx — the structured diagnostic log.
//
// ─────────────────────────────────────────────────────────────────────────
// DESIGN
// ─────────────────────────────────────────────────────────────────────────
// One logging system with four properties the ad-hoc predecessors lacked
// (dbglog's fopen-per-line, provider/debug.hpp's raw dump, the scattered
// AGENTTY_*_PROF fopen sites):
//
//   1. LEVELED + CHANNELED, filtered like RUST_LOG. Every event carries a
//      (Channel, Level) pair; the filter is a per-channel minimum level
//      parsed once from $AGENTTY_LOG:
//
//          AGENTTY_LOG=debug                   # everything at debug+
//          AGENTTY_LOG=wire=trace              # one channel wide open
//          AGENTTY_LOG=warn,wire=trace,auth=debug
//                                              # default warn, overrides
//
//      The gate is ONE relaxed atomic load + compare per call site —
//      when a channel is off, a log statement costs ~1ns and formats
//      NOTHING (the macro short-circuits before the format arguments
//      are even evaluated).
//
//   2. ATOMIC APPENDS, file opened ONCE. Events format into a stack
//      buffer and hit the file as a single write(2) on an O_APPEND fd —
//      POSIX guarantees appends don't interleave, so there is NO MUTEX
//      on the write path. (The predecessor re-opened the file per line
//      under a global lock.) The file rotates once at startup when it
//      exceeds 32 MB (renamed to .old), so a long-lived enabled log
//      can't grow unboundedly.
//
//   3. FLIGHT RECORDER, always on. The last kRingSlots events at
//      Warn+ (or everything, when a channel is enabled) are ALSO copied
//      into a fixed in-process ring — even when file logging is off.
//      On SIGSEGV/SIGABRT the crash handler calls dump_flight_recorder(),
//      which write(2)s the preformatted ring to a crash file: the last
//      ~256 things that happened ship with every crash report, at zero
//      steady-state cost beyond a memcpy per warning. Async-signal-safe
//      by construction (preformatted bytes, raw write, no allocation).
//
//   4. SPANS. AGT_SPAN(channel, "name") logs entry at trace and exit
//      with a monotonic duration — RAII, exception-safe, nestable. The
//      poor-man's tracing profile that replaces the one-off
//      AGENTTY_LOAD_PROF / AGENTTY_CACHE_PROF fopen sites.
//
// FORMAT (logfmt-ish, one line per event, grep-friendly):
//
//   2026-08-28T01:23:45.678 +0012345ms 1a2b W wire     openai.stream: connect refused host=localhost:8080
//   └─ wall clock ─────────┘ └ mono ─┘ tid  L channel  site: message
//
// LEGACY COMPAT: AGENTTY_DEBUG_LOG=<path> (the old dbglog var) still
// works — it sets the file AND acts as AGENTTY_LOG=debug when AGENTTY_LOG
// is unset. The old util::dbglog(where,msg) API forwards here (General/
// Error). AGENTTY_DEBUG_API's raw wire dump stays separate on purpose:
// full request/chunk bodies are a byte-level tool, not log events.
//
// THREADING: format on the caller's stack; publish to the ring with a
// relaxed fetch_add claim; append with one write(2). No background
// thread — nothing to join at shutdown, no teardown races (see the
// prewarm-thread shutdown bug for why agentty avoids detached loggers).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace agentty::logx {

// ── Channels ──────────────────────────────────────────────────────────
// One per subsystem. Adding one: extend the enum, kChannelNames, and the
// static_assert — the parser and filter pick it up automatically.
enum class Channel : std::uint8_t {
    General,   // uncategorised (the dbglog shim lands here)
    Wire,      // HTTP/SSE transports, request/response lifecycle
    Auth,      // OAuth flows, key resolution, account switching
    Persist,   // settings/threads/memory disk IO
    Tool,      // tool dispatch, permissions, sandbox
    Ui,        // reducer/view anomalies
    Rag,       // retrieval engine
    Mcp,       // MCP bridge + plugins
    Acp,       // ACP server/adapter
    Smart,     // smart-mode routing decisions
    Net,       // sockets, TLS, proxy, prewarm
    kCount_,   // sentinel — keep last
};
inline constexpr std::size_t kChannels =
    static_cast<std::size_t>(Channel::kCount_);

inline constexpr std::string_view kChannelNames[kChannels] = {
    "general", "wire", "auth", "persist", "tool", "ui",
    "rag", "mcp", "acp", "smart", "net",
};

// ── Levels ────────────────────────────────────────────────────────────
enum class Level : std::uint8_t {
    Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4,
    Off   = 5,   // filter value only — no event carries Off
};
inline constexpr char kLevelChar[] = { 'T', 'D', 'I', 'W', 'E', '-' };

// ── The gate (hot path) ───────────────────────────────────────────────
// Per-channel minimum level, parsed once from the environment. Relaxed
// loads are correct: the array is written once during init() (before any
// reader can observe a torn value — init runs from a magic static) and
// never changes after.
namespace detail {
extern std::atomic<std::uint8_t> g_min_level[kChannels];
extern std::atomic<bool>         g_ring_all;   // ring captures < Warn too
bool init();                                    // idempotent, cheap after first
inline bool inited() {
    static const bool done = init();
    return done;
}
} // namespace detail

// True iff an event on (ch, lv) would be written to the FILE. The macro
// calls this before evaluating its format arguments.
[[nodiscard]] inline bool enabled(Channel ch, Level lv) noexcept {
    (void)detail::inited();
    return static_cast<std::uint8_t>(lv) >=
           detail::g_min_level[static_cast<std::size_t>(ch)]
               .load(std::memory_order_relaxed);
}

// True iff the event should be recorded AT ALL (file OR flight ring).
// Warn+ always records to the ring, even with file logging off.
[[nodiscard]] inline bool recorded(Channel ch, Level lv) noexcept {
    return lv >= Level::Warn || enabled(ch, lv)
        || detail::g_ring_all.load(std::memory_order_relaxed);
}

// ── Emission (cold path — call through the macros) ────────────────────
// `site` is a short static tag ("openai.stream", "persistence.save").
// `msg` is the fully formatted body. Writes the file line (if enabled)
// and the flight-recorder slot (if recorded). Never throws, never
// allocates on the ring path.
void emit(Channel ch, Level lv, std::string_view site,
          std::string_view msg) noexcept;

// Dump the flight recorder to `fd` using only async-signal-safe calls
// (raw write of preformatted slots, oldest first). Called from the
// SIGSEGV/SIGABRT handler; also usable from tests. Returns bytes written.
std::size_t dump_flight_recorder(int fd) noexcept;

// Open (creating) the crash-dump file and dump the ring into it, with a
// small header. Returns true if anything was written. NOT signal-safe in
// the path-building part — main.cpp precomputes the path at startup and
// the handler calls the (int fd) overload; this convenience exists for
// tests and orderly shutdown paths.
bool dump_flight_recorder_to(const char* path) noexcept;

// The resolved log-file path ("" = file logging disabled). For status UI.
[[nodiscard]] std::string_view log_file() noexcept;

// ── Spans ─────────────────────────────────────────────────────────────
// RAII scope timer: entry at Trace, exit at Trace with the duration.
// The exit line always emits when the channel is at Trace — including
// via early return or exception unwind.
class Span {
public:
    Span(Channel ch, std::string_view name) noexcept
        : ch_(ch), name_(name), live_(enabled(ch, Level::Trace)) {
        if (live_) {
            t0_ = std::chrono::steady_clock::now();
            emit(ch_, Level::Trace, name_, "begin");
        }
    }
    ~Span() {
        if (!live_) return;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0_).count();
        char buf[48];
        const int n = std::snprintf(buf, sizeof(buf), "end (%lld.%03lld ms)",
                                    static_cast<long long>(us / 1000),
                                    static_cast<long long>(us % 1000));
        emit(ch_, Level::Trace, name_,
             std::string_view{buf, n > 0 ? static_cast<std::size_t>(n) : 0});
    }
    Span(const Span&)            = delete;
    Span& operator=(const Span&) = delete;

private:
    Channel          ch_;
    std::string_view name_;
    bool             live_;
    std::chrono::steady_clock::time_point t0_{};
};

// ── Formatting front-end ──────────────────────────────────────────────
// std::format with compile-time checked format strings. The enabled()/
// recorded() gate runs BEFORE argument evaluation via the macro, so a
// disabled statement never formats, never converts, never allocates.
template <class... Args>
void logf(Channel ch, Level lv, std::string_view site,
          std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        emit(ch, lv, site, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) { /* diagnostics must never throw into callers */ }
}

} // namespace agentty::logx

// ── Macros ────────────────────────────────────────────────────────────
// AGT_LOG(wire, Warn, "openai.stream", "connect refused host={}", host);
// Channel name is the bare enum arm; level the bare Level arm. The gate
// short-circuits before `__VA_ARGS__` evaluates — a disabled log line
// costs one atomic load.
#define AGT_LOG(ch, lv, site, ...)                                          \
    do {                                                                    \
        if (::agentty::logx::recorded(::agentty::logx::Channel::ch,         \
                                      ::agentty::logx::Level::lv))          \
            ::agentty::logx::logf(::agentty::logx::Channel::ch,             \
                                  ::agentty::logx::Level::lv,               \
                                  site, __VA_ARGS__);                       \
    } while (0)

// AGT_SPAN(persist, "save_thread"); — names the local so two spans in one
// scope need distinct names (deliberate: nested spans should be visible).
#define AGT_SPAN(ch, name)                                                  \
    ::agentty::logx::Span agt_span_##ch{::agentty::logx::Channel::ch, name}
