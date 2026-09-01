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
#include "agentty/domain/bundled_catalog.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::kimi {

namespace {
// Kimi Code inference API (packages/oauth/src/managed-usage.ts, e22479a6):
//   DEFAULT_KIMI_CODE_BASE_URL = https://api.kimi.com/coding/v1
constexpr const char* kApiHost   = "api.kimi.com";
constexpr const char* kBasePath  = "/coding/v1";

std::mutex& models_mu() { static std::mutex m; return m; }
std::vector<ModelInfo>& models_cache() { static std::vector<ModelInfo> c; return c; }

// Query /coding/v1/usages. Returns a human message when the account is out of
// credits (Kimi reports 429 resource_exhausted here even though /chat 500s),
// or nullopt when usage looks fine / the probe is inconclusive.
std::optional<std::string> quota_error_message(const std::string& access_token) {
    http::Request r;
    r.method = http::HttpMethod::Get;
    r.host   = kApiHost;
    r.port   = 443;
    r.path   = std::string{kBasePath} + "/usages";
    r.headers = {
        {"accept", "application/json"},
        {"authorization", "Bearer " + access_token},
    };
    for (auto& h : device_headers()) r.headers.push_back({h.first, h.second});
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        r.dial_host = ov.host; r.dial_port = ov.port;
    }
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5000);
    tos.total   = std::chrono::milliseconds(8000);
    auto resp = http::default_client().send(r, tos);
    if (!resp) return std::nullopt;
    // A 402/429 or a resource_exhausted / quota body = out of credits.
    const bool http_quota = resp->status == 402 || resp->status == 429;
    bool body_quota = false;
    try {
        auto j = nlohmann::json::parse(resp->body);
        std::string code = j.value("code", std::string{});
        std::string msg  = j.value("message", std::string{});
        if (code == "resource_exhausted" || msg.find("balance") != std::string::npos
            || msg.find("Credits") != std::string::npos
            || msg.find("quota") != std::string::npos)
            body_quota = true;
    } catch (const std::exception& e) {
        util::dbglog("kimi.quota_probe.parse", e.what());
    } catch (...) {
        util::dbglog("kimi.quota_probe.parse", "non-std exception");
    }
    if (http_quota || body_quota)
        return std::string{
            "Kimi credits exhausted \xe2\x80\x94 your Kimi Code plan is out of "
            "balance. Top up your plan at kimi.ai, or switch providers with ^P."};
    return std::nullopt;
}
} // namespace

provider::openai::Endpoint KimiProvider::make_endpoint() {
    provider::openai::Endpoint ep;
    ep.host        = kApiHost;
    ep.port        = 443;
    ep.use_tls     = true;
    ep.path        = std::string{kBasePath} + "/chat/completions";
    ep.models_path = std::string{kBasePath} + "/models";
    ep.label       = "kimi";
    // Kimi's servers expect the same X-Msh-* device-identity headers on API
    // requests (chat + models) that the OAuth flow sends — the official
    // kimi_code_cli attaches them everywhere. Without them the /models catalog
    // comes back empty and some requests are rejected. SSOT: one builder in
    // kimi_oauth::device_headers(). Applied by openai transport's
    // build_request_headers on both the chat and the models paths.
    ep.extra_headers = device_headers();
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
    // Kimi's coding API returns a bare HTTP 500 ("server had an error") when the
    // account is out of credits — the /usages endpoint reports the real reason
    // (429 resource_exhausted). On a 500/429, probe /usages and surface a clear
    // "credits exhausted" message instead of the opaque server error.
    if (!result.ok() && (result.http_status == 500 || result.http_status == 429)) {
        if (auto q = quota_error_message(tok->access_token)) {
            sink(StreamError{*q});
            return provider::StreamResult::failed("kimi: " + *q);
        }
    }
    return result;
}

// ── Model listing ────────────────────────────────────────────────────────────
static std::vector<ModelInfo> bundled_models() {
    // Single bundled catalog; the live catalog supersedes it the moment the
    // account can be reached.
    return catalog::bundled("kimi");
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
    // Kimi's /models payload carries no context length, so the generic OpenAI
    // parser leaves every row on ModelInfo's 200k default — 56k short of K2's
    // real 256k window, which makes the context gauge and the compaction
    // threshold fire early. Adopt the bundled window for any id we know.
    const auto seed = bundled_models();
    for (auto& m : models) {
        m.provider = "kimi";
        for (const auto& b : seed)
            if (b.id.value == m.id.value) { m.context_window = b.context_window; break; }
    }

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
