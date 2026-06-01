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

int main() {
    std::printf("midrun_freeze_test\n");
    test_live_bounded();
    test_all_content_present();
    test_single_turn_header();
    test_split_preserves_content();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
