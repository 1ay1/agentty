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
    // Tool names the user granted "always allow" (PermissionApproveAlways).
    // Persisted so the grant survives restarts — Zed's always_allow rules.
    // Loaded into Model::session_grants at init; note CycleProfile still
    // clears the in-memory set for the session (tightening the profile
    // re-arms prompts), but the grants reload on next launch.
    std::vector<std::string> always_allow_tools;
    // Diff-review workflow toggle. When false, completed edits are
    // treated as auto-accepted: nothing accumulates in the pending
    // change set, the changes strip stays hidden, and Ctrl+R points the
    // user at the palette toggle. Flipped by ToggleDiffReview (command
    // palette → "Toggle diff review"); default on.
    bool review_enabled = true;
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
    { s.load_settings() }    -> std::same_as<Settings>;
    { s.save_settings(settings) } -> std::same_as<void>;
    { s.new_id() }           -> std::convertible_to<ThreadId>;
    { s.title_from(std::string_view{}) } -> std::convertible_to<std::string>;
};

} // namespace agentty::store
