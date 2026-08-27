#pragma once
// agentty::store — the abstraction over "somewhere threads and settings live".
// The concept is pure domain; concrete adapters (FsStore, in-memory test
// stores, hypothetical cloud sync) live outside this header.

#include <concepts>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/profile.hpp"

namespace agentty::store {

// Persisted RAG configuration. The USER-FACING surface is a single mode
// (the RAG picker cycles it); every other field is an internal default the
// adapter still honours (tunable via env for power users) but the UI no
// longer exposes — keeping the picker to one decision. `configured=false`
// means the user never touched the picker, so the adapter keeps its
// env-derived defaults; once the picker commits it flips true and `mode`
// becomes authoritative for the proactive/pre-turn behaviour.
enum class RagMode : std::uint8_t {
    On = 0,        // proactive pre-turn retrieval on every turn
    FirstTurnOnly, // proactive retrieval only on a thread's first turn
    Off,           // no proactive injection (search_docs/search_code still work)
};

[[nodiscard]] constexpr std::string_view to_string(RagMode m) noexcept {
    switch (m) {
        case RagMode::On:            return "On";
        case RagMode::FirstTurnOnly: return "First turn only";
        case RagMode::Off:           return "Off";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view describe(RagMode m) noexcept {
    switch (m) {
        case RagMode::On:            return "inject retrieved context before every turn";
        case RagMode::FirstTurnOnly: return "ground the first turn, then stay quiet";
        case RagMode::Off:           return "no pre-turn injection (tools still work)";
    }
    return "";
}

struct RagConfig {
    bool     configured = false;   // has the user ever saved RAG settings?
    RagMode  mode       = RagMode::On;

    // ── Internal defaults (not picker-exposed; env-tunable) ──
    // Knowledge sources.
    bool  skills        = true;
    bool  memory        = true;
    bool  mcp_resources = false;

    // Pipeline stages.
    bool  contextual = true;
    bool  dedup      = true;
    bool  mmr        = true;
    bool  stitch     = true;
    bool  autocut    = true;

    // Power modes (latency / round-trip cost).
    bool  prf        = false;   // pseudo-relevance-feedback expansion
    bool  corrective = false;   // CRAG grading
    bool  graph      = false;   // GraphRAG expansion
    bool  expand     = false;   // multi-query / RAG-Fusion
    bool  hyde       = false;   // HyDE

    // Fusion.
    std::string fusion = "convex";   // "convex" | "rrf"
    bool  adaptive_fusion = true;

    // Proactive / pre-turn injection tuning. `proactive` is DERIVED from
    // `mode` (On/FirstTurnOnly ⇒ true, Off ⇒ false) at apply time; the bar
    // and byte cap stay internal defaults.
    bool   proactive          = true;
    double proactive_min_conf = 0.35;
    int    proactive_bytes    = 6144;

    // Infrastructure.
    bool  persist = true;   // .ragdb cache under .agentty/
    bool  learn   = false;  // implicit file-open feedback loop
    bool  trace   = false;  // fold per-stage trace into the mode label
};

// Persisted user settings — model + profile + favorites.  Lives with the
// Store concept because it's what the Store reads/writes, not because
// settings are themselves a first-class domain.
struct Settings {
    ModelId              model_id;
    Profile              profile = Profile::Write;
    std::vector<ModelId> favorite_models;
    // Active LLM backend. Empty / "anthropic" = the default Claude path
    // (OAuth/Pro/Max). Any other value ("openai" | "groq" | "openrouter" |
    // "together" | "cerebras" | "ollama" | "host[:port]") routes through
    // the OpenAI-compatible transport. Set by `--provider`; consulted at
    // startup in main.cpp.
    std::string          provider;
    // Per-provider API keys entered via the in-app login modal, keyed by
    // the provider's canonical id ("openai", "groq", …). A saved key here
    // takes precedence over the env-var chain so a user who pasted a key
    // once doesn't have to re-export it every shell. Anthropic is NOT
    // stored here — its creds live in credentials.json.
    std::map<std::string, std::string> provider_keys;
    // Last model selected per provider, keyed by canonical provider id
    // ("anthropic", "openai", "ollama", …). Lets a provider switch restore
    // the model the user last used on that backend instead of carrying a
    // model id that doesn't exist on the new provider. The global `model_id`
    // above stays the active model; this map is just the per-provider recall.
    std::map<std::string, std::string> provider_models;
    // Reasoning effort tier (output_config.effort wire value, e.g. "high";
    // empty = off, the default). Reloaded into Model::effort at startup.
    std::string          effort;
    // Per-model reasoning-effort capability overrides (issue #20). Keyed by
    // model id; true = force the effort knob ON for a compat model the
    // catalog doesn't recognize as a reasoner, false = force it OFF for one
    // that 400s on `reasoning_effort`. Absent id = use catalog inference.
    // Pushed into the catalog's override registry at startup and on every
    // in-app toggle (^E in the model picker). Claude/GPT stay family-gated.
    std::map<std::string, bool> reasoning_effort_overrides;
    // Tool names the user granted "always allow" (PermissionApproveAlways).
    // Persisted so the grant survives restarts — Zed's always_allow rules.
    // Loaded into Model::session_grants at init; note CycleProfile still
    // clears the in-memory set for the session (tightening the profile
    // re-arms prompts), but the grants reload on next launch.
    std::vector<std::string> always_allow_tools;
    // DISCOVERED entitlement fact: this account's subscription rejected the
    // context-1m-2025-08-07 beta (HTTP 400 "long context beta is not yet
    // available for this subscription"). We only learn this by trying — the
    // OAuth token doesn't say. Once set: the catalog stops offering `[1m]`
    // picker variants and the wire never sends the beta header. Cleared on
    // sign-out / account switch (the next account may be entitled).
    bool                 context_1m_blocked = false;
    // User-configured RAG behaviour (the RAG settings picker). Defaults to
    // configured=false ⇒ the adapter keeps its env-derived config.
    RagConfig rag;

    // Smart Mode (role-based execution routing, docs/design/smart-mode.md).
    // Off by default. The three slot fields are WIRE model ids the user
    // pinned for each role; empty = auto-fill from the catalog. Effort
    // strings mirror the `effort` field's grammar (""/"low"/"medium"/…).
    // Reloaded into Model::Domain::smart at startup.
    bool                 smart_enabled = false;
    bool                 smart_route_internal  = true;
    bool                 smart_orchestrate     = true;
    bool                 smart_route_subagents = true;
    bool                 smart_learn_routing    = true;
    bool                 smart_outcome_feedback = true;
    bool                 smart_speculative      = false;
    bool                 smart_recall_plans     = true;
    std::string          smart_strategic_model,      smart_strategic_effort;
    std::string          smart_impl_model,           smart_impl_effort;
    std::string          smart_utility_model,        smart_utility_effort;

    // Show the persistent "N changes" review strip after the agent edits
    // files (the banner above the composer). OFF by default — edits apply
    // quietly; turn it on (Ctrl+K → "Changes strip") for the always-on review
    // banner. Ctrl+R / "Review changes" opens the review pane either way.
    bool                 show_changes_strip = false;
    // Show the model's reasoning/thinking. When on: the transcript renders a
    // reasoning block (live thought stream, then a one-line "Thought for N
    // tokens" summary), AND Anthropic requests VISIBLE thinking (the
    // interleaved-thinking beta) so its deltas aren't redacted. Off by default
    // — keeps the wire lean and the transcript focused on the answer. Uniform
    // across every provider (Anthropic / Codex / OpenAI-compat reasoners).
    // Toggle: Ctrl+K → "Reasoning".
    bool                 show_reasoning = false;
};

template <class S>
concept Store = requires(S& s, const Thread& t, const ThreadId& id,
                         const Settings& settings) {
    // load_threads returns thread *metadata only* (id, title, timestamps).
    // The messages vector on each returned Thread is empty — full bodies
    // are fetched lazily via load_thread on selection. This keeps startup
    // RAM proportional to thread count, not total transcript bytes.
    { s.load_threads() }     -> std::same_as<std::vector<Thread>>;
    { s.load_thread(id) }    -> std::same_as<std::optional<Thread>>;
    { s.save_thread(t) }     -> std::same_as<void>;
    { s.delete_thread(id) }  -> std::same_as<void>;
    { s.load_settings() }    -> std::same_as<Settings>;
    { s.save_settings(settings) } -> std::same_as<void>;
    { s.new_id() }           -> std::convertible_to<ThreadId>;
    { s.title_from(std::string_view{}) } -> std::convertible_to<std::string>;
};

} // namespace agentty::store
