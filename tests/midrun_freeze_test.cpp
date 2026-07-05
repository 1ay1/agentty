// midrun_freeze_test — correctness for the single-freeze discipline
// (agent_session-style): the whole active run stays LIVE; it freezes
// into m.ui.frozen exactly once, at settle (finalize_turn). The frozen
// transcript and the live render must be VISUALLY SEAMLESS — one turn
// reads as ONE turn:
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
// with one settled edit, plus a streaming tail. Only the User is frozen
// (submit-time freeze_through); the WHOLE active run stays live — the
// agent_session discipline. The run freezes once, at settle.
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
    return m;
}

// 1. During an active run NOTHING beyond the User is frozen (the whole
//    run is live, agent_session-style); at settle the run freezes whole.
static void test_live_bounded() {
    Model m = active_run(40);
    CHECK(m.ui.frozen_through == 1,
          "mid-run freeze fired during an active run — the single freeze "
          "site is settle (finalize_turn)");
    // Settle: stream finished. finalize_turn drains streaming_text into
    // text before the settle freeze; mirror that — the hardened
    // run_is_freezable refuses any run with un-drained streaming bytes.
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    CHECK(m.ui.frozen_through == m.d.current.messages.size(),
          "settle freeze didn't freeze the whole run");
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

// 4. Compare a SETTLED+frozen render of the run against the same
//    messages rendered fully live: the freeze changes WHERE rows render
//    (frozen vs live), never WHICH rows.
static void test_split_preserves_content() {
    constexpr int N = 15;
    Model split = active_run(N);
    // Settle + freeze whole (production path).
    split.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(split,
        split.d.current.messages.size());

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

// 5. The freeze gate never freezes a non-terminal run: with a Running
//    tool in the tail, freeze_through stops at the run start; once the
//    run is fully terminal it freezes whole. (run_is_freezable — the
//    only mid-run safety the single-freeze discipline still needs.)
static void test_trailing_tool_batch_freezes() {
    Model m;
    m.d.current.id = agentty::ThreadId{"trailing"};
    Message u; u.role = Role::User; u.text = "write a file";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    ToolUse running = settled_edit("only");
    running.status = ToolUse::Running{steady_clock::now(), ""};
    a.tool_calls.push_back(std::move(running));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    // Even an (erroneous) whole-thread freeze attempt must stop at the
    // non-terminal run — run_is_freezable is the gate.
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    CHECK(m.ui.frozen_through == 1,
          "froze a run with a Running tool — spinner pinned in scrollback");

    // The tool settles; the turn finishes; the settle freeze takes all.
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"edit_only"}, [](ToolUse& t) {
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now, "edited"};
        });
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    CHECK(m.ui.frozen_through == m.d.current.messages.size(),
          "settled run did not freeze at the settle boundary");
    auto txt = render_dump(m);
    CHECK(txt.find("src/only.cpp") != std::string::npos,
          "frozen tool batch body vanished from the render");
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

// 8. The over-budget trim drops the oldest frozen entries from the MODEL
//    (a top-DELETION of the frozen prefix) and MUST commit exactly the
//    dropped rows to native scrollback via Cmd::commit_scrollback(N).
//    This is NOT a bottom-shrink: maya's render-time shrink
//    reconciliation cannot fix a top-deletion. After the drop the new
//    canvas content shifts up, so canvas row 0 differs from prev_cells
//    and scrollback_prefix_matches() returns false; maya's only
//    prefix-mismatch recovery commits the NEW prefix into scrollback
//    rows that PHYSICALLY hold OLD content, stranding a duplicate of the
//    trimmed boundary one screen up. Returning none() TRIGGERS that bug,
//    it does not avoid it. commit_scrollback(removed_rows) runs against
//    the prior (old-content) frame, advancing prev_rows/prev_cells in
//    lockstep so the next render aligns and the boundary lands once.
//    CONTRACT: the trim mutates the model AND returns
//    commit_scrollback(removed_rows).
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

    // Simulate one rendered frame: the ledger's provability gate only
    // drops blocks whose height maya's paint has recorded, and this
    // harness never runs the real paint pass. Stamp each block's policy
    // height as its recorded height — exactly what the ledger-tagged
    // paint does with the laid-out height in production.
    for (std::size_t k = 0; k < m.ui.frozen.size(); ++k)
        m.ui.frozen.record_paint(
            k, static_cast<int>(m.ui.frozen.block_rows(k)));

    // Record the row totals before the trim so we can compute exactly
    // how many rows the trim drops.
    const std::size_t rows_before = m.ui.frozen.row_total();
    const std::size_t entries_before = m.ui.frozen.size();

    auto cmd = agentty::app::detail::trim_frozen_if_oversized(m);

    const std::size_t rows_after   = m.ui.frozen.row_total();
    const std::size_t dropped_rows = rows_before - rows_after;
    CHECK(entries_before > m.ui.frozen.size(),
          "trim did not drop any entries despite an over-budget prefix");
    CHECK(dropped_rows > 0, "trim dropped entries but no rows");

    // The returned Cmd MUST be commit_scrollback(N) where N is the exact
    // count of rows the trim removed from the TOP of the frozen prefix.
    // A top-deletion is invisible to maya's render-time reconciliation;
    // the host must commit the dropped rows against the still-valid old
    // frame so the next render's shorter canvas aligns. Returning none()
    // (or commit_scrollback_overflow) strands a duplicate boundary.
    using Cmd = maya::Cmd<agentty::Msg>;
    const auto* commit = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
    CHECK(commit != nullptr,
          "trim must return commit_scrollback(N) for the top-deletion; "
          "none() leaves maya unable to reconcile and strands a duplicate "
          "of the trimmed boundary one screen up.");
    if (commit) {
        CHECK(commit->rows == static_cast<int>(dropped_rows),
              "trim committed the wrong row count — must equal the rows "
              "dropped from the frozen prefix, or the scrollback boundary "
              "misaligns (under-commit strands the new prefix; over-commit "
              "eats a visible row).");
    }
}

// 9. (deleted) trim_frozen_above_viewport was the mid-run-safe trim for
//    the mid-run carve cadence. Both are gone: the single freeze site is
//    settle (finalize_turn), so the only trim is trim_frozen_if_oversized
//    — covered by test 8.

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

    CHECK(m.ui.frozen.row_total() < static_cast<std::size_t>(real_rows) + 32,
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
    test_output_elided_tool_row_estimate_matches_render();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
