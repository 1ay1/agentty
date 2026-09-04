#pragma once
// agentty::provider — WHICH WIRE DIALECT does (provider, model) speak?
//
// ─────────────────────────────────────────────────────────────────────────
// Why this file exists
// ─────────────────────────────────────────────────────────────────────────
// A provider row carries a DEFAULT dialect (`ProviderDescriptor::wire`), but
// a host is not always one dialect. The same OpenAI API key reaches both
// /v1/chat/completions and /v1/responses; the same Copilot session serves
// claude-* on chat and mai-code-* on Responses only. So "which dialect" is a
// property of the (provider, model) PAIR, not of the row alone.
//
// That question was previously answered in three places that disagreed:
//
//   1. the row's `wire` field                     (static, and it lied)
//   2. copilot::prefers_responses_dialect()       (per-provider, hardcoded)
//   3. wire_streams_reasoning_text()              (UI: "can we show ^R?")
//
// (2) and (3) are the SAME question asked by different callers — one to pick
// a URL, one to decide whether the thinking pane may promise output. When
// they drift the UI promises reasoning the wire never sends, which is
// exactly the bug the mislabelled `openai` row shipped. This header makes
// them one function so the drift is unrepresentable.
//
// ─────────────────────────────────────────────────────────────────────────
// Why the answer MATTERS (it is not a style preference)
// ─────────────────────────────────────────────────────────────────────────
// Per OpenAI's own reasoning guide, on Chat Completions:
//
//   • from GPT-5.4 on, tool calling is UNSUPPORTED with any reasoning_effort
//     other than `none`;
//   • GPT-6-class models drop chat function-calling entirely.
//
// An agent is a tool-calling reasoner by definition, so for those models the
// chat dialect is not "degraded", it is a 400. Responses additionally keeps
// reasoning state across tool rounds (`encrypted_content`, which our codec
// already replays), which is what stops a long tool chain from re-deriving
// its plan on every hop.
//
// ─────────────────────────────────────────────────────────────────────────
// Design constraints this obeys
// ─────────────────────────────────────────────────────────────────────────
// • NO user-visible surface. The user picks a model, not a protocol. There
//   is no picker row, no flag, no modal. The only thing they observe is that
//   thinking shows up and tool chains stay coherent.
// • Model-name matching ROTS. Every table here is a heuristic about a moving
//   target, so a wrong guess must be SURVIVABLE, never fatal: the runtime
//   records an observed failure (`note_dialect_rejected`) and the next turn
//   routes the other way. The table is the prior; the wire is the truth.
// • Answers are pure + cheap: callers hit this per frame (the ^R affordance
//   asks it while the picker is open).

#include <cstdint>
#include <string>
#include <string_view>

namespace agentty::provider {

// The dialect a concrete turn should be sent on.
enum class Dialect : std::uint8_t {
    Chat,        // OpenAI /chat/completions (or a compat host's equivalent).
    Responses,   // OpenAI /responses.
    Native,      // not an OpenAI-family dialect (Anthropic, Ollama, ACP).
};

[[nodiscard]] constexpr std::string_view dialect_name(Dialect d) noexcept {
    switch (d) {
        case Dialect::Chat:      return "chat";
        case Dialect::Responses: return "responses";
        case Dialect::Native:    return "native";
    }
    return "";
}

// ── THE predicate ────────────────────────────────────────────────────────
//
// Which dialect should `model` on `provider_id` be streamed over? Consults,
// in order: the row's advertised capability (no responses_path → Chat), the
// per-turn runtime evidence below (a host that already rejected a dialect is
// never retried on it), then the model-family prior.
//
// `model` may be empty, meaning "does this provider EVER prefer Responses?"
// — the provider-level question the picker asks before a model is chosen.
[[nodiscard]] Dialect dialect_for(std::string_view provider_id,
                                  std::string_view model) noexcept;

// True when (provider, model) can actually deliver reasoning TEXT to the UI
// on the dialect it will be dialled over. The ^R affordance and the thinking
// pane both gate on this, so it MUST be the same authority that picks the
// URL — hence its home here rather than beside the registry table.
[[nodiscard]] bool streams_reasoning_text(std::string_view provider_id,
                                          std::string_view model) noexcept;

// ── Runtime correction (the anti-rot mechanism) ──────────────────────────
//
// The family tables above are a PRIOR, and every prior about model names
// goes stale the week a new family ships. These two calls let the wire
// correct the table without a release:
//
//   • a host answering 404/400/`unsupported_api_for_model` on /responses
//     demotes that (provider, model) to Chat for the rest of the process;
//   • a host rejecting chat tool-calls for a reasoning model promotes it.
//
// Recorded per (provider, model) so one bad model never disables the dialect
// for its neighbours. Process-scoped by design: it is an observation about
// today's deployment, not a preference worth persisting into a config file
// the user would then have to understand and un-set.
void note_dialect_rejected(std::string_view provider_id,
                           std::string_view model, Dialect rejected) noexcept;

// Test seam: drop all learned rejections so cases start from the prior.
void reset_dialect_observations() noexcept;

// ── Model-family priors (exposed for tests + the Copilot shim) ───────────
//
// True when the model is a REASONING model whose tool calling is degraded or
// outright rejected on Chat Completions — the population that must be on
// Responses. Kept as one function with the measured/documented rationale
// beside each family so the table has a single place to be updated.
[[nodiscard]] bool model_requires_responses(std::string_view model) noexcept;

// True when the model returns reasoning SUMMARY text on Responses. A model
// can require Responses without emitting visible summaries, so this is a
// strictly narrower question than the one above.
[[nodiscard]] bool model_emits_reasoning_summaries(std::string_view model) noexcept;

} // namespace agentty::provider
