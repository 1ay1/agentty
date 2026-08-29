#pragma once
// build_fused_rows — the pure ranking core of the unified cross-provider model
// picker. Given every authed provider's catalog (plus optional sign-in offers
// for un-authed providers), the recents (MRU), the active (provider,model),
// and the live query, it produces the ORDERED, SECTIONED FusedRow list the
// reducer selects from and the view renders.
//
// Pure and header-only so it unit-tests without a Model/deps/network. The
// reducer assembles the inputs (which providers are authed, their catalogs)
// via the deps/registry seams; the RANKING lives here so reducer and view
// agree by construction (the SSOT discipline the pickers hold).
//
// Ordering:
//   1. RECENT   — MRU (provider,model) the user actually toggles between,
//                 most-recent first, active pinned at the top with `active`.
//   2. all providers — every authed catalog, fuzzy-ranked by the query;
//                 ties break favorite → provider order → context window.
//   3. sign in to … — un-authed provider offers (only when the query is
//                 empty or fuzzy-matches the provider name), dim, at the end.
//
// Section boundaries are conveyed by `FusedRow::recent` (section 1) and
// `FusedRow::is_signin_offer()` (section 3); everything else is section 2.
// The view inserts a header when the section changes between adjacent rows.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/fuzzy.hpp"

namespace agentty::ui {

using agentty::SigninOffer;   // moved to domain (catalog.hpp); alias for callers

struct FusedInputs {
    const std::vector<ProviderCatalog>* catalogs = nullptr;  // authed providers
    const std::vector<SigninOffer>*     offers   = nullptr;  // un-authed
    const std::vector<ModelRef>*        recents  = nullptr;  // MRU, newest first
    ModelRef    active;                                       // current prov+model
    std::string query;
    int         recent_cap = 6;
};

namespace detail {

// The searchable text for a (provider, model) row: "<label> <model display>".
[[nodiscard]] inline std::string fused_haystack(std::string_view label,
                                                const ModelInfo& mi) {
    std::string h;
    h.reserve(label.size() + 1 + mi.display_name.size() + 1
              + mi.id.value.size());
    h += label;
    h += ' ';
    h += mi.display_name;
    // Also match the raw id so "gpt-5-codex" matches even when the display
    // name is prettified.
    h += ' ';
    h += mi.id.value;
    return h;
}

// Find a model in a catalog by wire id. Returns nullptr if absent.
[[nodiscard]] inline const ModelInfo* find_model(const ProviderCatalog& c,
                                                 std::string_view model_id) {
    for (const auto& mi : c.models)
        if (mi.id.value == model_id) return &mi;
    return nullptr;
}

[[nodiscard]] inline const ProviderCatalog*
find_catalog(const std::vector<ProviderCatalog>& cats, std::string_view pid) {
    for (const auto& c : cats)
        if (c.provider_id == pid) return &c;
    return nullptr;
}

} // namespace detail

// Build the ordered, sectioned fused row list. `catalogs`/`offers`/`recents`
// may be null (treated as empty).
[[nodiscard]] inline std::vector<FusedRow> build_fused_rows(const FusedInputs& in) {
    using detail::find_catalog;
    using detail::find_model;
    using detail::fused_haystack;

    static const std::vector<ProviderCatalog> kNoCatalogs;
    static const std::vector<SigninOffer>     kNoOffers;
    static const std::vector<ModelRef>        kNoRecents;
    const auto& catalogs = in.catalogs ? *in.catalogs : kNoCatalogs;
    const auto& offers   = in.offers   ? *in.offers   : kNoOffers;
    const auto& recents  = in.recents  ? *in.recents  : kNoRecents;

    const std::string& q = in.query;
    const bool no_query = q.empty();

    std::vector<FusedRow> out;
    out.reserve(recents.size() + 32);

    auto matches = [&](std::string_view label, const ModelInfo& mi) -> bool {
        return no_query || fuzzy::matches(fused_haystack(label, mi), q);
    };

    // ── Section 1: RECENT (MRU) ──────────────────────────────────────────
    // The active row is pinned first even if not literally the newest MRU
    // entry, so `●` always leads. De-dup against what we emit here so the
    // "all providers" section never repeats a recent row.
    std::vector<ModelRef> seen;
    auto already = [&](const ModelRef& r) {
        return std::find(seen.begin(), seen.end(), r) != seen.end();
    };
    auto push_recent = [&](const ModelRef& r) {
        if (already(r)) return;
        const ProviderCatalog* c = find_catalog(catalogs, r.provider_id);
        if (!c) return;                              // provider signed out
        const ModelInfo* mi = find_model(*c, r.model_id);
        if (!mi) return;                             // model gone from catalog
        if (!matches(c->label, *mi)) return;
        FusedRow row;
        row.provider_id = r.provider_id;
        row.label       = c->label;
        row.model       = *mi;
        row.authed      = true;
        row.active      = (r == in.active);
        row.recent      = true;
        row.reasons     = effort_capable(ModelCapabilities::from_id(mi->id.value));
        out.push_back(std::move(row));
        seen.push_back(r);
    };

    if (!in.active.empty()) push_recent(in.active);
    for (const auto& r : recents) {
        if (static_cast<int>(seen.size()) >= in.recent_cap) break;
        push_recent(r);
    }

    // ── Section 2: all providers, fuzzy-ranked ───────────────────────────
    struct Scored { FusedRow row; int score; int prov_ord; };
    std::vector<Scored> scored;
    int prov_ord = 0;
    for (const auto& c : catalogs) {
        for (const auto& mi : c.models) {
            ModelRef r{c.provider_id, mi.id.value};
            if (already(r)) continue;                // already in RECENT
            const auto h = fused_haystack(c.label, mi);
            const auto sc = fuzzy::score(h, q);
            if (!no_query && !sc.matched()) continue;
            FusedRow row;
            row.provider_id = c.provider_id;
            row.label       = c.label;
            row.model       = mi;
            row.authed      = true;
            row.active      = (r == in.active);
            row.recent      = false;
            row.reasons     = effort_capable(ModelCapabilities::from_id(mi.id.value));
            scored.push_back({std::move(row), sc.score, prov_ord});
        }
        ++prov_ord;
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const Scored& a, const Scored& b) {
            // favorite first, then fuzzy score, then provider registry order,
            // then wider context window.
            if (a.row.model.favorite != b.row.model.favorite)
                return a.row.model.favorite;
            if (a.score != b.score) return a.score > b.score;
            if (a.prov_ord != b.prov_ord) return a.prov_ord < b.prov_ord;
            return a.row.model.context_window > b.row.model.context_window;
        });
    for (auto& s : scored) out.push_back(std::move(s.row));

    // ── Section 3: sign-in offers (un-authed providers) ──────────────────
    for (const auto& off : offers) {
        if (!no_query && !fuzzy::matches(off.label, q)) continue;
        FusedRow row;
        row.provider_id = off.provider_id;
        row.label       = off.label;
        row.authed      = false;               // model.id stays empty ⇒ offer
        out.push_back(std::move(row));
    }

    return out;
}

} // namespace agentty::ui
