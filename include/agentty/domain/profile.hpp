#pragma once
// agentty::Profile — the permission tier a user is running under.
// Read by both the tool domain (`needs_permission(Profile)`) and the
// provider domain (when surfacing permission-requiring tools).

#include <cstdint>
#include <string_view>

namespace agentty {

enum class Profile : std::uint8_t { Write, Ask, Minimal };

// Write MUST stay the enum's zero value. Two shipped paths depend on it: a
// default-constructed store::Settings uses Profile::Write, and
// persistence::load_settings() resolves a missing "profile" key via
// value("profile", 0) — so a fresh install AND a legacy config both default
// to Write (autonomous, contained by the sandbox + workspace boundary). This
// is an intentional product decision documented at /docs/profiles; reordering
// this enum would silently flip the default and desync the docs. Pinned by
// tests/settings_default_test.cpp at runtime too.
static_assert(static_cast<std::uint8_t>(Profile::Write) == 0,
              "Profile::Write must remain the enum's zero value.");

[[nodiscard]] constexpr std::string_view to_string(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return "Write";
        case Profile::Ask:     return "Ask";
        case Profile::Minimal: return "Minimal";
    }
    return "?";
}

} // namespace agentty
