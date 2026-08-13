#pragma once
// agentty::smart — turn-complexity classification for Smart-Mode effort scaling.
//
// The research-backed lever (Anthropic, "Building a multi-agent research
// system": *scale effort to query complexity*; RouteLLM / cascade survey:
// upfront intent/complexity routing, *route conservatively for ambiguous
// queries*). A tiny local classifier hits ~90% accuracy for well-defined
// tiers at ~0 latency — no model call, no network. We classify the user's
// message into four tiers and let the orchestrator scale the Strategic
// model's reasoning effort accordingly (and drive the delegation budget the
// system prompt advertises).
//
// Conservative by design: DEFAULT is Standard, and a strong complexity signal
// escalates while only a clearly-trivial message de-escalates. Over-routing to
// "think harder" is far cheaper than under-thinking a hard turn, so ties break
// upward.

#include <cstdint>
#include <string>
#include <string_view>

namespace agentty::smart {

enum class Complexity : std::uint8_t {
    Trivial,   // "yes", "thanks", "run it", "commit" — 1 agent, no thinking
    Simple,    // a pointed lookup / one-liner fix
    Standard,  // the default working turn (the conservative fallback)
    Complex,   // architecture / design / debug / multi-file / "why"
};

[[nodiscard]] constexpr std::string_view to_string(Complexity c) noexcept {
    switch (c) {
        case Complexity::Trivial:  return "trivial";
        case Complexity::Simple:   return "simple";
        case Complexity::Standard: return "standard";
        case Complexity::Complex:  return "complex";
    }
    return "standard";
}

// Classify a user turn's text. Pure, allocation-light, case-insensitive.
//
// This is a small ADDITIVE FEATURE SCORE, not a keyword lookup: three
// orthogonal, mostly language-agnostic signal families each contribute weight,
// and the sum is thresholded into a tier. That fixes the old all-or-nothing
// behaviour (one stray "design" forcing Complex) and generalises past a fixed
// English lexicon:
//   • STRUCTURAL (language-agnostic): enumerated asks, conjunction/clause
//     density, code-token density, question shape, glyph length. A request's
//     complexity lives mostly in its STRUCTURE, not its verbs.
//   • LEXICAL (multilingual): weighted "hard"/"trivial" keyword sets across the
//     major languages — evidence that ADDS weight, never a hard override.
//   • MORPHOLOGICAL: token-shape variety (prose vs. identifiers vs. paths).
// Conservative: ties break upward (under-thinking a hard turn costs more than
// over-thinking a cheap one).
[[nodiscard]] Complexity classify_complexity(std::string_view text) noexcept;

// The scored classification: the tier PLUS the continuous score and the margin
// to the nearest tier boundary (0 = right on a threshold, larger = more
// confident). classify_with_context uses the margin to blend a follow-up
// smoothly instead of snapping between tiers. Score units are arbitrary but
// monotonic in complexity.
struct ComplexityScore {
    Complexity tier   = Complexity::Standard;
    int        score  = 0;   // additive feature score
    int        margin = 0;   // distance to the nearest tier boundary
};
[[nodiscard]] ComplexityScore classify_score(std::string_view text) noexcept;

// Context-aware classification. classify_complexity is turn-local, so a short
// follow-up ("now do the same for the other module", "and the tests?") after a
// Complex decomposition classifies as Simple and DROPS effort a step — even
// though it inherits the prior turn's complexity. This lifts a non-Trivial
// continuation part-way back toward the previous turn's tier: the follow-up is
// still cheaper than the turn that spawned it, but not misclassified as a
// throwaway one-liner. A Trivial turn ("thanks", "commit it") is always taken
// at face value — an ack is an ack regardless of what came before. A fresh
// Complex signal in the text always wins outright.
[[nodiscard]] inline Complexity classify_with_context(
        std::string_view text, Complexity prev) noexcept {
    const Complexity self = classify_complexity(text);
    if (self == Complexity::Trivial || self == Complexity::Complex) return self;
    if (static_cast<int>(prev) <= static_cast<int>(self))           return self;
    // Inherit ONE tier below the previous turn (a continuation of hard work is
    // usually a shade easier than the original), never below the text's own
    // tier. prev is Complex ⇒ lift to Standard; prev is Standard ⇒ stays.
    const auto inherited =
        static_cast<Complexity>(static_cast<int>(prev) - 1);
    return static_cast<int>(inherited) > static_cast<int>(self) ? inherited : self;
}

} // namespace agentty::smart
