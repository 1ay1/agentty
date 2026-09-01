#pragma once
// agentty::provider — the provider REGISTRY: one static, ordered table that
// is the single source of truth for every backend agentty can talk to.
//
// Why a registry (and not an if/else chain): provider selection is growing
// toward "all providers, first-class". Every place that needs to know about
// a backend — the provider picker UI, `parse_selection`, the per-provider
// auth-env resolution in main.cpp, `Endpoint::from_spec`, the model badge —
// used to hardcode a 2-way Anthropic/OpenAI branch (or a chain of
// `if (label == "groq")`). Adding a provider meant editing N call sites and
// hoping you found them all.
//
// Now: add ONE row to `kProviders` below and the picker shows it, the auth
// resolver knows its env var, the badge labels it. The table is `constexpr`
// where it can be (ids/labels/flags) with the endpoint left to
// `openai::Endpoint::from_spec` so the wire-shape (path/port/tls) lives next
// to the transport that uses it.
//
// The registry deliberately does NOT own the concrete Provider type — that
// stays behind the type-erased `Deps::stream` seam (see runtime/app/deps.hpp).
// A registry row only describes *which* backend and *how to authenticate*;
// constructing the right transport is main.cpp's job, dispatched on `Kind`.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace agentty::provider {

// ── The two orthogonal axes of a provider ────────────────────────────────
//
// `Kind` used to conflate them into one enum, which is why "is this ChatGPT?"
// had to be re-derived as `kind == OpenAI && label == "chatgpt"` at six sites.
// They are separated here:
//
//   Wire     — the on-the-wire DIALECT (which serializer/parser a turn uses).
//              This is the ONLY thing the transport layer branches on.
//   Lifetime — whether the concrete Provider is built once and reused for the
//              whole process (it holds connection / OAuth-token state) or is a
//              cheap value rebuilt per call from the active Endpoint.
//
// Provider IDENTITY ("chatgpt" vs "groq") is never an enum — it is the row's
// `id`. Anything that used to switch on identity now reads a capability field
// off the row (see ProviderDescriptor), so adding a provider is adding a row.

// Which wire dialect a turn is serialized/parsed as. Multiple providers share
// one dialect (every hosted OpenAI-compat clone speaks OpenAIChat); a provider
// is special only if it needs a NEW dialect.
enum class Wire : std::uint8_t {
    AnthropicMessages,  // Anthropic /v1/messages SSE.
    OpenAIResponses,    // OpenAI /responses SSE (Codex/ChatGPT + api.openai.com).
    OpenAIChat,         // OpenAI /chat/completions SSE + Ollama /api/chat NDJSON.
    Acp,                // external ACP agent subprocess (no HTTP dialect).
};

// Is the concrete Provider long-lived (owns refreshed tokens / a warm pool, so
// it is constructed once in main() and reused) or built per call?
enum class Lifetime : std::uint8_t {
    PerCall,    // cheap value transport, rebuilt from the active Endpoint.
    LongLived,  // holds cross-turn state; one instance owned by main().
};

// `Kind` is retained as a COARSE, derived view of `Wire` for the handful of
// call sites that only care "Anthropic vs OpenAI-family vs ACP subprocess".
// It is no longer stored on a row — it is a pure function of Wire — so it can
// never drift from the dialect. New code should prefer `Wire` + capability
// fields; `Kind` stays only to keep the migration mechanical.
enum class Kind : std::uint8_t { Anthropic, OpenAI, ExternalAcp };

[[nodiscard]] constexpr Kind kind_of(Wire w) noexcept {
    switch (w) {
        case Wire::AnthropicMessages: return Kind::Anthropic;
        case Wire::Acp:               return Kind::ExternalAcp;
        case Wire::OpenAIResponses:
        case Wire::OpenAIChat:        return Kind::OpenAI;
    }
    return Kind::OpenAI;
}

// How a provider authenticates — drives both the UI hint and which env vars
// the auth resolver consults.
enum class AuthStyle : std::uint8_t {
    OAuthOrKey,  // Anthropic: OAuth (Pro/Max) or x-api-key.
    ApiKey,      // hosted OpenAI-family: a bearer key from env / --key.
    None,        // local server (Ollama / llama.cpp): no auth needed.
};

// One backend agentty knows how to reach: its identity, how it authenticates,
// which wire dialect it speaks, and its lifetime. POD + string_view so the
// whole table is a constant-initialised `constexpr` array with zero heap.
//
// This is THE source of truth. Every subsystem that used to switch on a
// provider (dispatch, model-list, prewarm, login gate, picker) reads a field
// here instead. "Add a provider" == "append a row".
struct ProviderDescriptor {
    std::string_view id;        // canonical spec token ("anthropic", "groq").
    std::string_view label;     // display name for the picker / badge.
    std::string_view blurb;     // one-line description (picker trailing text).
    Wire             wire;       // on-the-wire dialect (transport branches here).
    Lifetime         lifetime;   // long-lived (owns state) vs rebuilt per call.
    AuthStyle        auth;
    // TRUE only for a backend that runs on this machine (Ollama, a custom
    // localhost host). NOT "needs no API key" — that is AuthStyle::None, and
    // conflating the two is why ChatGPT once claimed to be local. Read by the
    // credential layer to skip key resolution and by the picker to skip the
    // key prompt.
    bool             is_local;

    // Env vars consulted (in order) to find this provider's API key. The
    // last entry is usually the generic OPENAI_API_KEY fallback. Empty for
    // Anthropic (creds come from `agentty login`) and local backends.
    // Stored as a fixed 3-slot array of string_views; unused slots are "".
    std::array<std::string_view, 3> auth_env;

    // The FIXED host to open a warm TLS connection to before the first turn,
    // when it is NOT derivable from the runtime Endpoint. Two backends need
    // this: Anthropic (the transport hardcodes api.anthropic.com; no Endpoint
    // is dialled) and ChatGPT (talks to chatgpt.com/backend-api/codex while
    // its Endpoint carries a sentinel port 0). Empty for every other row:
    // hosted OpenAI-compat backends prewarm their own Endpoint host, and
    // local / ACP backends have nothing worth warming. This makes
    // prewarm_active_provider a data lookup instead of a hand-written switch.
    std::string_view prewarm_host;

    // OAuth-native: the backend authenticates by "sign in with <provider>"
    // (its own OAuth flow, tokens auto-refreshed in-process) rather than a
    // bearer API key, AND it rides a DEDICATED long-lived transport instead of
    // the generic OpenAI-compat one. Today only ChatGPT/Codex sets this
    // (Anthropic OAuth speaks AnthropicMessages, so it never needs the flag to
    // be routed). "This endpoint is special" is a DATUM on the row, read at the
    // routing/prewarm/login sites — a second such provider just sets the flag.
    bool oauth_native = false;

    // ── Wire endpoint (the HTTP facts) ───────────────────────────────
    // Where this provider is actually dialled. These used to live in a
    // 14-arm if-chain inside Endpoint::from_spec, RE-STATING host/path
    // facts that were already half-present on this row. Two sources of
    // truth drifted exactly as you'd expect: the `openai` row claimed
    // Wire::OpenAIResponses while the transport dialled
    // /v1/chat/completions, which silently made the reasoning-text UI
    // promise something the wire never delivers.
    //
    // Keeping them HERE, one column over from `wire`, makes that class of
    // drift unrepresentable: a row that says Responses but points at
    // /chat/completions is now a visible contradiction on a single line,
    // and a static_assert below rejects it at compile time.
    //
    // Empty `host` means "not dialled over the generic OpenAI-compat
    // transport": Anthropic (own transport, hardcoded host), ChatGPT
    // (dedicated OAuth transport) and ACP (subprocess, no HTTP) leave
    // these blank and are routed by `wire` / `oauth_native` instead.
    std::string_view host;          // "api.groq.com"; "" = not HTTP-dialled.
    std::string_view path;          // completions path, e.g. "/v1/chat/completions".
    std::string_view models_path;   // model-list path, e.g. "/v1/models".
    std::uint16_t    port = 443;
    bool             use_tls = true;

    // Ollama's NATIVE /api/chat protocol (NDJSON, structured tool_calls)
    // instead of its OpenAI-compat shim, which makes weak local models
    // leak tool calls as raw JSON in `content`.
    bool             native_api = false;

    // ── Auth capabilities (what the LOGIN flow needs to know) ───────
    // These four used to be ~26 provider-name string compares scattered
    // through login.cpp / modal.cpp / pickers.cpp — exactly the
    // `if anthropic {} else if copilot {}` chain the registry exists to
    // delete. Each is the CAPABILITY the branch was really testing, so a
    // new OAuth provider becomes a row edit rather than a grep.

    // Runs a device/browser OAuth flow (no key prompt). Copilot and Kimi
    // share the generic launcher; ChatGPT has its own because the Codex
    // flow negotiates device-vs-browser at runtime (see oauth_native).
    bool device_login = false;

    // The login modal offers a CHOICE of method (OAuth subscription vs
    // API key). Only Anthropic does today: everything else is either
    // key-only or OAuth-only, and showing a one-item menu is noise.
    bool method_menu = false;

    // Tokens live in the provider's OWN store (the transport reads them
    // per turn) rather than in a resolvable AuthHeader. On account switch
    // these need the cached header CLEARED, not replaced — resolve()
    // returns empty for them by design.
    bool token_in_transport = false;

    // Anthropic-only extras that are genuinely not uniform: a long-idle
    // OAuth token gets a proactive background refresh on account switch,
    // and the learned "1M context is blocked" flag is re-armed because the
    // switched-to account may hold a different entitlement.
    bool oauth_proactive_refresh = false;

    // Model to select when switching TO this provider with no recalled
    // choice, and no live catalog yet (the switch runs on the UI thread, so
    // it cannot fetch). Empty = "let the ModelsLoaded refetch auto-select
    // the first available", which is right for local backends and for any
    // provider whose line-up is server-driven.
    //
    // A static default belongs on the row; a DERIVED one does not — ChatGPT
    // reads its cached catalog and Copilot's line-up varies by tier, so those
    // stay in code with the reason written down at the call site.
    std::string_view default_model;

    // True when this row is reached over the generic OpenAI-compat
    // transport (i.e. it carries real endpoint data above).
    [[nodiscard]] constexpr bool http_dialled() const noexcept {
        return !host.empty();
    }

    // Coarse Kind view, derived from `wire` so it can never drift. Kept for
    // the call sites that only need Anthropic-vs-OpenAI-family-vs-ACP.
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_of(wire); }
};

// Legacy name. `ProviderPreset` was the row type before it grew wire/lifetime
// and became the behaviour source of truth; kept as an alias so the ~dozen
// call sites naming it keep compiling during and after the migration.
using ProviderPreset = ProviderDescriptor;

// ── The table ────────────────────────────────────────────────────────────
// Order = display order in the picker. Anthropic first (the default), then
// hosted OpenAI-family by popularity, then the local backend last.
//
// To add a provider: append ONE row here. The endpoint columns below are the
// only place its host/path live — Endpoint::from_spec is a lookup over this
// table, so there is no second arm to keep in sync.
inline constexpr std::array<ProviderDescriptor, 15> kProviders{{
    {.id = "anthropic", .label = "Anthropic",
     .blurb = "Claude — OAuth (Pro/Max) or API key",
     // Own transport (not the OpenAI-compat one): no endpoint columns.
     // The one row that gets a proactive token refresh + 1M-entitlement
     // re-arm on an account switch.
     .wire = Wire::AnthropicMessages, .lifetime = Lifetime::LongLived,
     .auth = AuthStyle::OAuthOrKey,
     .prewarm_host = "api.anthropic.com",
     .method_menu = true, .oauth_proactive_refresh = true,
     .default_model = "claude-opus-4-5"},

    {.id = "openai", .label = "OpenAI", .blurb = "GPT / Codex — api.openai.com",
     // Chat Completions, NOT Responses. The row used to claim
     // Wire::OpenAIResponses while dialling /v1/chat/completions — the label
     // was decorative (nothing dispatched on it) so the lie went unnoticed and
     // made the reasoning-text UI over-promise. Wire and path must agree.
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"OPENAI_API_KEY", "CODEX_API_KEY", ""},
     .host = "api.openai.com", .path = "/v1/chat/completions",
     .models_path = "/v1/models",
     .default_model = "gpt-4o"},

    {.id = "chatgpt", .label = "ChatGPT",
     .blurb = "Sign in with ChatGPT — Codex models, no API key",
     // Genuinely Responses — but over its OWN OAuth transport
     // (/backend-api/codex/responses), not the compat one, so no endpoint
     // columns. is_local stays FALSE: chatgpt.com is a remote host, and
     // "needs no API key" is what AuthStyle::None already says.
     // No default_model: the cached catalog picks.
     .wire = Wire::OpenAIResponses, .lifetime = Lifetime::LongLived,
     .auth = AuthStyle::None,
     .prewarm_host = "chatgpt.com", .oauth_native = true,
     .token_in_transport = true},

    {.id = "copilot", .label = "GitHub Copilot",
     .blurb = "Sign in with GitHub — Copilot models, no API key",
     // gpt-4o runs on every Copilot tier; the async fetch replaces it with
     // the account's real line-up (incl. Auto) shortly after the switch.
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::LongLived,
     .auth = AuthStyle::None,
     .prewarm_host = "api.githubcopilot.com", .oauth_native = true,
     .host = "api.githubcopilot.com", .path = "/chat/completions",
     .models_path = "/models",
     .device_login = true, .token_in_transport = true,
     .default_model = "gpt-4o"},

    {.id = "kimi", .label = "Kimi",
     .blurb = "Sign in with Kimi — Kimi K2 models, no API key",
     // Kimi Code inference API: base https://api.kimi.com/coding/v1 (its own
     // provider builds this; the columns must agree with it).
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::LongLived,
     .auth = AuthStyle::None,
     .prewarm_host = "api.kimi.com", .oauth_native = true,
     .host = "api.kimi.com", .path = "/coding/v1/chat/completions",
     .models_path = "/coding/v1/models",
     .device_login = true, .token_in_transport = true},

    {.id = "groq", .label = "Groq",
     .blurb = "Llama/Mixtral on Groq LPUs — very fast",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"GROQ_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.groq.com", .path = "/openai/v1/chat/completions",
     .models_path = "/openai/v1/models"},

    {.id = "openrouter", .label = "OpenRouter",
     .blurb = "Any model via openrouter.ai",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"OPENROUTER_API_KEY", "OPENAI_API_KEY", ""},
     .host = "openrouter.ai", .path = "/api/v1/chat/completions",
     .models_path = "/api/v1/models"},

    {.id = "together", .label = "Together",
     .blurb = "Open models on together.ai",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"TOGETHER_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.together.xyz", .path = "/v1/chat/completions",
     .models_path = "/v1/models"},

    {.id = "cerebras", .label = "Cerebras",
     .blurb = "Wafer-scale inference — very fast",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"CEREBRAS_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.cerebras.ai", .path = "/v1/chat/completions",
     .models_path = "/v1/models"},

    {.id = "deepseek", .label = "DeepSeek",
     .blurb = "DeepSeek V4 — api.deepseek.com",
     // DeepSeek serves completions WITHOUT the /v1 prefix.
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"DEEPSEEK_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.deepseek.com", .path = "/chat/completions",
     .models_path = "/models"},

    {.id = "xai", .label = "xAI (Grok)", .blurb = "Grok models — api.x.ai",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"XAI_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.x.ai", .path = "/v1/chat/completions",
     .models_path = "/v1/models"},

    {.id = "mistral", .label = "Mistral",
     .blurb = "Mistral / Codestral / Magistral — api.mistral.ai",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"MISTRAL_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.mistral.ai", .path = "/v1/chat/completions",
     .models_path = "/v1/models"},

    {.id = "gemini", .label = "Google Gemini",
     .blurb = "Gemini models via the OpenAI-compat API",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"GEMINI_API_KEY", "GOOGLE_API_KEY", "OPENAI_API_KEY"},
     .host = "generativelanguage.googleapis.com",
     .path = "/v1beta/openai/chat/completions",
     .models_path = "/v1beta/openai/models"},

    {.id = "fireworks", .label = "Fireworks",
     .blurb = "Open models on fireworks.ai",
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::ApiKey,
     .auth_env = {"FIREWORKS_API_KEY", "OPENAI_API_KEY", ""},
     .host = "api.fireworks.ai", .path = "/inference/v1/chat/completions",
     .models_path = "/inference/v1/models"},

    {.id = "ollama", .label = "Ollama",
     .blurb = "Local models at localhost:11434",
     // Plain HTTP on loopback, and the NATIVE /api/chat protocol.
     .wire = Wire::OpenAIChat, .lifetime = Lifetime::PerCall,
     .auth = AuthStyle::None, .is_local = true,
     .host = "localhost", .path = "/api/chat", .models_path = "/api/tags",
     .port = 11434, .use_tls = false, .native_api = true},

    // NOTE: no "llama.cpp" preset row. A llama.cpp / vLLM / LM Studio server is
    // just a generic OpenAI-compatible host — use "Custom host…" (which the
    // picker offers, saves, and can delete). The from_spec("llama.cpp") alias in
    // openai/transport.cpp is kept so `--provider llama.cpp` on the CLI still
    // resolves to localhost:8080.
}};

// All presets, for the picker / iteration.
[[nodiscard]] inline std::span<const ProviderPreset> providers() noexcept {
    return {kProviders.data(), kProviders.size()};
}

// Look up a preset by its canonical id. Returns nullptr for an unknown id
// (e.g. a raw "host:port" custom endpoint, which has no preset row).
[[nodiscard]] inline const ProviderPreset* preset_for(std::string_view id) noexcept {
    for (const auto& p : kProviders)
        if (p.id == id) return &p;
    return nullptr;
}

// ── Compile-time invariants ─────────────────────────────────────────────
// These are what make the single source of truth actually HOLD. The
// `openai` row previously claimed Wire::OpenAIResponses while the
// transport dialled /v1/chat/completions; nothing dispatched on the enum,
// so the contradiction survived review and silently taught the UI to
// promise reasoning text the wire never sends. Now the dialect and the
// path live on one row, and disagreeing is a BUILD ERROR rather than a
// bug someone notices months later.
namespace detail {

[[nodiscard]] constexpr bool ends_with(std::string_view s,
                                      std::string_view suf) noexcept {
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

[[nodiscard]] constexpr bool endpoints_consistent() noexcept {
    for (const auto& p : kProviders) {
        if (!p.http_dialled()) {
            // Not dialled over the compat transport ⇒ must not carry half
            // an endpoint (a stray path with no host is how rows rot).
            if (!p.path.empty() || !p.models_path.empty()) return false;
            continue;
        }
        if (p.path.empty() || p.models_path.empty()) return false;
        if (p.port == 0) return false;
        // Anthropic/ACP are never reached over the OpenAI-compat transport.
        if (p.wire == Wire::AnthropicMessages || p.wire == Wire::Acp)
            return false;
        if (p.native_api) continue;   // Ollama's /api/chat is its own shape.
        if (p.wire == Wire::OpenAIChat
            && !ends_with(p.path, "/chat/completions")) return false;
        if (p.wire == Wire::OpenAIResponses
            && !ends_with(p.path, "/responses")) return false;
    }
    return true;
}

// Ids are the primary key: a duplicate makes preset_for() ambiguous.
[[nodiscard]] constexpr bool ids_unique() noexcept {
    for (std::size_t i = 0; i < kProviders.size(); ++i)
        for (std::size_t j = i + 1; j < kProviders.size(); ++j)
            if (kProviders[i].id == kProviders[j].id) return false;
    return true;
}

// Auth capabilities must be internally consistent. These fields replaced a
// chain of provider-name compares in login.cpp, so a row that contradicts
// itself is exactly the bug the migration set out to make impossible — catch
// it at build time rather than at a user's login prompt.
[[nodiscard]] constexpr bool auth_caps_consistent() noexcept {
    for (const auto& p : kProviders) {
        // A method menu only makes sense when there IS more than one method.
        if (p.method_menu && p.auth != AuthStyle::OAuthOrKey) return false;
        // Device login is an OAuth flow: a row that also advertises a key
        // env var would present two conflicting paths to the same account.
        if (p.device_login && p.auth != AuthStyle::None) return false;
        // Local backends never authenticate at all.
        if (p.is_local
            && (p.device_login || p.method_menu || p.token_in_transport))
            return false;
        // A transport-held token implies an OAuth backend, never a keyed
        // one — an API key is resolvable, so it would never need the cached
        // header cleared. NOT equality with oauth_native: that flag means
        // specifically "OpenAI-FAMILY OAuth transport" and is false for
        // Anthropic, whose OAuth rides its own AnthropicMessages transport.
        // (An earlier draft asserted the two were equal; this static_assert
        // caught it, which is the point of encoding the rule.)
        if (p.token_in_transport && p.auth == AuthStyle::ApiKey) return false;
        if (p.oauth_native && !p.token_in_transport) return false;
        // The proactive-refresh extra is meaningless without OAuth.
        if (p.oauth_proactive_refresh && p.auth == AuthStyle::ApiKey)
            return false;
    }
    return true;
}

} // namespace detail

static_assert(detail::endpoints_consistent(),
              "provider row: Wire disagrees with the path it dials, or an "
              "HTTP-dialled row is missing endpoint data");
static_assert(detail::ids_unique(), "duplicate provider id in kProviders");
static_assert(detail::auth_caps_consistent(),
              "provider row: auth capability flags contradict the row's "
              "AuthStyle (see ProviderDescriptor's auth capability block)");

// Can this wire DIALECT carry reasoning TEXT back to us, for THIS model?
//
// A model reasoning and a model letting you SEE it are different things.
// OpenAI's Chat Completions dialect does not transmit reasoning text for
// the GPT-5 family: the tokens are generated and billed, but the response
// carries no reasoning_content / summary — that is exclusive to the
// Responses API. Anthropic Messages streams thinking blocks natively, and
// many OpenAI-COMPAT servers (DeepSeek-R1, Mistral, vLLM, OpenRouter) do
// populate reasoning_content on the Chat dialect, which is why the
// transport parses it unconditionally.
//
// `model` matters because a provider is not necessarily ONE dialect.
// GitHub Copilot serves the same account over both: gpt-5* and mai-code-*
// stream over the Responses API (where reasoning summaries DO come back —
// measured), while claude-* and gpt-4.x are chat-only (where they do not).
// Passing an empty model asks the coarser question "could ANY model on this
// provider show reasoning", which is what a provider-level row wants.
[[nodiscard]] bool wire_streams_reasoning_text(
        std::string_view provider_id, std::string_view model = {}) noexcept;

// The default provider's id — first row of the table.
[[nodiscard]] inline std::string_view default_provider_id() noexcept {
    return kProviders.front().id;
}

} // namespace agentty::provider
