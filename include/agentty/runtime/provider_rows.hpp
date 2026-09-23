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

#include <algorithm>
#include <string>
#include <string_view>
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
    struct NewCustomHost {
        // When the QUERY itself looks like an endpoint, the sentinel stops
        // being a generic "Custom host…" escape hatch and becomes a concrete
        // offer: "Use yolo-auto.com as a custom host". Non-empty here means
        // the row is promoted to the TOP and Enter pre-fills the modal, so
        // the user never retypes what they already typed.
        std::string prefill;
    };

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
    // The endpoint the "Custom host…" row would pre-fill, or empty when it is
    // the plain escape hatch. Lets the view render the promoted phrasing and
    // the reducer seed the modal from one place.
    [[nodiscard]] const std::string* new_host_prefill() const {
        if (auto* c = std::get_if<NewCustomHost>(&kind))
            return c->prefill.empty() ? nullptr : &c->prefill;
        return nullptr;
    }
};

// Does this query look like an ENDPOINT the user wants to dial, rather than
// the name of a built-in provider? True for "yolo-auto.com",
// "localhost:8080", "https://gw.internal/api" — false for "kimi", "grok".
//
// WHY THIS EXISTS: agentty has spoken to any OpenAI-compatible host since
// forever (`--provider host:port`, and the "Custom host…" row), but the picker
// never SAID so. Someone who typed a hostname saw every familiar provider
// vanish and one muted grey row left behind, which reads as "no, you can't".
// The field evidence was a pull request adding a registry row for a service
// the generic path already handled. The feature was there; the UI hid it.
//
// Deliberately loose: a dot, a colon, or a scheme is enough. A false positive
// costs one extra row offering something that would have worked anyway; a
// false negative costs the user the whole feature.
[[nodiscard]] inline bool query_looks_like_host(std::string_view q) {
    if (q.size() < 3) return false;
    if (q.find(' ') != std::string_view::npos) return false;
    if (q.starts_with("http://") || q.starts_with("https://")) return true;
    const auto dot   = q.find('.');
    const auto colon = q.find(':');
    // A dot with something on both sides ("a.com", not ".x" or "x."), or a
    // colon followed by a digit ("localhost:8080").
    if (dot != std::string_view::npos && dot > 0 && dot + 1 < q.size()) return true;
    if (colon != std::string_view::npos && colon + 1 < q.size()
        && q[colon + 1] >= '0' && q[colon + 1] <= '9') return true;
    return false;
}

// Build the provider picker's rows in display order, filtered by `query`.
//
//   [ "Use <query> as a custom host" ]  (FIRST, only when the query is
//                                        endpoint-shaped — see below)
//   [ presets matching query (fuzzy, ranked) ]
//   [ saved custom hosts matching query ]
//   [ ACP agents ]         (shown only when the query is empty)
//   [ "Custom host…" ]      (always last, so the escape hatch is reachable)
//
// `saved_custom_hosts` is passed in (the caller loads Settings once).
//
// TWO RULES EARN THEIR KEEP HERE:
//
// 1. A host-shaped query PROMOTES the sentinel to the top and pre-fills it.
//    Typing an endpoint is an unambiguous statement of intent, and the answer
//    should be the first thing you see, not the last — with Enter going
//    straight to the probe instead of reopening an empty text field.
//
// 2. Saved custom hosts are SEARCHABLE. They used to vanish the moment you
//    typed, which meant the one way to reach a host you had already saved was
//    to scroll past every preset. A saved host is a provider; it belongs in
//    the provider search space.
[[nodiscard]] inline std::vector<ProviderRow> build_provider_rows(
    const std::vector<std::string>& saved_custom_hosts,
    std::string_view query) {
    std::vector<ProviderRow> rows;
    const auto presets = provider::providers();
    rows.reserve(provider::providers().size() + saved_custom_hosts.size() + 4);

    // A saved host that EXACTLY matches the query is already reachable as its
    // own row below; offering to re-add it would be a duplicate.
    const bool already_saved =
        std::find(saved_custom_hosts.begin(), saved_custom_hosts.end(),
                  provider::canonical_spec(query)) != saved_custom_hosts.end();

    const bool offer_host = query_looks_like_host(query) && !already_saved;
    if (offer_host)
        rows.push_back({ProviderRow::NewCustomHost{std::string{query}}});

    // Presets, fuzzy-filtered + ranked by the shared SSOT filter so the reducer
    // and view see the exact same order.
    const std::vector<int> vis = provider::filter_provider_indices(query);
    for (int idx : vis)
        rows.push_back({ProviderRow::Preset{&presets[static_cast<std::size_t>(idx)]}});

    // Saved custom hosts: substring match on the spec (they have no label or
    // blurb to fuzzy-rank, and a host is something you recall by prefix).
    for (const auto& spec : saved_custom_hosts) {
        if (query.empty() || spec.find(query) != std::string::npos)
            rows.push_back({ProviderRow::CustomHost{spec}});
    }

    // ACP agents are not part of the provider text search.
    if (query.empty()) {
        for (auto& agent : provider::enumerate_acp_agents())
            rows.push_back({ProviderRow::Acp{std::move(agent)}});
    }

    // The plain "Custom host…" sentinel is always reachable — unless it is
    // already sitting at the top as a concrete offer.
    if (!offer_host)
        rows.push_back({ProviderRow::NewCustomHost{}});
    return rows;
}

} // namespace agentty::ui
