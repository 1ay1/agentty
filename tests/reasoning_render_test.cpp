// reasoning_render_test.cpp — the reasoning ("Thinking") block renders across
// providers and lifecycle states.
//
// Reasoning text from EVERY provider funnels into Message::thinking via
// StreamThinkingDelta (Anthropic thinking_delta, Codex reasoning_summary_text
// .delta, OpenAI-compat reasoning_content), and Message::reasoning_display_text
// () is the single unified accessor the view keys off. This pins:
//   1. the unified accessor's precedence (thinking, else reasoning_summary),
//   2. that a SETTLED reasoning turn renders the permanent one-line summary
//      ("Thought for ~N tokens · <glimpse>") ABOVE the answer,
//   3. that a turn with NO reasoning renders neither.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include <maya/widget/app_layout.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <print>
#include <string>
#include <vector>

using namespace agentty;

namespace {

// Render the whole app (transcript included) to plain ASCII text. Mirrors
// midrun_seam_test's render_rows harness; non-ASCII glyphs (the ✦/· sigils)
// map to '?', so assertions target the ASCII words.
std::string render_text(const Model& m, int width = 100, int height = 4000) {
    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        out.push_back('\n');
    }
    return out;
}

bool has(const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

Message assistant(std::string text) {
    Message a;
    a.role = Role::Assistant;
    a.id   = MessageId{"a1"};
    a.text = std::move(text);
    return a;
}

} // namespace

TEST_CASE("reasoning: unified accessor precedence") {
    Message a = assistant("answer");
    check(a.reasoning_display_text().empty(), "no reasoning by default");
    check(!a.has_reasoning(), "has_reasoning false when empty");

    a.reasoning_summary = "legacy summary";
    check(a.reasoning_display_text() == "legacy summary",
          "falls back to reasoning_summary");

    a.thinking = "primary thinking";
    check(a.reasoning_display_text() == "primary thinking",
          "thinking (the unified stream field) wins over reasoning_summary");
    check(a.has_reasoning(), "has_reasoning true when thinking present");
}

TEST_CASE("reasoning: settled turn shows the permanent summary line") {
    Model m;
    m.d.show_reasoning = true;   // the global ^R switch is on
    Message a = assistant("Here is the FINAL_ANSWER_MARKER.");
    // Reasoning arrived (any provider) and the answer is present => SETTLED:
    // render the one-line summary, not the full stream.
    a.thinking = "Analyze the request FIRST_LINE_MARKER\n"
                 "then a SECOND_LINE_MARKER the glimpse must not show.";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};

    const std::string out = render_text(m);
    check(has(out, "Thought"),
          "settled reasoning renders a 'Thought' summary line");
    check(has(out, "token"),
          "the summary names an approximate token count");
    check(has(out, "FINAL_ANSWER_MARKER"),
          "the answer still renders alongside the reasoning summary");
    // The summary is a one-line GLIMPSE of the first reasoning line only.
    check(!has(out, "SECOND_LINE_MARKER"),
          "settled summary is a one-line glimpse, not the full thought text");
}

TEST_CASE("reasoning: no block when the turn never reasoned") {
    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(assistant("PLAIN_ANSWER no reasoning here."));
    m.s.phase = phase::Idle{};
    const std::string out = render_text(m);
    check(!has(out, "Thought"),
          "no reasoning summary when the turn produced no reasoning");
    check(has(out, "PLAIN_ANSWER"), "the plain answer still renders");
}

TEST_CASE("reasoning: the ^R switch hides the block even when text exists") {
    Model m;
    m.d.show_reasoning = false;   // global switch OFF (the default)
    Message a = assistant("Here is the ANSWER_STILL_SHOWN.");
    a.thinking = "lots of hidden reasoning the user chose not to see";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};
    const std::string out = render_text(m);
    check(!has(out, "Thought"),
          "reasoning block is suppressed when show_reasoning is off");
    check(!has(out, "hidden reasoning"),
          "reasoning text never reaches the screen when the switch is off");
    check(has(out, "ANSWER_STILL_SHOWN"),
          "the answer renders normally regardless of the reasoning switch");
}
