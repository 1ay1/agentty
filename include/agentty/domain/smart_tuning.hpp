#pragma once
// agentty::smart::tuning — advanced numeric knobs for Smart Mode.
//
// The Smart Mode FEATURE toggles (which layers run) live in the Ctrl+S overlay
// and persist to settings.json. THIS file is the layer below that: the handful
// of NUMERIC policy constants a power user might legitimately want to retune for
// their workflow, exposed as environment variables in the same style as the
// AGENTTY_RAG_* knobs (read at point of use, clamped to a safe range, unset ⇒
// the shipped default).
//
// Only genuine POLICY is exposed. Implementation internals that would corrupt
// stored data or break invariants if changed (the signature hash space + FNV
// seed, the storage compaction thresholds, the individual classifier feature
// weights) are deliberately NOT here — the tier THRESHOLDS are the right control
// surface for classification, not fifteen fiddly per-feature weights.
//
//   AGENTTY_SMART_PRIOR_EVIDENCE   (double, default 5.0, range 1..100)
//       Pseudo-count controlling how much evidence the per-workspace learned
//       routing prior needs before it's trusted. Lower ⇒ the store reacts
//       faster (fewer turns to move a prior); higher ⇒ more conservative.
//
//   AGENTTY_SMART_DEEP_MARGIN      (int, default 3, range 1..8)
//       Classifier-score margin at which a turn is "deep" in its band and earns
//       the extra continuous effort step. Lower ⇒ continuous scaling is more
//       eager to add/drop the extra step; higher ⇒ it stays close to the
//       discrete tier behaviour.
//
//   AGENTTY_SMART_BIAS_CLAMP       (int, default 2, range 1..4)
//       Symmetric clamp on the session cascade effort bias (±N steps). Caps how
//       far this session's self-correction can drift effort from baseline.
//
//   AGENTTY_SMART_COMPLEX_THRESHOLD (int, default 3, range 1..8)
//       Feature-score at/above which a turn classifies as Complex. Lower ⇒ more
//       turns escalate to Complex (more reasoning, more cost); higher ⇒ fewer.
//       The Simple/Standard boundary tracks it (Standard is the band just below).
//
//   AGENTTY_SMART_MODE / AGENTTY_SMART_ENABLED  (0 or 1, unset ⇒ settings.json)
//       SESSION override for the Smart Mode master switch. 1 forces it on,
//       0 forces it off, for THIS process only — the persisted setting is
//       neither read as the source of truth nor overwritten (persist skips
//       the field while the override is active, and the ^S overlay shows
//       the pin). Both names accepted; AGENTTY_SMART_MODE wins if both set.
//       Useful for scripted runs (CI, benchmarks, bisecting) where you want
//       deterministic routing without touching the user's config.

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

namespace agentty::smart::tuning {

namespace detail {

inline double env_double(const char* var, double dflt, double lo, double hi) noexcept {
    if (const char* v = std::getenv(var); v && v[0]) {
        try { return std::clamp(std::stod(v), lo, hi); } catch (...) {}
    }
    return dflt;
}

inline int env_int(const char* var, int dflt, int lo, int hi) noexcept {
    if (const char* v = std::getenv(var); v && v[0]) {
        try { return std::clamp(std::stoi(v), lo, hi); } catch (...) {}
    }
    return dflt;
}

} // namespace detail

// Evidence pseudo-count for the learned routing prior (RoutingMemory::kPriorN).
[[nodiscard]] inline double prior_evidence() noexcept {
    return detail::env_double("AGENTTY_SMART_PRIOR_EVIDENCE", 5.0, 1.0, 100.0);
}

// Margin at which continuous effort scaling adds the deep-band extra step.
[[nodiscard]] inline int deep_margin() noexcept {
    return detail::env_int("AGENTTY_SMART_DEEP_MARGIN", 3, 1, 8);
}

// Symmetric clamp (±N) on the session cascade effort bias.
[[nodiscard]] inline int bias_clamp() noexcept {
    return detail::env_int("AGENTTY_SMART_BIAS_CLAMP", 2, 1, 4);
}

// Feature-score at/above which a turn is Complex. The Standard band is the two
// score points below it; Simple is everything at or below that.
[[nodiscard]] inline int complex_threshold() noexcept {
    return detail::env_int("AGENTTY_SMART_COMPLEX_THRESHOLD", 3, 1, 8);
}

// Session override for the Smart Mode master switch. nullopt = no override
// (settings.json governs); true/false = pinned for this process. Reads
// AGENTTY_SMART_MODE first, then AGENTTY_SMART_ENABLED (alias). Any value
// other than empty/"0" counts as on — so =1, =true, =yes all work — and a
// literal "0" (or "false") is off.
[[nodiscard]] inline std::optional<bool> enabled_override() noexcept {
    for (const char* var : {"AGENTTY_SMART_MODE", "AGENTTY_SMART_ENABLED"}) {
        if (const char* v = std::getenv(var); v && v[0]) {
            const std::string s{v};
            return !(s == "0" || s == "false" || s == "off" || s == "no");
        }
    }
    return std::nullopt;
}

} // namespace agentty::smart::tuning
