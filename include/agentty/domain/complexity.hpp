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
// Heuristics (in priority order):
//   • very short acknowledgements / imperatives with no question → Trivial
//   • presence of design/architecture/debug/why vocabulary, OR a large
//     message, OR many enumerated asks → Complex
//   • a short single-clause request → Simple
//   • everything else → Standard (the conservative default)
[[nodiscard]] Complexity classify_complexity(std::string_view text) noexcept;

} // namespace agentty::smart
