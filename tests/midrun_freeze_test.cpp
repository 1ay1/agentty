// midrun_freeze_test — correctness for freeze_settled_subturns.
//
// The mid-run freeze splits an active auto-pilot run: completed leading
// sub-turns move into m.ui.frozen (zero-copy, cached) while the active
// tail stays live. This must be VISUALLY SEAMLESS — the frozen prefix
// and the live remainder read as ONE turn:
//
//   • exactly ONE speaker header for the whole turn (the frozen prefix
//     carries it; the live remainder is a continuation, header
//     suppressed),
//   • EVERY completed sub-turn's body renders in full (nothing hidden /
//     elided),
//   • the turn number is not duplicated or advanced mid-run,
//   • frozen_through advances and exactly one sub-turn stays live.
//
// Verified structurally: render the whole conversation (frozen list +
// live tail) to a Canvas and read the cells back as text.

#include <algorithm>
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
using std::chrono::steady_clock;
using std::chrono::milliseconds;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                    \
                         __FILE__, __LINE__, (msg));                       \
        }                                                                  \
    } while (0)

// Render the whole conversation (frozen + live tail) and dump painted
// rows as ASCII-folded text.
static std::string render_dump(const Model& m, int width = 100,
                               int height = 40000) {
    auto root = maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out.push_back('\n');
    }
    return out;
}

static int count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

static std::string code_block(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "    auto x = compute(i) + offset; // plausible line\n";
    return out;
}

static ToolUse settled_edit(const std::string& tag) {
    ToolUse t;
    t.id   = ToolCallId{"edit_" + tag};
    t.name = ToolName{"edit"};
    nlohmann::json edits = nlohmann::json::array();
    edits.push_back({{"old_text", code_block(4)},
                     {"new_text", code_block(4)}});
    t.args = {{"path", "src/" + tag + ".cpp"}, {"edits", edits}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, "edited-" + tag};
    return t;
}

// Build an active auto-pilot run: User, then N assistant sub-turns each
// with one settled edit, plus a streaming tail. Freeze the User and run
// the mid-run freeze (as ToolExecOutput would).
static Model active_run(int n) {
    Model m;
    m.d.current.id = agentty::ThreadId{"midrun"};
    Message u; u.role = Role::User; u.text = "please do many edits";
    m.d.current.messages.push_back(std::move(u));
    for (int e = 0; e < n; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("s" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));
    }
    Message tail; tail.role = Role::Assistant;
    tail.streaming_text = "continuing to work";
    m.d.current.messages.push_back(std::move(tail));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);          // User
    agentty::app::detail::freeze_settled_subturns(m);    // settled prefix
    return m;
}

// 1. Only the active tail stays live; the rest is frozen.
static void test_live_bounded() {
    Model m = active_run(40);
    const std::size_t live = m.d.current.messages.size() - m.ui.frozen_through;
    CHECK(live <= 2,
          "more than the active sub-turn stayed live after mid-run freeze");
    CHECK(m.ui.frozen_midrun,
          "frozen_midrun not set after a mid-run freeze");
    CHECK(m.ui.frozen_through > 1,
          "frozen_through didn't advance past the settled prefix");
}

// 2. Every completed sub-turn renders in full (nothing hidden).
static void test_all_content_present() {
    constexpr int N = 40;
    Model m = active_run(N);
    auto txt = render_dump(m);
    int present = 0;
    for (int e = 0; e < N; ++e)
        if (txt.find("src/s" + std::to_string(e) + ".cpp") != std::string::npos)
            ++present;
    CHECK(present == N, "not every settled sub-turn rendered in full");
    CHECK(txt.find("earlier action") == std::string::npos,
          "an elision marker leaked into the render");
}

// 3. Exactly one speaker header for the turn (no duplicate from the
//    frozen↔live continuation seam). The assistant label is the model
//    badge; we count the meta "turn N" marker which only the HEADER row
//    carries — a continuation suppresses it.
static void test_single_turn_header() {
    Model m = active_run(20);
    auto txt = render_dump(m);
    // The turn meta carries "turn N". For one logical assistant turn it
    // must appear exactly once across the frozen prefix + live tail.
    const int turn_markers = count_occurrences(txt, "turn 1");
    CHECK(turn_markers == 1,
          "turn number appears != once \u2014 continuation seam duplicated header");
}

// 4. Compare against a NON-split render of the same logical turn: build
//    the same messages but DON'T mid-run freeze (whole run live). The
//    set of content lines must match \u2014 the split changes WHERE rows
//    render (frozen vs live), never WHICH rows.
static void test_split_preserves_content() {
    constexpr int N = 15;
    Model split = active_run(N);

    Model whole;
    whole.d.current.id = agentty::ThreadId{"whole"};
    Message u; u.role = Role::User; u.text = "please do many edits";
    whole.d.current.messages.push_back(std::move(u));
    for (int e = 0; e < N; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("s" + std::to_string(e)));
        whole.d.current.messages.push_back(std::move(a));
    }
    Message tail; tail.role = Role::Assistant;
    tail.streaming_text = "continuing to work";
    whole.d.current.messages.push_back(std::move(tail));
    whole.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    agentty::app::detail::clear_frozen(whole);
    agentty::app::detail::freeze_through(whole, 1);   // freeze User only

    auto a = render_dump(split);
    auto b = render_dump(whole);
    // Every edit marker present in both.
    int both = 0;
    for (int e = 0; e < N; ++e) {
        std::string needle = "src/s" + std::to_string(e) + ".cpp";
        if (a.find(needle) != std::string::npos
            && b.find(needle) != std::string::npos) ++both;
    }
    CHECK(both == N, "split and whole renders disagree on content presence");
}

// 5. agent_session roll-up parity, corrected. A settled tool batch is
//    frozen mid-run so its big write/edit card doesn't linger live
//    re-rendering its full body. BUT it must NOT be frozen while it is
//    still the mutable back message (a trailing "Done — …" text block
//    can still append to the SAME message, and freezing then snapshots
//    a stale card + advances frozen_through onto a message freeze_range
//    can roll back, re-pushing the card as a fresh head — the duplicated
//    write/edit the user sees). It freezes the instant cmd_factory has
//    pushed the continuation placeholder after it (no longer the back).
static void test_trailing_tool_batch_freezes() {
    Model m;
    m.d.current.id = agentty::ThreadId{"trailing"};
    Message u; u.role = Role::User; u.text = "write a file";
    m.d.current.messages.push_back(std::move(u));
    // ONE assistant sub-turn with a settled tool, still the live back
    // (exactly the state at ToolExecOutput, BEFORE the continuation
    // placeholder is pushed). It must stay LIVE — freezing the mutable
    // back is the duplicated-card bug.
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_edit("only"));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);          // User frozen
    agentty::app::detail::freeze_settled_subturns(m);    // back is mutable: no-op

    CHECK(m.ui.frozen_through == 1,
          "froze the mutable back tool message — the duplicated-card bug");

    // Now cmd_factory's continuation placeholder lands after it. The
    // settled tool message is no longer the back, so it freezes.
    Message cont; cont.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(cont));
    agentty::app::detail::freeze_settled_subturns(m);

    CHECK(m.ui.frozen_through == 2,
          "settled tool batch did NOT freeze once a continuation followed it");
    CHECK(m.ui.frozen_midrun,
          "frozen_midrun not set — the continuation would repaint a header");
    // The body must still be present (frozen, not dropped).
    auto txt = render_dump(m);
    CHECK(txt.find("src/only.cpp") != std::string::npos,
          "frozen tool batch body vanished from the render");

    // When the stream is NOT active, the clamp must still hold (no
    // continuation is coming, so freeze_through owns the final freeze).
    Model idle;
    idle.d.current.id = agentty::ThreadId{"idle"};
    Message u2; u2.role = Role::User; u2.text = "write a file";
    idle.d.current.messages.push_back(std::move(u2));
    Message a2; a2.role = Role::Assistant;
    a2.tool_calls.push_back(settled_edit("only"));
    idle.d.current.messages.push_back(std::move(a2));
    idle.s.phase = agentty::phase::Idle{};
    agentty::app::detail::clear_frozen(idle);
    agentty::app::detail::freeze_through(idle, 1);
    agentty::app::detail::freeze_settled_subturns(idle);
    CHECK(idle.ui.frozen_through == 1,
          "idle run froze the last sub-turn via the active-only path");
}

// Return the first NON-BLANK rendered row, or "" if every row is blank.
static std::string first_nonblank_row(const Model& m) {
    auto txt = render_dump(m);
    std::size_t p = 0;
    while (p < txt.size()) {
        std::size_t eol = txt.find('\n', p);
        std::string line = txt.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
        bool blank = line.find_first_not_of(" ") == std::string::npos;
        if (!blank) return line;
        if (eol == std::string::npos) break;
        p = eol + 1;
    }
    return {};
}

// How many leading rows are blank before the first content row.
static int leading_blank_rows(const Model& m) {
    auto txt = render_dump(m);
    int n = 0;
    std::size_t p = 0;
    while (p < txt.size()) {
        std::size_t eol = txt.find('\n', p);
        std::string line = txt.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
        if (line.find_first_not_of(" ") != std::string::npos) break;
        ++n;
        if (eol == std::string::npos) break;
        p = eol + 1;
    }
    return n;
}

// 6. Saved-thread reload (rehydrate). A long IDLE thread whose final
//    run is GIANT forces rehydrate_frozen's sub-turn cut. The rebuilt
//    frozen prefix must NOT open with a blank gap or a header-less
//    continuation — the top of the canvas must be a real turn header.
//    (The reported bug: saved threads render with a blank hole / a
//    turn whose header was cut off.)
static void test_rehydrate_top_is_header() {
    Model m;
    m.d.current.id = agentty::ThreadId{"saved"};
    // Several complete user+assistant exchanges, then one GIANT final
    // auto-pilot run (many big write/edit sub-turns) settled idle.
    for (int t = 0; t < 4; ++t) {
        Message u; u.role = Role::User;
        u.text = "request " + std::to_string(t);
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        a.text = "reply " + std::to_string(t);
        m.d.current.messages.push_back(std::move(a));
    }
    // Giant final run: a user turn then 60 settled-edit sub-turns +
    // a closing text sub-turn. ~60 fat cards >> the rehydrate budget,
    // so the cut lands MID-RUN.
    Message gu; gu.role = Role::User; gu.text = "do a huge refactor";
    m.d.current.messages.push_back(std::move(gu));
    for (int e = 0; e < 60; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("big" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));
    }
    Message done; done.role = Role::Assistant; done.text = "all done";
    m.d.current.messages.push_back(std::move(done));
    m.s.phase = agentty::phase::Idle{};

    agentty::app::detail::rehydrate_frozen(m);

    // The whole transcript is idle, so nothing should be live.
    CHECK(m.ui.frozen_through == m.d.current.messages.size(),
          "rehydrate left messages unfrozen on an idle thread");
    // The canvas must not open with a HOLE. A single blank row is the
    // Turn widget's own top margin (every turn has it); a real bug is a
    // stranded multi-row gap (a leading separator entry) or a header-
    // less continuation. Allow the 1-row intrinsic margin.
    CHECK(leading_blank_rows(m) <= 1,
          "rehydrated saved thread opens with a blank gap at the top");
    // The first visible row must be a turn header (model badge / meta),
    // not stray body content from a header-less continuation. The
    // assistant badge is the model name; a user header is the prompt.
    // Either way the first row should carry a speaker, not a bare edit
    // diff line.
    const std::string top = first_nonblank_row(m);
    CHECK(!top.empty(), "rehydrated thread rendered nothing");
    CHECK(top.find("src/big") == std::string::npos,
          "rehydrated thread opens MID-card (header-less continuation) "
          "— the run header was cut off");
}

// 7. Trim must never strand a leading blank gap row. After
//    trim_frozen_if_oversized drops front entries, the new first frozen
//    entry must be a turn, not the inter-turn gap_row that preceded it.
static void test_trim_no_leading_gap() {
    Model m;
    m.d.current.id = agentty::ThreadId{"trim"};
    // Many complete idle exchanges so freeze pushes [turn][gap][turn]...
    // and the row budget is exceeded, forcing a front trim.
    for (int t = 0; t < 80; ++t) {
        Message u; u.role = Role::User;
        u.text = "q" + std::to_string(t);
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("t" + std::to_string(t)));
        m.d.current.messages.push_back(std::move(a));
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto cmd = agentty::app::detail::trim_frozen_if_oversized(m);
    (void)cmd;

    CHECK(leading_blank_rows(m) <= 1,
          "trim stranded a blank gap row at the top of the frozen prefix");
    const std::string top = first_nonblank_row(m);
    CHECK(top.find("src/t") == std::string::npos,
          "trim left a header-less card at the top (dropped a run header "
          "but kept its body)");
}

// 8. The over-budget trim must commit EXACTLY the rows it dropped via a
//    row-counted commit_scrollback(removed_rows) — never the generic
//    commit_scrollback_overflow(), which releases down to a single
//    viewport (prev_rows - term_h) while the trim KEEPS ~1.5 viewports.
//    Over-committing the extra ~0.5 viewport releases rows still in the
//    live frozen tree; the next render re-emits them above the committed
//    boundary and the most-recent off-budget turn duplicates one screen
//    up. This is the "after the third write, the second duplicates" bug.
//    The commit must never exceed the dropped (estimated) rows.
static void test_trim_commits_exact_dropped_rows() {
    Model m;
    m.d.current.id = agentty::ThreadId{"trimrows"};
    // A handful of tall write turns, each well over a viewport, so the
    // frozen prefix blows past frozen_row_budget() and the front trim
    // fires — the exact shape of the reported repro (repeated big writes).
    for (int t = 0; t < 6; ++t) {
        Message u; u.role = Role::User;
        u.text = "write a file (" + std::to_string(t) + ")";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        agentty::ToolUse tw;
        tw.id   = agentty::ToolCallId{"w" + std::to_string(t)};
        tw.name = agentty::ToolName{"write"};
        std::string content;
        for (int i = 0; i < 80; ++i)
            content += "line " + std::to_string(i) + ": file body content\n";
        tw.args = {{"file_path", "/tmp/f" + std::to_string(t) + ".txt"},
                   {"content", content}};
        auto now = std::chrono::steady_clock::now();
        tw.status = agentty::ToolUse::Done{
            now - std::chrono::milliseconds{5}, now, "Created"};
        a.tool_calls.push_back(std::move(tw));
        m.d.current.messages.push_back(std::move(a));
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // Record the row totals before the trim so we can compute exactly
    // how many rows the trim drops.
    const std::size_t rows_before = m.ui.frozen_row_total;
    const std::size_t entries_before = m.ui.frozen.size();

    auto cmd = agentty::app::detail::trim_frozen_if_oversized(m);

    const std::size_t rows_after   = m.ui.frozen_row_total;
    const std::size_t dropped_rows = rows_before - rows_after;
    CHECK(entries_before > m.ui.frozen.size(),
          "trim did not drop any entries despite an over-budget prefix");
    CHECK(dropped_rows > 0, "trim dropped entries but no rows");

    // The returned Cmd MUST be a row-counted commit_scrollback whose
    // count equals exactly the dropped rows — NOT commit_scrollback_
    // overflow (which would over-commit and duplicate the kept turn).
    using Cmd = maya::Cmd<agentty::Msg>;
    const auto* exact = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
    const auto* overflow =
        std::get_if<Cmd::CommitScrollbackOverflow>(&cmd.inner);
    CHECK(overflow == nullptr,
          "trim used commit_scrollback_overflow() (over-commits the kept "
          "~0.5 viewport -> duplicate). Must use commit_scrollback(removed).");
    CHECK(exact != nullptr,
          "trim did not return a row-counted commit_scrollback");
    if (exact) {
        CHECK(static_cast<std::size_t>(exact->rows) == dropped_rows,
              "trim committed a row count != the rows it dropped "
              "(over-commit strands a duplicate, under-commit leaks rows)");
    }
}

// 9. trim_frozen_above_viewport (mid-run-safe variant) must ALSO commit
//    exactly the rows it dropped — NOT commit_scrollback_overflow. Wired
//    into the mid-run paths (tool.cpp after each tool settles, meta.cpp on
//    Tick), where a row-exact commit is safe by construction: commit_
//    inline_prefix clamps the count to (prev_rows - term_h), so a still-
//    visible row can never be committed and no duplicate can strand.
static void test_trim_above_viewport_commits_exact() {
    Model m;
    m.d.current.id = agentty::ThreadId{"trimabove"};
    // Many tall write turns so frozen_row_total blows well past the
    // ~1.5-viewport keep margin and the above-viewport trim fires.
    for (int t = 0; t < 8; ++t) {
        Message u; u.role = Role::User;
        u.text = "write a file (" + std::to_string(t) + ")";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        agentty::ToolUse tw;
        tw.id   = agentty::ToolCallId{"a" + std::to_string(t)};
        tw.name = agentty::ToolName{"write"};
        std::string content;
        for (int i = 0; i < 80; ++i)
            content += "line " + std::to_string(i) + ": file body content\n";
        tw.args = {{"file_path", "/tmp/a" + std::to_string(t) + ".txt"},
                   {"content", content}};
        auto now = std::chrono::steady_clock::now();
        tw.status = agentty::ToolUse::Done{
            now - std::chrono::milliseconds{5}, now, "Created"};
        a.tool_calls.push_back(std::move(tw));
        m.d.current.messages.push_back(std::move(a));
    }
    // Leave an active streaming tail so it models a mid-run state (the
    // only context this trim was ever meant to run in).
    Message tail; tail.role = Role::Assistant; tail.streaming_text = "...";
    m.d.current.messages.push_back(std::move(tail));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size() - 1);

    const std::size_t rows_before    = m.ui.frozen_row_total;
    const std::size_t entries_before = m.ui.frozen.size();

    auto cmd = agentty::app::detail::trim_frozen_above_viewport(m);

    const std::size_t dropped_rows = rows_before - m.ui.frozen_row_total;
    CHECK(entries_before > m.ui.frozen.size(),
          "above-viewport trim dropped nothing despite a tall prefix");
    CHECK(dropped_rows > 0, "above-viewport trim dropped entries but no rows");

    using Cmd = maya::Cmd<agentty::Msg>;
    const auto* exact = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
    const auto* overflow =
        std::get_if<Cmd::CommitScrollbackOverflow>(&cmd.inner);
    CHECK(overflow == nullptr,
          "above-viewport trim used commit_scrollback_overflow() — must use "
          "row-exact commit_scrollback(removed) to stay scrollback-safe");
    CHECK(exact != nullptr,
          "above-viewport trim did not return a row-counted commit_scrollback");
    if (exact) {
        CHECK(static_cast<std::size_t>(exact->rows) == dropped_rows,
              "above-viewport trim committed a row count != the rows it "
              "dropped");
    }
}

// 10. Output-elided tools (bash, read, grep, …) render a fixed head/tail
//     budget, NOT their full output. The frozen_rows estimate must track
//     the ELIDED render height, not the full output line count — an
//     over-count trips the mid-run keep-loop into dropping an on-screen
//     entry and stranding a committed-scrollback ghost band (the bash-card
//     overlap bug). Assert the stored estimate stays close to the real
//     rendered height for a bash card with a huge output.
static void test_output_elided_tool_row_estimate_matches_render() {
    Model m;
    m.d.current.id = agentty::ThreadId{"bashcap"};
    Message u; u.role = Role::User; u.text = "run a noisy command";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    agentty::ToolUse tb;
    tb.id   = agentty::ToolCallId{"bash_noisy"};
    tb.name = agentty::ToolName{"bash"};
    tb.args = {{"command", "grep -rn pattern src"}};
    std::string output;
    for (int i = 0; i < 200; ++i)
        output += "src/file" + std::to_string(i) + ".cpp:"
                + std::to_string(i) + ": some matching line of text\n";
    auto now = steady_clock::now();
    tb.status = agentty::ToolUse::Done{now - milliseconds{5}, now,
                                       std::move(output)};
    a.tool_calls.push_back(std::move(tb));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Idle{};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // The bash card elides 200 output lines to ~4 tail rows. The stored
    // frozen_row_total must be in the same ballpark as the REAL rendered
    // height — a pre-fix estimate counted all 200 lines (~200+ rows),
    // wildly above the ~15-row render.
    auto txt = render_dump(m);
    int real_rows = 0;
    for (char c : txt) if (c == '\n') ++real_rows;

    CHECK(m.ui.frozen_row_total < static_cast<std::size_t>(real_rows) + 32,
          "bash output row estimate vastly over-counts the elided render "
          "(would trip the mid-run keep-loop into a ghost band)");
    // Sanity: the body is still actually rendered (not dropped).
    CHECK(txt.find("file199.cpp") != std::string::npos
          || txt.find("file19") != std::string::npos,
          "bash tail rows missing from the render");
}

int main() {
    std::printf("midrun_freeze_test\n");
    test_live_bounded();
    test_all_content_present();
    test_single_turn_header();
    test_split_preserves_content();
    test_trailing_tool_batch_freezes();
    test_rehydrate_top_is_header();
    test_trim_no_leading_gap();
    test_trim_commits_exact_dropped_rows();
    test_trim_above_viewport_commits_exact();
    test_output_elided_tool_row_estimate_matches_render();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
