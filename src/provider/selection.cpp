// agentty::provider — active-provider selection (process-global).

#include "agentty/provider/selection.hpp"

#include "agentty/util/logx.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/provider/registry.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/copilot/provider.hpp"
#include "agentty/provider/kimi/provider.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/runtime/fuzzy.hpp"
#include "agentty/io/http.hpp"

namespace agentty::provider {

namespace {
Selection  g_active{};
std::mutex g_active_mu;   // guards g_active across the UI/worker thread split

std::string g_auth_header;         // --auth-header override, "" = Bearer
std::mutex  g_auth_header_mu;      // same UI/worker split as g_active

// Persist-on-success: the CLI custom-host spec awaiting proof (empty =
// none). Written once at startup (main thread, pre-UI), consumed on the
// UI thread by the ModelsLoaded reducer — the mutex covers the overlap.
std::string g_unproven_spec;
std::string g_unproven_model;
std::mutex  g_unproven_mu;

std::string env_or_empty(std::string_view name) {
    if (name.empty()) return {};
    const char* v = std::getenv(std::string{name}.c_str());
    return (v && *v) ? std::string{v} : std::string{};
}

// Small per-provider seed catalog used ONLY when the live /models fetch comes
// back empty (no API key set yet, network down, or a provider that gates its
// list behind auth). Lets a freshly selected hosted provider show sensible
// models in the picker immediately; the real catalog supersedes it the moment
// the account is reachable. Keyed on the registry `id` (== endpoint label).
// Deliberately short — a couple of current, agent-capable ids per provider,
// newest first (front() becomes the default when the user gives no -m).
std::vector<ModelInfo> bundled_models_for(std::string_view label) {
    // Delegates to the single bundled catalog (catalog::bundled) so every
    // provider's offline floor lives in ONE place — no per-site drift.
    return catalog::bundled(label);
}
} // namespace

void set_unproven_spec(std::string spec, std::string model_recall) {
    std::lock_guard<std::mutex> lk(g_unproven_mu);
    g_unproven_spec  = std::move(spec);
    g_unproven_model = std::move(model_recall);
}

std::optional<std::pair<std::string, std::string>>
take_unproven_spec(std::string_view spec_now) {
    std::lock_guard<std::mutex> lk(g_unproven_mu);
    if (g_unproven_spec.empty() || g_unproven_spec != spec_now)
        return std::nullopt;
    auto out = std::make_pair(std::move(g_unproven_spec),
                              std::move(g_unproven_model));
    g_unproven_spec.clear();
    g_unproven_model.clear();
    return out;
}

void set_custom_auth_header(std::string name) {
    std::lock_guard lk(g_auth_header_mu);
    g_auth_header = std::move(name);
}

std::string custom_auth_header() {
    std::lock_guard lk(g_auth_header_mu);
    return g_auth_header;
}

Selection parse_selection(std::string_view spec) {
    Selection s;
    // Registry-driven: an empty spec or any preset whose kind is Anthropic
    // routes to the Anthropic transport. Everything else (OpenAI-family
    // presets AND raw "host[:port]" custom endpoints) goes through the
    // OpenAI-compatible transport with the matching Endpoint.
    const ProviderPreset* p = spec.empty() ? preset_for(default_provider_id())
                                            : preset_for(spec);
    // Bind the row ONCE, here, on every path out of this function. Downstream
    // predicates then read identity off the row instead of re-deriving it from
    // `openai_endpoint.label` — a display string the custom-host flow
    // overwrites, which is how a Copilot-backed custom provider used to lose
    // its identity and fall through to the generic /v1/models fetch.
    s.row = p;
    if (p && p->kind() == Kind::Anthropic) {
        s.kind = Kind::Anthropic;
        return s;
    }
    // External ACP agent: only an explicitly configured launchable id routes
    // here. There are no privileged built-ins.
    if ((p && p->kind() == Kind::ExternalAcp)
        || (!p && !spec.empty() && is_acp_agent_id(spec))) {
        s.kind         = Kind::ExternalAcp;
        s.acp_agent_id = std::string{spec};
        return s;
    }
    // A removed/stale ACP selection must not fall through to Endpoint::from_spec
    // and be interpreted as an OpenAI hostname. Fall back to the native default
    // until the user explicitly configures that agent again.
    if (!p && spec.ends_with("-acp")) {
        s.kind = Kind::Anthropic;
        return s;
    }
    s.kind = Kind::OpenAI;
    s.openai_endpoint = openai::Endpoint::from_spec(
        spec.empty() ? default_provider_id() : spec);

    // A CUSTOM HOST that happens to name a provider we know is that provider.
    //
    // "Custom host" specs (raw `host[:port]`, or a full URL) have no registry
    // row — preset_for() only matches canonical ids. So a user who added
    // `api.githubcopilot.com` as a custom provider got the GENERIC OpenAI
    // defaults: /v1/models instead of Copilot's /models, and none of the
    // OAuth session handling. The picker showed no models at all, with no
    // error, because an empty catalog and a failed fetch look identical.
    //
    // Match on HOST, which is the one part of a custom spec that is not a
    // display string the user can rename. Only adopt rows for hosts we
    // actually special-case (oauth_native backends); a generic hosted
    // provider gains nothing from adoption and keeps the user's endpoint
    // exactly as typed.
    if (!p && !s.openai_endpoint.host.empty()) {
        for (const auto& row : kProviders) {
            if (!row.oauth_native) continue;
            if (row.host != s.openai_endpoint.host) continue;
            // Adopt the row's identity AND its endpoint columns — the paths
            // are part of what makes it that provider.
            s.row = &row;
            s.openai_endpoint = openai::Endpoint::from_spec(row.id);
            break;
        }
    }

    // Stamp the session's --auth-header override onto every OpenAI-family
    // endpoint, here rather than at the call sites so live provider switches
    // (picker / login reducers) keep it without knowing it exists.
    s.openai_endpoint.auth_header_name = custom_auth_header();
    return s;
}

auth::AuthHeader resolve_auth_for(std::string_view spec,
                                  const auth::AuthHeader& anthropic_creds,
                                  std::string_view cli_key,
                                  std::string_view saved_key) {
    const ProviderPreset* p = spec.empty() ? preset_for(default_provider_id())
                                            : preset_for(spec);

    // Anthropic (or an unknown spec that parsed to Anthropic): use the creds
    // resolved from `agentty login`.
    if ((p && p->kind() == Kind::Anthropic)
        || (!p && parse_selection(spec).kind == Kind::Anthropic)) {
        return anthropic_creds;
    }

    // Local backends need no key.
    if (p && p->auth == AuthStyle::None)
        return auth::AuthHeader{auth::ApiKeyHeader{std::string{}}};

    // OpenAI-family (preset or custom host): bearer key, precedence
    //   --key  >  saved key (in-app paste)  >  env-var chain  >  OPENAI_API_KEY.
    std::string key{cli_key};
    if (key.empty()) key = std::string{saved_key};
    if (key.empty() && p) {
        for (auto env : p->auth_env) {
            key = env_or_empty(env);
            if (!key.empty()) break;
        }
    }
    if (key.empty())
        key = env_or_empty("OPENAI_API_KEY");
    return auth::AuthHeader{auth::ApiKeyHeader{std::move(key)}};
}

void select(Selection s) {
    // Publish the provider id as the capability-key scope BEFORE swapping the
    // selection in, so a resolved_caps() racing the switch sees, at worst,
    // the OLD scope with the OLD selection — never a mismatched pair.
    //
    // The value MUST match what the rest of the tree calls the provider id
    // (modal.cpp's active_provider_id(), which every catalog/recents/fused
    // row is keyed by): OpenAI-family → the endpoint label, external ACP →
    // the agent id, everything else → the default id. Collapsing ACP onto
    // the default id (as this did) filed its capability facts under
    // "anthropic/..." while every lookup asked for "<agent-id>/...", so an
    // ACP agent's learned effort ladder could never be found again.
    set_caps_provider_scope(
        s.kind == Kind::OpenAI      ? s.openai_endpoint.label
      : s.kind == Kind::ExternalAcp ? s.acp_agent_id
                                    : std::string{default_provider_id()});
    // Record WHICH backend every subsequent turn will use. select() is the one
    // seam both the startup path and every later provider switch pass through,
    // so one line here means a log can always answer "which provider/endpoint
    // was active when this broke" — the question that cost the most time on
    // the custom-host and Copilot reports, because the answer was invisible.
    // Endpoint columns are meaningful ONLY for the OpenAI-family wire —
    // Anthropic and ACP leave openai_endpoint at its struct defaults, and
    // printing those (host=api.openai.com for an Anthropic turn) actively
    // misleads whoever reads the log.
    const bool http_wire = s.kind == Kind::OpenAI;
    // Warn for the same retention reason as the startup banner: "which
    // provider/endpoint was active" is the second question on every report,
    // and a handful of lines per session is a fair price for always having it.
    AGT_LOG(Wire, Warn, "provider.select",
            "provider={} kind={} endpoint={} agent={}",
            s.provider_id().empty()
                ? (s.openai_endpoint.label.empty() ? std::string{"custom"}
                                                   : s.openai_endpoint.label)
                : std::string{s.provider_id()},
            s.kind == Kind::Anthropic      ? "anthropic"
          : s.kind == Kind::ExternalAcp    ? "acp"
                                           : "openai",
            http_wire ? s.openai_endpoint.host + s.openai_endpoint.path
                      : std::string{"-"},
            s.kind == Kind::ExternalAcp ? s.acp_agent_id : std::string{"-"});

    std::lock_guard lk(g_active_mu);
    g_active = std::move(s);
}

Selection active() {
    std::lock_guard lk(g_active_mu);
    return g_active;   // snapshot copy — see header for the race it closes
}

std::string provider_display_name(const Selection& s) {
    if (s.kind == Kind::Anthropic) return "Anthropic";
    if (s.kind == Kind::ExternalAcp) {
        // No hardcoded registry rows for ACP agents (Zed-style: config-driven),
        // so the display name is just the agent id the user selected.
        return s.acp_agent_id.empty() ? std::string{"ACP agent"} : s.acp_agent_id;
    }
    // OpenAI-family: map the endpoint label ("groq", "ollama", …) to its
    // registry display name; fall back to the raw label for a custom host.
    const std::string& lbl = s.openai_endpoint.label;
    if (const auto* p = preset_for(lbl)) return std::string{p->label};
    if (lbl.empty()) return std::string{"OpenAI"};
    // URL-form custom hosts (https://host/path, http://host:port/path) carry
    // the full spec as their label (see Endpoint::from_spec). The badge and
    // the switch toast should show host[:port], not the long URL. ep.label itself
    // is untouched — preset lookup and the openai_transport_test label
    // round-trip assertion still see the full spec.
    if (lbl.starts_with("https://") || lbl.starts_with("http://")) {
        const bool tls = lbl.starts_with("https://");
        std::string_view s = lbl;
        s.remove_prefix(tls ? 8 : 7);   // "https://" = 8, "http://" = 7
        // Strip path at first '/' (everything after the authority).
        if (auto slash = s.find('/'); slash != std::string_view::npos)
            s = s.substr(0, slash);
        std::string out;
        std::uint16_t port = tls ? 443 : 80;
        // IPv6 literals are bracketed ("[::1]" / "[::1]:8080") — the port colon
        // is the one after "]", and the brackets are kept in the label so it
        // round-trips as a valid authority.
        if (!s.empty() && s.front() == '[') {
            auto close = s.find(']');
            if (close != std::string_view::npos) {
                out = std::string{s.substr(0, close + 1)};   // keep [ ... ]
                auto rest = s.substr(close + 1);
                if (rest.size() > 1 && rest.front() == ':') {
                    try {
                        int p = std::stoi(std::string{rest.substr(1)});
                        if (p > 0 && p <= 65535) port = static_cast<std::uint16_t>(p);
                    } catch (...) {}
                }
            } else {
                out = std::string{s};
            }
        } else if (auto colon = s.rfind(':'); colon != std::string_view::npos) {
            out = std::string{s.substr(0, colon)};
            try {
                int p = std::stoi(std::string{s.substr(colon + 1)});
                if (p > 0 && p <= 65535) port = static_cast<std::uint16_t>(p);
            } catch (...) {}
        } else {
            out = std::string{s};
        }
        // Omit the port when it's the scheme default.
        if (port != (tls ? 443 : 80)) out += ":" + std::to_string(port);
        return out;
    }
    return lbl;
}

PrewarmTarget prewarm_target(const Selection& s) {
    // ACP subprocess: no HTTP layer to warm.
    if (s.kind == Kind::ExternalAcp) return {};

    // Registry-driven fixed host. The two backends that don't dial their
    // Endpoint carry a `prewarm_host` on their preset row: Anthropic
    // (transport hardcodes api.anthropic.com) and ChatGPT (talks to
    // chatgpt.com while its Endpoint port is the 0 sentinel). We look the row
    // up by the id the Selection resolves to — "anthropic" for the Anthropic
    // kind, else the OpenAI endpoint label ("chatgpt", "groq", …).
    const std::string_view id =
        s.kind == Kind::Anthropic ? std::string_view{"anthropic"}
                                  : std::string_view{s.openai_endpoint.label};
    if (const ProviderPreset* p = preset_for(id); p && !p->prewarm_host.empty()) {
        PrewarmTarget t;
        t.host = std::string{p->prewarm_host};
        t.port = 443;
        // Anthropic honours the AGENTTY_API_HOST dial so the warm socket
        // targets the real upstream (kept out of the registry: it's a
        // per-run env override, not a static provider fact).
        if (s.kind == Kind::Anthropic) {
            const auto& ov = http::agentty_api_host_override();
            if (ov.active()) {
                t.override_host = ov.host;
                t.override_port = ov.port;
            }
        }
        return t;
    }

    // Every other OpenAI-compat backend: warm its own Endpoint host. Skip
    // locals — no TLS handshake to amortise, and the port may be 0 (a
    // sentinel) or a local dev server that isn't up yet.
    const auto& ep = s.openai_endpoint;
    if (!ep.use_tls || ep.host.empty() || ep.host == "localhost"
        || ep.host == "127.0.0.1" || ep.port == 0)
        return {};
    PrewarmTarget t;
    t.host = ep.host;
    t.port = ep.port;
    return t;
}

void prewarm_active_provider() {
    // Uniform across native backends: resolve the warm target from the active
    // selection (pure, registry-driven — see prewarm_target) and open the
    // socket. No provider is privileged; the routing table lives in one
    // testable function, not this side-effecting wrapper.
    const PrewarmTarget t = prewarm_target(active());
    if (!t.should_warm()) return;
    http::default_client().prewarm(t.host, t.port, t.override_host,
                                   t.override_port);
}

std::vector<ModelInfo> list_models_for(const Selection& sel,
                                       const auth::AuthHeader& auth) {
    // Dispatch on the CARRIED provider id, not on a re-derived label. The
    // three OAuth-native backends fetch their own catalogs because their
    // endpoints expose metadata the generic parser discards (Copilot's
    // policy/tier, ChatGPT's account line-up, Kimi's coding models).
    //
    // This used to compare `openai_endpoint.label`, which the custom-host flow
    // overwrites — so a Copilot-backed custom host matched nothing, fell
    // through to the generic /v1/models GET (Copilot serves /models), and the
    // picker came up empty with no error.
    if (sel.kind == Kind::ExternalAcp) return {};   // the agent picks its own
    if (sel.is_copilot()) return copilot::list_models();
    if (sel.is_kimi())    return kimi::list_models();
    if (sel.is_chatgpt()) return chatgpt::list_models();
    if (sel.kind == Kind::Anthropic) return anthropic::list_models(auth);

    auto models = openai::list_models(auth, sel.openai_endpoint);
    // Hosted providers return nothing before a key is set (or when the fetch
    // fails). Seed from the bundled catalog so the picker is never stranded
    // empty; custom hosts have no seed and legitimately stay empty.
    if (models.empty()) return bundled_models_for(sel.openai_endpoint.label);
    return models;
}

std::vector<int> filter_provider_indices(std::string_view query) {
    const auto ps = providers();
    const int n = static_cast<int>(ps.size());
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(n));
    if (query.empty()) {
        for (int i = 0; i < n; ++i) out.push_back(i);
        return out;
    }
    // Fuzzy-score against id, label, and blurb; keep the best of the three so
    // "kimi" matches the id, "grok" matches the label, "wafer" matches the
    // Cerebras blurb. Rank by score (stable, so ties keep registry order).
    struct Scored { int idx; int score; };
    std::vector<Scored> scored;
    scored.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& p = ps[static_cast<std::size_t>(i)];
        int best = -1;
        for (std::string_view field : {p.id, p.label, p.blurb}) {
            auto m = fuzzy::score(field, query);
            if (m.matched() && m.score > best) best = m.score;
        }
        if (best >= 0) scored.push_back({i, best});
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const Scored& a, const Scored& b) { return a.score > b.score; });
    for (const auto& s : scored) out.push_back(s.idx);
    return out;
}

} // namespace agentty::provider
