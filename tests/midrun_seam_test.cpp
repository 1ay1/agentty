// midrun_seam_test — scrollback-duplication regression for the freeze
// discipline (agent_session-style: ONE freeze per turn, at settle).
//
// The real symptom users hit: maya's inline diff treats any row that has
// scrolled past the viewport top as IMMUTABLE — already committed to the
// terminal's native scrollback, unrewritable. So the rendering
// contract is: once a row is emitted at canvas position Y, the row at
// position Y must NEVER change in a later frame (until a full
// Divergent repaint). If any step of a turn — a tool settling, the
// stream finishing, the settle-time freeze — shifts the rows above it
// by even one (a gap appears/disappears, a header row toggles), every
// already-committed row below the shift is re-emitted at a new
// position — and the old copy is stuck in scrollback. That's the
// duplicated-turn ghost.
//
// This test simulates the production cadence (sub-turns land live, the
// whole run freezes once at settle) and asserts the rendered row PREFIX
// is stable across every step: frame N+1's rows [0..K) must byte-match
// frame N's rows [0..K) where K is the shorter common prefix that was
// already on screen. Any divergence in the committed prefix is a
// duplication bug.

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
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

// A unified diff payload the edit tool emits into tc.output(), wrapped in
// the ```diff fence the terminal-state renderer parses. n hunks of `rows`
// changed lines each — enough body to span past a viewport top.
static std::string edit_diff_output(const std::string& tag, int rows) {
    std::string d = "```diff\n";
    d += "--- a/src/" + tag + ".cpp\n";
    d += "+++ b/src/" + tag + ".cpp\n";
    d += "@@ -1," + std::to_string(rows) + " +1," + std::to_string(rows) + " @@\n";
    for (int i = 0; i < rows; ++i) {
        d += "-    auto x = compute(" + std::to_string(i) + "); // was\n";
        d += "+    auto x = compute(" + std::to_string(i) + ") + offset; // now\n";
    }
    d += "```\n";
    return d;
}

// Render the CONVERSATION region only (frozen + live tail), one row per
// line, ASCII-folded, trailing blanks stripped.
static std::vector<std::string> render_rows(const Model& m,
                                            int width = 100,
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

    std::vector<std::string> rows;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    return rows;
}

// Compare two frames under maya's inline-overflow contract. Only rows
// that have scrolled ABOVE the viewport top are committed to native
// scrollback and immutable; on-screen rows (the bottom `term_h` of the
// canvas) may be freely rewritten frame-to-frame.
//
// A row at index i in frame `prev` was committed iff it had already
// overflowed: i < prev.size() - term_h. For the duplication bug we only
// care about those — if a committed row's content differs at the same
// index in `cur`, the renderer would re-emit it below a row already
// stuck in scrollback. Returns the first diverging COMMITTED row index,
// or -1 if every committed row is stable.
static int first_committed_divergence(const std::vector<std::string>& prev,
                                      const std::vector<std::string>& cur,
                                      int term_h) {
    // Rows of `prev` that had overflowed the viewport last frame.
    const int committed_end =
        static_cast<int>(prev.size()) - term_h;
    const int common =
        std::min<int>(static_cast<int>(cur.size()), committed_end);
    for (int i = 0; i < common; ++i)
        if (prev[static_cast<std::size_t>(i)] != cur[static_cast<std::size_t>(i)])
            return i;
    return -1;
}

// Build the User + N settled edit sub-turns landing one at a time, all
// staying LIVE (the production discipline — no mid-run freeze), then
// settle-freeze the whole run once at the end. Assert the committed
// prefix never changes between consecutive snapshots, INCLUDING across
// the final settle-time freeze.
static void test_incremental_freeze_prefix_stable() {
    constexpr int N = 30;
    // A realistic viewport: maya keeps the bottom `term_h` rows on
    // screen; everything above has overflowed into native scrollback
    // and is immutable. Use a typical 40-row terminal. With N=30 edit
    // sub-turns (~6 rows each) the canvas grows well past 40 rows, so
    // early sub-turns genuinely commit to scrollback — exactly the
    // condition under which a prefix shift becomes a visible duplicate.
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"seam"};
    Message u; u.role = Role::User; u.text = "please do many edits";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // User frozen

    std::vector<std::string> prev;
    int worst_divergence = -1;
    int diverging_step   = -1;

    auto note_divergence = [&](int e, const std::vector<std::string>& p,
                               const std::vector<std::string>& c) {
        int d = first_committed_divergence(p, c, kTermH);
        if (d >= 0 && (worst_divergence < 0 || d < worst_divergence)) {
            worst_divergence = d;
            diverging_step   = e;
            // Dump the neighbourhood of the FIRST divergence so a
            // regression shows exactly which committed row moved.
            std::fprintf(stderr,
                "  --- step %d, committed-row divergence at row %d ---\n",
                e, d);
            for (int y = std::max(0, d);
                 y < std::min<int>(d + 3,
                     (int)std::max(p.size(), c.size()));
                 ++y) {
                const char* pv = (y < (int)p.size()) ? p[y].c_str() : "<none>";
                const char* cv = (y < (int)c.size()) ? c[y].c_str() : "<none>";
                std::fprintf(stderr, "    row %2d PREV |%s|\n", y, pv);
                std::fprintf(stderr, "    row %2d CUR  |%s|\n", y, cv);
            }
        }
    };

    for (int e = 0; e < N; ++e) {
        // A settled edit sub-turn lands.
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("s" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));

        // A streaming successor placeholder (exactly what the post-tool
        // continuation pushes in the real reducer). NO mid-run freeze —
        // the whole run stays live until settle.
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "working";
        m.d.current.messages.push_back(std::move(ph));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

        auto cur = render_rows(m);
        if (!prev.empty()) note_divergence(e, prev, cur);
        prev = std::move(cur);

        // Drop the placeholder before the next real sub-turn (the live
        // tail is rebuilt each cycle; the next edit message replaces it).
        m.d.current.messages.pop_back();
    }

    // Settle: the run finishes; the single settle-time freeze fires.
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frozen = render_rows(m);
    if (!prev.empty()) note_divergence(N, prev, frozen);

    if (worst_divergence >= 0) {
        std::fprintf(stderr,
            "  committed prefix changed at row %d on step %d "
            "(a scrollback row was rewritten -> duplication)\n",
            worst_divergence, diverging_step);
    }
    CHECK(worst_divergence < 0,
          "committed prefix mutated across the live run / settle freeze");
}

// THE single-tool transition the incremental test above never exercises:
// ONE edit streamed through Running (args only, no output) -> Done (diff
// fence in tc.output()) -> mid-run freeze. The renderer SWITCHES shape
// across this transition (streaming EditDiff-from-args, elided, ->
// terminal GitDiff-from-fence, full). The seam contract says: any row
// that already overflowed the viewport top while streaming must render
// byte-identical after the settle+freeze, or the committed scrollback
// copy is orphaned and the card duplicates below it.
//
// To make the transition actually commit rows we sandwich the streaming
// edit between a block of already-frozen edits (so its early rows are
// pushed above the kTermH viewport top). Then we snapshot:
//   frame A: edit Running (args preview)
//   frame B: edit Done    (terminal GitDiff body)
//   frame C: turn settles — the single settle-time freeze fires
// and assert the committed prefix is stable A->B and B->C.
static void test_single_edit_stream_to_freeze() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"single"};
    Message u; u.role = Role::User; u.text = "edit one file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Lead-in: a run of settled edits. In production this prior portion
    // of the thread is frozen whole (run fully terminal — freeze_range's
    // run_is_freezable gate passes), so plenty of rows sit above the
    // viewport top before the edit-under-test even appears.
    for (int e = 0; e < 20; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("lead" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));
    }
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    const std::string tag = "target";
    constexpr int kRows = 30;

    // ── frame A: the edit is RUNNING. Args carry the hunk; no output yet.
    {
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id   = ToolCallId{"edit_" + tag};
        t.name = ToolName{"edit"};
        nlohmann::json edits = nlohmann::json::array();
        // Hunk text mirrors the diff body so widths line up.
        std::string ot, nt;
        for (int i = 0; i < kRows; ++i) {
            ot += "    auto x = compute(" + std::to_string(i) + "); // was\n";
            nt += "    auto x = compute(" + std::to_string(i) + ") + offset; // now\n";
        }
        edits.push_back({{"old_text", ot}, {"new_text", nt}});
        t.args = {{"path", "src/" + tag + ".cpp"}, {"edits", edits}};
        t.status = ToolUse::Running{steady_clock::now(), ""};
        a.tool_calls.push_back(std::move(t));
        // Active placeholder behind it so freeze_settled stays valid.
        Message ph2; ph2.role = Role::Assistant; ph2.streaming_text = "...";
        m.d.current.messages.push_back(std::move(a));
        m.d.current.messages.push_back(std::move(ph2));
    }
    auto frame_a = render_rows(m);

    // ── frame B: the edit SETTLES. Same args; status -> Done with the
    //    ```diff fence the terminal renderer parses (renderer SWAPS here).
    {
        // Locate the live edit and flip it to Done.
        agentty::app::detail::with_live_tool(
            m, ToolCallId{"edit_" + tag}, [&](ToolUse& t) {
                auto now = steady_clock::now();
                t.status = ToolUse::Done{now - milliseconds{5}, now,
                                         edit_diff_output(tag, kRows)};
            });
    }
    auto frame_b = render_rows(m);

    // ── frame C: the turn settles — the single settle-time freeze fires
    //    (finalize_turn → pending_settle_freeze → freeze_through).
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frame_c = render_rows(m);

    int d_ab = first_committed_divergence(frame_a, frame_b, kTermH);
    int d_bc = first_committed_divergence(frame_b, frame_c, kTermH);

    if (d_ab >= 0) {
        std::fprintf(stderr,
            "  single-edit Running->Done committed-row divergence at %d\n", d_ab);
        for (int y = d_ab; y < std::min<int>(d_ab + 3,
                 (int)std::max(frame_a.size(), frame_b.size())); ++y) {
            std::fprintf(stderr, "    row %2d A |%s|\n", y,
                         y < (int)frame_a.size() ? frame_a[y].c_str() : "<none>");
            std::fprintf(stderr, "    row %2d B |%s|\n", y,
                         y < (int)frame_b.size() ? frame_b[y].c_str() : "<none>");
        }
    }
    if (d_bc >= 0) {
        std::fprintf(stderr,
            "  single-edit Done->freeze committed-row divergence at %d\n", d_bc);
        for (int y = d_bc; y < std::min<int>(d_bc + 3,
                 (int)std::max(frame_b.size(), frame_c.size())); ++y) {
            std::fprintf(stderr, "    row %2d B |%s|\n", y,
                         y < (int)frame_b.size() ? frame_b[y].c_str() : "<none>");
            std::fprintf(stderr, "    row %2d C |%s|\n", y,
                         y < (int)frame_c.size() ? frame_c[y].c_str() : "<none>");
        }
    }

    CHECK(d_ab < 0, "single edit Running->Done rewrote a committed scrollback row");
    CHECK(d_bc < 0, "single edit Done->freeze rewrote a committed scrollback row");
}

// THE exact screenshot: a READ batch (INSPECT) then an EDIT batch
// (MUTATE) in ONE turn. The read settles to a full 12-line body while
// the run is still active. With a frozen lead-in pushing the read's top
// above the viewport, the read body's downward growth must NOT rewrite
// any committed row of the MUTATE batch / continuation below it.
//
// frame A: read RUNNING (compact, no body) + edit batch below it
// frame B: read DONE (full body) — card grows by ~12 rows
// frame C: mid-run freeze fires
// The committed prefix must be byte-stable A->B and B->C.
static void test_read_then_edit_batch_freeze() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"readedit"};
    Message u; u.role = Role::User; u.text = "inspect then edit";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Lead-in: settled edits frozen whole (terminal run) so the read
    // card's top is well above the viewport top before it settles.
    for (int e = 0; e < 20; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("rlead" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));
    }
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // The INSPECT sub-turn: a read, RUNNING (no body yet).
    {
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id   = ToolCallId{"read_target"};
        t.name = ToolName{"read"};
        t.args = {{"path", "src/target.cpp"}};
        t.status = ToolUse::Running{steady_clock::now(), ""};
        a.tool_calls.push_back(std::move(t));
        m.d.current.messages.push_back(std::move(a));
    }
    // The MUTATE sub-turn already streamed in below the read (the model
    // emitted both in sequence; the read result just hasn't landed yet).
    {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("mutate"));
        m.d.current.messages.push_back(std::move(a));
    }
    // Active placeholder behind both.
    Message ph2; ph2.role = Role::Assistant; ph2.streaming_text = "...";
    m.d.current.messages.push_back(std::move(ph2));
    auto frame_a = render_rows(m);

    // frame B: read SETTLES — full 12-line body appears, growing the
    // INSPECT card and shifting the MUTATE batch below it down.
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"read_target"}, [&](ToolUse& t) {
            auto now = steady_clock::now();
            std::string body;
            for (int i = 0; i < 12; ++i)
                body += "line " + std::to_string(i) + ": target file content\n";
            t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(body)};
        });
    auto frame_b = render_rows(m);

    // frame C: the turn settles — the single settle-time freeze fires.
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frame_c = render_rows(m);

    int d_ab = first_committed_divergence(frame_a, frame_b, kTermH);
    int d_bc = first_committed_divergence(frame_b, frame_c, kTermH);

    auto dump = [&](const char* what, int d,
                    const std::vector<std::string>& p,
                    const std::vector<std::string>& c) {
        if (d < 0) return;
        std::fprintf(stderr, "  read+edit %s committed-row divergence at %d\n",
                     what, d);
        for (int y = d; y < std::min<int>(d + 4,
                 (int)std::max(p.size(), c.size())); ++y) {
            std::fprintf(stderr, "    row %2d P |%s|\n", y,
                         y < (int)p.size() ? p[y].c_str() : "<none>");
            std::fprintf(stderr, "    row %2d C |%s|\n", y,
                         y < (int)c.size() ? c[y].c_str() : "<none>");
        }
    };
    if (d_ab >= 0 || d_bc >= 0)
        std::fprintf(stderr, "  read+edit frames: A=%zu B=%zu C=%zu (kTermH=%d)\n",
                     frame_a.size(), frame_b.size(), frame_c.size(), kTermH);
    dump("Running->Done", d_ab, frame_a, frame_b);
    dump("Done->freeze", d_bc, frame_b, frame_c);

    CHECK(d_ab < 0,
          "read settle grew its body and rewrote a committed row of the "
          "batch below it (the duplicated INSPECT card)");
    CHECK(d_bc < 0, "read+edit Done->freeze rewrote a committed scrollback row");
}

// THE write-tool transition the user actually hits: a single `write`
// streamed through Running (compact tail-window preview, show_all=false)
// -> Done (FULL body, show_all=true) -> mid-run freeze. The streaming
// preview is a small compact card; the settled card is the full file
// (viewport-overflowing). With a frozen lead-in pushing the card's top
// rows above the viewport, any height/shape change across the settle
// strands the streaming copy in scrollback and the settled card paints
// below it — the duplicated Actions box the user reports.
static void test_single_write_stream_to_freeze() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"write"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Lead-in: settled edits frozen whole (terminal run) so the write
    // card's top rows are pushed well above the viewport top before the
    // write settles.
    for (int e = 0; e < 20; ++e) {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("wlead" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));
    }
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // The full file content (~170 lines, viewport-overflowing) — the
    // exact shape of the write that exposed the duplicate.
    std::string content;
    for (int i = 0; i < 170; ++i)
        content += "line " + std::to_string(i) + ": some plausible file content\n";

    // ── frame A: write RUNNING. Args carry the full content (it streams
    //    in token-by-token; by terminal time it's all present). The body
    //    config slices a tail window with show_all=false -> compact card.
    {
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id   = ToolCallId{"write_big"};
        t.name = ToolName{"write"};
        t.args = {{"file_path", "/tmp/big.txt"}, {"content", content}};
        t.status = ToolUse::Running{steady_clock::now(), ""};
        a.tool_calls.push_back(std::move(t));
        Message ph2; ph2.role = Role::Assistant; ph2.streaming_text = "...";
        m.d.current.messages.push_back(std::move(a));
        m.d.current.messages.push_back(std::move(ph2));
    }
    auto frame_a = render_rows(m);

    // ── frame B: write SETTLES. show_all flips true -> full body.
    {
        agentty::app::detail::with_live_tool(
            m, ToolCallId{"write_big"}, [&](ToolUse& t) {
                auto now = steady_clock::now();
                t.status = ToolUse::Done{now - milliseconds{5}, now,
                                         "Created /tmp/big.txt (170+ 0-)"};
            });
    }
    auto frame_b = render_rows(m);

    // ── frame C: the turn settles — the single settle-time freeze fires.
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frame_c = render_rows(m);

    // The settled write MUST be frozen at the settle boundary — the
    // whole turn graduates into m.ui.frozen at once.
    {
        const std::size_t live =
            m.d.current.messages.size() - m.ui.frozen_through;
        CHECK(live == 0,
              "settled turn did not freeze whole at the settle boundary");
    }

    // Both the FROZEN snapshot (frame_c) and the LIVE settled card
    // (frame_b) carry the WHOLE file — byte-identical. A settled write
    // renders its FULL body (show_all) in the live tail too, NOT only the
    // frozen snapshot: feeding the same bytes to both keeps the freeze
    // handoff a pure cache hit (the per-event hash_id in agent_timeline.
    // cpp makes the tall card a paint-once blit, so the full body is free
    // per frame after the first paint — no windowing needed). Live ==
    // frozen body is exactly what keeps the committed prefix byte-stable
    // across the handoff (checked via first_committed_divergence below).
    // Assert the full body in BOTH: last, deep-middle, and first lines.
    {
        auto joined = [](const std::vector<std::string>& rows) {
            std::string s;
            for (const auto& r : rows) { s += r; s.push_back('\n'); }
            return s;
        };
        const std::string b = joined(frame_b);
        const std::string c = joined(frame_c);
        CHECK(c.find("line 169: some plausible") != std::string::npos,
              "frozen write snapshot missing the LAST (tail) line");
        CHECK(b.find("line 169: some plausible") != std::string::npos,
              "settled-live write card missing the LAST (tail) line");
        CHECK(c.find("line 85: some plausible") != std::string::npos,
              "frozen write snapshot elided a deep-middle line — settled "
              "write must show the FULL body, not a capped preview");
        CHECK(b.find("line 85: some plausible") != std::string::npos,
              "settled-live write card elided a deep-middle line — settled "
              "write must show the FULL body, not a capped preview");
        CHECK(c.find("line 0: some plausible") != std::string::npos,
              "frozen write snapshot missing the FIRST (head) line");
        CHECK(b.find("line 0: some plausible") != std::string::npos,
              "settled-live write card missing the FIRST (head) line");
    }

    int d_ab = first_committed_divergence(frame_a, frame_b, kTermH);
    int d_bc = first_committed_divergence(frame_b, frame_c, kTermH);

    auto dump = [&](const char* what, int d,
                    const std::vector<std::string>& p,
                    const std::vector<std::string>& c) {
        if (d < 0) return;
        std::fprintf(stderr, "  write %s committed-row divergence at %d\n", what, d);
        for (int y = d; y < std::min<int>(d + 3,
                 (int)std::max(p.size(), c.size())); ++y) {
            std::fprintf(stderr, "    row %2d P |%s|\n", y,
                         y < (int)p.size() ? p[y].c_str() : "<none>");
            std::fprintf(stderr, "    row %2d C |%s|\n", y,
                         y < (int)c.size() ? c[y].c_str() : "<none>");
        }
    };
    if (d_ab >= 0 || d_bc >= 0)
        std::fprintf(stderr, "  write frames: A=%zu B=%zu C=%zu rows (kTermH=%d)\n",
                     frame_a.size(), frame_b.size(), frame_c.size(), kTermH);
    dump("Running->Done", d_ab, frame_a, frame_b);
    dump("Done->freeze", d_bc, frame_b, frame_c);

    CHECK(d_ab < 0, "write Running->Done rewrote a committed scrollback row");
    CHECK(d_bc < 0, "write Done->freeze rewrote a committed scrollback row");
}

// Reassemble the full assistant body the user should see from the
// model's messages: every Assistant message's settled `text` plus the
// active tail's `streaming_text`, in order. The split must be a pure
// partition of the bytes — no loss, no reorder, no duplication.
static std::string assembled_body(const Model& m) {
    std::string out;
    for (const auto& msg : m.d.current.messages) {
        if (msg.role != Role::Assistant) continue;
        out += msg.text;
        out += msg.streaming_text;
    }
    return out;
}

// A long PURE-TEXT answer streaming in. Under the agent_session
// discipline the WHOLE growing body stays live (no mid-stream carve —
// maya's StreamingMarkdown two-tier renderer keeps the element-tree
// height monotone in byte count, so committed rows never move). This
// test feeds the body in chunks (the real per-tick cadence) and asserts:
//   (1) the committed scrollback prefix never mutates (no duplication),
//   (2) the assembled body equals the original bytes at every step,
//   (3) the settle-time freeze (the ONLY freeze) is row-stable too.
static void test_streaming_text_prefix_freeze() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"stext"};
    Message u; u.role = Role::User; u.text = "explain in detail";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Push the active streaming assistant message.
    Message a; a.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // The full body the model will produce — prose with the occasional
    // heading and a fenced code block. Long enough that the canvas
    // overflows several viewports (committed rows exist), short enough
    // that the per-chunk full-body re-render keeps the test fast.
    std::string full;
    for (int i = 0; i < 400; ++i) {
        if (i % 11 == 0) full += "## Heading " + std::to_string(i) + "\n\n";
        if (i % 23 == 0) {
            full += "```cpp\n";
            full += code_block(3);
            full += "```\n\n";
        }
        full += "Sentence number " + std::to_string(i)
             +  " of a long streaming prose answer.\n\n";
    }

    std::vector<std::string> prev;
    int worst_divergence = -1, diverging_step = -1;
    bool content_ok = true;

    constexpr std::size_t kChunk = 1024;
    std::size_t fed = 0, step = 0;
    while (fed < full.size()) {
        const std::size_t n = std::min(kChunk, full.size() - fed);
        m.d.current.messages.back().streaming_text.append(full, fed, n);
        fed += n;
        ++step;

        // (2) content integrity: assembled == bytes fed so far.
        if (assembled_body(m) != full.substr(0, fed)) content_ok = false;

        // (1) committed-prefix stability.
        auto cur = render_rows(m);
        if (!prev.empty()) {
            int d = first_committed_divergence(prev, cur, kTermH);
            if (d >= 0 && (worst_divergence < 0 || d < worst_divergence)) {
                worst_divergence = d;
                diverging_step   = static_cast<int>(step);
            }
        }
        prev = std::move(cur);
    }

    // Final settle (finalize_turn): the tail commits to text and the
    // whole run freezes — the single freeze site. Committed prefix must
    // be stable across the freeze too.
    {
        auto& last = m.d.current.messages.back();
        last.text += last.streaming_text;
        last.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frozen = render_rows(m);
    if (!prev.empty()) {
        int d = first_committed_divergence(prev, frozen, kTermH);
        if (d >= 0 && (worst_divergence < 0 || d < worst_divergence)) {
            worst_divergence = d;
            diverging_step   = static_cast<int>(step) + 1;
        }
    }

    CHECK(content_ok,
          "assembled body diverged from fed bytes");
    if (worst_divergence >= 0) {
        std::fprintf(stderr,
            "  committed prefix changed at row %d on step %d\n",
            worst_divergence, diverging_step);
    }
    CHECK(worst_divergence < 0,
          "committed prefix mutated while streaming / at settle freeze");
    CHECK(assembled_body(m) == full,
          "final assembled body != original after settle");
}

// (deleted) strip_synthetic_fences + run_giant_fence +
// test_giant_fence_prefix_freeze exercised the fence close/reopen
// fallback of freeze_streaming_text_prefix — machinery that is gone.
// A giant single fence now simply stays live until settle (the
// agent_session behaviour); its committed-prefix stability is covered
// by test_streaming_text_prefix_freeze above (which includes fences).

// THE reported bug shape: a settled write/edit card TALLER than the
// viewport sits in the live tail until the turn settles, then the
// settle-time freeze moves the whole run into the frozen prefix. The
// live tail and freeze_range key the run through the SAME
// assistant_run_hash_id, so the handoff must be a pure cache hit — any
// row shift across the freeze strands the live copy in scrollback and
// the card duplicates.
static void test_tall_card_live_to_frozen_seam() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"tall"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // User frozen

    // A settled edit whose diff body is FAR taller than the viewport.
    Message a; a.role = Role::Assistant;
    {
        ToolUse t;
        t.id   = ToolCallId{"edit_tall"};
        t.name = ToolName{"edit"};
        t.args = {{"path", "src/tall.cpp"}};
        auto now = steady_clock::now();
        t.status = ToolUse::Done{now - milliseconds{5}, now,
                                 edit_diff_output("tall", 120)};
        a.tool_calls.push_back(std::move(t));
    }
    m.d.current.messages.push_back(std::move(a));

    // The post-tool continuation placeholder lands (still streaming).
    // The whole run — tall card included — stays LIVE (agent_session
    // discipline; no mid-run freeze).
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "working";
    m.d.current.messages.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Frame LIVE: the settled card renders in the live tail.
    const std::size_t frozen_through_before = m.ui.frozen_through;
    auto live = render_rows(m);
    CHECK(frozen_through_before == 1,
          "run unexpectedly frozen while still active");

    // The turn settles — the single settle-time freeze fires.
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frozen = render_rows(m);

    // The freeze MUST have taken the whole run.
    CHECK(m.ui.frozen_through == m.d.current.messages.size(),
          "settle freeze did not take the whole run");

    // Committed prefix (rows that overflowed the viewport top) must be
    // byte-identical across the live->frozen handoff.
    int d = first_committed_divergence(live, frozen, kTermH);
    if (d >= 0) {
        std::fprintf(stderr,
            "  --- live->frozen committed divergence at row %d ---\n", d);
        for (int y = d; y < std::min<int>(d + 3,
                 (int)std::max(live.size(), frozen.size())); ++y) {
            const char* lv = (y < (int)live.size())   ? live[y].c_str()   : "<none>";
            const char* fv = (y < (int)frozen.size()) ? frozen[y].c_str() : "<none>";
            std::fprintf(stderr, "    row %2d LIVE   |%s|\n", y, lv);
            std::fprintf(stderr, "    row %2d FROZEN |%s|\n", y, fv);
        }
    }
    CHECK(d < 0,
          "tall settled card shifted the committed prefix on freeze "
          "(the overflow->scrollback duplication)");

    // And the diff body must appear EXACTLY once across the whole render
    // (no second copy stranded). The settled edit shows its FULL body, so
    // any body line appears exactly once — count a line from the last
    // hunk; >1 copy means a stranded scrollback duplicate.
    int copies = 0;
    for (const auto& row : frozen)
        if (row.find("compute(119) + offset") != std::string::npos) ++copies;
    CHECK(copies == 1,
          "edit diff body rendered more than once after freeze (duplicate)");
}

// Compaction-boundary seam: after a WIRE-ONLY compaction, the live tail
// of the post-compaction turn must already carry the `≡ Conversation
// compacted` divider that freeze_range stamps at the boundary. If the
// live tail omits it, the settle-freeze INSERTS the divider as a +1 row
// shift at the top of the just-streamed turn — every committed row below
// re-emits over its scrollback copy (the post-compaction duplicate-turn /
// clipped-panel bug from the mobile screenshots). Models the production
// state precisely: the pre-compaction prefix froze BEFORE any record
// existed (so its frozen rows carry no divider), a CompactionRecord then
// lands at up_to_index == frozen_through (compaction does NOT mutate
// messages), and a tall post-compaction turn streams LIVE then settles.
// See INLINE_SCROLLBACK.md pin #3 (divider symmetry).
static void test_compaction_boundary_live_to_frozen_seam() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"compact"};
    agentty::app::detail::clear_frozen(m);
    auto& msgs = m.d.current.messages;

    // ── Pre-compaction history: a short user+assistant exchange that
    //    froze before any compaction record existed, so its frozen prefix
    //    carries NO divider (exactly how production reaches this state).
    {
        Message u; u.role = Role::User; u.text = "old question";
        msgs.push_back(std::move(u));
        Message a; a.role = Role::Assistant; a.text = "old answer";
        msgs.push_back(std::move(a));
    }
    agentty::app::detail::freeze_through(m, msgs.size());
    const std::size_t boundary = msgs.size();
    CHECK(m.ui.frozen_through == boundary,
          "pre-compaction prefix did not freeze");

    // ── Wire-only compaction lands: a record covering [0, boundary).
    //    Messages are NOT mutated; the view draws the divider at
    //    index == up_to_index.
    {
        agentty::Thread::CompactionRecord rec;
        rec.up_to_index = boundary;
        rec.summary     = "summary of the old conversation";
        m.d.current.compactions.push_back(std::move(rec));
    }

    // ── Post-compaction turn: the user asks again and the assistant
    //    emits a settled edit whose diff body is FAR taller than the
    //    viewport. The whole run stays LIVE (settle-freeze hasn't fired).
    {
        Message u; u.role = Role::User;
        u.text = "new question after compaction";
        msgs.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id   = ToolCallId{"edit_post"};
        t.name = ToolName{"edit"};
        t.args = {{"path", "src/post.cpp"}};
        auto now = steady_clock::now();
        t.status = ToolUse::Done{now - milliseconds{5}, now,
                                 edit_diff_output("post", 120)};
        a.tool_calls.push_back(std::move(t));
        msgs.push_back(std::move(a));
    }
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "done";
    msgs.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Frame LIVE: divider + post-compaction turn render in the live tail.
    CHECK(m.ui.frozen_through == boundary,
          "run unexpectedly frozen while still active");
    auto live = render_rows(m);

    // The divider MUST already be in the LIVE tail (the fix). Without it
    // the divider would only materialise at freeze time.
    int live_dividers = 0;
    for (const auto& row : live)
        if (row.find("Conversation compacted") != std::string::npos)
            ++live_dividers;
    CHECK(live_dividers == 1,
          "live tail missing the compaction divider before freeze "
          "(it would materialise at freeze time — the +1 row shift)");

    // ── The turn settles — the single settle-time freeze fires.
    {
        auto& back = msgs.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, msgs.size());
    auto frozen = render_rows(m);

    CHECK(m.ui.frozen_through == msgs.size(),
          "settle freeze did not take the whole post-compaction run");

    // Committed prefix (rows that overflowed the viewport top) must be
    // byte-identical across the live->frozen handoff. Without the divider
    // in the live tail, the freeze inserts it as a +1 shift right here.
    int d = first_committed_divergence(live, frozen, kTermH);
    if (d >= 0) {
        std::fprintf(stderr,
            "  --- compaction live->frozen committed divergence row %d ---\n", d);
        for (int y = d; y < std::min<int>(d + 3,
                 (int)std::max(live.size(), frozen.size())); ++y) {
            const char* lv = (y < (int)live.size())   ? live[y].c_str()   : "<none>";
            const char* fv = (y < (int)frozen.size()) ? frozen[y].c_str() : "<none>";
            std::fprintf(stderr, "    row %2d LIVE   |%s|\n", y, lv);
            std::fprintf(stderr, "    row %2d FROZEN |%s|\n", y, fv);
        }
    }
    CHECK(d < 0,
          "compaction divider shifted the committed prefix on freeze "
          "(the post-compaction duplicate-turn bug)");

    // The divider must appear EXACTLY once in the frozen frame.
    int divider_copies = 0;
    for (const auto& row : frozen)
        if (row.find("Conversation compacted") != std::string::npos)
            ++divider_copies;
    CHECK(divider_copies == 1,
          "compaction divider rendered != once after freeze");

    // And the tall edit body appears exactly once (no stranded copy).
    int body_copies = 0;
    for (const auto& row : frozen)
        if (row.find("compute(119) + offset") != std::string::npos)
            ++body_copies;
    CHECK(body_copies == 1,
          "post-compaction edit body rendered more than once (duplicate)");
}

// Production-faithful compaction flow: submit_message (modal.cpp) pushes
// the next user message, freeze_through()s it, THEN pushes the assistant
// placeholder — so the `≡ Conversation compacted` divider is stamped into
// the FROZEN prefix at submit, before the live tail exists. The live tail
// therefore only ever holds the in-flight assistant run; the divider is
// never in it. This guards the REAL user-reported path: the submit must be
// a clean APPEND below the committed prefix, and the assistant settle
// freeze must not shift a committed row. (Distinct from the unit-level
// symmetry test above, which forces the boundary into the live tail to
// exercise build_live_tail's divider+number code directly.)
static void test_compaction_submit_freezes_divider() {
    constexpr int kTermH = 40;

    Model m;
    m.d.current.id = agentty::ThreadId{"compact2"};
    agentty::app::detail::clear_frozen(m);
    auto& msgs = m.d.current.messages;

    // Pre-compaction history, frozen before any record exists.
    {
        Message u; u.role = Role::User; u.text = "old question";
        msgs.push_back(std::move(u));
        Message a; a.role = Role::Assistant; a.text = "old answer";
        msgs.push_back(std::move(a));
    }
    agentty::app::detail::freeze_through(m, msgs.size());
    const std::size_t boundary = msgs.size();

    // Wire-only compaction record at the boundary.
    {
        agentty::Thread::CompactionRecord rec;
        rec.up_to_index = boundary;
        rec.summary     = "summary of the old conversation";
        m.d.current.compactions.push_back(std::move(rec));
    }

    // Before the next submit the divider is PENDING, not shown: it sits at
    // index == frozen_through == total, so neither builder emits it yet.
    auto pre_submit = render_rows(m);
    int pre_dividers = 0;
    for (const auto& row : pre_submit)
        if (row.find("Conversation compacted") != std::string::npos)
            ++pre_dividers;
    CHECK(pre_dividers == 0,
          "divider shown before the next submit (should be pending)");

    // ── submit_message's exact order: push user, freeze_through (stamps
    //    divider + user into FROZEN), then push the assistant placeholder.
    {
        Message u; u.role = Role::User;
        u.text = "new question after compaction";
        msgs.push_back(std::move(u));
    }
    agentty::app::detail::freeze_through(m, msgs.size());
    CHECK(m.ui.frozen_through == boundary + 1,
          "submit did not freeze through the new user message");
    // The divider is now in the FROZEN prefix — above frozen_through, so
    // build_live_tail's boundary check (up_to_index == i) can never fire.
    CHECK(m.ui.frozen_through > boundary,
          "frozen_through must pass the compaction boundary at submit");

    Message ph; ph.role = Role::Assistant;   // empty placeholder, as submit pushes
    msgs.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // The submit only APPENDED (divider + user + placeholder) below the
    // pre-submit committed prefix — nothing above may move.
    auto after_submit = render_rows(m);
    {
        int d = first_committed_divergence(pre_submit, after_submit, kTermH);
        CHECK(d < 0,
              "post-compaction submit shifted a committed scrollback row");
    }
    // Divider is now visible exactly once — and it lives in the frozen
    // prefix, never the live tail.
    int submit_dividers = 0;
    for (const auto& row : after_submit)
        if (row.find("Conversation compacted") != std::string::npos)
            ++submit_dividers;
    CHECK(submit_dividers == 1,
          "compaction divider not stamped exactly once at submit");

    // ── The assistant emits a settled edit whose diff body overflows the
    //    viewport. No streaming text on it: the deferred settle-freeze
    //    (meta.cpp) fires only after the reveal has drained, so the freeze
    //    instant compares a fully-settled body against the frozen one —
    //    modelling a live reveal here would just race the animation clock
    //    (that path is covered by reveal_scrollback_test).
    {
        auto& a = msgs.back();
        ToolUse t;
        t.id   = ToolCallId{"edit_post2"};
        t.name = ToolName{"edit"};
        t.args = {{"path", "src/post2.cpp"}};
        auto now = steady_clock::now();
        t.status = ToolUse::Done{now - milliseconds{5}, now,
                                 edit_diff_output("post2", 120)};
        a.tool_calls.push_back(std::move(t));
    }
    auto live = render_rows(m);

    // The turn settles — the single settle-time freeze fires.
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, msgs.size());
    auto frozen = render_rows(m);

    CHECK(m.ui.frozen_through == msgs.size(),
          "settle freeze did not take the whole post-compaction run");

    int d = first_committed_divergence(live, frozen, kTermH);
    if (d >= 0) {
        std::fprintf(stderr,
            "  --- submit-compaction live->frozen divergence row %d ---\n", d);
        for (int y = d; y < std::min<int>(d + 3,
                 (int)std::max(live.size(), frozen.size())); ++y) {
            const char* lv = (y < (int)live.size())   ? live[y].c_str()   : "<none>";
            const char* fv = (y < (int)frozen.size()) ? frozen[y].c_str() : "<none>";
            std::fprintf(stderr, "    row %2d LIVE   |%s|\n", y, lv);
            std::fprintf(stderr, "    row %2d FROZEN |%s|\n", y, fv);
        }
    }
    CHECK(d < 0,
          "post-compaction assistant settle shifted the committed prefix");

    int divider_copies = 0;
    for (const auto& row : frozen)
        if (row.find("Conversation compacted") != std::string::npos)
            ++divider_copies;
    CHECK(divider_copies == 1,
          "compaction divider rendered != once after the settle freeze");

    int body_copies = 0;
    for (const auto& row : frozen)
        if (row.find("compute(119) + offset") != std::string::npos)
            ++body_copies;
    CHECK(body_copies == 1,
          "post-compaction edit body rendered more than once (duplicate)");
}

// A full write turn's body: the settled write tool + a short text
// continuation, the exact (tool, text) pair shape the agent emits per
// turn. `content` rows make the write card overflow the viewport.
static void push_write_turn(Model& m, const std::string& tag, int rows) {
    Message a; a.role = Role::Assistant;
    ToolUse t;
    t.id   = ToolCallId{"write_" + tag};
    t.name = ToolName{"write"};
    std::string content;
    for (int i = 0; i < rows; ++i)
        content += tag + " line " + std::to_string(i)
                +  ": plausible file content here\n";
    t.args = {{"file_path", "/tmp/" + tag + ".txt"}, {"content", content}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now,
                             "Created /tmp/" + tag + ".txt"};
    a.tool_calls.push_back(std::move(t));
    m.d.current.messages.push_back(std::move(a));
}

// THE multi-turn reproduction: user asks for a write THREE times. Each
// turn is a (write tool, text summary) pair. "After the third write, the
// SECOND duplicates." By turn 3, turns 1 & 2 are fully frozen (each
// frozen whole at its settle boundary). When turn 3's write streams and
// overflows the viewport, the already-committed rows of turn 2 must NOT
// move. If the frozen prefix's row sequence shifts when turn 3's live
// tail grows, turn 2's card re-emits over its scrollback copy — the
// duplicate.
//
// We drive the production cadence for each turn:
//   submit (freeze_through prior turn + user) -> write Done ->
//   placeholder text streams (whole run LIVE) -> text settles ->
//   settle-time freeze_through (the single freeze). Then the next
//   submit. At turn 3 we snapshot every step and assert turn 2's
//   committed rows are byte-stable across the whole turn-3 sequence.
static void test_multi_turn_write_pairs_seam() {
    constexpr int kTermH = 40;
    constexpr int kRows  = 60;   // each write overflows a 40-row viewport

    Model m;
    m.d.current.id = agentty::ThreadId{"multi"};
    agentty::app::detail::clear_frozen(m);

    auto& msgs = m.d.current.messages;

    // A distinctive body line of a given turn, to count copies.
    auto body_marker = [](const std::string& tag) {
        return tag + " line 30:";
    };

    // Drive turns 1 and 2 to completion (fully frozen).
    for (int turn = 1; turn <= 2; ++turn) {
        const std::string tag = "t" + std::to_string(turn);
        // ── submit
        Message u; u.role = Role::User;
        u.text = "write a file (" + tag + ")";
        msgs.push_back(std::move(u));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        agentty::app::detail::freeze_through(m, msgs.size());

        // ── write settles; the continuation placeholder streams. The
        //    whole run stays LIVE (no mid-run freeze).
        push_write_turn(m, tag, kRows);
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "done writing";
        msgs.push_back(std::move(ph));

        // ── continuation text settles, turn finalizes (idle) — the
        //    single settle-time freeze takes the whole run.
        auto& back = msgs.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
        m.s.phase = agentty::phase::Idle{};
        agentty::app::detail::freeze_through(m, msgs.size());
    }

    // Both turns must be fully frozen now (nothing live).
    CHECK(m.ui.frozen_through == msgs.size(),
          "turns 1 & 2 not fully frozen before turn 3");

    // Snapshot the settled two-turn transcript — this is the committed
    // baseline turn 3 must never disturb.
    auto baseline = render_rows(m);

    // ── Turn 3: submit, then the third write streams in (overflowing).
    {
        Message u; u.role = Role::User; u.text = "write a file (t3)";
        msgs.push_back(std::move(u));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        agentty::app::detail::freeze_through(m, msgs.size());
    }
    auto after_submit = render_rows(m);

    // Write Running (full content present, card windowed) + placeholder.
    push_write_turn(m, "t3", kRows);
    // Demote the just-pushed write to Running to model the streaming
    // arrival before it settles.
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"write_t3"}, [&](ToolUse& t) {
            t.status = ToolUse::Running{steady_clock::now(), ""};
        });
    Message ph3; ph3.role = Role::Assistant; ph3.streaming_text = "...";
    msgs.push_back(std::move(ph3));
    auto frame_running = render_rows(m);

    // Write settles. The whole run stays live until the turn finishes.
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"write_t3"}, [&](ToolUse& t) {
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now,
                                     "Created /tmp/t3.txt"};
        });
    // Critical frame: write Done + placeholder present, freeze has NOT
    // run yet (run still active). The live tail keys this run with the
    // same assistant_run_hash_id the settle freeze will use.
    auto frame_settled = render_rows(m);

    // The turn settles — the single settle-time freeze fires.
    {
        auto& back = msgs.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, msgs.size());
    auto frame_frozen = render_rows(m);

    // The committed prefix (everything that overflowed the viewport top)
    // must be byte-stable across EVERY turn-3 step. Turn 2's card lives
    // in that committed region; any shift duplicates it.
    struct Step { const char* name; const std::vector<std::string>* rows; };
    const Step steps[] = {
        {"baseline",     &baseline},
        {"after_submit", &after_submit},
        {"running",      &frame_running},
        {"settled",      &frame_settled},
        {"frozen",       &frame_frozen},
    };
    for (std::size_t s = 1; s < std::size(steps); ++s) {
        int d = first_committed_divergence(*steps[s - 1].rows,
                                           *steps[s].rows, kTermH);
        if (d >= 0) {
            std::fprintf(stderr,
                "  multi-turn committed divergence %s->%s at row %d\n",
                steps[s - 1].name, steps[s].name, d);
            const auto& p = *steps[s - 1].rows;
            const auto& c = *steps[s].rows;
            for (int y = d; y < std::min<int>(d + 4,
                     (int)std::max(p.size(), c.size())); ++y) {
                std::fprintf(stderr, "    row %2d P |%s|\n", y,
                             y < (int)p.size() ? p[y].c_str() : "<none>");
                std::fprintf(stderr, "    row %2d C |%s|\n", y,
                             y < (int)c.size() ? c[y].c_str() : "<none>");
            }
        }
        CHECK(d < 0,
              "turn-3 write shifted a committed scrollback row "
              "(turn 2 duplicates)");
    }

    // Turn 2's body must appear EXACTLY once in the final frame.
    int t2_copies = 0;
    for (const auto& row : frame_frozen)
        if (row.find(body_marker("t2")) != std::string::npos) ++t2_copies;
    CHECK(t2_copies == 1,
          "turn 2's write body rendered more than once after the third "
          "write (the reported duplicate)");
}

// NOTE: test_incremental_body_freeze_seam / _seam_chunked (which forced
// the mid-stream carve on via set_incremental_freeze_override and drove
// maybe_incremental_freeze) are DELETED along with the incremental-
// freeze machinery itself. The production discipline — stay live, freeze
// once at settle — is covered by test_incremental_freeze_prefix_stable
// (above) and the single/write/read seam tests below.

// ── Settled-prefix HOIST correctness (the perf split must be invisible).
//    turn_config_for_assistant_run hoists the leading quiescent sub-turns
//    of an in-flight run into ONE cacheable bare Turn to keep per-frame
//    build O(1) in turn depth. The split must NEVER change what the user
//    sees: every settled tool card must still render EXACTLY once, and
//    the row output must be identical frame-to-frame as the prefix grows
//    (a hoisted card silently vanishing, or a stale cache blitting an
//    old prefix, is the "sometimes not working" bug this guards).
static void test_prefix_hoist_all_cards_present() {
    Model m;
    m.d.current.id = agentty::ThreadId{"hoist"};
    Message u; u.role = Role::User; u.text = "do many edits";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // User frozen

    constexpr int N = 24;   // deep enough that the hoist engages
    std::vector<std::string> prev;

    for (int e = 0; e < N; ++e) {
        // A settled edit sub-turn lands (all prior ones become the
        // hoistable quiescent prefix).
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_edit("h" + std::to_string(e)));
        m.d.current.messages.push_back(std::move(a));

        // Streaming successor placeholder = the live edge (never hoisted).
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "working";
        m.d.current.messages.push_back(std::move(ph));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

        auto cur = render_rows(m);

        // (1) EVERY settled edit card so far must be present EXACTLY
        //     once. settled_edit stamps tc.output()="edited-<tag>"; the
        //     card renders the tool name + path src/<tag>.cpp, so the
        //     unique path is our per-card marker. A hoisted card that
        //     vanished (defer poisoning / stale prefix cache) drops its
        //     marker; a double-emit (spurious gap / duplicated slot)
        //     bumps the count above one.
        for (int k = 0; k <= e; ++k) {
            const std::string marker = "src/h" + std::to_string(k) + ".cpp";
            int copies = 0;
            for (const auto& row : cur)
                if (row.find(marker) != std::string::npos) ++copies;
            CHECK(copies == 1,
                  ("settled card " + marker
                   + " not rendered exactly once in a "
                   + std::to_string(e + 1) + "-deep in-flight run "
                   + "(got " + std::to_string(copies)
                   + ") — prefix hoist dropped or duplicated a card").c_str());
        }

        // (2) The committed prefix must be byte-stable across the grow
        //     (a stale prefix cache blitting an old shape would rewrite
        //     an already-overflowed row).
        if (!prev.empty()) {
            int d = first_committed_divergence(prev, cur, /*term_h=*/40);
            CHECK(d < 0,
                  "prefix hoist rewrote a committed row as the run grew");
        }
        prev = std::move(cur);

        m.d.current.messages.pop_back();   // drop placeholder
    }

    // (3) Settle-freeze the whole run: the frozen output must contain
    //     each card exactly once too (the split→flat handoff is clean).
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    auto frozen = render_rows(m);
    for (int k = 0; k < N; ++k) {
        const std::string marker = "src/h" + std::to_string(k) + ".cpp";
        int copies = 0;
        for (const auto& row : frozen)
            if (row.find(marker) != std::string::npos) ++copies;
        CHECK(copies == 1,
              ("card " + marker + " not exactly once after settle-freeze")
                  .c_str());
    }
}

int main() {
    std::printf("midrun_seam_test\n");
    test_incremental_freeze_prefix_stable();
    test_prefix_hoist_all_cards_present();
    test_single_edit_stream_to_freeze();

    test_single_write_stream_to_freeze();
    test_read_then_edit_batch_freeze();
    test_streaming_text_prefix_freeze();
    test_tall_card_live_to_frozen_seam();
    test_compaction_boundary_live_to_frozen_seam();
    test_compaction_submit_freezes_divider();
    test_multi_turn_write_pairs_seam();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
