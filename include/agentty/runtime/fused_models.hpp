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

    // The visible model NAME (what the view puts in the row's leading cell).
    auto name_of = [](const ModelInfo& mi) -> std::string_view {
        return mi.display_name.empty() ? std::string_view{mi.id.value}
                                       : std::string_view{mi.display_name};
    };
    // Byte offsets of the query within the NAME, for fzf-style highlighting.
    // Empty when no query, or when the row matched only on the provider name.
    auto name_positions = [&](const ModelInfo& mi) -> std::vector<int> {
        if (no_query) return {};
        return fuzzy::score(name_of(mi), q).positions;
    };

    // ── Section 1: RECENT (MRU) ──────────────────────────────────────────
    // The active row is pinned first even if not literally the newest MRU
    // entry, so `●` always leads. De-dup against what we emit here so the
    // "all providers" section never repeats a recent row. Identity is the
    // capkey-FOLDED (provider, model): providers alias one model under
    // several spellings (mistral-medium-3-5 / -3.5), and a recent pick of
    // one spelling must suppress the catalog's twin — not sit above it.
    std::vector<ModelRef> seen;
    auto already = [&](const ModelRef& r) {
        const std::string folded = capkey::norm_model(r.model_id);
        for (const auto& s : seen)
            if (s.provider_id == r.provider_id
                && capkey::norm_model(s.model_id) == folded)
                return true;
        return false;
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
        row.reasons     = effort_capable(
            resolved_caps(mi->id.value, r.provider_id));
        row.match_positions = name_positions(*mi);
        out.push_back(std::move(row));
        seen.push_back(r);
    };

    if (!in.active.empty()) push_recent(in.active);
    for (const auto& r : recents) {
        if (static_cast<int>(seen.size()) >= in.recent_cap) break;
        push_recent(r);
    }

    // ── Section 2: all providers, fuzzy-ranked ───────────────────────
    // HOT LOOP — runs per keystroke over every model of every catalog (an
    // OpenRouter catalog alone is 300+). Three costs are deliberately kept
    // OUT of it:
    //   • ModelInfo copies: score first, materialise the FusedRow only for
    //     rows that MATCH (with no query every row matches, but that path
    //     runs once per open, not per keystroke).
    //   • fuzzy re-scoring: score once against the cached haystack; reuse
    //     the SAME result's positions when the match lies inside the name
    //     prefix (the common case) instead of re-scoring the bare name.
    //   • resolved_caps: 3 registry map-lookups behind a shared_mutex per
    //     call — memoised per (provider, model) for the duration of this
    //     build; catalogs repeat ids across rebuilds but never within one.
    struct Scored { FusedRow row; int score; int prov_ord; };
    std::vector<Scored> scored;
    scored.reserve(64);
    int prov_ord = 0;
    for (const auto& c : catalogs) {
        const std::size_t nkeys = c.search_keys.size();
        for (std::size_t i = 0; i < c.models.size(); ++i) {
            const auto& mi = c.models[i];
            ModelRef r{c.provider_id, mi.id.value};
            if (already(r)) continue;    // in RECENT, or an alias spelling
                                         // of a row already emitted — the
                                         // catalog itself lists twins
                                         // (mistral-medium-3-5 AND -3.5)
            int mscore = 0;
            std::vector<int> positions;
            if (!no_query) {
                // Prefer the catalog's PRECOMPUTED lowercased haystack (built
                // once per catalog change), so a keystroke never re-allocates
                // or re-lowercases a haystack per model. Fall back to composing
                // it inline only when the cache isn't populated (e.g. unit
                // tests that call build_fused_rows directly).
                std::string tmp;
                std::string_view h = i < nkeys
                    ? std::string_view{c.search_keys[i]}
                    : std::string_view{(tmp = fused_haystack(c.label, mi))};
                const auto sc = fuzzy::score(h, q);
                if (!sc.matched()) continue;
                mscore = sc.score;
                // Highlight positions: the haystack is "<label> <name>", so a
                // match confined to the name substring maps by offset. When
                // any position falls inside the label, re-score just the name
                // (rare — only when the query matches the provider name).
                const std::size_t name_off = h.size() >= name_of(mi).size()
                    ? h.size() - name_of(mi).size() : 0;
                bool all_in_name = true;
                for (int p : sc.positions)
                    if (static_cast<std::size_t>(p) < name_off) {
                        all_in_name = false;
                        break;
                    }
                if (all_in_name) {
                    positions.reserve(sc.positions.size());
                    for (int p : sc.positions)
                        positions.push_back(p - static_cast<int>(name_off));
                } else {
                    positions = fuzzy::score(name_of(mi), q).positions;
                }
            }
            FusedRow row;
            row.provider_id = c.provider_id;
            row.label       = c.label;
            row.model       = mi;
            row.authed      = true;
            row.active      = (r == in.active);
            row.recent      = false;
            // Prefer the catalog's MEMOISED reasoning flag (built once per
            // catalog/capability change); resolve live only when the cache
            // isn't populated (unit tests calling build_fused_rows raw).
            row.reasons     = i < c.reason_flags.size()
                ? static_cast<bool>(c.reason_flags[i])
                : effort_capable(resolved_caps(mi.id.value, c.provider_id));
            row.match_positions = std::move(positions);
            // Register AFTER the query gate: a filtered-out twin must not
            // suppress its matching sibling.
            seen.push_back(std::move(r));
            scored.push_back({std::move(row), mscore, prov_ord});
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

    // ── Section 3: sign-in offers (un-authed providers) ────────────────────
    // QUERY-GATED: with no query the browse view stays clean (no "sign in"
    // clutter); a query that fuzzy-matches an un-authed provider's name
    // surfaces its offer so searching for a provider you haven't added is
    // never a dead end — the row IS the next step.
    for (const auto& off : offers) {
        if (no_query || !fuzzy::matches(off.label, q)) continue;
        FusedRow row;
        row.provider_id = off.provider_id;
        row.label       = off.label;
        row.authed      = false;               // model.id stays empty ⇒ offer
        out.push_back(std::move(row));
    }

    return out;
}

} // namespace agentty::ui
