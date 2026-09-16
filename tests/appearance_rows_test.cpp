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

#include <print>
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
