#pragma once
// agentty::store — the abstraction over "somewhere threads and settings live".
// The concept is pure domain; concrete adapters (FsStore, in-memory test
// stores, hypothetical cloud sync) live outside this header.

#include <concepts>
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
