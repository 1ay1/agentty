// provider.cpp — GitHub Copilot provider (thin wrapper over the openai transport).
//
// See provider.hpp. Copilot speaks the OpenAI-Chat wire, so each turn is:
//   1. fresh_token()  → a valid proxy token + the per-account inference host.
//   2. build the Endpoint (host from endpoints.api + the editor headers).
//   3. lower provider::Request → openai::Request, stamp the token as auth.
//   4. delegate to openai::run_stream_sync.
// On a 401 (proxy token revoked early) we invalidate + refresh once and retry.

#include "agentty/provider/copilot/provider.hpp"
#include "agentty/domain/bundled_catalog.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/responses/responses.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

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
        catch (...) { util::dbglog("copilot.api_base.parse", base); }
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

namespace {
// The synthetic "Auto" model id agentty exposes in the picker. Selecting it
// lets GitHub's server route each turn to the best model the account may use.
constexpr const char* kAutoId = "copilot-auto";

// gpt-4o-family base models run on every plan via a DIRECT chat request — they
// must NOT be forced through the Auto session (which doesn't list them).
bool is_base_direct(const std::string& id) {
    return id == "gpt-4o" || id == "gpt-4.1" || id == "gpt-4o-mini"
        || id == "gpt-4o-copilot" || id == "gpt-4.1-mini"
        || id.rfind("gpt-4o-mini", 0) == 0;
}

// Some Auto models only speak /responses (mai-code-*), which agentty's
// OpenAI-Chat transport can't drive. Prefer a chat-completions model.
//
// NOTE this is now only used to pick a CHAT-dialect fallback. Since the
// Responses site below exists, a mai-code-* pick is no longer a dead end:
// dialect_for() routes it to /responses instead of skipping it.
bool auto_chat_compatible(const std::string& id) {
    return id.rfind("mai-code", 0) != 0;
}

// ── Which dialect does THIS model speak? ────────────────────────────
//
// Copilot is the first provider that is genuinely MIXED: the same account,
// over the same host, serves some models on /chat/completions and others on
// /responses. Measured against api.individual.githubcopilot.com with an Auto
// session (see the probe notes in the commit):
//
//   gpt-5-mini          both  — /responses returns REAL reasoning summaries
//                             (SSE response.reasoning_summary_text.delta)
//   mai-code-1.1-flash  /responses ONLY (400 unsupported_api_for_model on chat)
//   claude-*, gpt-4.1   /chat/completions ONLY (400 on /responses)
//
// Two hard rules, both measured: the `copilot-session-token` from an Auto
// session is REQUIRED (without it even gpt-5-mini 400s model_not_supported),
// and only models the session lists in available_models[] are accepted.
//
// So Responses is used when we hold an Auto session that blesses the model.
// Everything else keeps the Chat path — no guessing, no hardcoded allowlist
// beyond what the SERVER told us it can route.
enum class Dialect { Chat, Responses };

[[nodiscard]] bool session_lists_model(const AutoSession& as,
                                       const std::string& model) {
    for (const auto& m : as.available_models)
        if (m == model) return true;
    return false;
}

// Models we have OBSERVED to reject /chat/completions. Learned, not baked:
// a 400 unsupported_api_for_model on the chat path records the model here so
// the next turn goes straight to /responses.
std::mutex& responses_only_mu() { static std::mutex m; return m; }
std::set<std::string>& responses_only_set() {
    static std::set<std::string> s;
    return s;
}
void note_responses_only(const std::string& model) {
    std::scoped_lock lk(responses_only_mu());
    responses_only_set().insert(model);
}
[[nodiscard]] bool is_responses_only(const std::string& model) {
    std::scoped_lock lk(responses_only_mu());
    return responses_only_set().count(model) > 0;
}

// Does this model return reasoning TEXT on /responses?
//
// MEASURED against a live Auto session: gpt-5-* answers /responses with a
// `reasoning` output item carrying real summary text (and streams
// response.reasoning_summary_text.delta), while the same model on
// /chat/completions returns none — which is the whole reason this fork
// exists. Claude and GPT-4.x reject /responses outright, so they are never
// candidates. Kept as a narrow prefix test rather than a hardcoded model
// list so new gpt-5 spellings are picked up without a release; anything
// wrong here degrades to "stay on chat", never to a broken turn.
[[nodiscard]] bool wants_reasoning_text(const std::string& model) {
    return model.rfind("gpt-5", 0) == 0;
}

// Which concrete model will the Auto session stream?
//
// ONE implementation, used by BOTH dialect paths — the Responses fork must
// resolve the same slug the chat path would, or a user switching between
// them would silently get different models.
//
// A CONCRETE request is honoured or it FAILS. It is never quietly swapped for
// another model: a user (or a Smart Mode role slot) that pinned `gpt-5.6-luna`
// and silently got `gpt-5.3-codex` gets wrong latency, wrong cost, wrong
// capabilities — and, when the substitute is Responses-only but the chat path
// was taken, an outright 400. Substitution is only correct when the user asked
// us to choose, i.e. for the synthetic Auto entry.
//
// Returns "" when a concrete request cannot be served; the caller turns that
// into a clear error naming the model.
[[nodiscard]] std::string pick_auto_model(const AutoSession& as,
                                          const std::string& requested) {
    if (requested != kAutoId) {
        for (const auto& m : as.available_models)
            if (m == requested) return requested;
        return {};   // pinned but unavailable — say so, don't reroute
    }
    // Auto: the user delegated the choice. Prefer the server's own pick, then
    // any chat-capable model, then whatever is on offer.
    //
    // The emptiness check matters: auto_chat_compatible("") is true (an empty
    // string doesn't start with "mai-code"), so a session that returned no
    // selected_model would otherwise resolve to the EMPTY model slug and
    // stream a request with no model at all.
    if (!as.selected_model.empty() && auto_chat_compatible(as.selected_model))
        return as.selected_model;
    for (const auto& m : as.available_models)
        if (auto_chat_compatible(m)) return m;
    return as.available_models.empty() ? std::string{}
                                       : as.available_models.front();
}

// ── The Copilot Responses site ──────────────────────────────────
//
// Copilot's /responses needs credentials that are resolved ONCE per turn
// (proxy token + Auto session, both refreshable). Site::authorize is a plain
// function pointer by design — the descriptor stays data — so the resolved
// turn context is handed over in this thread-local, set by stream() right
// before it calls into the shared codec. Per-thread because a turn runs on
// one worker and subagents stream concurrently on their own.
struct ResponsesTurn {
    std::string host;          // inference host for this session
    std::string token;         // proxy token (Bearer)
    std::string session_token; // copilot-session-token — REQUIRED, measured
    std::string model;         // server-blessed slug
};
ResponsesTurn& responses_turn() {
    static thread_local ResponsesTurn t;
    return t;
}

std::expected<provider::responses::Target, std::string>
copilot_authorize(provider::Request&) {
    const auto& t = responses_turn();
    if (t.token.empty() || t.session_token.empty())
        return std::unexpected(std::string{
            "copilot: no Auto session for the Responses API — sign in again "
            "with `agentty login` and choose GitHub Copilot"});
    provider::responses::Target target;
    target.host = t.host.empty() ? std::string{"api.githubcopilot.com"} : t.host;
    target.port = 443;
    target.path = "/responses";
    target.model = t.model;
    target.headers = {
        {"authorization", "Bearer " + t.token},
        {"content-type", "application/json"},
        {"accept", "text/event-stream"},
        // The Auto-session token is what unlocks /responses at all: without
        // it every model — including gpt-5-mini — 400s model_not_supported.
        {"copilot-session-token", t.session_token},
        {"x-github-api-version", kAutoApiVersion},
    };
    for (auto& h : copilot_headers()) target.headers.push_back({h.first, h.second});
    return target;
}

void copilot_decorate_body(nlohmann::json& body, const provider::Request&) {
    // Copilot is stateful server-side for Responses; unlike the Codex
    // backend we do NOT send store:false or ask for encrypted reasoning
    // (it rejects the include). `summary: auto` — already set by the shared
    // builder — is what makes the reasoning text actually come back.
    body.erase("store");
    body.erase("include");
}

std::string copilot_explain_http_error(int status, std::string_view body) {
    std::string detail;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("error")) {
            const auto& e = j["error"];
            if (e.is_string()) detail = e.get<std::string>();
            else if (e.is_object())
                detail = e.value("message", e.value("code", std::string{}));
        }
    } catch (const std::exception& e) {
        util::dbglog("copilot.http_error.decode", e.what());
    } catch (...) {}
    std::string msg = "copilot returned HTTP " + std::to_string(status);
    if (!detail.empty()) msg += ": " + detail;
    if (status == 401 || status == 403)
        msg += " — sign in again with `agentty login` and choose GitHub Copilot";
    return msg;
}

const provider::responses::Site kCopilotSite{
    .id                 = "copilot",
    .authorize          = &copilot_authorize,
    .decorate_body      = &copilot_decorate_body,
    .explain_http_error = &copilot_explain_http_error,
};

// An auto Endpoint carries the session token + CAPI api-version so the server
// accepts models that a free/limited plan can only reach via Auto.
provider::openai::Endpoint make_auto_endpoint(const std::string& api_base,
                                              const std::string& session_token) {
    auto hp = parse_api_base(api_base);
    provider::openai::Endpoint ep;
    ep.host        = hp.host;
    ep.port        = hp.port;
    ep.use_tls     = hp.tls;
    ep.path        = "/chat/completions";
    ep.models_path = "/models";
    ep.label       = "copilot";
    ep.extra_headers = copilot_headers();
    ep.extra_headers.push_back({"x-github-api-version", kAutoApiVersion});
    ep.extra_headers.push_back({"copilot-session-token", session_token});
    return ep;
}
} // namespace

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

    const std::string requested = req.model;
    // Route through Auto when the picked model is the synthetic Auto entry, or
    // a premium model the account can only reach via a session. Base gpt-4o
    // family models run DIRECT (they're not in the Auto set), and models we've
    // confirmed work directly also skip Auto.
    //
    // ALSO route through Auto when the model would prefer the Responses
    // dialect. The Auto session's `copilot-session-token` is the credential
    // /responses REQUIRES (measured: without it even gpt-5-mini 400s
    // model_not_supported), so a gpt-5 model that already works on the chat
    // path would otherwise never get a session — and never show reasoning.
    // Responses-only models (mai-code-*) need it for the same reason.
    const bool wants_auto = (requested == kAutoId)
        || (!is_base_direct(requested) && !is_supported_model(requested))
        || prefers_responses_dialect(requested);

    std::optional<AutoSession> as;
    if (wants_auto) as = auto_session();

    auto run = [&](const CopilotToken& t) -> provider::StreamResult {
        // ── Responses fork ─────────────────────────────────────────
        // MUST come before lower_shared(), which MOVES model/messages/tools
        // out of `req` into the chat-shaped request. Forking after it would
        // hand the Responses codec a gutted Request — empty input[], empty
        // instructions — which Copilot rejects with `One of "input" … must
        // be provided`. (It did, until this was measured.)
        //
        // The Auto session is exactly the credential /responses requires,
        // and it tells us which models the server will route. When the pick
        // is one of them, prefer the Responses dialect: it is the ONLY way
        // Copilot returns reasoning TEXT (measured — gpt-5-mini streams
        // response.reasoning_summary_text.delta there and nothing on
        // /chat/completions), and it is the only path at all for the
        // Responses-only models (mai-code-*) we used to skip entirely.
        //
        // Chat stays the default for everything else, so Claude / GPT-4.1
        // turns are byte-identical to before this fork existed.
        if (as && as->valid()) {
            const std::string picked = pick_auto_model(*as, requested);
            // A concrete pin the session can't serve is an ERROR, not a cue to
            // pick something else. Smart Mode pins a model per role slot; a
            // silent swap there spends the wrong model's budget and, when the
            // substitute is Responses-only, 400s on the chat path with a
            // message that blames the model the user never chose.
            if (picked.empty() && requested != kAutoId) {
                std::string avail;
                for (const auto& m : as->available_models) {
                    if (!avail.empty()) avail += ", ";
                    avail += m;
                }
                sink(StreamError{
                    "copilot: this account's Auto session does not offer `"
                    + requested + "`. Available: "
                    + (avail.empty() ? std::string{"(none)"} : avail)
                    + ". Pick one of those, or select Auto to let Copilot choose.",
                    std::nullopt});
                return provider::StreamResult{
                    .end   = provider::StreamEnd::TransportError,
                    .error = "copilot: model not in Auto session"};
            }
            const bool responses_capable =
                !picked.empty() && session_lists_model(*as, picked)
                && prefers_responses_dialect(picked);
            if (responses_capable) {
                auto& rt = responses_turn();
                rt.host          = parse_api_base(as->endpoint_api).host;
                rt.token         = t.token;
                rt.session_token = as->session_token;
                rt.model         = picked;
                provider::Request rr = req;   // still INTACT here
                rr.model = picked;
                return provider::responses::stream(kCopilotSite, std::move(rr),
                                                   sink);
            }
        }

        provider::openai::Request oreq;
        provider::lower_shared(oreq, req);
        oreq.context_window = req.context_window;
        oreq.session_key    = req.session_key;
        if (as && as->valid()) {
            // Same resolver the Responses fork used — one implementation. A
            // concrete pin is already proven available above (that path
            // returns early), so `picked` here is either the pin itself or
            // Auto's own choice. No substitution is possible, hence no notice.
            const std::string picked = pick_auto_model(*as, requested);
            if (picked.empty()) {
                // Auto had nothing to offer at all — fall back to the direct
                // endpoint rather than streaming an empty model slug.
                oreq.endpoint = make_endpoint(t.endpoint_api);
            } else {
                oreq.model    = picked;
                oreq.endpoint = make_auto_endpoint(as->endpoint_api, as->session_token);
            }
        } else {
            oreq.endpoint = make_endpoint(t.endpoint_api);
        }
        oreq.auth = auth::BearerHeader{t.token};
        return provider::openai::run_stream_sync(std::move(oreq), sink, req.cancel);
    };

    auto result = run(*tok);
    // Proxy token revoked mid-life → refresh + retry once.
    if (!result.ok() && (result.http_status == 401 || result.http_status == 403)) {
        invalidate_cached_token();
        if (auto fresh = fresh_token()) result = run(*fresh);
    }
    // Auto session expired/stale → refresh it + retry once.
    if (!result.ok() && wants_auto && result.http_status == 400) {
        invalidate_auto_session();
        as = auto_session();
        if (as) result = run(*tok);
    }
    // LEARN direct model support (only meaningful for a directly-requested
    // model, not the auto pseudo-id).
    if (requested != kAutoId) {
        if (result.http_status == 400 && result.error
            && result.error->find("not supported") != std::string::npos) {
            note_unsupported_model(requested);
            invalidate_model_cache();
        } else if (result.ok() && !wants_auto) {
            note_supported_model(requested);
        }
    }
    return result;
}

// ── Model listing ────────────────────────────────────────────────────────────
static std::vector<ModelInfo> bundled_models() {
    // Single bundled catalog; the live catalog (list_models) supersedes it the
    // moment the account can be reached.
    return catalog::bundled("copilot");
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

    // AUTHORITATIVE entitlement: does this account's billing tier include the
    // premium model families at all? On Copilot Free (premium_available=false)
    // only the base gpt-4o-family models run — every premium model 400s. We
    // fetch this once and HIDE the models the account can't use.
    const Entitlement ent = account_entitlement();
    const bool hide_premium = ent.known && !ent.premium_available;

    // AUTO session: the per-account set of models reachable via server-side
    // routing (the ONLY way a free/limited plan runs premium models). These
    // become first-class usable entries even though a DIRECT request 400s.
    auto as = auto_session();
    std::set<std::string> auto_ok;
    if (as) for (auto& m : as->available_models)
        if (auto_chat_compatible(m)) auto_ok.insert(m);   // skip /responses-only

    // A model is PREMIUM (needs the premium_interactions quota) when it's not
    // in the base free-tier line. Base = the current-gen gpt-4o / gpt-4.1
    // models GitHub documents as included on every plan (incl. Copilot Free).
    // A `policy` block on these is just terms-acceptance, not a premium gate.
    auto is_base_family = [](const std::string& id) {
        return id == "gpt-4o" || id == "gpt-4.1" || id == "gpt-4o-mini"
            || id == "gpt-4o-copilot" || id == "gpt-4.1-mini"
            || id.rfind("gpt-4o-mini", 0) == 0;
    };

    struct Row { ModelInfo info; int rank = 0; };
    std::vector<Row> rows;
    try {
        auto j = nlohmann::json::parse(resp->body);
        const auto& data = j.contains("data") ? j["data"] : j;
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
            // the canonical id (gpt-4o, gpt-4) already appears.
            {
                auto is_date_tail = [&](std::size_t pos) {
                    if (pos == std::string::npos) return false;
                    bool has_digit = false;
                    for (std::size_t k = pos + 1; k < id.size(); ++k) {
                        if (id[k] == '-') continue;
                        if (id[k] < '0' || id[k] > '9') return false;
                        has_digit = true;
                    }
                    return has_digit;
                };
                auto dash = id.find("-20");
                if (dash != std::string::npos && is_date_tail(dash)) continue;
                if (id.size() >= 5) {
                    auto tail = id.rfind('-');
                    if (tail != std::string::npos && id.size() - tail == 5
                        && is_date_tail(tail)) continue;
                }
            }

            const bool has_policy   = m.contains("policy") && m["policy"].is_object();
            const bool base         = is_base_family(id);   // policy = terms, not premium
            const bool learned_bad  = is_unsupported_model(id);
            const bool learned_good = is_supported_model(id);
            const bool auto_usable  = auto_ok.count(id) > 0;   // reachable via Auto

            // A model is premium (draws the premium quota) unless it's a base
            // family model. Confirmed-good overrides (we've actually run it).
            const bool premium = !base && !learned_good && !auto_usable;

            // FILTER: hide models this account can't run.
            //   • anything we've confirmed 400s (learned_bad) — always hide,
            //     UNLESS it's reachable via the Auto session.
            //   • premium models when the plan has no premium entitlement AND
            //     they're not in the Auto set.
            if (learned_bad && !auto_usable) continue;
            if (hide_premium && premium) continue;

            ModelInfo info;
            info.id           = ModelId{id};
            info.display_name = m.value("name", id)
                              + std::string{auto_usable && !base ? " (auto)" : ""};
            info.provider     = "copilot";
            // ★ the models we're CONFIDENT the account can use: base family,
            // confirmed-good, Auto-reachable, or (premium plan) any premium.
            info.favorite     = base || learned_good || auto_usable
                              || (!hide_premium && !has_policy);
            if (caps.contains("limits"))
                info.context_window =
                    caps["limits"].value("max_context_window_tokens", 200000);
            if (caps.contains("supports"))
                info.supports_tools = caps["supports"].value("tool_calls", true);

            const std::string cat = m.value("model_picker_category", "");
            int cat_w = cat == "powerful" ? 0 : cat == "versatile" ? 1
                      : cat == "lightweight" ? 2 : 3;
            // Confirmed-good / base first, then Auto-reachable, then the rest.
            int tier = (learned_good || base) ? 0 : auto_usable ? 10 : 100;
            rows.push_back({std::move(info), tier + cat_w});
        }
    } catch (...) { return bundled_models(); }

    // Prepend the synthetic "Auto" model — the top pick. Selecting it lets the
    // server route each turn to the best model the account may use (the same
    // "Auto" VS Code offers). Only shown when a session is actually available.
    if (as && as->valid()) {
        ModelInfo autom;
        autom.id           = ModelId{kAutoId};
        autom.display_name = "Auto (best available)";
        autom.provider     = "copilot";
        autom.favorite     = true;
        autom.context_window = 200000;
        autom.supports_tools = true;
        rows.insert(rows.begin(), {std::move(autom), -1});   // rank -1 = very top
    }

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
    // Prefer a base-allowlist model that works on every Copilot tier, so a
    // fresh sign-in never lands on a model that 400s on the first turn.
    for (const char* id : {"gpt-4o", "gpt-4.1", "gpt-4o-mini"})
        if (!is_unsupported_model(id)) return id;
    auto ms = list_models();
    return ms.empty() ? std::string{"gpt-4o"} : ms.front().id.value;
}

// Drop the cached catalog so the next list_models() re-ranks with freshly
// learned support (called after a turn records a 400/200 outcome).
// ── Dialect selection (public; see provider.hpp for the measured table) ──
bool prefers_responses_dialect(const std::string& model) {
    // Responses-only models must use it; gpt-5* should, because that is the
    // only dialect on which Copilot returns reasoning TEXT.
    return chat_dialect_unsupported(model) || wants_reasoning_text(model);
}

bool chat_dialect_unsupported(const std::string& model) {
    // Learned at runtime (a 400 unsupported_api_for_model on the chat path)
    // OR known by family: mai-code-* has never accepted /chat/completions.
    return is_responses_only(model) || !auto_chat_compatible(model);
}

std::string pick_auto_model_for_test(
        const std::vector<std::string>& available_models,
        const std::string& selected_model,
        const std::string& requested) {
    AutoSession as;
    as.available_models = available_models;
    as.selected_model   = selected_model;
    return pick_auto_model(as, requested);
}

void invalidate_model_cache() {
    std::lock_guard<std::mutex> lk(models_mu());
    models_cache().clear();
}

} // namespace agentty::provider::copilot
