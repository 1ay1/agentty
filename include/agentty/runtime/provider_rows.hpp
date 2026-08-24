#pragma once
// agentty::ui — the provider picker's row model: ONE ordered list, built once,
// consumed by both the reducer (src/runtime/app/update/picker.cpp) and the view
// (src/runtime/view/pickers.cpp).
//
// Before this, the composite list — built-in presets, then external ACP agents,
// then saved custom hosts, then the "Custom host…" sentinel — lived implicitly
// as `n_presets + n_acp + n_custom + 1` arithmetic duplicated in the reducer and
// the view. Every index (open-at-active, move, jump, select, render) recomputed
// the same offsets by hand, and a search filter would have to reproduce that
// remapping in two places without drifting. That is exactly the class of bug an
// index-into-a-heterogeneous-list invites.
//
// Now the composite list is a value: `build_provider_rows(...)` returns the rows
// in display order, each tagged with what it IS (a preset, an ACP agent, a saved
// host, or the new-host sentinel). The cursor is just an index into THIS vector;
// resolving a selection is `rows[index]`, with no offset math. The search filter
// is applied where the rows are built, so the reducer and view can never
// disagree about which rows are visible or where the cursor lands.

#include <string>
#include <variant>
#include <vector>

#include "agentty/provider/registry.hpp"      // ProviderPreset, providers()
#include "agentty/provider/acp_agents.hpp"     // AcpAgentSpec, enumerate_acp_agents()
#include "agentty/provider/selection.hpp"      // filter_provider_indices, saved_custom_hosts

namespace agentty::ui {

// One row in the provider picker, tagged by kind. The payloads are cheap views
// / copies owned elsewhere for the lifetime of a single reduce/render pass.
struct ProviderRow {
    // A built-in registry provider (anthropic, chatgpt, kimi, groq, …).
    struct Preset { const provider::ProviderPreset* preset; };
    // A configured external ACP agent subprocess row.
    struct Acp { provider::AcpAgentSpec agent; };
    // A saved custom OpenAI-compatible host (a Settings.provider_keys spec that
    // is not a built-in preset), switchable without re-entering its key.
    struct CustomHost { std::string spec; };
    // The trailing "Custom host…" sentinel that opens the free-text modal.
    struct NewCustomHost {};

    std::variant<Preset, Acp, CustomHost, NewCustomHost> kind;

    [[nodiscard]] const provider::ProviderPreset* preset() const {
        if (auto* p = std::get_if<Preset>(&kind)) return p->preset;
        return nullptr;
    }
    [[nodiscard]] const provider::AcpAgentSpec* acp() const {
        if (auto* a = std::get_if<Acp>(&kind)) return &a->agent;
        return nullptr;
    }
    [[nodiscard]] const std::string* custom_host() const {
        if (auto* c = std::get_if<CustomHost>(&kind)) return &c->spec;
        return nullptr;
    }
    [[nodiscard]] bool is_new_custom_host() const {
        return std::holds_alternative<NewCustomHost>(kind);
    }
};

// Build the provider picker's rows in display order, filtered by `query`.
//
//   [ presets matching query (fuzzy, ranked) ]
//   [ ACP agents ]         (shown only when the query is empty — they aren't
//   [ saved custom hosts ]  part of the provider search space)
//   [ "Custom host…" ]      (always last, so the escape hatch is reachable)
//
// `saved_custom_hosts` is passed in (the caller loads Settings once). When
// `query` is non-empty, only preset rows are searched; the ACP / saved-host /
// sentinel rows are hidden so the filtered list reads as "these providers match
// what I typed" rather than a mixed bag. The sentinel is always appended so a
// user can still open the custom-host modal after clearing the query.
[[nodiscard]] inline std::vector<ProviderRow> build_provider_rows(
    const std::vector<std::string>& saved_custom_hosts,
    std::string_view query) {
    std::vector<ProviderRow> rows;
    const auto presets = provider::providers();

    // Presets, fuzzy-filtered + ranked by the shared SSOT filter so the reducer
    // and view see the exact same order.
    const std::vector<int> vis = provider::filter_provider_indices(query);
    rows.reserve(vis.size() + 4);
    for (int idx : vis)
        rows.push_back({ProviderRow::Preset{&presets[static_cast<std::size_t>(idx)]}});

    // ACP agents + saved custom hosts are not part of the provider text search;
    // hide them while filtering so a query narrows to matching providers only.
    if (query.empty()) {
        for (auto& agent : provider::enumerate_acp_agents())
            rows.push_back({ProviderRow::Acp{std::move(agent)}});
        for (const auto& spec : saved_custom_hosts)
            rows.push_back({ProviderRow::CustomHost{spec}});
    }

    // The "Custom host…" sentinel is always reachable.
    rows.push_back({ProviderRow::NewCustomHost{}});
    return rows;
}

} // namespace agentty::ui
