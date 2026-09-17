#include "agentty/runtime/view/status_bar/model_badge.hpp"

#include <maya/widget/model_badge.hpp>

#include "agentty/domain/model_name.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/view/palette.hpp"   // fg_dim / muted palette

namespace agentty::ui {

maya::Element model_badge_config(const Model& m) {
    using namespace maya;
    using namespace maya::dsl;

    // While a Smart Mode turn is in flight the badge names the model ACTUALLY
    // serving it (m.s.smart_turn_model), not the picker selection: under
    // orchestration those differ, and showing the selection meant the chip
    // claimed "Mistral" while every token came from the Strategic model. The
    // selection reappears the moment the turn settles.
    const std::string& model = !m.s.smart_turn_model.empty()
                                   ? m.s.smart_turn_model.value
                                   : m.d.model_id.value;

    // ONE decode, from the domain SSOT (domain/model_name.hpp). Every surface
    // that names this model — this chip, the turn header, the picker rows —
    // reads the same decoded value, so they cannot disagree about the family,
    // the version, or the colour. The widget below is presentation-only.
    const auto name = model_name::decode(model);

    // `medium()` — "Opus 4.8". The version is deliberately kept: the old
    // compact badge dropped it (it returned before appending), so this chip
    // could not distinguish Opus 4.5 from 4.8. The `· 1M` annotation is the
    // one thing shed here, because the composer footer is width-tight and the
    // picker (which shows `full()`) is where you choose a context variant.
    maya::ModelBadge mb{{
        .label    = name.medium(),
        .version  = {},          // already folded into medium()
        .color    = name.color,
        // No dot: the filled provider tab to our left already anchors the
        // chip, and a status dot in front of it would be a third competing
        // marker in a ~15-column span.
        .show_dot = false,
    }};

    // Provider prefix: a filled tab chip, "[ Anthropic ] Opus 4.8".
    //
    // PROVIDER (who serves the bytes) and VENDOR (who trained the model) are
    // independent — Copilot serves Claude, GPT and Gemini alike. So provider
    // identity is rendered exactly once, here, from the registry row; it is
    // never inferred from a model id, and the model name carries no vendor
    // prefix of its own (see model_name.hpp's "what is deliberately NOT
    // here" note). "Opus 4.8" under a Copilot chip is honest; "Claude Opus
    // 4.8" under a Copilot chip invites the misreading that you are talking
    // to Anthropic.
    //
    // It gets a BACKGROUND rather than a coloured foreground because it is a
    // label for the thing beside it, not another peer in a dim ·-separated
    // run. A filled block reads as "this is the container" at a glance, which
    // is exactly the provider→model relationship, and it needs no separator
    // glyph: the fill's edge IS the boundary.
    //
    // The INK stays the theme's normal TEXT colour — the same colour as
    // prose — and the BAND is the family hue, tinted until that text reads
    // on it (ui::chip_style).
    //
    // That is the right way round for a label. Ink that changes per chip is
    // a second thing for the eye to resolve; ink that stays the prose
    // colour makes the chip read as text on a tint, which is what a badge
    // is. It also means nothing has to guess: the theme already pairs text
    // with its own background, so the only job is moving the hue far enough
    // from the text to clear a contrast margin, keeping it recognisably
    // itself.
    //
    // inverse_text was the obvious slot and the wrong one — under native it
    // is Default, the same colour as ordinary text, so the chip painted
    // normal foreground on a bright band (agentty #45).
    const std::string prov = provider::provider_display_name(provider::active());
    const Style prov_style = ui::chip_style(name.color).with_bold();

    // Reasoning-effort chip: when a tier is active AND the model can reason,
    // ride a compact "· ◇high" so the current effort is visible at a glance
    // without opening the picker — the same tier you set there (←/→). Uses
    // resolved_caps so it never shows on a model that can't take effort (or
    // where a stale pick would be dropped at send time). An empty Element
    // when absent, so it composes without a presence flag.
    Element effort_chip = text("");
    if (m.d.effort != Effort::None && !model.empty()) {
        const auto caps = resolved_caps(model);
        if (effort_capable(caps))
            effort_chip = h(text(" \xc2\xb7 ", fg_dim(muted)),
                            text("\xe2\x97\x87" +
                                 std::string{effort_label(m.d.effort)},
                                 fg_dim(muted))).build();
    }

    if (model.empty() || name.name.empty()) {
        // No model yet (e.g. an ACP agent that picks its own): show just the
        // provider chip so the slot is never blank. Same band as the normal
        // path — the chip should not change colour just because the model
        // is still unknown.
        return text(" " + prov + " ", prov_style);
    }

    // Update chip: when a newer release is known (background check), a
    // compact "⬆ vX.Y.Z" rides beside the model badge — bright enough to
    // notice, quiet enough to ignore. The palette's "Update agentty" (and
    // `agentty update`) are the actions; this chip is only the signal.
    Element update_chip = text("");
    if (!m.s.update_latest.empty())
        update_chip = text("  \xe2\xac\x86 v" + m.s.update_latest,
                           fg_of(ui::status_ok));

    // One UNFILLED space between the chip and the model. The chip's own
    // trailing space is background-filled, so it reads as part of the chip
    // (its right padding), not as separation — dropping this separator left
    // the two words visually touching. The gap has to be outside the fill to
    // be a gap.
    return h(text(" " + prov + " ", prov_style),
             text(" "),
             mb.build(),
             std::move(effort_chip),
             std::move(update_chip)).build();
}

} // namespace agentty::ui
