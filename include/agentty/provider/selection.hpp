#pragma once
// agentty::provider — process-wide selection of which backend the runtime
// talks to. The hot path (Deps::stream) is already type-erased, so the
// reducer / view never name a concrete provider. Two call sites still need
// to know the active backend out-of-band:
//
//   • cmd::fetch_models() — routes to the right list_models() impl.
//   • main.cpp / ACP / subagent — construct the right concrete Provider.
//
// This header centralises that choice. `select()` is called once at startup
// from main(); `active()` is read wherever the out-of-band routing is needed.

#include <cstdint>
#include <string>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/registry.hpp"

namespace agentty::provider {

// `Kind` lives in registry.hpp (the single source of truth for backend
// metadata); Selection reuses it.

// The resolved active-provider config. For OpenAI-family backends the
// Endpoint carries the base URL / port / TLS / label; for Anthropic it's
// unused (the anthropic transport hardcodes api.anthropic.com).
struct Selection {
    Kind             kind = Kind::Anthropic;
    openai::Endpoint openai_endpoint;   // meaningful only when kind == OpenAI
    // The external ACP agent's spec id ("claude-agent-acp", "codex-acp", or a
    // config-defined id). Meaningful only when kind == ExternalAcp; the runtime
    // hands it to stream_external_acp() to spawn/drive the right subprocess.
    std::string      acp_agent_id;

    // The native ChatGPT/Codex OAuth backend: an OpenAI-Kind selection whose
    // endpoint label is "chatgpt". This ONE predicate replaces the
    // `kind == Kind::OpenAI && openai_endpoint.label == "chatgpt"` idiom that
    // used to be re-derived at ~6 call sites (dispatch, prewarm, model list,
    // effort ladder, login gate, picker). "chatgpt is special" is now a
    // capability read from one place, not a string compared everywhere.
    [[nodiscard]] bool is_chatgpt() const noexcept {
        return kind == Kind::OpenAI && openai_endpoint.label == "chatgpt";
    }
};

// Parse a provider spec into a Selection. Accepts:
//   ""          → Anthropic (default)
//   "anthropic" → Anthropic
//   "openai" / "groq" / "openrouter" / "together" / "cerebras" / "ollama"
//               → OpenAI-compatible with the matching Endpoint preset
//   "host[:port]" → OpenAI-compatible against a custom base URL
[[nodiscard]] Selection parse_selection(std::string_view spec);

// Session-wide custom auth header NAME (--auth-header) for OpenAI-family
// backends whose gateway doesn't accept `Authorization: Bearer` (e.g.
// `X-API-Key`). Stored process-globally so every parse_selection — startup
// AND live provider switches from the picker — stamps it onto the resulting
// Endpoint. Empty (the default) keeps the standard bearer header.
void set_custom_auth_header(std::string name);
[[nodiscard]] std::string custom_auth_header();

// Install the active selection (process-global). Called at startup and by
// the provider-picker reducer for live switches (UI thread).
void select(Selection s);

// Read the active selection. Defaults to Anthropic before select() runs.
// Returns a BY-VALUE snapshot taken under the selection mutex: the stream
// worker thread reads this (launch_stream's task / run_stream_sync) while
// the UI thread may be mid-`select()` from the provider picker. Handing
// back a reference let the worker observe a torn move-assign of the
// embedded endpoint strings (data race → possible crash); the snapshot
// makes every read a consistent copy.
[[nodiscard]] Selection active();

// Human display name for the active backend — "Anthropic", "Groq",
// "Ollama", "OpenAI", or the raw endpoint label for a custom host with
// no preset row. Used by the status bar provider badge.
[[nodiscard]] std::string provider_display_name(const Selection& s);

// Open a TCP+TLS connection to the ACTIVE provider's host on a detached
// background thread, parking it in the http client's pool so the first real
// request skips the cold handshake. Uniform across native backends — Anthropic
// and ChatGPT/Codex (and any hosted OpenAI-family endpoint) get the same head
// start. Locals (Ollama / llama.cpp) and ACP subprocesses are no-ops.
// Idempotent per (host,port); safe to call fire-and-forget from any thread.
void prewarm_active_provider();

// Resolve the AuthHeader for a provider spec, registry-driven.
//   • Anthropic   → derived from `anthropic_creds` (OAuth / x-api-key from
//                   `agentty login`), passed in by the caller.
//   • OpenAI-family → a bearer key, in precedence order: `cli_key` (--key)
//                   > `saved_key` (pasted in-app, from Settings.provider_keys)
//                   > the first non-empty env var in the preset's auth_env.
//   • Local (Ollama) → an empty ApiKeyHeader (the local server needs no auth).
// Used by main.cpp at startup AND by the provider-picker reducer for live
// switches, so the two can never disagree about how a backend authenticates.
[[nodiscard]] auth::AuthHeader resolve_auth_for(
    std::string_view spec,
    const auth::AuthHeader& anthropic_creds,
    std::string_view cli_key = {},
    std::string_view saved_key = {});

} // namespace agentty::provider
