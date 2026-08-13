// provider.cpp — GitHub Copilot provider (thin wrapper over the openai transport).
//
// See provider.hpp. Copilot speaks the OpenAI-Chat wire, so each turn is:
//   1. fresh_token()  → a valid proxy token + the per-account inference host.
//   2. build the Endpoint (host from endpoints.api + the editor headers).
//   3. lower provider::Request → openai::Request, stamp the token as auth.
//   4. delegate to openai::run_stream_sync.
// On a 401 (proxy token revoked early) we invalidate + refresh once and retry.

#include "agentty/provider/copilot/provider.hpp"

#include <algorithm>
#include <mutex>

#include <nlohmann/json.hpp>

#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/io/http.hpp"

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

    // Fetch /models directly so we can read Copilot's rich per-model metadata
    // (policy.state, capabilities, model_picker_category) that the generic
    // OpenAI parser discards. This is what lets us surface the models the
    // account can ACTUALLY use on top.
    std::string host = tok->endpoint_api;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    if (auto slash = host.find('/'); slash != std::string::npos) host = host.substr(0, slash);

    http::Request req;
    req.method  = http::HttpMethod::Get;
    req.host    = host;
    req.port    = 443;
    req.path    = "/models";
    req.headers = {
        {"authorization", "Bearer " + tok->token},
        {"copilot-integration-id", "vscode-chat"},
        {"editor-version", "vscode/1.104.3"},
        {"accept", "application/json"},
    };
    req.max_body_bytes = 4ull * 1024 * 1024;

    auto resp = http::default_client().send(req);
    if (!resp || resp->status < 200 || resp->status >= 300) return bundled_models();

    struct Row { ModelInfo info; int rank = 0; };
    std::vector<Row> rows;
    try {
        auto j = nlohmann::json::parse(resp->body);
        const auto& data = j.contains("data") ? j["data"] : j;
        // De-dup by family: Copilot lists many internal aliases (…-picker,
        // …-secondary, exec-agent-*, copilot-search-*). Keep the canonical id
        // per family, and only chat-type models.
        std::vector<std::string> seen_family;
        for (const auto& m : data) {
            std::string id = m.value("id", "");
            if (id.empty()) continue;
            const auto& caps = m.value("capabilities", nlohmann::json::object());
            if (caps.value("type", "") != "chat") continue;   // skip embeddings/search
            // Skip Copilot's internal routing aliases — not user-facing models.
            if (id.find("-picker") != std::string::npos
                || id.find("-secondary") != std::string::npos
                || id.find("-tertiary") != std::string::npos
                || id.rfind("exec-agent", 0) == 0
                || id.rfind("copilot-search", 0) == 0
                || id.rfind("trajectory-", 0) == 0
                || id.rfind("oswe-", 0) == 0) continue;
            // Skip pinned dated snapshots (gpt-4o-2024-11-20, gpt-4-0613, …):
            // the canonical id (gpt-4o, gpt-4) already appears and is what a
            // user wants. A trailing -YYYY-MM-DD or -NNNN date is the tell.
            {
                auto is_date_tail = [&](std::size_t pos) {
                    // pos points at a '-'; check the rest is all digits/'-'.
                    if (pos == std::string::npos) return false;
                    bool has_digit = false;
                    for (std::size_t k = pos + 1; k < id.size(); ++k) {
                        if (id[k] == '-') continue;
                        if (id[k] < '0' || id[k] > '9') return false;
                        has_digit = true;
                    }
                    return has_digit;
                };
                auto dash = id.find("-20");                 // -2024… / -2025…
                if (dash != std::string::npos && is_date_tail(dash)) continue;
                // -0613 / -0125 style 4-digit month-year snapshots.
                if (id.size() >= 5) {
                    auto tail = id.rfind('-');
                    if (tail != std::string::npos && id.size() - tail == 5
                        && is_date_tail(tail)) continue;
                }
            }

            // ENTITLEMENT: policy.state "enabled" or a model with no policy
            // block is usable on this plan; "disabled" needs opt-in at
            // github.com. Usable models sort first and get the ★.
            std::string policy_state = "none";
            if (m.contains("policy") && m["policy"].is_object())
                policy_state = m["policy"].value("state", "none");
            const bool usable = (policy_state == "enabled" || policy_state == "none");

            ModelInfo info;
            info.id           = ModelId{id};
            info.display_name = m.value("name", id);
            info.provider     = "copilot";
            info.favorite     = usable;
            if (caps.contains("limits"))
                info.context_window =
                    caps["limits"].value("max_context_window_tokens", 200000);
            if (caps.contains("supports"))
                info.supports_tools = caps["supports"].value("tool_calls", true);

            // Rank: usable first, then a rough category weight (powerful >
            // versatile > lightweight), then name for stability.
            const std::string cat = m.value("model_picker_category", "");
            int cat_w = cat == "powerful" ? 0 : cat == "versatile" ? 1
                      : cat == "lightweight" ? 2 : 3;
            int rank = (usable ? 0 : 100) + cat_w;
            rows.push_back({std::move(info), rank});
        }
    } catch (...) { return bundled_models(); }

    if (rows.empty()) return bundled_models();
    std::stable_sort(rows.begin(), rows.end(),
        [](const Row& a, const Row& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.info.display_name < b.info.display_name;
        });

    std::vector<ModelInfo> out;
    out.reserve(rows.size());
    for (auto& r : rows) out.push_back(std::move(r.info));

    std::lock_guard<std::mutex> lk(models_mu());
    models_cache() = out;
    return out;
}

std::string default_model() {
    auto ms = list_models();
    return ms.empty() ? std::string{"gpt-4o"} : ms.front().id.value;
}

} // namespace agentty::provider::copilot
