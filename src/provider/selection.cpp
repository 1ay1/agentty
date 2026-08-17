// agentty::provider — active-provider selection (process-global).

#include "agentty/provider/selection.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include "agentty/provider/registry.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/copilot/provider.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/io/http.hpp"

namespace agentty::provider {

namespace {
Selection  g_active{};
std::mutex g_active_mu;   // guards g_active across the UI/worker thread split

std::string g_auth_header;         // --auth-header override, "" = Bearer
std::mutex  g_auth_header_mu;      // same UI/worker split as g_active

std::string env_or_empty(std::string_view name) {
    if (name.empty()) return {};
    const char* v = std::getenv(std::string{name}.c_str());
    return (v && *v) ? std::string{v} : std::string{};
}
} // namespace

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
        // Split host / port at the last ':'.
        std::string out;
        std::uint16_t port = tls ? 443 : 80;
        if (auto colon = s.rfind(':'); colon != std::string_view::npos) {
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
    // Dispatch on the SAME axes as the stream path so the picker and the
    // transport can never disagree about which backend a selection names:
    //   • ACP subprocess  — no catalog endpoint; the agent picks its own model.
    //   • oauth_native     — ChatGPT/Codex: fetch from the account's /models via
    //                        its in-process OAuth creds (ignores `auth`).
    //   • OpenAI dialect   — the Endpoint's /v1/models with the bearer `auth`.
    //   • Anthropic        — the Messages backend's model list with `auth`.
    // Adding a provider does NOT touch this function unless it introduces a
    // brand-new catalog mechanism — it inherits one of these by its Wire /
    // oauth_native row fields.
    if (sel.kind == Kind::ExternalAcp) return {};
    if (sel.is_copilot())              return copilot::list_models();
    if (sel.is_oauth_native())         return chatgpt::list_models();
    if (sel.kind == Kind::OpenAI)
        return openai::list_models(auth, sel.openai_endpoint);
    return anthropic::list_models(auth);
}

} // namespace agentty::provider
