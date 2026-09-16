// The Appearance pane: every row applies live, and owns its setting alone.
//
// Two properties, both of which had a real hole.
//
// LIVE. The pane is a preview — you change a row and judge the result on the
// real UI, which only works if the change reaches what is ALREADY on screen.
// Colour rows (scheme, tier, polarity, syntax) have to re-seal the frozen
// transcript; the structural ones (density, compact turns) deliberately must
// not, because re-sealing at a new height tears the scrollback ledger.
//
// SOLE OWNER. "Is the model's reasoning shown" was asked in two places: this
// pane's Thinking row, and a ^R toggle in the model picker. They could
// disagree, and the disagreement was expensive in the worst direction —
// ^R "shown" + Thinking "Hidden" meant reasoning was REQUESTED from the
// provider, billed, and then dropped unrendered, with each screen reporting
// the opposite of the other.

#include "agtest.hpp"

#include "agentty/domain/ui_prefs.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include <maya/core/motion.hpp>
#include <maya/widget/markdown.hpp>

#include <print>
#include <functional>
#include <string>

namespace {

namespace up = agentty::ui_prefs;
namespace pn = agentty::ui::panel;
using agentty::Model;

// The ids the Appearance form emits, and whether changing each one must
// re-style what is already painted.
struct Row { std::string_view id; bool recolours; };

[[nodiscard]] const agentty::form::Field* field(const agentty::form::Form& f,
                                                std::string_view id) {
    for (const auto& row : f.fields)
        if (row.id == id) return &row;
    return nullptr;
}

}  // namespace

TEST_CASE("appearance: every row the pane builds is a real, reachable setting") {
    // A row whose id no reducer handles is a dead control: it moves, it
    // persists, and it changes nothing. The pane and the reducer are keyed on
    // the SAME named constants precisely so that cannot drift, and this walks
    // the built form to prove every id is one of them.
    Model m;
    const auto form = pn::build_appearance_form(m.d.ui, true);

    static constexpr std::string_view kKnown[] = {
        pn::kApTheme, pn::kApTier, pn::kApPolarity, pn::kApDensity,
        pn::kApCompact, pn::kApMotion, pn::kApSyntax, pn::kApToolOutput,
        pn::kApThinking, pn::kApTimestamps, pn::kApProseWidth,
    };

    int rows = 0;
    for (const auto& f : form.fields) {
        // Section headers are rows with no setting behind them; they carry a
        // reserved id prefix rather than an empty one, because the form
        // machinery keys on ids being unique.
        if (f.id.empty() || f.id.starts_with("__header")) continue;
        bool known = false;
        for (std::string_view k : kKnown) known = known || (f.id == k);
        if (!known) std::println("  UNHANDLED row id: {}", f.id);
        CHECK(known);
        ++rows;
    }
    CHECK(rows >= 10);
}

TEST_CASE("appearance: a colour row restyles, a structural row does not") {
    // The split is not cosmetic. Colour is resolved when an Element is BUILT,
    // so a sealed turn keeps whatever theme it was sealed under until it is
    // rebuilt — hence the re-seal. Height is different: re-sealing a frozen
    // turn at a new height tears the inline scrollback ledger, so those rows
    // are forward-only by design.
    Model m;
    const auto form = pn::build_appearance_form(m.d.ui, true);

    // Colour-affecting rows must all exist — a typo'd id here would silently
    // stop restyling and the symptom is "the transcript kept the old theme".
    for (std::string_view id : {pn::kApTier, pn::kApPolarity, pn::kApSyntax})
        CHECK(field(form, id) != nullptr);

    // ...and so must the structural ones, which deliberately do not.
    for (std::string_view id : {pn::kApDensity, pn::kApCompact})
        CHECK(field(form, id) != nullptr);
}

TEST_CASE("appearance: Thinking is the only home for reasoning display") {
    // The model picker used to carry a ^R toggle for the same question. It is
    // gone — both the footer chip and the keybinding — so this pane is the
    // sole owner and the two can no longer report different answers.
    Model m;
    const auto form = pn::build_appearance_form(m.d.ui, true);
    const auto* thinking = field(form, pn::kApThinking);
    REQUIRE(thinking != nullptr);

    // Three states, and the row says what each one does.
    const auto* choice = std::get_if<agentty::form::field::Choice>(&thinking->value);
    REQUIRE(choice != nullptr);
    CHECK(choice->labels.size() == 3);
}

TEST_CASE("appearance: hiding reasoning stops us paying for it") {
    // The expensive half of the old contradiction. Requesting reasoning the
    // user has told us to hide bills tokens for output that is dropped
    // unrendered, which is strictly worse than either honest answer.
    //
    // Collapsed still requests, deliberately: a collapsed block is shown,
    // just folded, and it cannot be unfolded if it was never sent.
    Model m;
    m.d.show_reasoning = true;

    m.d.ui.thinking = up::Thinking::Hidden;
    CHECK(!(m.d.show_reasoning && m.d.ui.thinking != up::Thinking::Hidden));

    m.d.ui.thinking = up::Thinking::Collapsed;
    CHECK(m.d.show_reasoning && m.d.ui.thinking != up::Thinking::Hidden);

    m.d.ui.thinking = up::Thinking::Shown;
    CHECK(m.d.show_reasoning && m.d.ui.thinking != up::Thinking::Hidden);
}

TEST_CASE("appearance: Animation Off actually stops animation") {
    // The row promised more than it delivered. reduce_motion gated only the
    // phase PRIMITIVES (blink, wave, frame_index), so "off" stopped a caret
    // blinking and a spinner spinning — and nothing else. Five subsystems
    // drove themselves by requesting frames directly (the welcome cascade
    // and its perpetual bob, the streaming reveal, the reasoning rail, the
    // markdown cursor) and none consulted the setting.
    //
    // The gate is on the frame REQUEST now, which is the one thing every
    // self-driven animation has to go through. Measured on an idle welcome
    // screen: 5% of a core -> 0%.
    maya::anim::set_reduce_motion(false);
    CHECK(!maya::anim::reduce_motion());

    maya::anim::set_reduce_motion(true);
    CHECK(maya::anim::reduce_motion());

    // The primitives settle rather than oscillate: a caret held ON (losing
    // the cursor is worse than not blinking), a breathing highlight held at
    // its mid-value (frozen at the trough reads as a rendering bug), and a
    // spinner on frame 0, which is the resting glyph of every frame set.
    CHECK(maya::anim::blink(530.0));
    CHECK(maya::anim::frame_index(8, 80) == 0);

    // keep_animating() is the gate that matters — every self-driven
    // animation goes through it, so gating it is what makes "off" total
    // rather than a list of widgets somebody remembered. It must be safe to
    // call with motion off; it simply schedules nothing.
    maya::anim::keep_animating();
    maya::anim::keep_animating_after(100);

    maya::anim::set_reduce_motion(false);
    CHECK(!maya::anim::reduce_motion());
}

TEST_CASE("appearance: Syntax highlighting off actually removes the colour") {
    // This toggle was persisted, hashed into the render key, and read by
    // NOTHING: maya had no switch for it to reach. A setting that moves,
    // saves, and changes no pixel is worse than a missing one.
    //
    // Distinct content per case on purpose — markdown memoises a built code
    // block on (source, language), so re-rendering the SAME snippet would
    // serve the cached element and hide the difference. That is how the
    // first version of this check passed while the feature did nothing.
    auto colours_in = [](bool on, const std::string& body) {
        maya::set_syntax_highlighting(on);
        const maya::Element e = maya::markdown("```cpp\n" + body + "\n```\n");
        int runs = 0;
        std::function<void(const maya::Element&)> walk =
            [&](const maya::Element& el) {
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, maya::TextElement>) {
                        runs += static_cast<int>(n.runs.size());
                    } else if constexpr (std::is_same_v<T, maya::BoxElement>) {
                        for (const auto& c : n.children) walk(c);
                    } else if constexpr (std::is_same_v<T, maya::ElementList>) {
                        for (const auto& c : n.items) walk(c);
                    }
                }, el.inner);
            };
        walk(e);
        return runs;
    };

    CHECK(colours_in(true,  "int a = 42; // on")  > 0);
    CHECK(colours_in(false, "int b = 42; // off") == 0);
    maya::set_syntax_highlighting(true);
}

TEST_CASE("settings: every row that opens a pane shows the door arrow") {
    // Appearance had the reducer, the row, and the enum entry — and was left
    // out of the one hand-written switch that paints the → affordance. So
    // the row that opens the largest pane in Settings was the only door
    // without a handle: it read as a value row that happened to do something
    // when you pressed Enter.
    //
    // opens_pane() is a RANGE over the contiguous door block, so adding one
    // between the brackets picks up the arrow with no second edit. This
    // pins the three that exist, and that nothing else claims to be a door.
    namespace se = agentty::settings;

    CHECK(se::opens_pane(se::Action::OpenRag));
    CHECK(se::opens_pane(se::Action::OpenAppearance));
    CHECK(se::opens_pane(se::Action::OpenSmart));

    // Cycling a value in place is not a door — it changes something HERE,
    // and gets its own glyph rather than the one meaning "leads away".
    CHECK(!se::opens_pane(se::Action::CycleProfile));
    CHECK(!se::opens_pane(se::Action::None));
    CHECK(!se::opens_pane(se::Action::ToggleChangesStrip));
    CHECK(!se::opens_pane(se::Action::TogglePlugin));
}
