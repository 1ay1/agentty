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
    // the generic OpenAI-compat one. Today only ChatGPT/Codex sets this
    // (Anthropic OAuth speaks AnthropicMessages, so it never needs the flag to
    // be routed). "This endpoint is special" is a DATUM on the row, read at the
    // routing/prewarm/login sites — a second such provider just sets the flag.
    bool oauth_native = false;

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
// To add a provider: append a row here, and — if it's OpenAI-compatible with
// a non-default wire path — add the matching `Endpoint` arm in
// openai/transport.cpp::from_spec keyed on the same `id`.
inline constexpr std::array<ProviderDescriptor, 16> kProviders{{
    {"anthropic",  "Anthropic",  "Claude — OAuth (Pro/Max) or API key",
     Wire::AnthropicMessages, Lifetime::LongLived, AuthStyle::OAuthOrKey, false, {"", "", ""}, "api.anthropic.com"},
    {"openai",     "OpenAI",     "GPT / Codex — api.openai.com",
     Wire::OpenAIResponses,   Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"OPENAI_API_KEY", "CODEX_API_KEY", ""}, ""},
    {"chatgpt",   "ChatGPT",    "Sign in with ChatGPT — Codex models, no API key",
     Wire::OpenAIResponses,   Lifetime::LongLived, AuthStyle::None,       true,  {"", "", ""}, "chatgpt.com", /*oauth_native=*/true},
    {"copilot",   "GitHub Copilot", "Sign in with GitHub — Copilot models, no API key",
     Wire::OpenAIChat,        Lifetime::LongLived, AuthStyle::None,       false, {"", "", ""}, "api.githubcopilot.com", /*oauth_native=*/true},
    {"kimi",      "Kimi",       "Sign in with Kimi — Kimi K2 models, no API key",
     Wire::OpenAIChat,        Lifetime::LongLived, AuthStyle::None,       false, {"", "", ""}, "api.kimi.com", /*oauth_native=*/true},
    {"groq",       "Groq",       "Llama/Mixtral on Groq LPUs — very fast",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"GROQ_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"openrouter", "OpenRouter", "Any model via openrouter.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"OPENROUTER_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"together",   "Together",   "Open models on together.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"TOGETHER_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"cerebras",   "Cerebras",   "Wafer-scale inference — very fast",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"CEREBRAS_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"deepseek",   "DeepSeek",   "DeepSeek V4 — api.deepseek.com",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"DEEPSEEK_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"xai",        "xAI (Grok)", "Grok models — api.x.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"XAI_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"mistral",    "Mistral",    "Mistral / Codestral / Magistral — api.mistral.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"MISTRAL_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"gemini",     "Google Gemini", "Gemini models via the OpenAI-compat API",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"GEMINI_API_KEY", "GOOGLE_API_KEY", "OPENAI_API_KEY"}, ""},
    {"fireworks",  "Fireworks",  "Open models on fireworks.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"FIREWORKS_API_KEY", "OPENAI_API_KEY", ""}, ""},
    {"ollama",     "Ollama",     "Local models at localhost:11434",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::None,       true,  {"", "", ""}, ""},
    {"llama.cpp",  "llama.cpp",  "Local llama.cpp server at localhost:8080",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::None,       true,  {"", "", ""}, ""},
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
