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

// Which wire dialect a provider speaks. The runtime branches on this exactly
// once (when building the concrete Provider); everything else is data.
enum class Kind : std::uint8_t { Anthropic, OpenAI, ExternalAcp };

// How a provider authenticates — drives both the UI hint and which env vars
// the auth resolver consults.
enum class AuthStyle : std::uint8_t {
    OAuthOrKey,  // Anthropic: OAuth (Pro/Max) or x-api-key.
    ApiKey,      // hosted OpenAI-family: a bearer key from env / --key.
    None,        // local server (Ollama / llama.cpp): no auth needed.
};

// One backend agentty knows how to reach. POD + string_view so the whole
// table is a constant-initialised `constexpr` array with zero heap.
struct ProviderPreset {
    std::string_view id;        // canonical spec token ("anthropic", "groq").
    std::string_view label;     // display name for the picker / badge.
    std::string_view blurb;     // one-line description (picker trailing text).
    Kind             kind;
    AuthStyle        auth;
    bool             is_local;  // localhost backend — no network key needed.

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
    // the generic OpenAI-compat one. Today only ChatGPT/Codex under Kind::OpenAI
    // sets this (Anthropic OAuth is Kind::Anthropic, so it never needs the
    // flag to be routed). Selection::is_oauth_native() reads this so "this
    // endpoint is special" is a DATUM on the row, not a label compared at the
    // routing/prewarm/login sites — a second such provider just sets the flag.
    bool oauth_native = false;
};

// ── The table ────────────────────────────────────────────────────────────
// Order = display order in the picker. Anthropic first (the default), then
// hosted OpenAI-family by popularity, then the local backend last.
//
// To add a provider: append a row here, and — if it's OpenAI-compatible with
// a non-default wire path — add the matching `Endpoint` arm in
// openai/transport.cpp::from_spec keyed on the same `id`.
inline constexpr std::array<ProviderPreset, 9> kProviders{{
    {"anthropic",  "Anthropic",  "Claude — OAuth (Pro/Max) or API key",
     Kind::Anthropic, AuthStyle::OAuthOrKey, false, {"", "", ""}, "api.anthropic.com"},
    {"openai",     "OpenAI",     "GPT / Codex — api.openai.com",
     Kind::OpenAI,    AuthStyle::ApiKey,     false, {"OPENAI_API_KEY", "CODEX_API_KEY", ""}, ""},
    {"chatgpt",   "ChatGPT",    "Sign in with ChatGPT — Codex models, no API key",
     Kind::OpenAI,    AuthStyle::None,       true,  {"", "", ""}, "chatgpt.com", /*oauth_native=*/true},
    {"groq",       "Groq",       "Llama/Mixtral on Groq LPUs — very fast",
     Kind::OpenAI,    AuthStyle::ApiKey,     false, {"GROQ_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"openrouter", "OpenRouter", "Any model via openrouter.ai",
     Kind::OpenAI,    AuthStyle::ApiKey,     false, {"OPENROUTER_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"together",   "Together",   "Open models on together.ai",
     Kind::OpenAI,    AuthStyle::ApiKey,     false, {"TOGETHER_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"cerebras",   "Cerebras",   "Wafer-scale inference — very fast",
     Kind::OpenAI,    AuthStyle::ApiKey,     false, {"CEREBRAS_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"ollama",     "Ollama",     "Local models at localhost:11434",
     Kind::OpenAI,    AuthStyle::None,       true,  {"", "", ""}, ""},
    {"llama.cpp",  "llama.cpp",  "Local llama.cpp server at localhost:8080",
     Kind::OpenAI,    AuthStyle::None,       true,  {"", "", ""}, ""},
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

// The default provider's id — first row of the table.
[[nodiscard]] inline std::string_view default_provider_id() noexcept {
    return kProviders.front().id;
}

} // namespace agentty::provider
