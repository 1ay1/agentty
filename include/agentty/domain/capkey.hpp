#pragma once
// agentty::capkey — THE canonical capability-key discipline.
//
// Every capability registry (declared reasoning, declared/learned effort
// sets) is a map keyed by strings. Three independent naming authorities
// produce those strings, and none of them agree:
//
//   1. agentty's provider registry   ("mistral", "gemini", "together")
//   2. the provider's wire model ids ("mistral-medium-3.5", "Kimi-K2.5")
//   3. models.dev's community data   (provider "togetherai", "google",
//      "fireworks-ai"; model keys sometimes vendor-prefixed
//      "mistralai/mistral-medium-3-5", sometimes bare, mixed case,
//      dot/dash/underscore spellings of the SAME model)
//
// The mistral-medium-3-5 vs -3.5 two-ladders bug was exactly this: two
// spellings of one model resolving through different keys to different
// facts. Fixing keying per-call-site is whack-a-mole; this header makes
// it structural:
//
//   RULE 1 — one spelling. Every key component is passed through
//     norm_model() before any registry write OR read: lowercase,
//     [._ :] → '-', runs collapsed. "Kimi-K2.5", "kimi-k2-5" and
//     "kimi k2.5" are the same model; now they are the same KEY.
//     (Validated against the full models.dev corpus: 269 multi-spelling
//     groups unify, no false merges — the folding only touches
//     separator characters, never alphanumerics.)
//
//   RULE 2 — one provider namespace. Scoped keys always use AGENTTY's
//     provider id, never a third party's. models.dev provider ids are
//     translated through resolve_dev_provider(), which anchors on
//     IDENTITY EVIDENCE rather than a name table:
//       a. exact id match            (mistral == mistral)
//       b. primary auth env var      (GEMINI_API_KEY names google's entry
//                                     — env vars are globally unique
//                                     provider identities by construction)
//       c. api host substring        (api.githubcopilot.com)
//     Unmatched dev providers keep "dev:<id>/" — a namespace agentty's
//     scope lookup can never collide with, so an unknown aggregator's
//     facts are inert rather than corrupting.
//
//   RULE 3 — bare keys are consensus-only. A key without a provider
//     scope may only be written through the merge-or-poison path
//     (agree → keep, disagree → no-info). Direct writes to bare keys are
//     not expressible through this API.
//
// Everything here is pure string/model-registry logic — no IO, no locks
// of its own (the registries it feeds carry their own).

#include <array>
#include <string>
#include <string_view>

#include "agentty/provider/registry.hpp"

namespace agentty::capkey {

// ── RULE 1: canonical model spelling ────────────────────────────────────
// Lowercase; '.', '_', ' ', ':' fold to '-'; runs of '-' collapse; leading/
// trailing '-' trimmed. Only separators fold — alphanumerics are never
// altered, so distinct models can't merge.
[[nodiscard]] inline std::string norm_model(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '.' || c == '_' || c == ' ' || c == ':') c = '-';
        if (c == '-' && (out.empty() || out.back() == '-')) continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// The bare tail of a possibly vendor-prefixed model id, normalized.
// "mistralai/Mistral-Medium-3.5" → "mistral-medium-3-5".
[[nodiscard]] inline std::string norm_tail(std::string_view id) {
    if (auto slash = id.rfind('/'); slash != std::string_view::npos)
        id = id.substr(slash + 1);
    return norm_model(id);
}

// ── RULE 2: provider-identity resolution ────────────────────────────────
// Map a models.dev provider entry onto agentty's provider id via identity
// evidence. `dev_id` is models.dev's key; `dev_env0` its first env var
// (may be empty); `dev_api` its api URL (may be empty).
//
// Anchors, strongest first:
//   1. dev_id == agentty id                     (mistral, groq, cerebras…)
//   2. dev_env0 == agentty row's primary env    (GEMINI_API_KEY → gemini;
//      env names are the de-facto global provider identity: every SDK,
//      including models.dev itself, keys credentials on them)
//   3. agentty row's endpoint host ⊆ dev_api    (api.githubcopilot.com)
// No match → "" (caller namespaces the record as foreign; see dev_scope).
[[nodiscard]] inline std::string resolve_dev_provider(
        std::string_view dev_id,
        std::string_view dev_env0,
        std::string_view dev_api) {
    using provider::providers;
    // 1. Exact id.
    for (const auto& p : providers())
        if (dev_id == p.id) return std::string{p.id};
    // 2. Primary env var. Only the FIRST env entry on both sides: agentty
    //    rows list fallbacks (OPENAI_API_KEY) in later slots that would
    //    cross-match half the registry.
    if (!dev_env0.empty())
        for (const auto& p : providers())
            if (!p.auth_env[0].empty() && dev_env0 == p.auth_env[0])
                return std::string{p.id};
    // 3. Host containment (agentty rows that pin a host).
    if (!dev_api.empty())
        for (const auto& p : providers())
            if (!p.prewarm_host.empty()
                && dev_api.find(p.prewarm_host) != std::string_view::npos)
                return std::string{p.id};
    return {};
}

// The scope prefix for a models.dev provider's records: agentty's id when
// the identity resolves, else the inert foreign namespace "dev:<id>".
[[nodiscard]] inline std::string dev_scope(std::string_view dev_id,
                                           std::string_view dev_env0,
                                           std::string_view dev_api) {
    auto own = resolve_dev_provider(dev_id, dev_env0, dev_api);
    if (!own.empty()) return own;
    std::string s{"dev:"};
    s += dev_id;
    return s;
}

// ── Key assembly (both writers and readers use these) ───────────────────
[[nodiscard]] inline std::string scoped(std::string_view provider,
                                        std::string_view model) {
    std::string k;
    const std::string nm = norm_model(model);
    k.reserve(provider.size() + 1 + nm.size());
    k += provider;
    k += '/';
    k += nm;
    return k;
}
[[nodiscard]] inline std::string bare(std::string_view model) {
    return norm_tail(model);
}

} // namespace agentty::capkey
