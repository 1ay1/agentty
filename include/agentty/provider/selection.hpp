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
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"   // ModelInfo
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

    // The provider row this selection names, resolved ONCE by parse_selection.
    //
    // Identity and endpoint are ORTHOGONAL, and collapsing them is what broke
    // the custom-host flow. Identity used to be re-derived on every query by
    // comparing `openai_endpoint.label` against a literal — but pointing a
    // provider at a custom base URL OVERWRITES that label, so the identity was
    // destroyed and a Copilot-backed custom host silently degraded to the
    // generic OpenAI path (empty model list, no error).
    //
    // Carrying the row makes identity a value, not a derivation. Points into
    // the constexpr kProviders table (static storage, never dangles); null
    // only for a genuinely unknown custom host, which correctly has no
    // registry identity.
    const ProviderPreset* row = nullptr;

    // This selection's provider id ("copilot", "kimi", …), or "" for an
    // unrecognised custom host. THE identity accessor — prefer it to any
    // is_X() predicate at new call sites.
    [[nodiscard]] std::string_view provider_id() const noexcept {
        return row ? row->id : std::string_view{};
    }

    // Rides a dedicated long-lived transport with its own OAuth store
    // (ChatGPT/Codex, Copilot, Kimi) rather than the generic key-authed
    // OpenAI path. A new such provider becomes true by setting one row flag.
    [[nodiscard]] bool is_oauth_native() const noexcept {
        return row && row->oauth_native;
    }

    // Identity by registry id — the row's immutable primary key. NOT the
    // display label, which the custom-host flow overwrites.
    [[nodiscard]] bool is_copilot() const noexcept { return provider_id() == "copilot"; }
    [[nodiscard]] bool is_kimi()    const noexcept { return provider_id() == "kimi"; }
    [[nodiscard]] bool is_chatgpt() const noexcept { return provider_id() == "chatgpt"; }
};

// Parse a provider spec into a Selection. Accepts:
//   ""          → Anthropic (default)
//   "anthropic" → Anthropic
//   "openai" / "groq" / "openrouter" / "together" / "cerebras" / "ollama"
//               → OpenAI-compatible with the matching Endpoint preset
//   "host[:port]" → OpenAI-compatible against a custom base URL
[[nodiscard]] Selection parse_selection(std::string_view spec);

// CANONICALISE a user-typed provider spec before it becomes an IDENTITY.
// The raw string is used as (1) the wire spec, (2) the settings key for
// provider_keys / provider_models, (3) the picker row identity, and (4) the
// display label — so two spellings of the same endpoint ("http://h:8080" vs
// "http://h:8080/") would otherwise split keys, models, and rows. This was
// the root cause behind the whole custom-host PR series (#8/#10/#11/#17/#19
// each patched one downstream site): normalisation must happen ONCE, at
// every entry point (CLI --provider, the TUI custom-host modal), before the
// spec is persisted or compared. Rules: trim whitespace; strip trailing
// '/' (but never the scheme's "//"); presets and ACP ids pass through
// untouched. The "#name" account fragment is PRESERVED — it is part of the
// identity by design.
[[nodiscard]] inline std::string canonical_spec(std::string_view spec) {
    std::size_t b = 0, e = spec.size();
    while (b < e && (spec[b] == ' ' || spec[b] == '\t')) ++b;
    while (e > b && (spec[e - 1] == ' ' || spec[e - 1] == '\t'
                     || spec[e - 1] == '\r' || spec[e - 1] == '\n')) --e;
    std::string s{spec.substr(b, e - b)};
    while (s.size() > 1 && s.back() == '/' && s[s.size() - 2] != '/')
        s.pop_back();
    return s;
}

// ── Persist-on-success for CLI custom hosts ───────────────────────
// A custom --provider spec is raw INTENT until the host answers something;
// persisting it at parse time let a typo poison settings.json for every
// future bare launch. main.cpp registers the spec (and the -m recall to
// file with it) here instead of writing settings; the ModelsLoaded reducer
// calls take_unproven_spec() when a NON-EMPTY catalog lands for the active
// provider and persists then — proof-gated stickiness. Known presets never
// register (they persist at parse, as always). Thread-safe: set once on
// the main thread before the UI starts; consumed on the UI thread.
void set_unproven_spec(std::string spec, std::string model_recall);
// Returns the registered (spec, model_recall) if `spec_now` matches the
// pending one, clearing it; nullopt otherwise (nothing pending / different
// provider active by the time models landed).
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
take_unproven_spec(std::string_view spec_now);

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

// The host to warm before the first turn, fully derived from a Selection.
// `host` empty ⇒ nothing to warm (local backend / ACP subprocess / port-0
// sentinel). `override_host`/`override_port` carry the AGENTTY_API_HOST dial
// (Anthropic only) so the warm connection targets the real upstream. This is
// a PURE function of (Selection, registry, endpoint) — no globals, no I/O —
// so the prewarm routing table is unit-testable; prewarm_active_provider() is
// then just "resolve target, open socket".
struct PrewarmTarget {
    std::string   host;             // "" ⇒ skip prewarm
    std::uint16_t port = 443;
    std::string   override_host;    // AGENTTY_API_HOST host (Anthropic), else ""
    std::uint16_t override_port = 0;

    [[nodiscard]] bool should_warm() const noexcept { return !host.empty(); }
};

// Resolve the prewarm target for a selection. Registry-driven: a preset row's
// `prewarm_host` (Anthropic → api.anthropic.com, ChatGPT → chatgpt.com) wins;
// otherwise the OpenAI-compat Endpoint's own host is used, skipped for local /
// no-TLS / port-0 endpoints. Passed a Selection explicitly so tests can drive
// it; prewarm_active_provider() calls it on active().
[[nodiscard]] PrewarmTarget prewarm_target(const Selection& sel);

// Open a TCP+TLS connection to the ACTIVE provider's host on a detached
// background thread, parking it in the http client's pool so the first real
// request skips the cold handshake. Uniform across native backends — Anthropic
// and ChatGPT/Codex (and any hosted OpenAI-family endpoint) get the same head
// start. Locals (Ollama / llama.cpp) and ACP subprocesses are no-ops.
// Idempotent per (host,port); safe to call fire-and-forget from any thread.
// A thin wrapper over prewarm_target(active()) — the routing lives there.
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

// Fetch the model catalog for a selection — the ONE model-list router.
// Replaces the `is_chatgpt → OpenAI → Anthropic` ladder that fetch_models()
// hand-wrote: it dispatches on the same axes as the stream path (the Wire
// dialect + the oauth_native flag), so the picker and the transport can never
// disagree about which backend a selection names. ExternalAcp returns empty
// (an ACP agent picks its own model and exposes no catalog). `auth` is the
// active credential (used by the Anthropic / OpenAI-compat catalog endpoints;
// ChatGPT reads its own in-process OAuth creds and ignores it).
[[nodiscard]] std::vector<ModelInfo> list_models_for(
    const Selection& sel, const auth::AuthHeader& auth);

// ── Provider-picker search filter (shared by the reducer + the view) ────────
// Returns the indices into `providers()` whose id/label/blurb match `query`
// (empty query = all, in registry order). Fuzzy-scored and ranked so "kimi"
// or "ds" floats the right row up. Used by BOTH picker.cpp (index ↔ preset
// resolution on select/move) and pickers.cpp (row rendering) so the filtered
// list can never disagree between them. Defined in selection.cpp.
[[nodiscard]] std::vector<int> filter_provider_indices(std::string_view query);

// The user's saved custom OpenAI-compatible hosts, sorted. These are the
// Settings.provider_keys entries that are NOT built-in presets (e.g. a raw
// "host:port" endpoint). Both the provider-picker VIEW and its reducer build
// this list; keeping it here guarantees they render and index the SAME rows in
// the SAME order (a drift here would map a selection to the wrong host).
[[nodiscard]] inline std::vector<std::string> saved_custom_hosts(
    const std::map<std::string, std::string>& provider_keys) {
    std::vector<std::string> hosts;
    for (const auto& [spec, key] : provider_keys)
        if (!preset_for(spec)) hosts.push_back(spec);
    std::sort(hosts.begin(), hosts.end());
    return hosts;
}

} // namespace agentty::provider
