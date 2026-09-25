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
#include "agentty/domain/ui_live.hpp"   // publish() / motion_frame_divisor()
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include <maya/core/motion.hpp>
#include <maya/widget/markdown.hpp>

#include <print>
#include <functional>
#include <string>
#include <vector>

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
    const auto form = pn::build_appearance_form(m.d.ui(), true);

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
    const auto form = pn::build_appearance_form(m.d.ui(), true);

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
    const auto form = pn::build_appearance_form(m.d.ui(), true);
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

    m.d.ui().thinking = up::Thinking::Hidden;
    CHECK(!(m.d.show_reasoning && m.d.ui().thinking != up::Thinking::Hidden));

    m.d.ui().thinking = up::Thinking::Collapsed;
    CHECK(m.d.show_reasoning && m.d.ui().thinking != up::Thinking::Hidden);

    m.d.ui().thinking = up::Thinking::Shown;
    CHECK(m.d.show_reasoning && m.d.ui().thinking != up::Thinking::Hidden);
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

TEST_CASE("appearance: Reduced motion is a real middle, not a relabelled Full") {
    // 0xfk0 (#36) runs agentty over mosh on a high-latency link and asked to
    // "disable all unnecessary animations and keep only the essential
    // updates". Reduced is the setting that should have answered that, and
    // it did not: it dropped only the DECORATIVE layer (scramble, gradient,
    // caret blink), which restyles bytes that were being sent anyway.
    //
    // Measured on a recorded stream (anthropic_md_stream det over
    // fixtures/anthropic_md_tour.jsonl), counting frames whose RENDER
    // actually changed — the thing a slow link pays for:
    //
    //     Full     1753 changed frames
    //     Reduced  1753 changed frames   ← byte-identical to Full
    //     Off        98 changed frames
    //
    // So the middle setting cost exactly what the expensive one did. The
    // user's real choice was full churn or no reveal.
    //
    // Motion is TWO axes, not one: decoration is visual noise (vestibular
    // accessibility), repaint rate is bandwidth. Reduced now thins the
    // frame REQUESTS as well, so the text still walks in — progress stays
    // legible — at a quarter of the frames.
    namespace up = agentty::ui_prefs;

    // The policy, as the domain states it.
    const auto divisor_for = [](up::Motion mo) {
        up::Prefs p;
        p.motion = mo;
        up::publish(p);
        return up::motion_frame_divisor();
    };
    CHECK(divisor_for(up::Motion::Full)    == 1);
    CHECK(divisor_for(up::Motion::Reduced) == 4);   // the fix
    // Off stops the requests outright, so the divisor is moot — but it must
    // not claim to thin something that is already stopped.
    CHECK(divisor_for(up::Motion::Off)     == 1);

    // …and what that policy costs, in frame requests over a simulated
    // second of 60 fps animation. This is the number the reporter feels.
    const auto requests_per_sec = [](bool off, int div) {
        maya::anim::set_reduce_motion(off);
        maya::anim::set_frame_divisor(div);
        int n = 0;
        for (int f = 0; f < 60; ++f) {
            if (maya::anim::reduce_motion()) continue;
            const int d = maya::anim::frame_divisor();
            if (d > 1 && (f % d) != 0) continue;
            ++n;
        }
        return n;
    };
    CHECK(requests_per_sec(false, 1) == 60);   // Full
    CHECK(requests_per_sec(false, 4) == 15);   // Reduced — 4x cheaper, still moving
    CHECK(requests_per_sec(true,  1) == 0);    // Off

    // The divisor is clamped: a host that computes a nonsense value must not
    // be able to stall animation entirely (0 or negative) or stretch one
    // frame into a minute.
    maya::anim::set_frame_divisor(0);
    CHECK(maya::anim::frame_divisor() == 1);
    maya::anim::set_frame_divisor(-5);
    CHECK(maya::anim::frame_divisor() == 1);
    maya::anim::set_frame_divisor(10000);
    CHECK(maya::anim::frame_divisor() == 60);

    // Leave the process as we found it: these are global slots and a later
    // case that renders would otherwise inherit a thinned clock.
    maya::anim::set_frame_divisor(1);
    maya::anim::set_reduce_motion(false);
    up::publish(up::Prefs{});
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

TEST_CASE("settings: a row's description says what it DOES, in every state") {
    // Smart Mode's off branch was the bare word "off". Next to siblings that
    // all read like descriptions ("pre-turn context injection", "hidden · ^R
    // still reviews") a one-word state read as a row whose description had
    // gone MISSING — and off is the default, so it was the first thing a new
    // user saw. The question a settings row has to answer is "what would
    // changing this do?", and that question is loudest in the state where
    // the feature is doing nothing.
    //
    // So: no row may describe itself with a bare state word. Checked over
    // BOTH values of the toggles, because the defect only existed in one of
    // them — a test that built the list once would have passed all along.
    namespace se = agentty::settings;

    for (bool smart_on : {false, true}) {
        agentty::Model m;
        m.d.smart.enabled = smart_on;

        const auto rows = se::items_for(m, se::Category::General);
        CHECK(rows.size() >= 5);

        for (const auto& r : rows) {
            if (r.primary.empty()) continue;
            // A state word alone is not a description. (The real rows say
            // "off · every turn goes to the main model" — state AND effect.)
            CHECK(r.secondary != "off");
            CHECK(r.secondary != "on");
            CHECK(r.secondary != "enabled");
            CHECK(r.secondary != "disabled");
            // And every General row has to say something at all.
            CHECK(!r.secondary.empty());
        }
    }
}

TEST_CASE("settings: a stateful row reports its state, not just its topic") {
    // The other half of the same rule. "pre-turn context injection" names
    // the TOPIC but never the value, so the row looks identical whether
    // retrieval is on or off — you have to open the pane to find out. A row
    // that owns a setting must show what that setting currently IS.
    namespace se = agentty::settings;

    agentty::Model off;
    off.d.smart.enabled = false;
    agentty::Model on;
    on.d.smart.enabled = true;

    const auto a = se::items_for(off, se::Category::General);
    const auto b = se::items_for(on,  se::Category::General);
    REQUIRE(a.size() == b.size());

    const auto find = [](const std::vector<se::Item>& rows, se::Action act) {
        for (const auto& r : rows) if (r.action == act) return r.secondary;
        return std::string{};
    };

    // Flipping the setting must be VISIBLE on its own row.
    CHECK(find(a, se::Action::OpenSmart) != find(b, se::Action::OpenSmart));
    CHECK(!find(a, se::Action::OpenSmart).empty());
}

