// agentty::provider — active-provider selection (process-global).

#include "agentty/provider/selection.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include "agentty/provider/registry.hpp"
#include "agentty/provider/acp_agents.hpp"
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
    // External ACP agent: a registry row with Kind::ExternalAcp, OR any spec id
    // that names a launchable agent (built-in default / config entry) even
    // without a registry row. The runtime spawns/drives the subprocess.
    if ((p && p->kind() == Kind::ExternalAcp)
        || (!p && !spec.empty() && is_acp_agent_id(spec))) {
        s.kind         = Kind::ExternalAcp;
        s.acp_agent_id = std::string{spec};
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
    return lbl.empty() ? std::string{"OpenAI"} : lbl;
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

} // namespace agentty::provider
