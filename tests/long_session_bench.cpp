// long_session_bench — stress + timing for tool-heavy long sessions.
//
// Builds synthetic Threads with parametrised (turns × write_bytes ×
// edit_hunks × bash_output) shapes, then times every hot path that
// the resume/render flow walks. Designed to catch perf regressions
// against the seven pins listed in docs/INLINE_SCROLLBACK.md.
//
// Each scenario reports median / p50 / p99 / mean across K iterations.
// Phases timed per scenario:
//
//   construct        — build the Thread value
//   render_key/tail  — sum compute_render_key() over the live tail
//                       (the per-frame work visual_hash pays)
//   freeze           — clear + freeze_through over the full thread
//                       (cost of building every Element snapshot)
//   rehydrate        — rehydrate_frozen (bounded-tail resume path)
//   view_build       — conversation_config + AppLayout::build
//                       (per-frame Element construction)
//   cold_render      — first render_tree into a fresh canvas+pool
//                       (the user-visible first frame on resume)
//   warm_render      — second render_tree on same canvas+pool
//                       (cache-hit steady state)
//   trim             — trim_frozen_if_oversized when oversized
//
// Build: linked from CMake via the AGENTTY_RUNTIME_NOMAIN_SOURCES
// runtime bundle so we reuse the production reducer + view code
// without a main() collision.
//
// Run: ./build/long_session_bench [scenario_glob]
//      No arg → run all scenarios.
//      Arg    → run only scenarios whose name contains the substring.
//
// Env:
//   BENCH_ITERS=N  override per-phase iteration count (default 5).
//   BENCH_JSON=1   emit a JSON line per scenario instead of the table
//                  (for automation / CI regression tracking).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using namespace std::chrono;
using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::Thread;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;

// ─────────────────────────────────────────────────────────────────────────
// Timing primitives
// ─────────────────────────────────────────────────────────────────────────

using Clock = steady_clock;

[[nodiscard]] double ms(Clock::duration d) noexcept {
    return duration_cast<duration<double, std::milli>>(d).count();
}

struct Stats {
    double median = 0;
    double p99    = 0;
    double mean   = 0;
    double min    = 0;
    double max    = 0;
    int    n      = 0;
};

[[nodiscard]] Stats summarise(std::vector<double>& samples) {
    Stats s;
    s.n = static_cast<int>(samples.size());
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.min  = samples.front();
    s.max  = samples.back();
    s.mean = 0;
    for (double v : samples) s.mean += v;
    s.mean /= static_cast<double>(samples.size());
    s.median = samples[samples.size() / 2];
    // p99 with linear interp for small N; floor when N < 100.
    if (samples.size() == 1) {
        s.p99 = samples[0];
    } else {
        const double pos = 0.99 * static_cast<double>(samples.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(pos);
        const std::size_t hi = std::min(lo + 1, samples.size() - 1);
        const double frac = pos - static_cast<double>(lo);
        s.p99 = samples[lo] + (samples[hi] - samples[lo]) * frac;
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────
// Synthetic content generators
// ─────────────────────────────────────────────────────────────────────────
//
// Goal: bytes that look like real tool output so render_tree exercises
// the same per-line layout cost we'd see in production. We pick from
// a small pool of plausible source lines + shell output so:
//
//   • compute_render_key changes between turns (length differs).
//   • the markdown / code highlighting paths inside Turn rendering
//     hit realistic line shapes (not all-spaces, not all-identical).
//   • UTF-8 doesn't dominate (a sprinkle of multi-byte chars only).
//
// Determinism: a single seeded RNG for the whole run so two
// invocations of the binary produce identical Thread inputs.

namespace gen {

std::mt19937_64& rng() {
    static std::mt19937_64 r{0xC0FFEEULL};
    return r;
}

// Plausible source lines — wide enough variety to bust hash short-circuits.
constexpr std::string_view kCodeLines[] = {
    "    auto it = std::find_if(begin(v), end(v), [&](const auto& x) { return x.id == needle; });",
    "    if (status == Status::Running && !is_terminal()) { schedule_retry(ctx); }",
    "    return std::format(\"[{}] {} -> {}ms\", tag, name, elapsed.count());",
    "#include <chrono>",
    "",
    "    // Bail out if we already drained this turn — see comment in update/stream.cpp.",
    "    const auto t0 = std::chrono::steady_clock::now();",
    "    for (std::size_t i = 0; i < messages.size(); ++i) {",
    "        if (messages[i].role == Role::Assistant && !messages[i].is_terminal()) {",
    "            return std::nullopt;",
    "        }",
    "    }",
    "    static_assert(sizeof(Header) == 16, \"layout drift\");",
    "    Result<Element> el = builder.with_color(Color::rgb(0x4a, 0x9e, 0xff)).build();",
    "    std::shared_ptr<const AgentTimelineEvent> ev = std::make_shared<...>(...);",
    "}",
    "namespace agentty::detail {",
    "",
    "void freeze_range(Model& m, std::size_t from, std::size_t to) {",
    "    if (from >= to) return;",
};
constexpr std::size_t kCodeLinesN = sizeof(kCodeLines) / sizeof(kCodeLines[0]);

[[nodiscard]] std::string code_block(int n_lines) {
    std::string out;
    out.reserve(static_cast<std::size_t>(n_lines) * 64);
    auto& r = rng();
    for (int i = 0; i < n_lines; ++i) {
        out += kCodeLines[r() % kCodeLinesN];
        out += '\n';
    }
    return out;
}

[[nodiscard]] std::string bash_output(int n_lines) {
    static constexpr std::string_view sh[] = {
        "[  0.012s] linking target maya::maya",
        "[  0.084s] linking target agentty",
        "warning: unused parameter 'ctx' [-Wunused-parameter]",
        "/usr/bin/ld: /tmp/foo.o: in function `bar': undefined reference to `baz'",
        "PASS test_render_scaling.cpp:118  cold_paint_under_budget",
        "PASS test_render_scaling.cpp:140  warm_paint_under_budget",
        "FAIL test_render_scaling.cpp:683  per_event_hash_id_bounds_cost",
        "21 tests passed, 1 failed",
    };
    std::string out;
    auto& r = rng();
    for (int i = 0; i < n_lines; ++i) {
        out += sh[r() % (sizeof(sh) / sizeof(sh[0]))];
        out += '\n';
    }
    return out;
}

[[nodiscard]] std::string assistant_prose(int n_paragraphs) {
    static constexpr std::string_view paras[] = {
        "I'll start by exploring the auth flow so I can see what's actually wired together. "
        "The login handler in `src/auth/login.cpp` looks like the right entry point.",
        "The provider factory currently constructs a `LegacyAuth` on every call. "
        "I'll swap that for the new `NewAuth::create` builder, which returns a `Result<Session>` "
        "so the caller can surface init errors instead of crashing.",
        "Three callers depend on the old signature. I'll touch each one in turn: "
        "`src/api/login.cpp`, `src/cli/auth_cmd.cpp`, and `tests/test_auth_flow.cpp`.",
    };
    std::string out;
    auto& r = rng();
    for (int i = 0; i < n_paragraphs; ++i) {
        out += paras[r() % (sizeof(paras) / sizeof(paras[0]))];
        out += "\n\n";
    }
    return out;
}

} // namespace gen

// ─────────────────────────────────────────────────────────────────────────
// Tool builders
// ─────────────────────────────────────────────────────────────────────────
//
// Each builder produces a TERMINAL ToolUse (status = Done) — the freeze
// gate refuses to freeze non-terminal runs, so non-terminal tools would
// silently skip every freeze path and skew the timings.

namespace tool {

int s_id_counter = 0;

[[nodiscard]] ToolUse done(std::string name, nlohmann::json args, std::string output) {
    ToolUse t;
    t.id   = ToolCallId{"call_" + std::to_string(++s_id_counter)};
    t.name = ToolName{std::move(name)};
    t.args = std::move(args);
    auto now = Clock::now();
    t.status = ToolUse::Done{
        .started_at  = now - milliseconds{42},
        .finished_at = now,
        .output      = std::move(output),
    };
    t.expanded = true;
    return t;
}

[[nodiscard]] ToolUse write_tool(std::string_view path, int n_lines) {
    nlohmann::json args = {
        {"file_path", std::string{path}},
        {"content",   gen::code_block(n_lines)},
    };
    // Write tool's output is the canonical "wrote N lines · M bytes" line.
    const std::string body = args["content"].get<std::string>();
    std::string out = "wrote " + std::to_string(n_lines) + " lines · "
                    + std::to_string(body.size()) + " bytes";
    return done("write", std::move(args), std::move(out));
}

[[nodiscard]] ToolUse edit_tool(std::string_view path, int n_hunks) {
    nlohmann::json edits = nlohmann::json::array();
    auto& r = gen::rng();
    for (int i = 0; i < n_hunks; ++i) {
        edits.push_back({
            {"old_text", gen::code_block(3 + static_cast<int>(r() % 6))},
            {"new_text", gen::code_block(3 + static_cast<int>(r() % 6))},
        });
    }
    nlohmann::json args = {
        {"file_path", std::string{path}},
        {"edits",     std::move(edits)},
    };
    std::string out = "applied " + std::to_string(n_hunks) + " edits to "
                    + std::string{path};
    return done("edit", std::move(args), std::move(out));
}

[[nodiscard]] ToolUse bash_tool(std::string_view cmd, int n_lines) {
    nlohmann::json args = {{"command", std::string{cmd}}};
    return done("bash", std::move(args), gen::bash_output(n_lines));
}

[[nodiscard]] ToolUse read_tool(std::string_view path, int n_lines) {
    nlohmann::json args = {{"path", std::string{path}}};
    return done("read", std::move(args), gen::code_block(n_lines));
}

} // namespace tool

// ─────────────────────────────────────────────────────────────────────────
// Scenario shape — declarative parametrisation
// ─────────────────────────────────────────────────────────────────────────

struct Shape {
    std::string name;
    int  n_turns           = 0;     // assistant turns; user turn paired with each
    int  write_lines       = 0;     // lines per Write tool body
    int  penult_write_lines = 0;    // override Write lines for the 2nd-to-last
                                    // turn (reproduces an off-screen GIANT body
                                    // kept by rehydrate behind a small result)
    int  edit_hunks        = 0;     // per Edit tool, 0 to skip
    int  bash_lines        = 0;     // per Bash tool, 0 to skip
    int  read_lines        = 0;     // per Read tool, 0 to skip
    int  assistant_prose_p = 1;     // paragraphs of assistant text per turn
    int  user_text_chars   = 80;    // user message length per turn
    int  iters             = 5;     // outer-loop iterations for stats
};

[[nodiscard]] std::string user_prompt(int chars) {
    std::string s;
    s.reserve(static_cast<std::size_t>(chars));
    static constexpr std::string_view tpl =
        "Please refactor the auth flow to use the new provider, "
        "and run the test suite afterwards. Note any flakes. ";
    while (static_cast<int>(s.size()) < chars) s += tpl;
    s.resize(static_cast<std::size_t>(chars));
    return s;
}

[[nodiscard]] Thread build_thread(const Shape& sh) {
    Thread t;
    t.id    = agentty::ThreadId{"bench_thread"};
    t.title = "Long-session bench: " + sh.name;
    t.messages.reserve(static_cast<std::size_t>(sh.n_turns) * 2);

    for (int turn = 0; turn < sh.n_turns; ++turn) {
        // User turn.
        Message u;
        u.role = Role::User;
        u.text = user_prompt(sh.user_text_chars);
        t.messages.push_back(std::move(u));

        // Assistant turn: prose + tools.
        Message a;
        a.role = Role::Assistant;
        a.text = gen::assistant_prose(sh.assistant_prose_p);
        int wl = sh.write_lines;
        if (sh.penult_write_lines > 0 && turn == sh.n_turns - 2)
            wl = sh.penult_write_lines;   // off-screen giant behind a small tail
        if (wl > 0)
            a.tool_calls.push_back(tool::write_tool(
                "src/auth/login.cpp", wl));
        if (sh.edit_hunks > 0)
            a.tool_calls.push_back(tool::edit_tool(
                "src/api/login.cpp", sh.edit_hunks));
        if (sh.bash_lines > 0)
            a.tool_calls.push_back(tool::bash_tool(
                "cmake --build build -j10", sh.bash_lines));
        if (sh.read_lines > 0)
            a.tool_calls.push_back(tool::read_tool(
                "tests/test_auth.cpp", sh.read_lines));
        t.messages.push_back(std::move(a));
    }
    return t;
}

[[nodiscard]] Model build_model(const Shape& sh) {
    Model m;
    m.d.current = build_thread(sh);
    return m;
}

// ─────────────────────────────────────────────────────────────────────────
// Timed phases — each returns the Stats struct over `sh.iters` runs.
// ─────────────────────────────────────────────────────────────────────────

namespace phase {

[[nodiscard]] Stats construct(const Shape& sh) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto t0 = Clock::now();
        auto m  = build_model(sh);
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
        (void)m;
    }
    return summarise(samples);
}

[[nodiscard]] Stats render_key_tail(const Shape& sh) {
    auto m = build_model(sh);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto t0 = Clock::now();
        std::uint64_t acc = 0;
        for (auto& msg : m.d.current.messages) acc ^= msg.compute_render_key();
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
        volatile std::uint64_t anti_dce = acc; (void)anti_dce;   // anti-DCE (portable)
    }
    return summarise(samples);
}

[[nodiscard]] Stats freeze(const Shape& sh) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto m = build_model(sh);
        auto t0 = Clock::now();
        agentty::app::detail::clear_frozen(m);
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
    }
    return summarise(samples);
}

[[nodiscard]] Stats rehydrate(const Shape& sh) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto m = build_model(sh);   // Model is move-only — rebuild per iter
        auto t0 = Clock::now();
        agentty::app::detail::rehydrate_frozen(m);
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
    }
    return summarise(samples);
}

[[nodiscard]] Stats view_build(const Shape& sh) {
    auto m = build_model(sh);
    agentty::app::detail::rehydrate_frozen(m);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto t0 = Clock::now();
        auto root = maya::AppLayout{{
            .thread        = agentty::ui::thread_config(m),
            .changes_strip = agentty::ui::changes_strip_config(m),
            .composer      = agentty::ui::composer_config(m),
            .status_bar    = agentty::ui::status_bar_config(m),
            .overlay       = std::nullopt,
        }}.build();
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
        volatile const void* anti_dce = &root; (void)anti_dce;   // anti-DCE (portable)
    }
    return summarise(samples);
}

// Render phase: cold vs warm on the SAME canvas + pool, so we measure
// the cache-hit win warmup_render is designed to deliver.
struct RenderStats { Stats cold; Stats warm; };

[[nodiscard]] RenderStats render(const Shape& sh) {
    constexpr int kCanvasW = 120;
    constexpr int kCanvasH = 800;

    std::vector<double> cold_samples;
    std::vector<double> warm_samples;
    cold_samples.reserve(static_cast<std::size_t>(sh.iters));
    warm_samples.reserve(static_cast<std::size_t>(sh.iters));

    for (int i = 0; i < sh.iters; ++i) {
        auto m = build_model(sh);
        agentty::app::detail::rehydrate_frozen(m);

        auto root = maya::AppLayout{{
            .thread        = agentty::ui::thread_config(m),
            .changes_strip = agentty::ui::changes_strip_config(m),
            .composer      = agentty::ui::composer_config(m),
            .status_bar    = agentty::ui::status_bar_config(m),
            .overlay       = std::nullopt,
        }}.build();

        // Fresh pool + canvas → cache is empty → cold path.
        maya::StylePool pool;
        maya::Canvas canvas(kCanvasW, kCanvasH, &pool);
        canvas.clear();

        auto t0 = Clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark,
                          /*auto_height=*/true);
        auto t1 = Clock::now();
        cold_samples.push_back(ms(t1 - t0));

        // Warm: same canvas + pool, same root → cache should blit.
        canvas.clear();
        auto t2 = Clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark,
                          /*auto_height=*/true);
        auto t3 = Clock::now();
        warm_samples.push_back(ms(t3 - t2));
    }
    return { summarise(cold_samples), summarise(warm_samples) };
}

[[nodiscard]] Stats trim(const Shape& sh) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    for (int i = 0; i < sh.iters; ++i) {
        auto m = build_model(sh);   // Model is move-only — rebuild per iter
        agentty::app::detail::rehydrate_frozen(m);
        // Force-pad m.ui.frozen past the soft cap so the trim actually
        // fires. The ledger keeps its element/meta stores in lockstep
        // internally (the old three-parallel-vector UB class is
        // structurally gone); we just seal duplicates of an existing
        // block and stamp their paint-recorded heights so the trim's
        // provability gate lets them drop.
        if (!m.ui.frozen.empty()) {
            const auto exemplar =
                m.ui.frozen.elements()[m.ui.frozen.size() - 1];
            const std::size_t exemplar_rows =
                std::max<std::size_t>(1,
                    m.ui.frozen.block_rows(m.ui.frozen.size() - 1));
            while (m.ui.frozen.size() < 200)
                m.ui.frozen.seal(exemplar, exemplar_rows);
            for (std::size_t k = 0; k < m.ui.frozen.size(); ++k)
                m.ui.frozen.record_paint(
                    k, static_cast<int>(m.ui.frozen.block_rows(k)));
        }
        auto t0 = Clock::now();
        (void)agentty::app::detail::trim_frozen_if_oversized(m);
        auto t1 = Clock::now();
        samples.push_back(ms(t1 - t0));
    }
    return summarise(samples);
}

// Mid-run per-frame steady state — the cost the user actually feels
// DURING a long auto-pilot turn. Production bounds the canvas with
// trim_frozen_if_oversized at turn boundaries (frozen kept to the
// frozen_row_budget) and only the live tail is rebuilt each frame; the
// frozen prefix blits. This phase reproduces that: rehydrate, run the
// trim until it's a no-op (frozen at its bounded steady size), build the
// element tree ONCE, then time a WARM render (same canvas/pool) which is
// the real per-tick cost. If this stays flat across the D/E/G/H shapes
// (200t, 3000-line writes) the per-frame path is genuinely bounded; if
// it scales with thread size the trim isn't engaging on that shape.
struct MidrunStats { Stats frame; std::size_t frozen_rows_after = 0;
                     std::size_t frozen_entries_after = 0; };

[[nodiscard]] MidrunStats midrun_frame(const Shape& sh) {
    auto m = build_model(sh);
    agentty::app::detail::rehydrate_frozen(m);
    // Drive the trim to its fixed point so frozen is at the bounded
    // steady size production holds.
    for (int guard = 0; guard < 64; ++guard) {
        auto c = agentty::app::detail::trim_frozen_if_oversized(m);
        if (c.is_none()) break;
    }

    auto root = maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    constexpr int kCanvasW = 120;
    constexpr int kCanvasH = 4000;
    maya::StylePool pool;
    maya::Canvas canvas(kCanvasW, kCanvasH, &pool);
    canvas.clear();
    // Prime once (cold) so the warm timings below are pure cache-hit.
    maya::render_tree(root, canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sh.iters));
    double clear_sum = 0, render_sum = 0;
    // Mirror the production inline path: after the first (priming)
    // frame, preserve the immutable frozen prefix above the viewport
    // and clear only the live window, so render_tree's cached blit can
    // skip the unchanged prefix. Assume an 80-row terminal viewport
    // (the fallback geometry) with the same 8-row margin app.cpp uses.
    constexpr int kTermH = 80;
    constexpr int kPreserveMargin = 8;
    const bool no_preserve = std::getenv("BENCH_NOPRESERVE") != nullptr;
    for (int i = 0; i < sh.iters; ++i) {
        auto tc0 = Clock::now();
        const int prev_ch = maya::content_height(canvas);
        int keep_top = 0;
        if (!no_preserve && prev_ch > kTermH) {
            keep_top = prev_ch - kTermH - kPreserveMargin;
            if (keep_top < 0) keep_top = 0;
        }
        if (keep_top > 0) canvas.clear_below(keep_top);
        else              canvas.clear();
        auto tc1 = Clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark,
                          /*auto_height=*/true);
        auto t1 = Clock::now();
        clear_sum  += ms(tc1 - tc0);
        render_sum += ms(t1 - tc1);
        samples.push_back(ms(t1 - tc0));
    }
    if (std::getenv("BENCH_SPLIT") && sh.iters > 0) {
        std::fprintf(stderr, "    [split] %-30s clear=%.3f render=%.3f ms/frame\n",
                     sh.name.c_str(), clear_sum / sh.iters, render_sum / sh.iters);
    }
    if (std::getenv("BENCH_DUMP")) {
        std::fprintf(stderr, "    [dump] %s frames:", sh.name.c_str());
        for (double s : samples) std::fprintf(stderr, " %.2f", s);
        std::fprintf(stderr, "\n");
    }
    MidrunStats out;
    out.frame                = summarise(samples);
    out.frozen_rows_after    = m.ui.frozen.row_total();
    out.frozen_entries_after = m.ui.frozen.size();
    return out;
}

} // namespace phase

// ─────────────────────────────────────────────────────────────────────────
// Reporting
// ─────────────────────────────────────────────────────────────────────────

struct ScenarioResult {
    Shape shape;
    Stats construct;
    Stats render_key;
    Stats freeze;
    Stats rehydrate;
    Stats view_build;
    Stats cold_render;
    Stats warm_render;
    Stats trim;
    Stats midrun_frame;
    std::size_t midrun_rows = 0;
    std::size_t midrun_entries = 0;
    std::size_t total_msgs = 0;
    std::size_t total_bytes = 0;
    std::size_t frozen_entries = 0;
};

void print_header() {
    std::printf("\n%-30s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s\n",
        "scenario",
        "construct",
        "render_key/tail",
        "freeze (full)",
        "rehydrate",
        "view_build",
        "cold render",
        "warm render",
        "trim");
    std::printf("%-30s + %-19s + %-19s + %-19s + %-19s + %-19s + %-19s + %-19s + %-19s\n",
        "------------------------------",
        "-------------------",
        "-------------------",
        "-------------------",
        "-------------------",
        "-------------------",
        "-------------------",
        "-------------------",
        "-------------------");
}

static void fmt_cell(char* dst, std::size_t cap, const Stats& s) {
    std::snprintf(dst, cap, "%7.2f / %7.2fp99", s.median, s.p99);
}

void print_row(const ScenarioResult& r) {
    char c0[32], c1[32], c2[32], c3[32], c4[32], c5[32], c6[32], c7[32];
    fmt_cell(c0, sizeof(c0), r.construct);
    fmt_cell(c1, sizeof(c1), r.render_key);
    fmt_cell(c2, sizeof(c2), r.freeze);
    fmt_cell(c3, sizeof(c3), r.rehydrate);
    fmt_cell(c4, sizeof(c4), r.view_build);
    fmt_cell(c5, sizeof(c5), r.cold_render);
    fmt_cell(c6, sizeof(c6), r.warm_render);
    fmt_cell(c7, sizeof(c7), r.trim);
    std::printf("%-30s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s | %-19s\n",
        r.shape.name.c_str(),
        c0, c1, c2, c3, c4, c5, c6, c7);
}

void print_footnote(const ScenarioResult& r) {
    const double warm_speedup = r.warm_render.median > 0.0
        ? r.cold_render.median / r.warm_render.median
        : 0.0;
    std::printf("    %-26s   msgs=%zu  bytes=%zu  frozen=%zu  warm/cold=%.1fx\n",
        "",
        r.total_msgs,
        r.total_bytes,
        r.frozen_entries,
        warm_speedup);
    std::printf("    %-26s   MIDRUN per-frame=%.2f / %.2fp99 ms  "
                "(trimmed frozen: %zu rows, %zu entries)\n",
        "",
        r.midrun_frame.median, r.midrun_frame.p99,
        r.midrun_rows, r.midrun_entries);
}

void emit_json(const ScenarioResult& r) {
    nlohmann::json j;
    j["scenario"]       = r.shape.name;
    j["n_turns"]        = r.shape.n_turns;
    j["write_lines"]    = r.shape.write_lines;
    j["edit_hunks"]     = r.shape.edit_hunks;
    j["bash_lines"]     = r.shape.bash_lines;
    j["read_lines"]     = r.shape.read_lines;
    j["iters"]          = r.shape.iters;
    j["total_msgs"]     = r.total_msgs;
    j["total_bytes"]    = r.total_bytes;
    j["frozen_entries"] = r.frozen_entries;
    auto pack = [](const Stats& s) {
        return nlohmann::json{
            {"median_ms", s.median},
            {"p99_ms",    s.p99},
            {"mean_ms",   s.mean},
            {"min_ms",    s.min},
            {"max_ms",    s.max},
            {"n",         s.n},
        };
    };
    j["construct"]       = pack(r.construct);
    j["render_key_tail"] = pack(r.render_key);
    j["freeze"]          = pack(r.freeze);
    j["rehydrate"]       = pack(r.rehydrate);
    j["view_build"]      = pack(r.view_build);
    j["cold_render"]     = pack(r.cold_render);
    j["warm_render"]     = pack(r.warm_render);
    j["trim"]            = pack(r.trim);
    j["midrun_frame"]    = pack(r.midrun_frame);
    j["midrun_rows"]     = r.midrun_rows;
    j["midrun_entries"]  = r.midrun_entries;
    std::printf("%s\n", j.dump().c_str());
    std::fflush(stdout);
}

// ─────────────────────────────────────────────────────────────────────────
// Scenarios
// ─────────────────────────────────────────────────────────────────────────

std::vector<Shape> all_scenarios(int iters_override) {
    auto apply_iters = [&](Shape s) {
        if (iters_override > 0) s.iters = iters_override;
        return s;
    };
    return {
        // Baseline shapes from the resume-perf commit message.
        apply_iters({.name = "A: 6t × 300-line write",
                     .n_turns = 6,  .write_lines = 300}),
        apply_iters({.name = "B: 6t × 800-line write",
                     .n_turns = 6,  .write_lines = 800}),
        apply_iters({.name = "C: 20t × 500-line write",
                     .n_turns = 20, .write_lines = 500}),
        apply_iters({.name = "D: 80t × 500-line write",
                     .n_turns = 80, .write_lines = 500}),
        // Stress shape — well past kFrozenMax (80) so trim is meaningful
        // and per-frame layout dominates the cold render.
        apply_iters({.name = "E: 200t × 500-line write",
                     .n_turns = 200, .write_lines = 500, .iters = 3}),
        // Edit-heavy: many hunks per call, no Write — stresses the
        // edit-diff body preview path inside agent_timeline.
        apply_iters({.name = "F: 20t × 10-hunk edit",
                     .n_turns = 20, .edit_hunks = 10}),
        // Mixed realistic: Write + Edit + Bash + Read per turn, long
        // session. This is the closest to a real heavy session.
        apply_iters({.name = "G: 80t × Write+Edit+Bash+Read",
                     .n_turns = 80,
                     .write_lines = 200,
                     .edit_hunks  = 3,
                     .bash_lines  = 30,
                     .read_lines  = 80,
                     .assistant_prose_p = 2,
                     .iters = 3}),
        // Pathological — huge single Write to simulate a "dump entire
        // file" turn that ships ~3000 lines.
        apply_iters({.name = "H: 6t × 3000-line write",
                     .n_turns = 6, .write_lines = 3000, .iters = 3}),
        // OFF-SCREEN giant: a 3000-line write as the 2nd-to-last turn, with
        // small turns around it. rehydrate keeps the giant in-window (behind
        // the small current result), so the canvas is ~3000 rows. The
        // rehydrate-time collapse (now OFF by default — it hid disk-loaded
        // bodies behind a misleading stub) would turn the off-screen giant
        // into a 1-row stub. A/B by setting AGENTTY_FROZEN_COLLAPSE=1.
        apply_iters({.name = "I: off-screen 3000-line write + small tail",
                     .n_turns = 6, .write_lines = 8,
                     .penult_write_lines = 3000, .iters = 3}),
    };
}

// ─────────────────────────────────────────────────────────────────────────
// Driver
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::size_t total_bytes(const Thread& t) {
    std::size_t b = 0;
    for (const auto& m : t.messages) {
        b += m.text.size();
        for (const auto& tc : m.tool_calls) {
            b += tc.args_streaming.size();
            b += tc.output().size();
        }
    }
    return b;
}

ScenarioResult run_one(const Shape& sh) {
    ScenarioResult r;
    r.shape = sh;

    // Phase order — each is independent (each builds its own Model
    // unless explicitly stated). We run heavier phases later so the
    // earlier numbers aren't biased by allocator warmup; the iters
    // loop inside each phase also rules out one-shot artefacts.
    r.construct   = phase::construct(sh);
    r.render_key  = phase::render_key_tail(sh);
    r.freeze      = phase::freeze(sh);
    r.rehydrate   = phase::rehydrate(sh);
    r.view_build  = phase::view_build(sh);
    auto rs       = phase::render(sh);
    r.cold_render = rs.cold;
    r.warm_render = rs.warm;
    r.trim        = phase::trim(sh);
    auto mid      = phase::midrun_frame(sh);
    r.midrun_frame   = mid.frame;
    r.midrun_rows    = mid.frozen_rows_after;
    r.midrun_entries = mid.frozen_entries_after;

    // Stats snapshot of the model shape for the footnote.
    auto m = build_model(sh);
    agentty::app::detail::rehydrate_frozen(m);
    r.total_msgs     = m.d.current.messages.size();
    r.total_bytes    = total_bytes(m.d.current);
    r.frozen_entries = m.ui.frozen.size();
    return r;
}

int main(int argc, char** argv) {
    const char* filter   = (argc > 1) ? argv[1] : "";
    const char* iters_e  = std::getenv("BENCH_ITERS");
    const int iters_override = iters_e ? std::atoi(iters_e) : 0;
    const bool emit_json_lines = std::getenv("BENCH_JSON") != nullptr;

    const auto shapes = all_scenarios(iters_override);

    if (!emit_json_lines) {
        std::printf("long_session_bench — agentty resume/render hot paths\n");
        std::printf("  scenarios: %zu, filter: %s, iters override: %s\n",
                    shapes.size(),
                    *filter ? filter : "(none)",
                    iters_e ? iters_e : "(none)");
        std::printf("  cells: median / p99 milliseconds\n");
        print_header();
    }

    int failures = 0;
    for (const auto& sh : shapes) {
        if (*filter && sh.name.find(filter) == std::string::npos) continue;

        ScenarioResult r{};
        try {
            r = run_one(sh);
        } catch (const std::exception& e) {
            ++failures;
            std::fprintf(stderr, "scenario %s threw: %s\n",
                         sh.name.c_str(), e.what());
            continue;
        }

        if (emit_json_lines) {
            emit_json(r);
        } else {
            print_row(r);
            print_footnote(r);
        }
    }

    if (!emit_json_lines) {
        std::printf("\n");
        std::printf("Notes:\n");
        std::printf("  • cold render = first render_tree on fresh canvas/pool (resume frame).\n");
        std::printf("  • warm render = second render_tree, same canvas/pool (cache-hit frame).\n");
        std::printf("  • freeze = clear_frozen + freeze_through over the FULL thread (worst case).\n");
        std::printf("  • rehydrate = bounded-tail rebuild (the actual ThreadLoaded path).\n");
        std::printf("  • view_build excludes render — it's the cost of constructing the Element tree only.\n");
        std::printf("  • render_key/tail = ⊕ compute_render_key() across every message;\n");
        std::printf("    this is the per-frame work visual_hash pays. Should be sub-microsecond.\n");
    }
    return failures == 0 ? 0 : 1;
}
