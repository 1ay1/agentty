// frozen_invariant_fuzz — randomized property test for the frozen-
// scrollback subsystem (src/runtime/app/update/frozen.cpp).
//
// The curated midrun_* tests pin specific, known-bad scenarios. This
// harness attacks the SAME invariants from the other direction: it
// drives ARBITRARY sequences of conversation growth + frozen-subsystem
// operations and asserts a fixed set of structural invariants holds
// after EVERY operation — not just the hand-picked states.
//
// The class of bug this exists to catch is exactly where the real
// scrollback corruption has always lived: a freeze/trim path that
// leaves the parallel frozen arrays out of lockstep, strands a leading
// separator, lets frozen_through outrun messages.size(), under/over-
// commits scrollback rows, or freezes the mutable streaming back
// message. Each is a one-line invariant here; a random walk that trips
// it prints the seed + op trace so it's deterministically reproducible.
//
// Invariants checked after every op (see check_invariants):
//   I1  lockstep: frozen.size()==frozen_rows.size()==frozen_is_separator.size()
//   I2  frozen_row_total == sum(frozen_rows), and never < 0 (size_t underflow)
//   I3  frozen_through <= messages.size()
//   I4  the frozen prefix never OPENS on a separator (gap) entry
//   I5  during an ACTIVE stream, frozen_through never reaches the
//       mutable back message (messages.size()) — the duplicated-card bug
//   I6  a trim Cmd is either none, or a CommitScrollback whose count is
//       a conservative UNDER-estimate of the rows dropped (commit <=
//       dropped, never an OVER-commit; never CommitScrollbackOverflow).
//       Over-commit makes the next frame grow → maya scrolls the visible
//       tail into native scrollback = a duplicate band. Under-commit is
//       reconciled in place by maya's render-time shift recovery
//       (scrollback_prefix_matches → commit overflow + case-B), so it is
//       safe — verified against real maya bytes and mirrored by
//       midrun_wire_test, which renders the trim through maya and accepts
//       that reconciliation. This is agent_session's discipline (it
//       commits a deliberate n*ROWS_PER_DROP_LOWER under-estimate).
//   I7  frozen_rows entries are all >= 0
//
// Repro: a failure prints `SEED=<n>` — re-run pins that walk because the
// PRNG and op selection are pure functions of the seed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

static int g_failures = 0;
static int g_checks   = 0;

// ── Deterministic PRNG (splitmix64) ──────────────────────────────────────
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    // [0, n)
    int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool chance(int pct) { return below(100) < pct; }
};

// ── Builders ──────────────────────────────────────────────────────────────
static std::string code_block(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "    auto x = compute(i) + offset; // plausible line\n";
    return out;
}

static ToolUse settled_edit(const std::string& tag, int lines) {
    ToolUse t;
    t.id   = ToolCallId{"edit_" + tag};
    t.name = ToolName{"edit"};
    nlohmann::json edits = nlohmann::json::array();
    edits.push_back({{"old_text", code_block(lines)},
                     {"new_text", code_block(lines)}});
    t.args = {{"path", "src/" + tag + ".cpp"}, {"edits", edits}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, "edited-" + tag};
    return t;
}

static ToolUse settled_write(const std::string& tag, int lines) {
    ToolUse t;
    t.id   = ToolCallId{"write_" + tag};
    t.name = ToolName{"write"};
    std::string content;
    for (int i = 0; i < lines; ++i)
        content += "line " + std::to_string(i) + ": file body content\n";
    t.args = {{"file_path", "/tmp/" + tag + ".txt"}, {"content", content}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, "Created"};
    return t;
}

static ToolUse settled_bash(const std::string& tag, int out_lines) {
    ToolUse t;
    t.id   = ToolCallId{"bash_" + tag};
    t.name = ToolName{"bash"};
    t.args = {{"command", "grep -rn pattern src # " + tag}};
    std::string output;
    for (int i = 0; i < out_lines; ++i)
        output += "src/f" + std::to_string(i) + ".cpp:" + std::to_string(i)
                + ": matching line\n";
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(output)};
    return t;
}

// ── The op log, printed on failure for a deterministic repro ──────────────
static std::vector<std::string> g_oplog;
static std::uint64_t g_seed = 0;

static void fail(const char* inv, const char* detail) {
    ++g_failures;
    std::fprintf(stderr, "  FAIL [%s] %s  (SEED=%llu)\n", inv, detail,
                 static_cast<unsigned long long>(g_seed));
    std::fprintf(stderr, "    op trace (%zu ops):\n", g_oplog.size());
    for (const auto& o : g_oplog) std::fprintf(stderr, "      %s\n", o.c_str());
}

#define INV(cond, name, detail)                          \
    do { ++g_checks; if (!(cond)) fail(name, detail); } while (0)

// ── The invariant battery ────────────────────────────────────────────────
static void check_invariants(const Model& m, bool stream_active,
                             const maya::Cmd<agentty::Msg>* trim_cmd,
                             std::size_t expected_dropped_rows) {
    const auto& f = m.ui;

    // I1 lockstep
    INV(f.frozen.size() == f.frozen_rows.size(), "I1",
        "frozen / frozen_rows length mismatch");
    INV(f.frozen.size() == f.frozen_is_separator.size(), "I1",
        "frozen / frozen_is_separator length mismatch");

    // I7 + I2 row accounting
    std::size_t sum = 0;
    bool any_negative = false;
    for (int r : f.frozen_rows) {
        if (r < 0) any_negative = true;
        else sum += static_cast<std::size_t>(r);
    }
    INV(!any_negative, "I7", "a frozen_rows entry is negative");
    INV(sum == f.frozen_row_total, "I2",
        "frozen_row_total != sum(frozen_rows)");

    // I3 bounds
    INV(f.frozen_through <= m.d.current.messages.size(), "I3",
        "frozen_through outran messages.size()");

    // I4 prefix never opens on a separator (gap) row
    if (!f.frozen_is_separator.empty())
        INV(!f.frozen_is_separator.front(), "I4",
            "frozen prefix opens on a separator/gap entry");

    // I5 never freeze the mutable streaming back
    if (stream_active && !m.d.current.messages.empty())
        INV(f.frozen_through < m.d.current.messages.size(), "I5",
            "froze the mutable streaming back message");

    // I6 trim Cmd contract. A front-trim (dropping the OLDEST frozen
    // entries) is a top-DELETION. maya's render-time shift reconciliation
    // (scrollback_prefix_matches → commit overflow + case-B soft repaint)
    // DOES reconcile it — but only safely when the host did not OVER-commit:
    // committing MORE rows than maya physically scrolled off makes the next
    // frame's content_rows exceed prev_rows, so maya reads it as growth and
    // scrolls the still-visible tail into native scrollback = a duplicate
    // band. Committing FEWER (or none) is reconciled in place. So the
    // contract is: none() when nothing dropped, OR a CommitScrollback whose
    // count is a conservative UNDER-estimate (<= rows dropped). Verified
    // against real maya bytes (over-commit dupes exactly C-actual rows;
    // under-commit reconciles clean) and mirrored by midrun_wire_test.
    // CommitScrollbackOverflow is never valid (it advances prev_rows past
    // whatever maya thinks overflowed, which over-commits).
    if (trim_cmd) {
        using Cmd = maya::Cmd<agentty::Msg>;
        const bool is_none =
            std::holds_alternative<Cmd::None>(trim_cmd->inner);
        const auto* commit =
            std::get_if<Cmd::CommitScrollback>(&trim_cmd->inner);
        INV(is_none || commit != nullptr, "I6",
            "trim returned an unexpected Cmd type — must be none() or a "
            "conservative commit_scrollback(N) (never CommitScrollbackOverflow).");
        if (commit) {
            // Never OVER-commit: committing more rows than the trim dropped
            // strands a duplicate of the visible tail. Under-commit is safe
            // (maya reconciles it), so the bound is <=, not ==.
            INV(static_cast<std::size_t>(std::max(0, commit->rows))
                    <= expected_dropped_rows,
                "I6",
                "trim's commit_scrollback row count > rows actually "
                "dropped (OVER-commit strands a duplicate band).");
            // And it must commit SOMETHING when it dropped rows (a positive
            // lower bound), so a real top-deletion is never left wholly
            // uncommitted-via-overflow-misread.
            INV(commit->rows >= 1, "I6",
                "trim dropped rows but committed 0 — expected a positive "
                "conservative count.");
        } else {
            // none() ⇒ nothing was dropped.
            INV(expected_dropped_rows == 0, "I6",
                "trim dropped rows but returned none() — the deletion is "
                "uncommitted, maya will re-emit the shifted prefix over "
                "committed scrollback.");
        }
    }
}

// Render the whole conversation to a BOUNDED canvas. This is purely a
// smoke check that no op produces a model maya cannot lay out (a throw
// here = a hard regression) — the structural invariants are checked
// separately and don't need a full-height paint. Cap the canvas height
// so a transcript with many tall bash/write cards doesn't make each
// smoke render O(thousands of rows).
static void render_smoke(const Model& m, int width) {
    auto root = maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();
    maya::StylePool pool;
    maya::Canvas canvas(width, 600, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);
}

// ── One random walk ───────────────────────────────────────────────────────
static void run_walk(std::uint64_t seed, int width) {
    g_seed = seed;
    g_oplog.clear();
    Rng rng(seed);

    Model m;
    m.d.current.id = agentty::ThreadId{"fuzz"};
    agentty::app::detail::clear_frozen(m);
    bool stream_active = false;

    const int steps = 20 + rng.below(40);
    for (int step = 0; step < steps; ++step) {
        // A streaming tail is ALWAYS messages.back() in the real app:
        // nothing is pushed on top of it until it settles. Model that
        // faithfully — if a stream is active, the only conversation
        // mutation allowed before a new push is to settle it. (A harness
        // that pushed past a live tail would manufacture an I5
        // "violation" that can't occur in production.)
        if (stream_active && rng.chance(60)) {
            if (!m.d.current.messages.empty()) {
                auto& back = m.d.current.messages.back();
                if (!back.streaming_text.empty()) {
                    back.text = back.streaming_text;
                    back.streaming_text.clear();
                }
            }
            stream_active = false;
            m.s.phase = agentty::phase::Idle{};
            g_oplog.push_back("settle stream -> idle");
        }

        // ── Mutate the conversation ──
        // Skip growth while a stream is active (the tail owns the back).
        const int grow = stream_active ? -1 : rng.below(6);
        if (grow < 0) {
            g_oplog.push_back("(stream active — no push this step)");
        } else if (grow == 0) {
            Message u; u.role = Role::User;
            u.text = "request " + std::to_string(step);
            m.d.current.messages.push_back(std::move(u));
            g_oplog.push_back("push user");
        } else if (grow == 1) {
            Message a; a.role = Role::Assistant;
            a.text = "reply " + std::to_string(step);
            m.d.current.messages.push_back(std::move(a));
            g_oplog.push_back("push assistant-text");
        } else if (grow == 2) {
            Message a; a.role = Role::Assistant;
            a.tool_calls.push_back(
                settled_edit("e" + std::to_string(step), 2 + rng.below(8)));
            m.d.current.messages.push_back(std::move(a));
            g_oplog.push_back("push assistant-edit");
        } else if (grow == 3) {
            Message a; a.role = Role::Assistant;
            a.tool_calls.push_back(
                settled_write("w" + std::to_string(step), 20 + rng.below(120)));
            m.d.current.messages.push_back(std::move(a));
            g_oplog.push_back("push assistant-write");
        } else if (grow == 4) {
            Message a; a.role = Role::Assistant;
            a.tool_calls.push_back(
                settled_bash("b" + std::to_string(step), 50 + rng.below(300)));
            m.d.current.messages.push_back(std::move(a));
            g_oplog.push_back("push assistant-bash");
        } else {
            // streaming tail (active)
            Message t; t.role = Role::Assistant;
            t.streaming_text = "continuing to work ...";
            m.d.current.messages.push_back(std::move(t));
            stream_active = true;
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            g_oplog.push_back("push streaming-tail (active)");
        }

        // ── Pick a frozen-subsystem op ──
        maya::Cmd<agentty::Msg> trim_cmd = maya::Cmd<agentty::Msg>::none();
        const maya::Cmd<agentty::Msg>* trim_ptr = nullptr;
        std::size_t dropped_rows = 0;

        const int op = rng.below(7);
        switch (op) {
        case 0: {
            // freeze_through up to a random live_start (clamped).
            std::size_t n = m.d.current.messages.size();
            std::size_t live_start = n == 0 ? 0 : rng.below(static_cast<int>(n) + 1);
            // Never freeze into the mutable back during an active stream.
            if (stream_active && live_start > n - 1) live_start = n - 1;
            agentty::app::detail::freeze_through(m, live_start);
            g_oplog.push_back("freeze_through(" + std::to_string(live_start) + ")");
            break;
        }
        case 1:
        case 2:
            // (retired ops) the mid-stream carves — freeze_settled_subturns
            // and freeze_streaming_text_prefix — are deleted; production
            // has exactly one freeze site (settle → freeze_through). Keep
            // the case numbers so seeds stay comparable across versions.
            g_oplog.push_back("(carve op retired — no-op)");
            break;
        case 3:
        case 4: {
            std::size_t before = m.ui.frozen_row_total;
            trim_cmd = agentty::app::detail::trim_frozen_if_oversized(m);
            dropped_rows = before - m.ui.frozen_row_total;
            trim_ptr = &trim_cmd;
            g_oplog.push_back("trim_frozen_if_oversized (dropped "
                              + std::to_string(dropped_rows) + " rows)");
            break;
        }
        case 5:
            // rehydrate is an idle-only op in production (thread switch /
            // load) — never fires mid-stream. Skip it while active so we
            // don't manufacture an impossible "froze the live back" state.
            if (stream_active) {
                g_oplog.push_back("(rehydrate skipped — stream active)");
                break;
            }
            agentty::app::detail::rehydrate_frozen(m);
            // rehydrate reasons about the whole transcript; the active
            // flag must reflect the real phase afterward.
            stream_active = std::holds_alternative<agentty::phase::Streaming>(m.s.phase);
            g_oplog.push_back("rehydrate_frozen");
            break;
        default:
            g_oplog.push_back("(no frozen op this step)");
            break;
        }

        check_invariants(m, stream_active, trim_ptr, dropped_rows);

        // Smoke: maya must lay the resulting model out without throwing.
        // Sampled (not every step) — the invariant battery above is the
        // real coverage; the render is just a crash/throw canary, and a
        // tall transcript paint is the harness's dominant cost.
        if (rng.chance(20)) {
            try {
                render_smoke(m, width);
            } catch (const std::exception& e) {
                fail("RENDER", e.what());
            } catch (...) {
                fail("RENDER", "non-std exception during render");
            }
        }

        if (g_failures) return;  // stop on first failure for a clean trace
    }
}

int main() {
    std::printf("frozen_invariant_fuzz\n");

    const int widths[] = {60, 80, 100, 140};
    const int walks_per_width = 120;

    for (int wi = 0; wi < 4 && !g_failures; ++wi) {
        for (int k = 0; k < walks_per_width && !g_failures; ++k) {
            std::uint64_t seed = 0xA11CE5ULL
                               + static_cast<std::uint64_t>(wi) * 1'000'003ULL
                               + static_cast<std::uint64_t>(k) * 2'654'435'761ULL;
            run_walk(seed, widths[wi]);
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
