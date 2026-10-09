#pragma once
// agentty::smart::tuning — advanced numeric knobs for Smart Mode.
//
// The Smart Mode FEATURE toggles (which layers run) live in the Ctrl+S overlay
// and persist to settings.json. THIS file is the layer below that: the handful
// of NUMERIC policy constants a power user might legitimately want to retune for
// their workflow, exposed as environment variables in the same style as the
// AGENTTY_RAG_* knobs (read once at launch by settings::registry::read_env,
// clamped to a safe range, unset ⇒ the persisted value). Only parsers live
// here: the domain does not read the environment.
//
// Only genuine POLICY is exposed. Implementation internals that would corrupt
// stored data or break invariants if changed (the signature hash space + FNV
// seed, the storage compaction thresholds, the individual classifier feature
// weights) are deliberately NOT here — the tier THRESHOLDS are the right control
// surface for classification, not fifteen fiddly per-feature weights.
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
//   AGENTTY_SMART_MODE  (0 or 1, unset ⇒ settings.json)
//       SESSION override for the Smart Mode master switch. 1 forces it on,
//       0 forces it off, for THIS process only — the persisted setting is
//       neither read as the source of truth nor overwritten (persist skips
//       the field while the override is active, and the ^S overlay shows
//       the pin).
//       Useful for scripted runs (CI, benchmarks, bisecting) where you want
//       deterministic routing without touching the user's config.

#include <algorithm>
#include <optional>
#include <string>

namespace agentty::smart::tuning {

namespace detail {

// Parse a knob's value, clamped. nullopt for unset or malformed: the least
// surprising response to a typo in a shell profile is for the configured
// value to stand.
inline std::optional<int> parse_int(const char* v, int lo, int hi) noexcept {
    if (v && v[0]) {
        try { return std::clamp(std::stoi(v), lo, hi); } catch (...) {}
    }
    return std::nullopt;
}

} // namespace detail

// ── The shipped values, named ───────────────────────────────────
// These are the ONE source of truth for the defaults and the enforced ranges.
// They were literals inside each accessor, which was fine while env was the
// only entry point — but the settings registry now carries the same three
// knobs as persisted rows with their own bounds, and two hand-copied numbers
// that must agree is precisely the drift the registry exists to prevent.
// settings_registry.hpp static_asserts its rows against these.
inline constexpr int kDeepMarginDefault = 3, kDeepMarginMin = 1, kDeepMarginMax = 8;
inline constexpr int kBiasClampDefault  = 2, kBiasClampMin  = 1, kBiasClampMax  = 4;
inline constexpr int kComplexDefault    = 3, kComplexMin    = 1, kComplexMax    = 8;

// Session override for the Smart Mode master switch. nullopt = no override
// (settings.json governs); true/false = pinned for this process. Any value
// other than empty counts as on — so =1, =true, =yes all work — and
// "0"/"false"/"off"/"no" are off. ONE env name: a second spelling is a
// second source of truth, and "which one wins" is a question no user should
// have to ask.
[[nodiscard]] inline std::optional<bool> parse_enabled(const char* v) noexcept {
    if (!v || !v[0]) return std::nullopt;
    const std::string s{v};
    return !(s == "0" || s == "false" || s == "off" || s == "no");
}

// ── Per-layer escape hatches ────────────────────────────────────
// Smart Mode's three behaviours used to be user-facing toggles. They are
// now folded into the master switch — nobody rationally ran Smart Mode with
// orchestration off — but each keeps a NEGATIVE env override so a
// developer can bisect a routing bug ("is this orchestration or the
// subagent router?") without editing settings or rebuilding.
//
// Deliberately env-only and deliberately negative: the default is on, the
// override is an escape hatch, and an escape hatch in the UI is just a
// toggle with extra steps.
//
//   AGENTTY_SMART_NO_INTERNAL=1     compaction/titles stay on the main model
//   AGENTTY_SMART_NO_ORCHESTRATE=1  main turn stays on the selected model
//   AGENTTY_SMART_NO_SUBAGENTS=1    workers use the tier auto-router
//
// The values reach RoleConfig::no_* through settings::registry::read_env.
[[nodiscard]] inline bool parse_disabled(const char* v) noexcept {
    if (!v || !v[0]) return false;
    const std::string s{v};
    return !(s == "0" || s == "false" || s == "off" || s == "no");
}

// Did the command line name a model?
//
// `--model X` is a contract, not a hint: every role runs on it. Set once at
// startup, read by whoever assembles a RoleConfig. It is process state
// rather than an env var because it is not a user knob -- it mirrors an
// argv fact, and a second spelling would be a second source of truth.
//
// The TUI reads this too: `agentty --model X` interactively had the same
// gap, where compaction routed away from the model the user named (#70).
inline bool& model_pinned() noexcept {
    static bool pinned = false;
    return pinned;
}

} // namespace agentty::smart::tuning
