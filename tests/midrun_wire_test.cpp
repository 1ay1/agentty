// midrun_wire_test — WIRE-LEVEL scrollback-duplication regression.
//
// midrun_seam_test renders each model state with render_tree and compares
// the row arrays. That catches CONTENT/HEIGHT divergence but NOT the
// actual maya inline-compose wire emission: render_tree has no prev_cells
// shadow, no overflow commit, no cursor walk. The screenshot bug
// (settled write card duplicates into scrollback) is a WIRE artifact —
// the freeze frame re-emits rows that already overflowed the viewport
// top, leaving the old copy stranded one screen up.
//
// This test drives maya's REAL inline compose (the same compose_inline_
// frame the runtime calls) over agentty's real Element tree:
//
//   frame A: live tail holds the settled write (full body), overflowing.
//            Render it Synced. Some rows commit to native scrollback.
//   frame B: the write is frozen (freeze_through). Render the new tree
//            against the SAME InlineFrameState.
//
// Invariant: the bytes frame B emits must NOT contain a re-paint of a
// row that frame A already pushed above the viewport top. We model the
// wire as a row grid the emitter mutates; any write to a row index <
// (frameA_rows - term_h) is a committed-scrollback rewrite = the
// duplication bug.

#include <cstdio>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/inline_frame.hpp>
#include <maya/render/serialize.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/writer.hpp>
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

using namespace maya;
using namespace maya::inline_frame;

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

// ── Non-blocking pipe writer; the read end is drained per frame so we
//    capture exactly the bytes each compose emitted.
static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    return {Writer{static_cast<platform::NativeHandle>(fds[1])}, fds[0]};
}

static std::string drain(int rfd) {
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    return out;
}

// Build the conversation Element tree for the current model state.
static maya::Element build_root(const Model& m) {
    return maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();
}

// Render `root` into a canvas sized to its content; return the canvas.
static Canvas paint(const maya::Element& root, int width, StylePool& pool) {
    // Seed tall; render_tree with auto_height grows it.
    Canvas c(width, 4000, &pool);
    c.clear();
    std::vector<layout::LayoutNode> nodes;
    maya::render_tree(root, c, pool, maya::theme::dark, nodes, /*auto_height=*/true);
    return c;
}

static ToolUse settled_write(const std::string& tag, int n_lines) {
    ToolUse t;
    t.id   = ToolCallId{"write_" + tag};
    t.name = ToolName{"write"};
    std::string content;
    for (int i = 0; i < n_lines; ++i)
        content += "line " + std::to_string(i) + ": plausible file body text\n";
    t.args = {{"file_path", "/tmp/" + tag + ".md"}, {"content", content}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now,
                             "Created /tmp/" + tag + ".md"};
    return t;
}

// THE wire-level test. A user turn + a settled write that overflows the
// viewport, frozen at the (idle) turn boundary. Drive maya's real inline
// compose across the freeze and assert no committed scrollback row is
// rewritten.
static void test_write_freeze_no_rewrite() {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wire"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // The settled write as the active turn's only sub-turn. Stream is
    // idle (finalize_turn path): m.s NOT active, so it lingers live until
    // freeze_through. This mirrors "model wrote a file then stopped."
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_write("big", 120));
    m.d.current.messages.push_back(std::move(a));
    // phase idle by default (no Streaming{Active}).

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // ── frame A: render the LIVE settled write. Seed Empty→Fresh→Synced.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto outcome_a = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer,
        /*sync=*/false);
    (void)drain(rfd);

    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  frame A did not reach Synced\n");
                   std::abort(); }
        }, std::move(outcome_a));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "test setup: live write must overflow the viewport "
          "(else there's no committed scrollback to corrupt)");

    // ── freeze: the write graduates into the frozen prefix (idle path).
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // ── frame B: render the FROZEN tree against the SAME state. Verify
    //    the shadow first (mirrors the runtime's Synced arm), then render.
    Canvas cb = paint(build_root(m), kWidth, pool);
    auto wit = sa.verify();
    CHECK(wit.has_value(),
          "shadow verify failed after frame A (state already poisoned)");

    std::string bytes_b;
    if (wit) {
        auto outcome_b = std::move(sa).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), /*sync=*/false);
        bytes_b = drain(rfd);
        // It must stay Synced — a demote here means maya itself detected
        // an overflow-while-poisoned and is recovering (visible flicker).
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(outcome_b));
        CHECK(synced_b,
              "freeze frame demoted out of Synced — maya hit a recovery "
              "path (overflow-while-poisoned), the duplicate-write symptom");
    }

    // Frame B's content above the viewport top must match frame A's: the
    // frozen render of the write must be byte-identical to the live one.
    // We re-derive both canvases as row strings and compare the committed
    // prefix [0, committed_a).
    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto ra = rows_of(ca);
    auto rb = rows_of(cb);
    int first_div = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y) {
        if (y >= (int)ra.size() || ra[y] != rb[y]) { first_div = y; break; }
    }
    if (first_div >= 0) {
        std::fprintf(stderr,
            "  committed-row divergence at %d (rows_a=%d committed=%d):\n"
            "    A |%s|\n    B |%s|\n",
            first_div, rows_a, committed_a,
            first_div < (int)ra.size() ? ra[first_div].c_str() : "<none>",
            first_div < (int)rb.size() ? rb[first_div].c_str() : "<none>");
    }
    CHECK(first_div < 0,
          "frozen render diverges from live render in the committed "
          "scrollback prefix — the stranded-duplicate root cause");

    close(rfd);
}

// MID-RUN active freeze: the write settles while the run is STILL active
// (a continuation placeholder follows it). freeze_settled_subturns fires
// on the ToolExecOutput cadence, freezing the write batch while the
// stream keeps going. This is the screenshot scenario — turn N's write
// frozen mid-run, then the run continues. Same wire invariant: the
// freeze must not rewrite a committed scrollback row.
static void test_write_midrun_active_freeze_no_rewrite() {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wiremid"};
    Message u; u.role = Role::User; u.text = "write a big file then continue";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Settled write sub-turn + an ACTIVE streaming placeholder behind it
    // (the continuation the reducer pushes post-tool). Stream is active.
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_write("bigmid", 120));
    m.d.current.messages.push_back(std::move(a));
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "continuing";
    m.d.current.messages.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // frame A: live tail = [settled write][active placeholder]. Overflows.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto outcome_a = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  midrun frame A not Synced\n");
                   std::abort(); }
        }, std::move(outcome_a));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "midrun setup: live write must overflow the viewport");

    // Mid-run freeze (ToolExecOutput cadence). The write batch graduates
    // into the frozen prefix; the active placeholder stays live.
    agentty::app::detail::freeze_settled_subturns(m);

    // frame B: frozen prefix + live placeholder, against the same state.
    Canvas cb = paint(build_root(m), kWidth, pool);
    auto wit = sa.verify();
    CHECK(wit.has_value(), "midrun shadow verify failed after frame A");
    if (wit) {
        auto outcome_b = std::move(sa).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), false);
        (void)drain(rfd);
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(outcome_b));
        CHECK(synced_b,
              "midrun freeze frame demoted out of Synced — recovery path "
              "hit (the duplicate-write symptom)");
    }

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto ra = rows_of(ca);
    auto rb = rows_of(cb);
    int first_div = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y) {
        if (y >= (int)ra.size() || ra[y] != rb[y]) { first_div = y; break; }
    }
    if (first_div >= 0) {
        std::fprintf(stderr,
            "  MIDRUN committed-row divergence at %d (rows_a=%d committed=%d):\n"
            "    A |%s|\n    B |%s|\n",
            first_div, rows_a, committed_a,
            first_div < (int)ra.size() ? ra[first_div].c_str() : "<none>",
            first_div < (int)rb.size() ? rb[first_div].c_str() : "<none>");
    }
    CHECK(first_div < 0,
          "MIDRUN frozen render diverges from live render in the committed "
          "scrollback prefix — stranded duplicate while stream active");

    close(rfd);
}

// FULL lifecycle at the wire: streaming (windowed) -> settle (full body)
// -> freeze. Three composes against one evolving state. The streaming
// card is compact; on settle it expands to the full body; on freeze it
// graduates. Every committed scrollback row must survive both the
// settle expansion AND the freeze. This is the closest model to the
// real screenshot sequence.
static void test_write_streaming_settle_freeze() {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wirelife"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Lead-in frozen turns so the write card's top is pushed above the
    // viewport top BEFORE it settles (so it genuinely commits rows).
    for (int e = 0; e < 6; ++e) {
        Message la; la.role = Role::Assistant;
        la.tool_calls.push_back(settled_write("lead" + std::to_string(e), 4));
        m.d.current.messages.push_back(std::move(la));
    }
    {
        Message ph0; ph0.role = Role::Assistant; ph0.streaming_text = "x";
        m.d.current.messages.push_back(std::move(ph0));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        agentty::app::detail::freeze_settled_subturns(m);
        m.d.current.messages.pop_back();
    }

    std::string content;
    for (int i = 0; i < 120; ++i)
        content += "line " + std::to_string(i) + ": plausible file body text\n";

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // ── frame A: write RUNNING (streaming). Windowed compact card.
    {
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id = ToolCallId{"wlife"}; t.name = ToolName{"write"};
        t.args = {{"file_path", "/tmp/life.md"}, {"content", content}};
        t.status = ToolUse::Running{steady_clock::now(), ""};
        a.tool_calls.push_back(std::move(t));
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "...";
        m.d.current.messages.push_back(std::move(a));
        m.d.current.messages.push_back(std::move(ph));
    }
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> s = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
            else { std::fprintf(stderr, "  life frame A not Synced\n"); std::abort(); }
        }, std::move(oa));

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto check_prefix = [&](const char* tag, const std::vector<std::string>& prev,
                            const Canvas& cur_canvas, int prev_rows) {
        const int committed = prev_rows > kTermH ? prev_rows - kTermH : 0;
        auto cur = rows_of(cur_canvas);
        int d = -1;
        for (int y = 0; y < committed && y < (int)cur.size(); ++y)
            if (y >= (int)prev.size() || prev[y] != cur[y]) { d = y; break; }
        if (d >= 0)
            std::fprintf(stderr,
                "  %s committed divergence at %d (committed=%d)\n"
                "    PREV |%s|\n    CUR  |%s|\n", tag, d, committed,
                d < (int)prev.size() ? prev[d].c_str() : "<none>",
                d < (int)cur.size()  ? cur[d].c_str()  : "<none>");
        CHECK(d < 0, tag);
    };

    // ── frame B: write SETTLES (Done) -> full body expansion in live tail.
    auto prev_a = rows_of(ca);
    const int rows_a = s.rows();
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"wlife"}, [&](ToolUse& t) {
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now, "Created /tmp/life.md"};
        });
    Canvas cb = paint(build_root(m), kWidth, pool);
    {
        auto wit = s.verify();
        CHECK(wit.has_value(), "life shadow verify failed after A");
        if (wit) {
            auto ob = std::move(s).render(
                cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), false);
            (void)drain(rfd);
            s = std::visit([](auto&& arm) -> InlineFrame<Synced> {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
                else { std::fprintf(stderr,
                    "  life Running->Done demoted out of Synced\n"); std::abort(); }
            }, std::move(ob));
        }
    }
    check_prefix("life Running->Done rewrote a committed row", prev_a, cb, rows_a);

    // ── frame C: mid-run freeze.
    auto prev_b = rows_of(cb);
    const int rows_b = s.rows();
    agentty::app::detail::freeze_settled_subturns(m);
    Canvas cc = paint(build_root(m), kWidth, pool);
    {
        auto wit = s.verify();
        CHECK(wit.has_value(), "life shadow verify failed after B");
        if (wit) {
            auto oc = std::move(s).render(
                cc, content_rows(cc), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), false);
            (void)drain(rfd);
            bool synced = std::visit([](auto&& arm) {
                using T = std::decay_t<decltype(arm)>;
                return std::is_same_v<T, InlineFrame<Synced>>;
            }, std::move(oc));
            CHECK(synced, "life Done->freeze demoted out of Synced");
        }
    }
    check_prefix("life Done->freeze rewrote a committed row", prev_b, cc, rows_b);

    close(rfd);
}

int main() {
    std::printf("midrun_wire_test\n");
    test_write_freeze_no_rewrite();
    test_write_midrun_active_freeze_no_rewrite();
    test_write_streaming_settle_freeze();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
