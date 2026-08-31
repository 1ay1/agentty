#pragma once
// agentty::auth::vault — ONE descriptor per credential store, answering
// EVERY auth question agentty asks about a provider.
//
// The multi-provider auth reality: N providers signed in simultaneously,
// each with its own storage shape, the active Selection just a pointer.
// Before this header, that reality was implemented as five independent
// per-provider if-ladders spread over four files:
//
//   account_switch.cpp  backend_for()          — where does the blob live?
//   login.cpp           sign_out()             — how do I clear it?
//   login.cpp           account_provider_id()  — what's the registry id?
//   login.cpp           signout_provider_id()  — (a byte-level clone of ^)
//   auth_state.cpp      provider_is_authed()   — is something saved?
//   credentials.cpp     resolve()              — what header goes on the wire?
//
// Every new provider (and both times a provider was RENAMED) meant finding
// all five ladders; missing one produced exactly the class of bug we kept
// shipping fixes for (sign-out clearing the wrong store, ^D leaving the
// live header armed, accounts drilling into the wrong provider).
//
// Now: `vault::of(provider_id)` returns the ONE descriptor — a small value
// type of function pointers + data, table-driven like provider registry
// rows — and the five call sites become one-liners. Adding a provider's
// auth story = adding ONE VaultDesc row. The compiler doesn't let a row be
// partial (aggregate init with designated initializers, every question
// answered or explicitly defaulted).
//
// Design notes (the "modern C++" here is restraint, not cleverness):
//   • A flat constexpr-friendly table of plain function pointers — no
//     virtual hierarchy, no std::function allocations, no registration
//     side effects at static-init time. The descriptor IS the interface.
//   • Identity is agentty's provider id (capkey RULE 2's namespace);
//     custom hosts — whose id is their endpoint spec — fall through to a
//     synthesized SettingsKey descriptor, so `of()` is total: every string
//     yields a usable vault.
//   • Questions are separated from POLICY: the vault knows how to clear a
//     store; whether sign-out should then fall back to another provider
//     or open the login modal stays in the reducer, where policy belongs.

#include <optional>
#include <string>
#include <string_view>

#include "agentty/auth/auth.hpp"

namespace agentty::auth::vault {

// How a provider's ACTIVE credential is stored.
enum class Kind : std::uint8_t {
    AnthropicFile,   // credentials.json (OAuth or x-api-key), sealed
    OAuthFile,       // provider-owned token file (ChatGPT/Copilot/Kimi)
    SettingsKey,     // Settings.provider_keys[id] — hosted keys + custom hosts
    None,            // local/no-auth backends (Ollama): nothing to store
};

struct Desc {
    std::string_view id;      // agentty provider id ("" ⇒ synthesized)
    Kind             kind = Kind::SettingsKey;

    // The five questions, answered uniformly. Function pointers (not
    // std::function): stateless, no allocation, trivially copyable.
    // `provider` is passed back in so SettingsKey descriptors (shared by
    // every hosted-key provider AND every custom host) know which slot.
    bool (*is_signed_in)(const std::string& provider);
    void (*clear)(const std::string& provider);          // sign out
    auth::AuthHeader (*resolve)(const std::string& provider);
    // Display label for the CURRENT credential (account switcher rows).
    std::string (*current_label)(const std::string& provider);
    // Post-activate hook (keystore re-seal / token-cache bust). May be null.
    void (*after_activate)(const std::string& provider);
};

// The total lookup: registry providers get their row; anything else is a
// custom host and gets the synthesized SettingsKey descriptor. Never fails.
[[nodiscard]] const Desc& of(std::string_view provider_id);

// Convenience passthroughs — the call-site vocabulary.
[[nodiscard]] inline bool signed_in(const std::string& p) {
    return of(p).is_signed_in(p);
}
inline void sign_out(const std::string& p) { of(p).clear(p); }
[[nodiscard]] inline auth::AuthHeader resolve(const std::string& p) {
    return of(p).resolve(p);
}

} // namespace agentty::auth::vault
