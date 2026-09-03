#pragma once
// agentty::provider — the ONE enumeration of "things that have models".
//
// The fused model picker draws from every backend the user can actually
// stream from: built-in presets they're signed into, and saved custom hosts
// (a raw "host:port", Ollama, llama.cpp, a private gateway). That list was
// never written down. `refresh_fused_sources` re-derived it inline, and for
// a long time it enumerated ONLY presets — so a saved custom host got no
// catalog, its live /v1/models fetch landed in `available_models`, and the
// picker showed nothing. The menu was simply blank for those users.
//
// The provider PICKER had already learned this lesson: provider_rows.hpp
// says "the composite list is a value", built once so the reducer and the
// view cannot disagree about which rows exist. The CATALOG layer never got
// the same treatment, which is exactly why a source could be forgotten and
// nobody noticed for a release.
//
// So: state it once, here. Adding a backend kind means editing this file and
// nothing else — every consumer follows automatically, and "we forgot to
// enumerate X" stops being a class of bug rather than a thing to remember.
//
// ── Why not just reuse ProviderRow? ──────────────────────────────────────
// ProviderRow answers a different question: what the provider PICKER shows,
// which includes ACP agents (no models to list — they own their own model
// choice) and the "Custom host…" sentinel (not a backend at all). Catalog
// sources are the subset that can answer a models query. Two questions, two
// types, one shared derivation for the part they have in common.

#include <string>
#include <vector>

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/store/store.hpp"

namespace agentty::provider {

// A backend the fused picker can list models for.
struct CatalogSource {
    // Stable identity, and the key every ProviderCatalog is stored under.
    // For a preset this is its registry id ("anthropic"); for a custom host
    // it is the spec the user typed ("api.my-gw.com", "localhost:11434").
    std::string id;
    // What the user sees. The preset's label, or the resolved display name
    // of the host.
    std::string label;
    // True when this came from the registry. Consumers that need the row
    // itself can look it up with preset_for(id) — kept as a flag rather than
    // a pointer so the struct stays copyable and lifetime-free.
    bool is_preset = false;
    // Un-authed presets are surfaced as QUERY-GATED sign-in offers rather
    // than catalogs: typing "mistral" with no account should offer a sign-in
    // instead of a dead end. Custom hosts are never in this state — holding
    // a saved key IS being authed for them.
    bool needs_signin = false;
};

// Everything the fused picker can list models for, in display order:
// registry presets first (registry order), then saved custom hosts (sorted).
//
// Adoption is handled by saved_custom_hosts(), which drops any spec that
// RESOLVES onto a preset row — "api.githubcopilot.com" is the copilot
// preset, not a second backend beside it. Without that, a consumer building
// one entry per element here would produce two catalogs for one endpoint and
// show its models twice.
[[nodiscard]] inline std::vector<CatalogSource> catalog_sources(
    const store::Settings& settings) {
    std::vector<CatalogSource> out;
    const auto presets = providers();
    out.reserve(presets.size() + settings.provider_keys.size());

    for (const auto& p : presets) {
        out.push_back(CatalogSource{
            .id           = std::string{p.id},
            .label        = std::string{p.label},
            .is_preset    = true,
            .needs_signin = !provider_is_authed(p, settings),
        });
    }
    for (const auto& spec : saved_custom_hosts(settings.provider_keys)) {
        out.push_back(CatalogSource{
            .id           = spec,
            .label        = provider_display_name(parse_selection(spec)),
            .is_preset    = false,
            .needs_signin = false,
        });
    }
    return out;
}

} // namespace agentty::provider
