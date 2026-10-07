#include "agentty/runtime/view/status_bar/model_badge.hpp"

#include <maya/widget/model_badge.hpp>

#include <string>

#include "agentty/auth/vault.hpp"
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
    const auto& active_sel = provider::active();
    const std::string prov = provider::provider_display_name(active_sel);
    const Style prov_style = ui::chip_style(name.color).with_bold();

    // First-run honesty. If the user escaped out of the login modal (or
    // launched for the first time with no env-var credentials), the active
    // selection falls back to the Anthropic default -- but there's nothing
    // signed in behind it. Showing "Anthropic Opus 4.5" there was a lie
    // the status bar told confidently every launch: the next Enter would
    // 401 against a credential the user never provided.
    //
    // Vault::signed_in is the uniform predicate (api-key store, OAuth
    // state, custom-host settings slot, keyless locals -- the vault
    // descriptor answers for all of them with one call) and it's cached
    // on the credentials-file (mtime,size) so it's cheap per frame.
    const bool authed =
        auth::vault::signed_in(std::string{active_sel.provider_id()});
    if (!authed) {
        // Dimmed provider chip + a muted "-- sign in" where the model name
        // would be. No family hue (there IS no model), no update chip, no
        // effort chip: all three claim a running configuration we don't
        // have. The provider name stays so the user can see which store
        // the next `login` paste would route to; it's rendered with the
        // muted hue, same as every other "disabled, actionable" chip.
        const Style unauth_prov =
            ui::chip_style(muted).with_bold();
        return h(text(" " + prov + " ", unauth_prov),
                 text(" "),
                 text("\xe2\x80\x94 sign in", fg_dim(muted))).build();
    }

    // Reasoning-effort chip: when a tier is active AND the model can reason,
    // ride a compact "· ✦ xhigh" so the current effort is visible at a glance
    // without opening the picker — the same tier you set there (←/→). Uses
    // resolved_caps so it never shows on a model that can't take effort (or
    // where a stale pick would be dropped at send time). An empty Element
    // when absent, so it composes without a presence flag.
    //
    // The sigil is ✦, the same one the reasoning block wears in the thread,
    // so one glyph means "reasoning" everywhere instead of a ◇ that appears
    // nowhere else. It carries the accent and the tier stays muted: the
    // chip then reads as a footnote on the model rather than a third peer
    // in the row, and the two are no longer glued into one "◇xhigh" word.
    Element effort_chip = text("");
    if (m.d.effort != Effort::None && !model.empty()) {
        const auto caps = resolved_caps(model);
        if (effort_capable(caps))
            effort_chip = h(text("  "),
                            text("\xe2\x9c\xa6 ", fg_of(ui::role_brand)),
                            // NOT fg_dim(muted). muted is already the
                            // palette's grey (bright_black), and dimming it
                            // again lands the tier a hair off the
                            // background on a dark theme -- on Phosphor that
                            // is #506258 dimmed against #0d1611, which is
                            // the "I can't see the reasoning mode" report.
                            // The sigil keeps the accent and carries the
                            // eye; the tier only has to be READABLE once
                            // the eye arrives, so plain muted is the floor.
                            text(std::string{effort_label(m.d.effort)},
                                 fg_of(muted))).build();
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
    //
    // After an update lands the chip does NOT vanish — it becomes
    // "↺ v<new>", and stays until the process actually restarts. The
    // download writes a new binary but this process keeps running the old
    // one, so "updated" is only half true; the restart is the outstanding
    // step and it needs a marker that outlives the status line.
    Element update_chip = text("");
    if (!m.s.update_pending_restart.empty())
        update_chip = text("  \xe2\x86\xba v" + m.s.update_pending_restart,
                           fg_of(ui::status_warn));
    else if (!m.s.update_latest.empty())
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
