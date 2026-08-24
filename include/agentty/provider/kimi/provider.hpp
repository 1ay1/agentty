#pragma once
// agentty::provider::kimi::KimiProvider — native Kimi Code provider.
//
// Kimi Code's inference API is OpenAI-Chat-compatible, served at a fixed base
// URL (https://api.kimi.com/coding/v1), so this is a THIN wrapper over the
// shared openai transport: on each turn it ensures a fresh OAuth access token
// (auto-refreshed via refresh_token from the persisted device-flow bundle),
// builds the Endpoint, stamps the token as the request auth, and delegates
// streaming to openai::run_stream_sync. A 401 (token revoked early) triggers
// one forced refresh + retry.
//
// It is a LongLived provider (owns the token cache) — constructed once in
// main() like the Anthropic / ChatGPT / Copilot providers.

#include <string>
#include <vector>

#include "agentty/domain/catalog.hpp"       // ModelInfo
#include "agentty/provider/provider.hpp"
#include "agentty/provider/openai/transport.hpp"  // openai::Endpoint
#include "agentty/provider/stream_epilogue.hpp"

namespace agentty::provider::kimi {

class KimiProvider {
public:
    KimiProvider() = default;

    provider::StreamResult stream(provider::Request req, provider::EventSink sink);

    // Builds the Kimi inference Endpoint. Shared by stream() and list_models().
    static provider::openai::Endpoint make_endpoint();
};

// The account's live model catalog from Kimi's /models (falls back to a small
// bundled list when offline / not signed in).
[[nodiscard]] std::vector<ModelInfo> list_models();

// The account's default model slug (first catalog entry) or a safe fallback.
[[nodiscard]] std::string default_model();

// Drop the cached model catalog so the next list_models() re-fetches.
void invalidate_model_cache();

} // namespace agentty::provider::kimi
