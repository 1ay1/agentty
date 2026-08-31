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
    {"anthropic",  "Anthropic",  "Claude — OAuth (Pro/Max) or API key",
     Wire::AnthropicMessages, Lifetime::LongLived, AuthStyle::OAuthOrKey, false, {"", "", ""}, "api.anthropic.com", false,
     // Own transport (not the OpenAI-compat one): no endpoint columns.
     "", "", ""},
    {"openai",     "OpenAI",     "GPT / Codex — api.openai.com",
     // Chat Completions, NOT Responses. The row used to claim
     // Wire::OpenAIResponses while dialling /v1/chat/completions — the
     // label was decorative (nothing dispatched on it) so the lie went
     // unnoticed and made the reasoning-text UI over-promise. The wire
     // and the path now sit on one line and must agree.
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"OPENAI_API_KEY", "CODEX_API_KEY", ""}, "", false,
     "api.openai.com", "/v1/chat/completions", "/v1/models"},
    {"chatgpt",   "ChatGPT",    "Sign in with ChatGPT — Codex models, no API key",
     // Genuinely Responses — but over its OWN OAuth transport
     // (/backend-api/codex/responses), not the compat one, so no columns.
     Wire::OpenAIResponses,   Lifetime::LongLived, AuthStyle::None,       true,  {"", "", ""}, "chatgpt.com", /*oauth_native=*/true,
     "", "", ""},
    {"copilot",   "GitHub Copilot", "Sign in with GitHub — Copilot models, no API key",
     Wire::OpenAIChat,        Lifetime::LongLived, AuthStyle::None,       false, {"", "", ""}, "api.githubcopilot.com", /*oauth_native=*/true,
     "api.githubcopilot.com", "/chat/completions", "/models"},
    {"kimi",      "Kimi",       "Sign in with Kimi — Kimi K2 models, no API key",
     // Kimi Code inference API: base https://api.kimi.com/coding/v1 (its
     // own provider builds this; the columns must agree with it).
     Wire::OpenAIChat,        Lifetime::LongLived, AuthStyle::None,       false, {"", "", ""}, "api.kimi.com", /*oauth_native=*/true,
     "api.kimi.com", "/coding/v1/chat/completions", "/coding/v1/models"},
    {"groq",       "Groq",       "Llama/Mixtral on Groq LPUs — very fast",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"GROQ_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.groq.com", "/openai/v1/chat/completions", "/openai/v1/models"},
    {"openrouter", "OpenRouter", "Any model via openrouter.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"OPENROUTER_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "openrouter.ai", "/api/v1/chat/completions", "/api/v1/models"},
    {"together",   "Together",   "Open models on together.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"TOGETHER_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.together.xyz", "/v1/chat/completions", "/v1/models"},
    {"cerebras",   "Cerebras",   "Wafer-scale inference — very fast",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"CEREBRAS_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.cerebras.ai", "/v1/chat/completions", "/v1/models"},
    {"deepseek",   "DeepSeek",   "DeepSeek V4 — api.deepseek.com",
     // DeepSeek serves completions WITHOUT the /v1 prefix.
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"DEEPSEEK_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.deepseek.com", "/chat/completions", "/models"},
    {"xai",        "xAI (Grok)", "Grok models — api.x.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"XAI_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.x.ai", "/v1/chat/completions", "/v1/models"},
    {"mistral",    "Mistral",    "Mistral / Codestral / Magistral — api.mistral.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"MISTRAL_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.mistral.ai", "/v1/chat/completions", "/v1/models"},
    {"gemini",     "Google Gemini", "Gemini models via the OpenAI-compat API",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"GEMINI_API_KEY", "GOOGLE_API_KEY", "OPENAI_API_KEY"}, "", false,
     "generativelanguage.googleapis.com", "/v1beta/openai/chat/completions", "/v1beta/openai/models"},
    {"fireworks",  "Fireworks",  "Open models on fireworks.ai",
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::ApiKey,     false, {"FIREWORKS_API_KEY", "OPENAI_API_KEY", ""}, "", false,
     "api.fireworks.ai", "/inference/v1/chat/completions", "/inference/v1/models"},
    {"ollama",     "Ollama",     "Local models at localhost:11434",
     // Plain HTTP on loopback, and the NATIVE /api/chat protocol.
     Wire::OpenAIChat,        Lifetime::PerCall,   AuthStyle::None,       true,  {"", "", ""}, "", false,
     "localhost", "/api/chat", "/api/tags", /*port=*/11434, /*use_tls=*/false,
     /*native_api=*/true},
    // NOTE: no "llama.cpp" preset row. A llama.cpp / vLLM / LM Studio server is
    // just a generic OpenAI-compatible host — use "Custom host…" (which the
    // picker offers, saves, and can delete). The from_spec("llama.cpp") alias in
    // openai/transport.cpp is kept so `--provider llama.cpp` on the CLI still
    // resolves to localhost:8080. Having BOTH a preset row and the custom-host
    // flow was the confusing duplicate behind the "dead loops when prompted"
    // report (the preset switched async and could be prompted before its
    // /models fetch landed; the empty-model guard in submit_message now stops
    // that dead-loop regardless).
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

} // namespace detail

static_assert(detail::endpoints_consistent(),
              "provider row: Wire disagrees with the path it dials, or an "
              "HTTP-dialled row is missing endpoint data");
static_assert(detail::ids_unique(), "duplicate provider id in kProviders");

// Can this wire DIALECT carry reasoning TEXT back to us?
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
// So this is deliberately about the FIRST-PARTY OpenAI/Copilot case:
// callers use it to explain that reasoning is happening but cannot be
// displayed, rather than showing a "shown" toggle that can never produce
// output. It takes the provider id (not just the Wire) precisely because
// the same dialect behaves differently across hosts.
[[nodiscard]] inline bool wire_streams_reasoning_text(
        std::string_view provider_id) noexcept {
    const ProviderPreset* p = preset_for(provider_id);
    if (!p) return true;                       // unknown/custom host: assume yes
    if (p->wire != Wire::OpenAIChat) return true;
    // The FIRST-PARTY OpenAI-family hosts on this dialect hide it:
    //   • copilot — routes GPT-5 through /chat/completions.
    //   • openai  — agentty dials api.openai.com/v1/chat/completions too.
    // The `openai` row used to be mislabelled Wire::OpenAIResponses, which
    // made this predicate answer "yes" BY ACCIDENT (it only checked the
    // dialect) and let the picker promise reasoning text the wire never
    // sends. The genuinely-Responses path is the `chatgpt` provider, which
    // has its own transport. Every
    // other OpenAI-COMPAT host on this dialect (DeepSeek, Mistral,
    // OpenRouter, vLLM, Ollama) populates reasoning_content.
    return provider_id != "copilot" && provider_id != "openai";
}

// The default provider's id — first row of the table.
[[nodiscard]] inline std::string_view default_provider_id() noexcept {
    return kProviders.front().id;
}

} // namespace agentty::provider
