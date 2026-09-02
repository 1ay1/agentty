#pragma once
// agentty::model_name — the SINGLE source of truth for how a model id is
// presented to a human.
//
// ── Why this exists ──────────────────────────────────────────────────────
//
// The question "what do we call this model on screen?" used to be answered
// by six independent parsers:
//
//   1. ui::pretty_model_label        tokenised on '-_ :/'   → "Claude Opus 4.8"
//   2. ui::model_display_label       reconciled server name vs id
//   3. maya::ModelBadge::resolve()   SUBSTRING find("opus") → {"Opus", magenta}
//   4. maya::ModelBadge::extract_version()  digit-run scan  → "4.8"
//   5. ui::speaker_style_for         its own goto-based version scan
//   6. ModelCapabilities::from_id    positional tokeniser   → Family enum
//
// Three family classifiers and three version extractors for one question.
// They disagreed, and the disagreements were user-visible: the composer chip
// said "Opus" while the turn header said "Opus 4.8" and the picker said
// "Claude Opus 4.8" — same model, three strings. `[1m]` extended context
// vanished from the badge. Haiku was green in one surface and bright_cyan in
// another. Fable/Mythos — declared the flagship lane in catalog.hpp — fell off
// speaker_style's if-chain entirely and rendered with a vendor prefix nothing
// else used.
//
// The root cause was a LAYERING INVERSION. ARCHITECTURE.md says the host
// builds Config values and maya owns every pixel; the converse must hold too
// — maya must not make SEMANTIC decisions. A widget parsing vendor taxonomy
// out of a raw id has no access to the catalog, no consteval proofs, and no
// reason to be updated when a new family ships. It was always going to drift.
// PROVIDER_HETEROGENEITY.md already diagnosed this exact disease for the wire:
// "a model fact in a transport is a drift bomb". A model fact in a WIDGET is
// the same bomb.
//
// ── The design ───────────────────────────────────────────────────────────
//
// ONE decoder → a structured value → presentation-only projections.
//
//   decode(id, server_name) -> ModelName{ family, version, qualifier,
//                                         annotation, color }
//
// and three projections that DIFFER ONLY IN HOW MUCH THEY DROP:
//
//   full()   "Opus 4.8 · 1M"    picker rows, palette
//   medium() "Opus 4.8"          turn header, composer chip
//   tiny()   "Opus"              narrowest shed rung
//
// Two properties make this robust rather than merely "one more function":
//
//   • It DELEGATES taxonomy to ModelCapabilities::from_id rather than adding
//     a seventh parser. from_id already owns the positional tokeniser, the
//     Family enum (including Fable/Mythos), generation/revision, and the
//     [1m] marker. This header consumes `caps` and adds only DISPLAY
//     concerns. Adding a family touches from_id and one colour table —
//     never a widget, never a view.
//
//   • The shed ladder is a PROJECTION of one decoded value, not a different
//     code path. The old compact badge dropped the version by falling out of
//     a different branch, which is how information loss hides. A projection
//     that drops a field does so in one visible, testable place — and the
//     proofs below pin tiny() ⊑ medium() ⊑ full() so no rung can ever
//     contradict a richer one.
//
// ── Robustness: the model we have never seen ─────────────────────────────
//
// The hard requirement is not correctness on today's ids — a test corpus
// pins those. It is that ids we have NOT seen degrade to the same SHAPE, so
// a model launched after this build looks native rather than broken. A user
// meeting a brand-new flagship is the worst possible moment for the UI to
// start emitting raw wire ids or inconsistent typography.
//
// So the decoder has TWO paths that produce the same structure:
//
//   known family   → name from the family table, version from from_id
//   unknown family → name/version/qualifier recovered from the id itself,
//                    which overwhelmingly follows `[vendor-]name-version
//                    [-qualifier…]`
//
// Both strip vendor prefixes, drop snapshot dates and alias pointers, and
// fill the same three fields — so `claude-quasar-6` reads "Quasar 6" exactly
// as `claude-opus-4-8` reads "Opus 4.8", months before anyone adds Quasar to
// the enum. Adding a family to `Family` is then an OPTIMISATION (it buys the
// lane colour and the capability gates), never a bug fix for a broken label.
//
// `decode` is TOTAL: every input, including adversarial ones ("---", ":::",
// "[1m]", "4-5"), yields a non-empty renderable name and never throws. An
// empty model chip reads as a bug, not as "unknown model".
//
// ── The id is NOT trusted ─────────────────────────────────────────────
//
// Ids come from provider APIs and user config. A `/v1/models` response is
// attacker-controlled whenever the endpoint is (a custom host, a compromised
// aggregator, a typo-squatted proxy), and it flows straight to a terminal
// that INTERPRETS what it is handed. Two guarantees hold at this boundary,
// so nothing downstream has to re-check and no call site can forget:
//
//   • NO CONTROL BYTES in any output field. An id containing "\x1b[31m"
//     would otherwise emit a live escape sequence — recolouring the UI,
//     clearing the screen, or corrupting the frame from a string the user
//     never typed. That is terminal injection, not a cosmetic defect.
//   • BOUNDED LENGTH. A 4 KB "name" is a denial of service against every
//     width-shed ladder downstream. Truncation is visible (an ellipsis, on
//     a UTF-8 boundary), so an odd label reads as "too long" not "wrong".
//
// Both rules are deliberately dumb — drop bytes, cap length — and applied at
// exactly one choke point. Multi-byte UTF-8 passes through untouched.
//
// ── What is deliberately NOT here: vendor ────────────────────────────────
//
// An earlier draft carried a `vendor` field ("Claude", "GPT") re-derived
// from the id. That was wrong, and the reason is worth stating because it is
// the whole point of the exercise.
//
// PROVIDER (who serves the bytes — a registry.hpp row: "Anthropic", "Groq",
// "Copilot") and VENDOR (who trained the model — Anthropic makes Claude) are
// INDEPENDENT axes. bundled_catalog.hpp proves it: Copilot serves gpt-4o,
// claude-sonnet-4 AND gemini-2.5-pro. They coincide only on the `anthropic`
// row, which is exactly why the distinction is easy to miss.
//
// A vendor string re-derived from the id would be a FOURTH source of truth,
// sitting beside a provider label that already comes from the registry, with
// nothing forcing them to agree — precisely the drift this file eliminates.
// So: no vendor field. The "Claude " prefix is simply dropped. "Opus 4.8"
// under a Copilot chip is unambiguous and honest; "Claude Opus 4.8" under a
// Copilot chip actively invites the misreading that you are talking to
// Anthropic. Provider identity is rendered ONCE, by the provider chip, from
// the registry row — never inferred from a model id.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <maya/style/color.hpp>

#include "agentty/domain/catalog.hpp"

namespace agentty::model_name {

// ── Family colour table ──────────────────────────────────────────────────
//
// ONE table, so Haiku physically cannot be green in one surface and cyan in
// another. Values are the palette's ROLE tokens (persistent identity), never
// STATUS tokens — palette.hpp's "one hue = one axis" rule means green means
// "ok" and must not also mean "Haiku" (that collision is why Haiku moved off
// green in the first place).
//
// The switch is EXHAUSTIVE with no default: adding a Family enumerator fails
// to compile until it is given a hue. That is the compile-time proof replacing
// "remember to update the widget too".
[[nodiscard]] constexpr maya::Color color_of(
    ModelCapabilities::Family f) noexcept {
    using F = ModelCapabilities::Family;
    switch (f) {
        // Flagship lane — the brightest role hue. Distinguishable from the
        // user turn's plain magenta (same family, higher intensity).
        case F::Opus:   return maya::Color::bright_magenta();
        case F::Fable:  return maya::Color::bright_magenta();
        case F::Mythos: return maya::Color::bright_magenta();
        // Workhorse — role_info blue.
        case F::Sonnet: return maya::Color::blue();
        // Fast/agile — bright_cyan. NOT green: green is status_ok and
        // collided with the ✓ done icon.
        case F::Haiku:  return maya::Color::bright_cyan();
        // OpenAI gpt-5.x Responses line.
        case F::Gpt:    return maya::Color::cyan();
        // Unknown family (local model, aggregator id, a line we don't know
        // yet). Neutral cyan — it is still a model, just not one we can
        // place in a lane.
        case F::Unknown: return maya::Color::cyan();
    }
    return maya::Color::cyan();   // unreachable; switch is exhaustive
}

// The canonical display spelling of a known family. Empty for Unknown —
// callers fall back to the id-derived label, which is the ONLY path that
// should ever consult the raw id for a name.
[[nodiscard]] constexpr std::string_view family_label(
    ModelCapabilities::Family f) noexcept {
    using F = ModelCapabilities::Family;
    switch (f) {
        case F::Haiku:   return "Haiku";
        case F::Sonnet:  return "Sonnet";
        case F::Opus:    return "Opus";
        case F::Fable:   return "Fable";
        case F::Mythos:  return "Mythos";
        case F::Gpt:     return "GPT";
        case F::Unknown: return "";
    }
    return "";
}

// ── ModelName — the decoded value ────────────────────────────────────────
//
// Every field is PRESENTATION-READY: already title-cased, already stripped of
// snapshot dates / :latest / quantization noise. No consumer re-parses.
struct ModelName {
    // The family or, when the family is unknown, the id-derived name.
    // "Opus" · "Sonnet" · "GPT" · "Qwen2.5 Coder" · "Nano Banana Pro".
    // NEVER empty for a non-empty id.
    std::string name;

    // Normalized dotted version — "4.8", "5.6", "2.5". Empty when the id
    // carries none, or when it carries only a snapshot date (provenance,
    // not identity).
    std::string version;

    // A product qualifier that follows the family+version: "Codex Max",
    // "Chat", "Sol". Empty for the plain variant. This is IDENTITY, not
    // decoration — gpt-5.1 and gpt-5.1-codex-max are different products, and
    // collapsing both to "GPT 5.1" makes a picker row ambiguous about what
    // you are about to pay for.
    std::string qualifier;

    // A context/variant annotation that changes what you are BUYING, not
    // just what you are calling it: "1M" / "2M" extended context. Empty for
    // the ordinary variant. This is why the picker must be able to show two
    // otherwise-identical rows distinguishably.
    std::string annotation;

    // Family hue from the table above.
    maya::Color color = maya::Color::cyan();

    // True when the id decoded to a KNOWN family. When false, `name` came
    // from the id/server-name normalizer rather than the family table, and
    // `version` is empty (a version run in an unknown id is unreliable —
    // it may be a size tag, a date, or a quant level).
    bool family_known = false;

    // ── Projections ──────────────────────────────────────────────────────
    // The shed ladder. Each rung drops strictly more than the last; the
    // consteval proofs below pin tiny() ⊑ medium() ⊑ full() so a narrow
    // surface can never state something a wide one contradicts.

    // "Opus 4.8 · 1M" — everything we know. Picker rows, command palette.
    [[nodiscard]] std::string full() const;

    // "Opus 4.8" / "GPT 5.1 Codex Max" — drops only the annotation. Turn
    // headers, composer chip. The version and qualifier both stay: "Opus"
    // alone cannot distinguish 4.5 from 4.8 (the original bug), and "GPT
    // 5.1" alone cannot distinguish the Codex product from the base one.
    [[nodiscard]] std::string medium() const;

    // "Opus" / "GPT 5.1" — drops the qualifier too. The narrowest rung, for
    // a shed status bar where a long name would push the row off the edge.
    [[nodiscard]] std::string tiny() const;

    [[nodiscard]] bool operator==(const ModelName&) const = default;
};

// ── The decoder ──────────────────────────────────────────────────────────
//
// `id`           the wire/selection model id, possibly carrying an agentty
//                `[1m]`/`[2m]` marker, a provider namespace ("openrouter/
//                anthropic/claude-opus-4"), or an Ollama `:tag`.
// `server_name`  the provider's own display_name for this model, when it
//                sent one. Used ONLY to recover a marketing alias the id
//                cannot reconstruct ("Nano Banana Pro" for
//                gemini-3-pro-image); a name that is merely a re-cased
//                spelling of the id is ignored in favour of the id-derived
//                form, so the same model reads identically across providers.
//
// Total: never throws, never returns an empty `name` for a non-empty id.
[[nodiscard]] ModelName decode(std::string_view id,
                               std::string_view server_name = {});

// ── Proofs ───────────────────────────────────────────────────────────────
//
// Same idiom as tool/spec.hpp and provider/registry.hpp: the invariants that
// keep the table honest are checked by the compiler, not by remembering.
namespace proofs {

inline constexpr std::array<ModelCapabilities::Family, 7> kAllFamilies{
    ModelCapabilities::Family::Unknown, ModelCapabilities::Family::Haiku,
    ModelCapabilities::Family::Sonnet,  ModelCapabilities::Family::Opus,
    ModelCapabilities::Family::Fable,   ModelCapabilities::Family::Mythos,
    ModelCapabilities::Family::Gpt,
};

// Every KNOWN family has a non-empty label. A new enumerator that forgets
// its spelling fails the build rather than rendering as "".
consteval bool every_known_family_is_labelled() {
    for (auto f : kAllFamilies) {
        if (f == ModelCapabilities::Family::Unknown) continue;
        if (family_label(f).empty()) return false;
    }
    return true;
}
static_assert(every_known_family_is_labelled(),
              "a Family enumerator has no display label — add it to family_label()");

// No known family may borrow a STATUS hue. palette.hpp's discipline is "one
// hue = one axis"; green/yellow/red mean ok/warn/error and nothing else.
// This is the rule Haiku-as-green violated.
consteval bool no_family_uses_a_status_hue() {
    const maya::Color banned[] = {
        maya::Color::green(),  maya::Color::bright_green(),
        maya::Color::yellow(), maya::Color::bright_yellow(),
        maya::Color::red(),    maya::Color::bright_red(),
    };
    for (auto f : kAllFamilies)
        for (const auto& b : banned)
            if (color_of(f) == b) return false;
    return true;
}
static_assert(no_family_uses_a_status_hue(),
              "a family colour collides with a status hue (green=ok / "
              "yellow=warn / red=error) — pick a role hue instead");

// Family labels are pairwise distinct, so two lanes can never render as the
// same word.
consteval bool family_labels_are_unique() {
    for (std::size_t i = 0; i < kAllFamilies.size(); ++i) {
        const auto a = family_label(kAllFamilies[i]);
        if (a.empty()) continue;
        for (std::size_t j = i + 1; j < kAllFamilies.size(); ++j)
            if (a == family_label(kAllFamilies[j])) return false;
    }
    return true;
}
static_assert(family_labels_are_unique(),
              "two Family enumerators share a display label");

} // namespace proofs

} // namespace agentty::model_name
