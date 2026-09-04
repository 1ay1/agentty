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
#include <unordered_set>
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
    // Restrict the list to ONE provider. Empty (the default) = the fused,
    // cross-provider view. Set to a provider id by Smart Mode slot-assign
    // mode, where a pinned model is handed to whatever provider is active
    // at turn time — so a row from another provider must not be selectable.
    // Filtering here rather than validating at Select keeps the invalid
    // choice unrepresentable instead of merely rejected. Sign-in offers are
    // suppressed too: you cannot pin a model you aren't signed in to.
    std::string only_provider;
    // The canonical model label + fuzzy-match anchor. Injected by the
    // view (ui::model_display_label) so the fused picker's rows read AND
    // match identically to the per-provider picker's. Defaulted to the
    // raw display_name-or-id so unit tests can call build_fused_rows
    // without pulling in the view layer.
    std::string (*label_fn)(std::string_view id, std::string_view name) =
        [](std::string_view id, std::string_view name) -> std::string {
            return std::string{name.empty() ? id : name};
        };
};

namespace detail {

// The searchable text for a (provider, model) row:
// "<provider label> <canonical model label> <raw id>". The canonical
// label (ui::model_display_label) is what the view shows, so matching
// and highlight offsets index the same string; the raw id is appended
// so "gpt-5-codex" still matches even after prettification.
[[nodiscard]] inline std::string fused_haystack(std::string_view label,
                                                std::string_view model_label,
                                                const ModelInfo& mi) {
    std::string h;
    h.reserve(label.size() + 1 + model_label.size() + 1
              + mi.id.value.size());
    h += label;
    h += ' ';
    h += model_label;
    h += ' ';
    h += mi.id.value;
    return h;
}

// Find a model in a catalog by wire id. Exact match first; on a miss,
// fall back to ROW IDENTITY (capkey::norm_row_id) so a recent survives
// the provider re-spelling its id — "mistral-medium-3.5" vs
// "mistral-medium-3-5", a case change, a stray separator. Without the
// fallback such a recent silently vanished from the MRU on the next
// catalog refresh even though the model was still served.
//
// Row identity (not norm_model) so a `[1m]` context variant still only
// matches its own row, never its base model.
[[nodiscard]] inline const ModelInfo* find_model(const ProviderCatalog& c,
                                                 std::string_view model_id) {
    for (const auto& mi : c.models)
        if (mi.id.value == model_id) return &mi;
    const std::string want = capkey::norm_row_id(model_id);
    if (want.empty()) return nullptr;
    for (const auto& mi : c.models)
        if (capkey::norm_row_id(mi.id.value) == want) return &mi;
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
    // Single-provider mode (Smart Mode slot-assign). Gates all three
    // sections: RECENT entries from other providers, their catalog rows,
    // and every sign-in offer.
    const bool one_provider = !in.only_provider.empty();
    auto in_scope = [&](std::string_view pid) {
        return !one_provider || pid == in.only_provider;
    };

    std::vector<FusedRow> out;
    out.reserve(recents.size() + 32);

    auto matches = [&](std::string_view label, const ModelInfo& mi) -> bool {
        return no_query ||
               fuzzy::matches(fused_haystack(label,
                                             in.label_fn(mi.id.value, mi.display_name),
                                             mi), q);
    };

    // The visible model NAME the view puts in the row's leading cell —
    // the canonical, provider-uniform label (ui::model_display_label via
    // in.label_fn), so matching + highlighting index the SAME string the
    // view renders. Returns by value (label_fn normalizes), so callers
    // bind it to a local before taking offsets against it.
    auto name_of = [&](const ModelInfo& mi) -> std::string {
        return in.label_fn(mi.id.value, mi.display_name);
    };
    // Byte offsets of the query within the NAME, for fzf-style highlighting.
    // Empty when no query, or when the row matched only on the provider name.
    auto name_positions = [&](std::string_view name) -> std::vector<int> {
        if (no_query) return {};
        return fuzzy::score(name, q).positions;
    };

    // ── Section 1: RECENT (MRU) ──────────────────────────────────────────
    // The active row is pinned first even if not literally the newest MRU
    // entry, so `●` always leads. De-dup against what we emit here so the
    // "all providers" section never repeats a recent row. Identity is the
    // capkey-FOLDED (provider, model): providers alias one model under
    // several spellings (mistral-medium-3-5 / -3.5), and a recent pick of
    // one spelling must suppress the catalog's twin — not sit above it.
    //
    // norm_ROW_id, not norm_model: the latter folds `[1m]`/`[2m]` away
    // (right for capability lookups, wrong for row identity), which made
    // every 1M-context variant look like an alias of its base model and
    // vanish from the list. Row identity keeps the marker while folding
    // genuine spelling aliases.
    //
    // Hash-set on "<provider>\n<folded-id>" — the old O(seen) linear scan
    // re-folded BOTH sides per probe (two allocations × candidates × seen:
    // ~200k allocs/keystroke on a 450-model list). Catalog rows use the
    // precomputed c.row_keys fold when present.
    std::unordered_set<std::string> seen;
    int emitted_recent = 0;
    std::vector<FusedRow> recent_rows;
    // One scratch buffer for probe keys — contains() takes it by const ref,
    // so probing never allocates once the buffer's capacity is warm.
    std::string probe;
    auto seen_key = [&probe](std::string_view pid,
                             std::string_view folded) -> const std::string& {
        probe.clear();
        probe.reserve(pid.size() + 1 + folded.size());
        probe += pid; probe += '\n'; probe += folded;
        return probe;
    };
    auto already = [&](const ModelRef& r) {
        return seen.contains(seen_key(r.provider_id,
                                      capkey::norm_row_id(r.model_id)));
    };
    auto push_recent = [&](const ModelRef& r) -> bool {
        if (already(r)) return false;
        if (!in_scope(r.provider_id)) return false;
        const ProviderCatalog* c = find_catalog(catalogs, r.provider_id);
        if (!c) return false;                              // provider signed out
        const ModelInfo* mi = find_model(*c, r.model_id);
        if (!mi) return false;                             // model gone from catalog
        if (!matches(c->label, *mi)) return false;
        FusedRow row;
        row.provider_id = r.provider_id;
        row.label       = c->label;
        row.model       = *mi;
        row.model_label = name_of(*mi);
        row.authed      = true;
        row.active      = (r == in.active);
        row.recent      = true;
        row.reasons     = effort_capable(
            resolved_caps(mi->id.value, r.provider_id));
        row.tier        = static_cast<std::uint8_t>(
            ModelCapabilities::tier_for(mi->id.value));
        row.tool_capable  = mi->supports_tools.value_or(true);
        row.match_positions = name_positions(row.model_label);
        recent_rows.push_back(std::move(row));
        seen.insert(seen_key(r.provider_id, capkey::norm_row_id(r.model_id)));
        ++emitted_recent;
        return true;
    };

    if (!in.active.empty()) push_recent(in.active);
    for (const auto& r : recents) {
        if (emitted_recent >= in.recent_cap) break;
        push_recent(r);
    }

    // Browsing: "recent" section is alphabetical, active provider's recents
    // first, then the rest. Filtering: keep MRU order.
    //
    // THE ACTIVE ROW IS NOT SORTED. It is emitted first by construction
    // (the push_recent(in.active) above, before any MRU entry), and browse
    // ordering applies only to what follows it.
    //
    // Expressing the pin as a RANGE rather than as a sort key is the whole
    // point. "● always leads" used to be re-derived inside the comparator,
    // and a comparator that forgets one clause fails silently: an
    // alphabetical sort with no `active` term displaced the active model
    // behind any earlier-sorting recent from the same provider (Haiku above
    // an active Opus), which is a regression no reviewer sees and no
    // single-recent test can catch. A range the sort never receives cannot
    // be reordered by a future comparator at all — the invariant holds by
    // construction instead of by remembering.
    const std::size_t pinned =
        (!recent_rows.empty() && recent_rows.front().active) ? 1u : 0u;
    if (no_query) {
        const std::string& ap = in.active.provider_id;
        std::vector<FusedRow*> active_rec, other_rec;
        for (std::size_t i = pinned; i < recent_rows.size(); ++i) {
            auto& r = recent_rows[i];
            (r.provider_id == ap ? active_rec : other_rec).push_back(&r);
        }
        auto by_label = [](const FusedRow* x, const FusedRow* y) {
            return capkey::norm_row_id(x->model_label)
                 < capkey::norm_row_id(y->model_label); };
        std::stable_sort(active_rec.begin(), active_rec.end(), by_label);
        std::stable_sort(other_rec.begin(), other_rec.end(), by_label);
        if (pinned) out.push_back(std::move(recent_rows.front()));
        for (auto* p : active_rec) out.push_back(std::move(*p));
        for (auto* p : other_rec) out.push_back(std::move(*p));
    } else {
        for (auto& r : recent_rows) out.push_back(std::move(r));
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
    // `tier` is precomputed per row rather than derived inside the
    // comparator: ModelCapabilities::tier_for tokenises the id and runs
    // several substring scans, and a comparator is called O(n log n) times.
    // On an aggregator's few-hundred-model catalog that was thousands of
    // redundant scans per keystroke. 0 when browsing is off (unused).
    struct Scored { FusedRow row; int score; int prov_ord; int tier; std::string akey; };
    std::vector<Scored> scored;
    scored.reserve(64);
    int prov_ord = 0;
    for (const auto& c : catalogs) {
        if (!in_scope(c.provider_id)) continue;
        const std::size_t nkeys = c.search_keys.size();
        for (std::size_t i = 0; i < c.models.size(); ++i) {
            const auto& mi = c.models[i];
            // Dedup (in RECENT, or an alias spelling of an emitted row —
            // catalogs list twins like mistral-medium-3-5 AND -3.5) via the
            // catalog's precomputed fold when present: zero allocations on
            // the per-keystroke path. (Two-step, not a ternary: a mixed
            // lvalue/prvalue conditional materialises a COPY of the cached
            // key, which would put the allocation right back.)
            std::string folded_tmp;
            std::string_view folded;
            if (i < c.row_keys.size()) folded = c.row_keys[i];
            else folded = (folded_tmp = capkey::norm_row_id(mi.id.value));
            if (seen.contains(seen_key(c.provider_id, folded))) continue;
            int mscore = 0;
            std::vector<int> positions;
            // Cached canonical label when the catalog has one (production);
            // materialise via label_fn only on the fallback path. Deferred
            // past the fuzzy gate so a narrow query never pays label cost
            // for eliminated rows.
            const bool have_label_cache = i < c.display_labels.size();
            std::string model_label;
            if (!no_query) {
                // Prefer the catalog's PRECOMPUTED lowercased haystack (built
                // once per catalog change), so a keystroke never re-allocates
                // or re-lowercases a haystack per model. Fall back to composing
                // it inline only when the cache isn't populated (e.g. unit
                // tests that call build_fused_rows directly).
                std::string tmp;
                std::string_view h;
                if (i < nkeys) {
                    h = c.search_keys[i];
                } else {
                    model_label = name_of(mi);
                    h = (tmp = fused_haystack(c.label, model_label, mi));
                }
                const auto sc = fuzzy::score(h, q);
                if (!sc.matched()) continue;
                mscore = sc.score;
                if (model_label.empty())
                    model_label = have_label_cache ? c.display_labels[i]
                                                   : name_of(mi);
                // Highlight offsets index the visible label, which is a
                // SUBSTRING of the haystack ("<provider> <label> <id>") at a
                // known start. Map by that offset when every matched char
                // lands inside the label span; otherwise the query matched
                // the provider name or the raw id — re-score just the label
                // so we only ever highlight what's on screen (empty result
                // is fine: nothing to light up).
                const std::size_t label_off = c.label.size() + 1;  // "<provider> "
                const std::size_t label_end = label_off + model_label.size();
                bool all_in_label = !sc.positions.empty();
                for (int p : sc.positions) {
                    const auto up = static_cast<std::size_t>(p);
                    if (up < label_off || up >= label_end) { all_in_label = false; break; }
                }
                if (all_in_label) {
                    positions.reserve(sc.positions.size());
                    for (int p : sc.positions)
                        positions.push_back(p - static_cast<int>(label_off));
                } else {
                    positions = name_positions(model_label);
                }
            } else {
                model_label = have_label_cache ? c.display_labels[i]
                                               : name_of(mi);
            }
            FusedRow row;
            row.provider_id = c.provider_id;
            row.label       = c.label;
            row.model       = mi;
            row.model_label = model_label;
            row.authed      = true;
            row.active      = (c.provider_id == in.active.provider_id
                               && mi.id.value == in.active.model_id);
            row.recent      = false;
            // Prefer the catalog's MEMOISED reasoning flag (built once per
            // catalog/capability change); resolve live only when the cache
            // isn't populated (unit tests calling build_fused_rows raw).
            row.reasons     = i < c.reason_flags.size()
                ? static_cast<bool>(c.reason_flags[i])
                : effort_capable(resolved_caps(mi.id.value, c.provider_id));
            row.match_positions = std::move(positions);
            // Tier: read by the browse-mode sort below AND by the view (which
            // hues the provider badge by it, so the strongest-first ordering
            // is legible). Computed once here rather than per-comparison or
            // per-frame — tier_for tokenises the id and runs substring scans.
            const int tier = static_cast<int>(
                ModelCapabilities::tier_for(mi.id.value));
            row.tier = static_cast<std::uint8_t>(tier);
            row.tool_capable  = mi.supports_tools.value_or(true);
            // Register AFTER the query gate: a filtered-out twin must not
            // suppress its matching sibling.
            seen.insert(seen_key(c.provider_id, folded));
            scored.push_back({std::move(row), mscore, prov_ord, tier,
                              no_query ? capkey::norm_row_id(model_label)
                                       : std::string{}});
        }
        ++prov_ord;
    }
    std::stable_sort(scored.begin(), scored.end(),
        [no_query](const Scored& a, const Scored& b) {
            // Pinned favourites always come first, regardless of mode.
            if (a.row.model.favorite != b.row.model.favorite)
                return a.row.model.favorite;
            // CAN'T-DO-THE-JOB SINKS. A model the provider says has no tool
            // support cannot drive the agent at all, so it must never
            // outrank a model that can — ranking is about "what should I
            // pick", and these are only pickable for plain chat.
            if (a.row.tool_capable != b.row.tool_capable)
                return a.row.tool_capable;
            if (no_query) {
                // Browsing: one flat "all providers" section, sorted
                // alphabetically by normalized model label; context window
                // is the final tie-break.
                if (a.akey  != b.akey)  return a.akey < b.akey;
                return a.row.model.context_window > b.row.model.context_window;
            }
            // FILTERING: relevance order — fuzzy score is the intent signal
            // and must not be re-ranked behind anything else. Provider
            // registry order then wider context break ties.
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
        if (one_provider) break;    // can't pin a model you aren't signed in to
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
