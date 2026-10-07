// turn_height_monotonic_test — a live turn's height only ever grows.
//
// The thread is printed ABOVE the composer, so a frame that is shorter than
// the one before it drags the composer and status bar up under the user's
// cursor; and once a row has scrolled past the viewport top it belongs to
// the terminal's own scrollback, where no repaint can reach it. A shrink
// there is not a wobble, it is corruption.
//
// Every widget in a turn has its own reason to be monotonic and they are
// each tested where they live (md_shape_sweep for the markdown shapes,
// reasoning_ticker_height_test for the thought ticker's row window, the
// tool bodies' row bounds in their own tests). This test is the one that
// asks the question the user actually experiences: drive ONE turn through
// the real reducers and the real ui::view the way a provider does —
// reasoning, then a tool call with streaming output, then prose, with
// animation frames in between — and watch the painted height.
//
// A failure prints the step where the height dropped and by how much.

#include "agtest.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/print.hpp>
#include <maya/style/theme.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/view.hpp"

namespace A = agentty;
namespace D = agentty::app::detail;

namespace {

constexpr int kWidth  = 72;
constexpr int kTermH  = 24;
constexpr std::uint64_t kExecSeq = 1;

A::Model apply(A::Model m, A::msg::StreamMsg event) {
    auto [next, cmd] = D::step(D::stream_update, std::move(m), std::move(event));
    (void)cmd;
    return std::move(next);
}

A::Model apply_tool(A::Model m, A::msg::ToolMsg event) {
    auto [next, cmd] = D::step(D::tool_update, std::move(m), std::move(event));
    (void)cmd;
    return std::move(next);
}

// Paint the REAL view and report the rows it occupies.
int view_rows(const A::Model& m) {
    maya::RenderContext ctx{kWidth, kTermH, maya::render_generation(),
                            /*auto_height=*/true};
    maya::RenderContextGuard guard(ctx);
    maya::StylePool pool;
    maya::Canvas c(kWidth, 8000, &pool);
    c.clear();
    std::vector<maya::layout::LayoutNode> nodes;
    maya::render_tree(A::ui::view(m), c, pool, maya::theme::native, nodes,
                      /*auto_height=*/true);
    return c.max_content_row() + 1;
}

std::string view_text(const A::Model& m) {
    maya::RenderContext ctx{kWidth, kTermH, maya::render_generation(),
                            /*auto_height=*/true};
    maya::RenderContextGuard guard(ctx);
    return maya::render_to_string(A::ui::view(m), kWidth);
}

struct Watch {
    const A::Model& m;
    int   prev = 0;
    int   drops = 0;
    int   worst = 0;
    std::string worst_step;

    void frame(const std::string& step) {
        const int rows = view_rows(m);
        if (rows < prev) {
            ++drops;
            if (prev - rows > worst) { worst = prev - rows; worst_step = step; }
        }
        prev = rows;
    }
    void tick(const std::string& step) {
        maya::testing::advance_anim_clock_ms(20);
        frame(step);
    }
};

// Reasoning the way a provider delivers it: a few sentences per delta, with
// paragraph breaks. Long enough to slide the live window past its cap, and
// shaped to include the case that collapsed a node-counted window — a SHORT
// paragraph becoming the newest node after a long one.
const std::vector<std::string>& reasoning_deltas() {
    static const std::vector<std::string> d = {
        "Let me look at what the user is asking for here, because there are "
        "two separate questions bundled together.",
        "\n\nFirst, the height question. The composer sits below the thread, "
        "so anything that shrinks mid-stream drags it up under the cursor.",
        "\n\nShort note.",
        "\n\nSecond, the continuity question: reasoning, then tools, then "
        "prose should read as one flow rather than three widgets taking "
        "turns to appear.",
        "\n\nOk.",
        "\n\nSo the plan is to make the window row-exact and keep every "
        "other block append-only, then pin it with a test that drives the "
        "whole turn rather than one widget.",
        "\n\nThe window is the interesting half. A node window cannot hold a "
        "height, because a paragraph wraps to however many rows it wraps to "
        "and the count changes as the newest node changes.",
        "\n\nRight.",
        "\n\nA row window can: cut the body at a row boundary, splitting a "
        "paragraph's wrapped lines if that is where the boundary falls, and "
        "the height becomes exactly the cap.",
        "\n\nOne more.",
        "\n\nThen the tool cards and the prose need the same promise, which "
        "they already have through their own row bounds, so the remaining "
        "work is a test that watches the whole turn at once.",
        "\n\nDone thinking.",
    };
    return d;
}

}  // namespace

static void sweep_one_turn(A::ui_prefs::Thinking thinking, const std::string& label) {
    maya::testing::freeze_anim_clock(0);

    A::Model m;
    m.d.current.id = A::ThreadId{"monotonic"};
    m.d.persisted.ui.thinking = thinking;   // view() publishes this
    D::clear_frozen(m);

    // A settled user turn above the stream, so the live content has a
    // committed prefix the way it does in production.
    {
        A::Message u;
        u.role = A::Role::User;
        u.text = "Walk me through how the turn is rendered while it streams.";
        m.d.current.messages.push_back(std::move(u));
    }

    A::Message a;
    a.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = A::phase::Streaming{A::phase::Active{}};

    Watch w{m};
    w.frame("open");

    // ── 1. Reasoning ────────────────────────────────────────────────────
    for (std::size_t i = 0; i < reasoning_deltas().size(); ++i) {
        m = apply(std::move(m), A::StreamThinkingDelta{reasoning_deltas()[i]});
        w.tick("reasoning" + std::to_string(i));
        // Animation frames with no new bytes: the reveal moves the live edge
        // on its own clock, and that must not change the height either.
        for (int f = 0; f < 3; ++f) w.tick("reasoning" + std::to_string(i) + "-anim");
    }

    // ── 2. A tool call, with output arriving in fragments ───────────────
    m = apply(std::move(m), A::StreamToolUseStart{A::ToolCallId{"call-1"},
                                                  A::ToolName{"shell"}});
    w.tick("tool-start");
    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-1"}, R"({"command":"cmake --build build -j12"})"});
    w.tick("tool-args");
    m = apply(std::move(m), A::StreamToolUseEnd{A::ToolCallId{"call-1"}});
    w.tick("tool-end");

    // Running, as the executor would leave it, so the progress snapshots
    // below go through the real reducer.
    {
        auto& calls = m.d.current.messages.back().tool_calls;
        REQUIRE(!calls.empty());
        const auto now = std::chrono::steady_clock::now();
        calls.back().status = A::ToolUse::Running{now, {}, {}, now, kExecSeq};
    }
    w.tick("tool-running");

    // Output the way a build actually arrives: many lines, alternating long
    // (wraps at this width) and short. A LINE-counted window over text that
    // WRAPS dips exactly like the node window did, so the window has to
    // slide for the check to mean anything. The wire sends a SNAPSHOT each
    // time, not a delta.
    std::vector<std::string> out;
    for (int i = 1; i <= 24; ++i) {
        if (i % 3 == 0) {
            out.push_back("ok\n");
        } else if (i % 3 == 1) {
            out.push_back("[" + std::to_string(i) +
                          "/24] Building CXX object CMakeFiles/agentty.dir/src/"
                          "runtime/view/thread/turn/agent_timeline/tool_args.cpp.o\n");
        } else {
            out.push_back("[" + std::to_string(i) + "/24] Building main.cpp.o\n");
        }
    }
    std::string snapshot;
    for (std::size_t i = 0; i < out.size(); ++i) {
        snapshot += out[i];
        m = apply_tool(std::move(m), A::ToolExecProgress{
            A::ToolCallId{"call-1"}, snapshot, kExecSeq});
        w.tick("tool-output" + std::to_string(i));
        w.tick("tool-output" + std::to_string(i) + "-anim");
    }

    // The tool FINISHES. The card stops rendering live progress and starts
    // rendering its result, and those are two different renderers with two
    // different row policies — the handover must not lose rows.
    {
        A::ToolExecOutput done{A::ToolCallId{"call-1"}, snapshot};
        done.exec_seq = kExecSeq;
        m = apply_tool(std::move(m), std::move(done));
    }
    for (int f = 0; f < 4; ++f) w.tick("tool-done");

    // ── 3. The answer ──────────────────────────────────────────────────
    const char* prose[] = {
        "The build is green.",
        " The turn renders top to bottom: the reasoning block first, then a "
        "card per tool call, then the answer prose.",
        "\n\nEach one is append-only while it streams.",
        "\n\nShort.",
        "\n\n- the reasoning window is cut by rows\n- the tool cards carry a "
        "row bound\n- the markdown keeps its shape across every prefix",
    };
    for (std::size_t i = 0; i < std::size(prose); ++i) {
        m = apply(std::move(m), A::StreamTextDelta{prose[i]});
        w.tick("prose" + std::to_string(i));
        for (int f = 0; f < 3; ++f) w.tick("prose" + std::to_string(i) + "-anim");
    }

    // ── 4. Settle ──────────────────────────────────────────────────────
    {
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        D::settle_message_md(m, b);
    }
    m.s.phase = A::phase::Idle{};
    for (int f = 0; f < 10; ++f) w.tick("settle");

    maya::testing::unfreeze_anim_clock();

    INFO(label, ": height drops: ", w.drops, ", worst ", w.worst,
         " rows at step '", w.worst_step, "'");
    CHECK(w.drops == 0);

    // Not vacuous: the turn really did render, and all three lanes are in
    // the frame. A view that drew nothing would sail through the check above.
    const std::string frame = view_text(m);
    INFO(label, " final frame:\n", frame);
    CHECK(w.prev > 20);
    CHECK(frame.find("Short.") != std::string::npos);          // answer prose
    CHECK(frame.find("row bound") != std::string::npos);       // answer list
    CHECK(frame.find("cmake --build") != std::string::npos);   // the tool card
}

TEST_CASE("turn: a live turn's height never shrinks, from reasoning to prose") {
    // Thinking::Collapsed is the default: the reasoning block is a row
    // window that grows to its cap and then holds.
    sweep_one_turn(A::ui_prefs::Thinking::Collapsed, "collapsed");
}

TEST_CASE("turn: the same holds with reasoning shown in full") {
    // Thinking::Shown has no window at all, so the block keeps growing --
    // the other way to be monotonic, and the one that stresses the markdown
    // underneath instead of the window.
    sweep_one_turn(A::ui_prefs::Thinking::Shown, "shown");
}
