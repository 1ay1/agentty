// agentty::provider — registry predicates whose answer depends on a
// PROVIDER'S OWN knowledge, so they can't live in the header.
//
// wire_streams_reasoning_text() is the one that matters: it decides whether
// the UI may promise "✦ shown" for reasoning. Getting it wrong is a silent
// lie — the turn works, the user just never sees thinking they were told to
// expect — so the answer is derived from MEASURED wire behaviour rather than
// from the provider's name.

#include "agentty/provider/registry.hpp"
#include "agentty/provider/copilot/provider.hpp"

#include <string>

namespace agentty::provider {

bool wire_streams_reasoning_text(std::string_view provider_id,
                                 std::string_view model) noexcept {
    const ProviderPreset* p = preset_for(provider_id);
    if (!p) return true;                    // unknown/custom host: assume yes
    if (p->wire != Wire::OpenAIChat) return true;

    // ── Copilot: genuinely MIXED, so the model decides ────────────────────
    // The registry row says OpenAIChat because that is the DEFAULT dialect,
    // but Copilot also exposes the Responses API, and agentty now streams
    // over it whenever the Auto session blesses the model. Measured on a
    // live session: gpt-5-mini and mai-code-* return real reasoning summary
    // text there; claude-* and gpt-4.x are chat-only and return none.
    if (provider_id == "copilot") {
        if (model.empty()) return true;     // "some model here can" — true
        return copilot::prefers_responses_dialect(std::string{model});
    }

    // ── First-party OpenAI on the chat dialect: still no reasoning text ───
    // agentty dials api.openai.com/v1/chat/completions, which does not
    // transmit GPT-5 reasoning. (This row was once mislabelled
    // Wire::OpenAIResponses, which made the old dialect-only predicate
    // answer "yes" by accident.) The genuinely-Responses OpenAI path is the
    // `chatgpt` provider, which has its own transport and its own Wire.
    if (provider_id == "openai") return false;

    // Every other OpenAI-COMPAT host on this dialect (DeepSeek, Mistral,
    // OpenRouter, vLLM, Ollama) populates reasoning_content, which the
    // transport parses unconditionally.
    return true;
}

} // namespace agentty::provider
