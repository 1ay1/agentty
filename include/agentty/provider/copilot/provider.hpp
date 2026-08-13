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

} // namespace agentty::provider::copilot
