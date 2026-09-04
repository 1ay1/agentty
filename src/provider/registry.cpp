// agentty::provider — registry helpers that need a .cpp.
//
// wire_streams_reasoning_text() is the one that matters: it decides whether
// the UI may offer the ^R thinking affordance for a (provider, model) pair.
//
// It used to answer that question ITSELF, from the row's `wire` field plus a
// per-provider special case for Copilot. That was a SECOND guess at a
// question the transport also answers when it picks a URL, and the two
// drifted — the `openai` row claimed Wire::OpenAIResponses while dialling
// /v1/chat/completions, so the dialect-only predicate said "yes, reasoning"
// for a wire that never sends any.
//
// The question now has exactly one authority: provider::dialect_for() in
// dialect.cpp, which both this predicate and the transports consult. This
// function is kept as the public spelling (callers and tests use this name)
// but is now a thin forward, so the UI and the wire cannot disagree.

#include "agentty/provider/registry.hpp"

#include "agentty/provider/dialect.hpp"

namespace agentty::provider {

bool wire_streams_reasoning_text(std::string_view provider_id,
                                 std::string_view model) noexcept {
    return streams_reasoning_text(provider_id, model);
}

} // namespace agentty::provider
