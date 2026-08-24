// provider.cpp — Kimi Code provider (thin wrapper over the openai transport).
//
// See provider.hpp. Kimi speaks the OpenAI-Chat wire at a fixed base URL, so
// each turn is:
//   1. fresh_token()  → a valid OAuth access token (refreshed if expired).
//   2. build the Endpoint (host api.kimi.com, path /coding/v1/chat/completions).
//   3. lower provider::Request → openai::Request, stamp the token as auth.
//   4. delegate to openai::run_stream_sync.
// On a 401 (token revoked early) we invalidate + refresh once and retry.

#include "agentty/provider/kimi/provider.hpp"

#include <mutex>
#include <utility>
#include <vector>

#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/openai/transport.hpp"

namespace agentty::provider::kimi {

namespace {
// Kimi Code inference API (packages/oauth/src/managed-usage.ts, e22479a6):
//   DEFAULT_KIMI_CODE_BASE_URL = https://api.kimi.com/coding/v1
constexpr const char* kApiHost   = "api.kimi.com";
constexpr const char* kBasePath  = "/coding/v1";

std::mutex& models_mu() { static std::mutex m; return m; }
std::vector<ModelInfo>& models_cache() { static std::vector<ModelInfo> c; return c; }
} // namespace

provider::openai::Endpoint KimiProvider::make_endpoint() {
    provider::openai::Endpoint ep;
    ep.host        = kApiHost;
    ep.port        = 443;
    ep.use_tls     = true;
    ep.path        = std::string{kBasePath} + "/chat/completions";
    ep.models_path = std::string{kBasePath} + "/models";
    ep.label       = "kimi";
    return ep;
}

provider::StreamResult KimiProvider::stream(provider::Request req,
                                            provider::EventSink sink) {
    auto tok = fresh_token();
    if (!tok) {
        sink(StreamError{"Kimi is not signed in (or the token could not be "
                         "refreshed) — run `agentty login` and choose Kimi."});
        return provider::StreamResult::failed("kimi: not authenticated");
    }

    auto run = [&](const KimiToken& t) -> provider::StreamResult {
        provider::openai::Request oreq;
        provider::lower_shared(oreq, req);
        oreq.context_window = req.context_window;
        oreq.session_key    = req.session_key;
        oreq.endpoint       = make_endpoint();
        oreq.auth           = auth::BearerHeader{t.access_token};
        return provider::openai::run_stream_sync(std::move(oreq), sink, req.cancel);
    };

    auto result = run(*tok);
    // Token revoked mid-life → refresh + retry once.
    if (!result.ok() && (result.http_status == 401 || result.http_status == 403)) {
        invalidate_cached_token();
        if (auto fresh = fresh_token()) result = run(*fresh);
    }
    return result;
}

// ── Model listing ────────────────────────────────────────────────────────────
static std::vector<ModelInfo> bundled_models() {
    // Conservative fallback when offline / not signed in. The live catalog
    // supersedes this the moment the account can be reached.
    auto mk = [](const char* id) {
        return ModelInfo{ .id = ModelId{id}, .display_name = id, .provider = "kimi" };
    };
    return { mk("kimi-k2-turbo-preview"), mk("kimi-k2-0905-preview"),
             mk("kimi-k2-0711-preview") };
}

std::vector<ModelInfo> list_models() {
    {
        std::lock_guard<std::mutex> lk(models_mu());
        if (!models_cache().empty()) return models_cache();
    }
    auto tok = fresh_token();
    if (!tok) return bundled_models();

    auto ep = KimiProvider::make_endpoint();
    auto models = provider::openai::list_models(auth::BearerHeader{tok->access_token}, ep);
    if (models.empty()) return bundled_models();
    for (auto& m : models) m.provider = "kimi";

    std::lock_guard<std::mutex> lk(models_mu());
    models_cache() = models;
    return models;
}

std::string default_model() {
    auto ms = list_models();
    return ms.empty() ? std::string{"kimi-k2-turbo-preview"} : ms.front().id.value;
}

void invalidate_model_cache() {
    std::lock_guard<std::mutex> lk(models_mu());
    models_cache().clear();
}

} // namespace agentty::provider::kimi
