#pragma once
// agentty/provider/openai/conformance.hpp — what we send, and to whom.
//
// ── WHY "100% SPEC-CONFORMANT" IS THE WRONG TARGET ───────────────────────
//
// It sounds like the obviously correct goal, and adopting it would break
// agentty on most of the endpoints it talks to. Concretely:
//
//   OpenAI DEPRECATED `max_tokens` on Chat Completions in favour of
//   `max_completion_tokens`. Strict conformance to OpenAI's document means
//   sending the new name. But vLLM, llama.cpp, Together, Fireworks and most
//   of the compat family accept ONLY `max_tokens` — so conforming to the
//   spec breaks the majority of the ecosystem that the spec is named after.
//
// There is no single authority to conform TO (see lens.hpp): OpenAI's own
// openapi document and vLLM's server are the two de-facto sources and they
// disagree. So the target is not "match the document", it is:
//
//        BE CORRECT FOR THE ENDPOINT WE ARE ACTUALLY TALKING TO.
//
// That is a per-endpoint decision, and this header is where those decisions
// are written down instead of being implied by an `if (use_tls)` buried in
// the body builder.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
// A request field falls into one of three tiers. The tier says who may
// receive it, and every tier assignment below cites the evidence.
//
//   Universal   every OpenAI-shaped endpoint accepts it. Send always.
//   Hosted      the big hosted APIs take it; local servers reject or choke.
//               Gated on TLS, which is the honest proxy for "not a laptop".
//   Probed      only send when the endpoint was OBSERVED to support it.
//               A registry row claiming support is not evidence (see
//               Observed<T> in lens.hpp, and PR #53).
//
// The failure modes are not symmetric, which is what sets the default:
// sending a field an endpoint rejects is a HARD 400/422 that kills the turn;
// omitting a field costs an optimisation. So when in doubt, omit.
//
// ── AND TOLERANCE VARIES, SO IT CANNOT BE ASSUMED ────────────────────────
//
// Measured on Yolo-Auto (vllm-0.29.1 behind a billing proxy, 2026-09-23):
// it accepts `prompt_cache_key`, `reasoning_effort` AND
// `max_completion_tokens` — all HTTP 200, unknown fields silently ignored.
// vLLM is permissive by default.
//
// That is exactly why the tiers are NOT derived from "did it work somewhere".
// One tolerant endpoint proves nothing about a strict one: Mistral MAGISTRAL
// returns 422 for the same `reasoning_effort` this host shrugs at. A field is
// Universal only when it works on the STRICTEST endpoint we support, never
// because the most permissive one let it through.
//
// GitHub Copilot's runtime reaches the same conclusion from the other side:
// its model records carry `supported_endpoints` and per-model
// `supportsReasoningEffort`, and it ships a whole
// `reasoning_effort_fallback` path (configured_effort → resultingEffort with
// a primary_reason) rather than assuming a configured effort is deliverable.
// Capability is per-endpoint data, not a global constant.

#include <cstdint>
#include <string_view>

namespace agentty::provider::openai::conformance {

enum class Tier : std::uint8_t {
    Universal,  // send to everything
    Hosted,     // TLS endpoints only
    Probed,     // only with positive evidence
};

struct FieldRule {
    std::string_view field;
    Tier             tier;
    std::string_view rationale;   // the evidence, not an opinion
};

// The request-side decisions, as data. Adding a field to the body without
// adding it here is how a 422 on one provider becomes a mystery six months
// later.
inline constexpr FieldRule kRequestFields[] = {
    {"model", Tier::Universal,
     "required by every implementation"},

    {"messages", Tier::Universal,
     "the payload; universal"},

    {"stream", Tier::Universal,
     "universal. agentty always streams."},

    {"max_tokens", Tier::Universal,
     "DELIBERATELY the deprecated OpenAI spelling. OpenAI prefers "
     "max_completion_tokens, but vLLM / llama.cpp / Together accept only "
     "max_tokens, and OpenAI still accepts it. One name that works "
     "everywhere beats conformance that works on one host."},

    {"stream_options.include_usage", Tier::Universal,
     "added by OpenAI later, widely adopted. Verified present on Yolo-Auto "
     "(vllm-0.29.1, 2026-09-23). Endpoints that do not know it IGNORE it "
     "rather than erroring, so it is safe to send broadly."},

    {"tools", Tier::Universal,
     "the modern shape; `functions` is long dead. Verified round-tripping "
     "on Yolo-Auto (finish_reason=tool_calls, well-formed arguments)."},

    {"tool_choice", Tier::Universal,
     "sent as \"auto\", which is the default everywhere; harmless."},

    {"prompt_cache_key", Tier::Hosted,
     "OpenAI prompt-cache routing. Local servers reject or ignore it and "
     "their KV cache is prefix-automatic anyway, so there is nothing to "
     "gain and a 400 to lose."},

    {"reasoning_effort", Tier::Probed,
     "o-series / DeepSeek-R1 / Grok / Mistral Small-Medium take it. Mistral "
     "MAGISTRAL reasons natively and REJECTS it with 422 — which is why the "
     "catalog gates it per-model and it arrives empty here rather than "
     "being re-checked at the wire. Evidence, not a capability table."},
};

// Fields agentty deliberately does NOT send, and why. Kept because "we tried
// that and it broke X" is knowledge that otherwise lives only in a git log.
inline constexpr FieldRule kOmittedFields[] = {
    {"max_completion_tokens", Tier::Probed,
     "OpenAI's replacement for max_tokens. NOT sent: most of the compat "
     "family does not accept it, and OpenAI still honours max_tokens. "
     "Revisit only if OpenAI removes the old name."},

    {"temperature", Tier::Universal,
     "not sent. A coding agent wants the model's default sampling; pinning "
     "it fights per-model tuning, and some reasoning models REJECT a "
     "non-default temperature outright."},

    {"n", Tier::Universal,
     "not sent. One completion. n>1 multiplies cost for no agent benefit."},

    {"functions", Tier::Universal,
     "superseded by `tools` years ago. Sending it would opt into the legacy "
     "path on endpoints that still honour it."},
};

} // namespace agentty::provider::openai::conformance
