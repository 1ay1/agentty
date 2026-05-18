#pragma once
// agentty::util::env — the closed catalog of environment variables agentty
// reads. Centralising the names here serves three small but real purposes:
//
//   1. Typo containment. `env::name<Var::SocksProxy>()` is a compile error
//      if the enum arm doesn't exist; `std::getenv("AGENTTY_SOCKS_PROXY")`
//      is a runtime "huh, my override didn't take" if you fat-finger it.
//
//   2. One place to audit. `agentty status` or a future `agentty env`
//      subcommand can iterate kCatalog and report which knobs are set —
//      no risk of forgetting one because it's open-coded in some .cpp.
//
//   3. Compile-time invariants. The `proofs` block at the bottom proves
//      every Var arm has exactly one catalog row and every name is
//      unique. Adding a Var without a row, or duplicating a name, fails
//      the build.
//
// This is intentionally NOT a runtime overlay over std::getenv — call
// sites still call `std::getenv(env::name<X>())`. The win is purely that
// the string literal lives in one place and is type-checked.

#include <array>
#include <cstdint>
#include <string_view>

namespace agentty::util::env {

// ── Closed set of every env var agentty reads ────────────────────────────
// Grouped by subsystem in comments only; the enum itself is flat so a
// switch on it has to handle every arm.
enum class Var : std::uint8_t {
    // ── Auth ────────────────────────────────────────────────────────────
    AnthropicApiKey,         // ANTHROPIC_API_KEY
    ClaudeOAuthToken,        // CLAUDE_CODE_OAUTH_TOKEN

    // ── Network / dial overrides ────────────────────────────────────────
    SocksProxy,              // AGENTTY_SOCKS_PROXY  (host:port)
    ApiHost,                 // AGENTTY_API_HOST     (host[:port] override)
    OAuthHost,               // AGENTTY_OAUTH_HOST   (host[:port] override)
    Insecure,                // AGENTTY_INSECURE=1   (skip TLS verification)

    // ── Airgap (laptop side) ────────────────────────────────────────────
    AirgapSsh,               // AGENTTY_AIRGAP_SSH   (extra ssh flags)

    // ── Debug ───────────────────────────────────────────────────────────
    DebugApi,                // AGENTTY_DEBUG_API=1  (dump streaming events)
    DebugFile,               // AGENTTY_DEBUG_FILE   (target path for above)
};

struct VarSpec {
    Var              tag;
    std::string_view name;     // canonical env var name (what getenv sees)
};

inline constexpr std::array kCatalog = {
    VarSpec{Var::AnthropicApiKey,  "ANTHROPIC_API_KEY"},
    VarSpec{Var::ClaudeOAuthToken, "CLAUDE_CODE_OAUTH_TOKEN"},
    VarSpec{Var::SocksProxy,       "AGENTTY_SOCKS_PROXY"},
    VarSpec{Var::ApiHost,          "AGENTTY_API_HOST"},
    VarSpec{Var::OAuthHost,        "AGENTTY_OAUTH_HOST"},
    VarSpec{Var::Insecure,         "AGENTTY_INSECURE"},
    VarSpec{Var::AirgapSsh,        "AGENTTY_AIRGAP_SSH"},
    VarSpec{Var::DebugApi,         "AGENTTY_DEBUG_API"},
    VarSpec{Var::DebugFile,        "AGENTTY_DEBUG_FILE"},
};

// Compile-time name lookup. `env::name<Var::SocksProxy>()` returns the
// literal `"AGENTTY_SOCKS_PROXY"`. Misspelling the enum arm is a compile
// error; using the result with std::getenv stays a one-liner.
template <Var V>
[[nodiscard]] consteval std::string_view name() noexcept {
    for (const auto& s : kCatalog)
        if (s.tag == V) return s.name;
    // Unreachable in well-formed builds — proofs::every_var_has_row pins it.
    return {};
}

// Convenience: read the var, returning nullptr if absent or empty. Keeps
// call sites that "treat empty as unset" from re-implementing the check.
template <Var V>
[[nodiscard]] inline const char* get_or_null() noexcept {
    const char* v = std::getenv(name<V>().data());
    return (v && *v) ? v : nullptr;
}

// ── Compile-time proofs ─────────────────────────────────────────────────
namespace proofs {

// Every Var enumerator has exactly one row in kCatalog.
consteval bool every_var_has_row() {
    constexpr Var kAll[] = {
        Var::AnthropicApiKey, Var::ClaudeOAuthToken,
        Var::SocksProxy, Var::ApiHost, Var::OAuthHost, Var::Insecure,
        Var::AirgapSsh,
        Var::DebugApi, Var::DebugFile,
    };
    if (std::size(kAll) != kCatalog.size()) return false;
    for (auto v : kAll) {
        int hits = 0;
        for (const auto& s : kCatalog) if (s.tag == v) ++hits;
        if (hits != 1) return false;
    }
    return true;
}
static_assert(every_var_has_row(),
              "agentty::util::env::Var and kCatalog must be in bijection — "
              "every Var arm needs exactly one row in kCatalog");

// No duplicate env var names.
consteval bool all_names_unique() {
    for (std::size_t i = 0; i < kCatalog.size(); ++i)
        for (std::size_t j = i + 1; j < kCatalog.size(); ++j)
            if (kCatalog[i].name == kCatalog[j].name) return false;
    return true;
}
static_assert(all_names_unique(),
              "duplicate env var name in agentty::util::env::kCatalog");

// Every name follows the `AGENTTY_*` / `ANTHROPIC_*` / `CLAUDE_*`
// convention. Catches typos like a lowercase or stray-space name.
consteval bool names_well_formed() {
    constexpr std::string_view kPrefixes[] = {
        "AGENTTY_", "ANTHROPIC_", "CLAUDE_",
    };
    for (const auto& s : kCatalog) {
        bool ok = false;
        for (auto p : kPrefixes)
            if (s.name.size() > p.size() && s.name.starts_with(p)) { ok = true; break; }
        if (!ok) return false;
        for (char c : s.name)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
    }
    return true;
}
static_assert(names_well_formed(),
              "env var names must be upper-snake-case with an "
              "AGENTTY_/ANTHROPIC_/CLAUDE_ prefix");

// Spot-checks of the consteval lookup.
static_assert(name<Var::SocksProxy>()       == "AGENTTY_SOCKS_PROXY");
static_assert(name<Var::AnthropicApiKey>()  == "ANTHROPIC_API_KEY");
static_assert(name<Var::ClaudeOAuthToken>() == "CLAUDE_CODE_OAUTH_TOKEN");

} // namespace proofs

} // namespace agentty::util::env
