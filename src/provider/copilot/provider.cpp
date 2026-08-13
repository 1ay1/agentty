// provider.cpp — GitHub Copilot provider (thin wrapper over the openai transport).
//
// See provider.hpp. Copilot speaks the OpenAI-Chat wire, so each turn is:
//   1. fresh_token()  → a valid proxy token + the per-account inference host.
//   2. build the Endpoint (host from endpoints.api + the editor headers).
//   3. lower provider::Request → openai::Request, stamp the token as auth.
//   4. delegate to openai::run_stream_sync.
// On a 401 (proxy token revoked early) we invalidate + refresh once and retry.

#include "agentty/provider/copilot/provider.hpp"

#include <mutex>

#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/openai/transport.hpp"

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::provider::copilot {
namespace {

// Split "https://host[:port]" → (host, port, tls). Copilot always uses https.
struct HostPort { std::string host; std::uint16_t port = 443; bool tls = true; };
HostPort parse_api_base(const std::string& base) {
    HostPort hp;
    std::string_view s = base;
    if (s.rfind("https://", 0) == 0) { s.remove_prefix(8); hp.tls = true; }
    else if (s.rfind("http://", 0) == 0) { s.remove_prefix(7); hp.tls = false; hp.port = 80; }
    // strip any trailing path
    if (auto slash = s.find('/'); slash != std::string_view::npos) s = s.substr(0, slash);
    if (auto colon = s.find(':'); colon != std::string_view::npos) {
        hp.host = std::string{s.substr(0, colon)};
        try { hp.port = static_cast<std::uint16_t>(std::stoi(std::string{s.substr(colon + 1)})); }
        catch (...) {}
    } else {
        hp.host = std::string{s};
    }
    if (hp.host.empty()) hp.host = "api.githubcopilot.com";
    return hp;
}

// The editor-identification header block Copilot requires on every request.
std::vector<std::pair<std::string, std::string>> copilot_headers() {
    return {
        {"copilot-integration-id", "vscode-chat"},
        {"editor-version", "vscode/1.104.3"},
        {"editor-plugin-version", "copilot-chat/0.26.7"},
        {"openai-intent", "conversation-panel"},
        {"user-agent", "GitHubCopilotChat/0.26.7"},
    };
}

std::vector<ModelInfo>& models_cache() { static std::vector<ModelInfo> c; return c; }
std::mutex& models_mu() { static std::mutex m; return m; }

} // namespace

provider::openai::Endpoint CopilotProvider::make_endpoint(const std::string& api_base) {
    auto hp = parse_api_base(api_base);
    provider::openai::Endpoint ep;
    ep.host          = hp.host;
    ep.port          = hp.port;
    ep.use_tls       = hp.tls;
    ep.path          = "/chat/completions";
    ep.models_path   = "/models";
    ep.label         = "copilot";
    ep.extra_headers = copilot_headers();
    return ep;
}

provider::StreamResult CopilotProvider::stream(provider::Request req,
                                               provider::EventSink sink) {
    auto tok = fresh_token();
    if (!tok) {
        sink(StreamError{"GitHub Copilot is not signed in (or the token could "
                         "not be refreshed) — run `agentty login` and choose "
                         "GitHub Copilot."});
        return provider::StreamResult::failed("copilot: not authenticated");
    }
    if (!tok->chat_enabled) {
        sink(StreamError{"This GitHub account has no Copilot Chat entitlement."});
        return provider::StreamResult::failed("copilot: chat not enabled");
    }
    if (tok->quota_exhausted) {
        sink(StreamError{"GitHub Copilot chat quota is exhausted for this "
                         "account (free tier). Try again later or upgrade your "
                         "Copilot plan."});
        return provider::StreamResult::failed("copilot: quota exhausted");
    }

    auto run = [&](const CopilotToken& t) -> provider::StreamResult {
        provider::openai::Request oreq;
        provider::lower_shared(oreq, req);
        oreq.context_window = req.context_window;
        oreq.session_key    = req.session_key;
        oreq.endpoint       = make_endpoint(t.endpoint_api);
        oreq.auth           = auth::BearerHeader{t.token};
        return provider::openai::run_stream_sync(std::move(oreq), sink, req.cancel);
    };

    auto result = run(*tok);
    // One forced-refresh retry if the proxy token was rejected mid-life (401/403).
    if (!result.ok() && (result.http_status == 401 || result.http_status == 403)) {
        invalidate_cached_token();
        if (auto fresh = fresh_token())
            result = run(*fresh);
    }
    return result;
}

// ── Model listing ────────────────────────────────────────────────────────────
static std::vector<ModelInfo> bundled_models() {
    // Conservative fallback when offline / not signed in. The live catalog
    // (list_models) supersedes this the moment the account can be reached.
    auto mk = [](const char* id) {
        return ModelInfo{ .id = ModelId{id}, .display_name = id, .provider = "copilot" };
    };
    return { mk("gpt-4o"), mk("gpt-4.1"), mk("o4-mini"),
             mk("claude-sonnet-4"), mk("gemini-2.5-pro") };
}

std::vector<ModelInfo> list_models() {
    {
        std::lock_guard<std::mutex> lk(models_mu());
        if (!models_cache().empty()) return models_cache();
    }
    auto tok = fresh_token();
    if (!tok || !tok->chat_enabled) return bundled_models();

    provider::openai::Endpoint ep = CopilotProvider::make_endpoint(tok->endpoint_api);
    auto live = provider::openai::list_models(auth::BearerHeader{tok->token}, ep);
    if (live.empty()) return bundled_models();

    std::lock_guard<std::mutex> lk(models_mu());
    models_cache() = live;
    return live;
}

std::string default_model() {
    auto ms = list_models();
    return ms.empty() ? std::string{"gpt-4o"} : ms.front().id.value;
}

} // namespace agentty::provider::copilot
