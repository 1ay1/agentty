#pragma once
// agentty::provider::copilot::CopilotProvider — native GitHub Copilot provider.
//
// Copilot's chat API is OpenAI-Chat-compatible, so this is a THIN wrapper over
// the shared openai transport: on each turn it ensures a fresh Copilot proxy
// token (auto-exchanged/refreshed from the persisted GitHub token), builds the
// per-account inference Endpoint (host from the token's `endpoints.api`, plus
// the mandatory editor-identification headers), stamps the token as the request
// auth, and delegates streaming to openai::OpenAIProvider. A 401 from the proxy
// (token revoked early) triggers one forced refresh + retry.
//
// It is a LongLived provider (owns the token cache) — constructed once in
// main() like the Anthropic / ChatGPT providers.

#include <string>
#include <vector>

#include "agentty/domain/catalog.hpp"       // ModelInfo
#include "agentty/provider/provider.hpp"
#include "agentty/provider/openai/transport.hpp"  // openai::Endpoint
#include "agentty/provider/stream_epilogue.hpp"

namespace agentty::provider::copilot {

class CopilotProvider {
public:
    CopilotProvider() = default;

    provider::StreamResult stream(provider::Request req, provider::EventSink sink);

    // Builds the Copilot inference Endpoint for the given host (from the token
    // exchange). Shared by stream() and list_models().
    static provider::openai::Endpoint make_endpoint(const std::string& api_base);
};

static_assert(provider::Provider<CopilotProvider>);

// The live model catalog for the signed-in Copilot account (GET
// {endpoints.api}/models). The set depends on the account's entitlements
// (gpt-4o, o-series, Claude, Gemini …), so it MUST be listed, not hardcoded.
// Falls back to a small bundled list when offline / not signed in.
[[nodiscard]] std::vector<ModelInfo> list_models();

// The account's default model slug (first catalog entry) or a safe fallback.
[[nodiscard]] std::string default_model();

// Drop the cached model catalog so the next list_models() re-ranks with any
// freshly learned per-model support (after a turn's 400/200 outcome).
void invalidate_model_cache();

// ── Dialect selection (exposed for tests) ──────────────────────────
//
// Copilot is agentty's only MIXED-dialect provider: the same account, over
// the same host, serves some models on /chat/completions and others on the
// Responses API. Which one a model speaks was MEASURED against a live Auto
// session, not guessed:
//
//   gpt-5-mini          both — but ONLY /responses returns reasoning text
//   mai-code-1.1-flash  /responses only (chat 400s unsupported_api_for_model)
//   claude-*, gpt-4.1   /chat/completions only (400 on /responses)
//
// True when the model should be streamed over the Responses dialect,
// GIVEN that the Auto session lists it (the session's copilot-session-token
// is a hard requirement for /responses — without it even gpt-5-mini is
// rejected with model_not_supported).
[[nodiscard]] bool prefers_responses_dialect(const std::string& model);

// True when the model is known to REJECT /chat/completions, so Responses is
// the only way to reach it at all (the mai-code-* family today). Such models
// used to be filtered out of the picker entirely.
[[nodiscard]] bool chat_dialect_unsupported(const std::string& model);

} // namespace agentty::provider::copilot
