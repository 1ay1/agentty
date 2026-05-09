#pragma once
// moha::tools::policy — the SINGLE source of truth for permission.
// Maps (effects × profile) → Decision. Every gating check in the
// runtime calls this; every tool registers an EffectSet and is
// gated identically. No bespoke per-tool lambdas, no surprises.
//
// The policy is intentionally a pure constexpr function: it has no
// state, no IO, no model dependency — easy to read top-to-bottom,
// easy to test exhaustively, easy to reason about ("under what
// circumstances does the model get to run a network call without
// asking?").

#include "moha/domain/profile.hpp"
#include "moha/tool/effects.hpp"

namespace moha::tools::policy {

enum class Decision : std::uint8_t {
    Allow,    // run unconditionally
    Prompt,   // show the permission card, wait for user
};

// The full table:
//
//   Profile::Write   →  Allow everything (autonomous mode).
//   Profile::Ask     →  Prompt for Exec / WriteFs / Net.
//                       Allow ReadFs / Pure (read-only inspection
//                       is the bread-and-butter of an agent loop —
//                       prompting per-read makes Ask unusable).
//   Profile::Minimal →  Prompt for everything that touches the
//                       outside world (Exec, WriteFs, Net, ReadFs).
//                       Allow only Pure.
//
// "Exec is more dangerous than WriteFs" is the type-theoretic claim:
// `bash` lets the model author the side effect, so it dominates any
// individual filesystem mutation we already gate.
[[nodiscard]] constexpr Decision permission(EffectSet effects,
                                            Profile profile) noexcept {
    if (profile == Profile::Write) return Decision::Allow;

    if (effects.has(Effect::Exec))    return Decision::Prompt;
    if (effects.has(Effect::WriteFs)) return Decision::Prompt;
    if (effects.has(Effect::Net))     return Decision::Prompt;

    if (profile == Profile::Minimal) {
        if (effects.has(Effect::ReadFs)) return Decision::Prompt;
        return Decision::Allow;       // Pure
    }
    // Profile::Ask: ReadFs and Pure trusted.
    return Decision::Allow;
}

// Friendly one-liner explaining WHY a tool is being gated. Rendered
// as the body of the permission card so the user knows what trust
// they're being asked to extend.
[[nodiscard]] inline std::string_view reason(EffectSet effects, Profile p) noexcept {
    if (p == Profile::Write)          return "auto-approved (Write profile)";
    if (effects.has(Effect::Exec))    return "wants to run an arbitrary subprocess";
    if (effects.has(Effect::WriteFs)) return "will modify files on disk";
    if (effects.has(Effect::Net))     return "will reach the network";
    if (p == Profile::Minimal && effects.has(Effect::ReadFs))
                                      return "wants to read from disk (Minimal profile)";
    return "no side effects (auto-approved)";
}

// ── Compile-time proofs of the trust matrix ──────────────────────────────
// The whole policy is constexpr, so we can verify it at compile time.
// EffectSet is a 4-bit bitset (ReadFs / WriteFs / Net / Exec) — 16 distinct
// sets — and there are 3 profiles, so the cell space is exactly
// 16 × 3 = 48 cells. The block below covers ALL of them: a separate
// `expected_decision()` function encodes the policy spec in prose-style
// short-circuit form, and an exhaustive constexpr loop checks
// `permission(e, p) == expected_decision(e, p)` over every cell. If
// anyone tweaks the table in a way that breaks any cell — or extends
// EffectSet without updating the spec — the static_assert fires; no
// test run required.
//
// (Earlier this block listed ~20 spot-checks against named EffectSets.
// That documented the intent well but only verified ~10 of the 48
// cells; the four-effect combinations and several effect-only-with-
// non-matching-profile cases were structurally implied by the policy
// function's short-circuit shape rather than actually asserted. The
// loop below closes that gap.)
namespace proofs {

inline constexpr EffectSet kPure  {};
inline constexpr EffectSet kRead  {Effect::ReadFs};
inline constexpr EffectSet kWrite {Effect::WriteFs};
inline constexpr EffectSet kNet   {Effect::Net};
inline constexpr EffectSet kExec  {Effect::Exec};
inline constexpr EffectSet kEdit  {Effect::ReadFs, Effect::WriteFs};

// Independent re-statement of the policy. Lives separate from
// `permission()` so the two are written as parallel expressions of the
// same rule — a one-handed change to either trips the exhaustive
// check below. Adding an Effect or a Profile arm: extend BOTH sides.
[[nodiscard]] constexpr Decision expected_decision(EffectSet e, Profile p) noexcept {
    // Profile::Write is fully autonomous: every effect is allowed, no
    // prompt is ever shown.
    if (p == Profile::Write) return Decision::Allow;

    // Anything that mutates state, runs code, or hits the network
    // prompts under any non-Write profile. Exec is the maximal
    // capability — a tool that has it prompts regardless of what else
    // it carries.
    if (e.has(Effect::Exec))    return Decision::Prompt;
    if (e.has(Effect::WriteFs)) return Decision::Prompt;
    if (e.has(Effect::Net))     return Decision::Prompt;

    // Profile::Minimal prompts even for read-only inspection — only
    // pure tools auto-allow. Profile::Ask trusts ReadFs so an agent
    // loop's read-file / grep / glob doesn't prompt on every step.
    if (p == Profile::Minimal && e.has(Effect::ReadFs)) return Decision::Prompt;
    return Decision::Allow;
}

// Walk every (EffectSet, Profile) cell and require the policy to agree
// with the spec. 4 effect bits × 3 profiles = 48 cells, all covered.
[[nodiscard]] constexpr bool exhaustive_matches_spec() noexcept {
    constexpr Profile kProfiles[] = {
        Profile::Write, Profile::Ask, Profile::Minimal,
    };
    constexpr std::uint8_t kAllEffectBits =
        static_cast<std::uint8_t>(Effect::ReadFs)
        | static_cast<std::uint8_t>(Effect::WriteFs)
        | static_cast<std::uint8_t>(Effect::Net)
        | static_cast<std::uint8_t>(Effect::Exec);
    for (std::uint16_t bits = 0; bits <= kAllEffectBits; ++bits) {
        // Skip combinations that include unallocated bits — keeps the
        // sweep honest if the Effect enum ever grows to non-contiguous
        // values; the static_assert below pins the bitset width.
        if ((bits & ~static_cast<std::uint16_t>(kAllEffectBits)) != 0) continue;
        EffectSet e{static_cast<std::uint8_t>(bits)};
        for (Profile p : kProfiles) {
            if (permission(e, p) != expected_decision(e, p)) return false;
        }
    }
    return true;
}
static_assert(exhaustive_matches_spec(),
              "policy::permission disagrees with expected_decision on at "
              "least one cell — update both sides together.");

// Pin the bitset width: if a fifth Effect tag is added, this fires and
// reminds you to update kAllEffectBits + expected_decision(). Without
// it the loop above silently wouldn't sweep the new bit.
static_assert(static_cast<std::uint8_t>(Effect::Exec) == (1u << 3),
              "Effect bit assignment changed — review exhaustive_matches_spec()");

// Named-cell spot-checks below are KEPT as documentation (they read
// better than the loop) but are now redundant with the exhaustive
// proof. Don't trust them as a coverage signal — the loop above is.
static_assert(permission(kPure,  Profile::Write)   == Decision::Allow);
static_assert(permission(kEdit,  Profile::Write)   == Decision::Allow);
static_assert(permission(kRead,  Profile::Ask)     == Decision::Allow);
static_assert(permission(kWrite, Profile::Ask)     == Decision::Prompt);
static_assert(permission(kExec,  Profile::Ask)     == Decision::Prompt);
static_assert(permission(kRead,  Profile::Minimal) == Decision::Prompt);
static_assert(permission(kPure,  Profile::Minimal) == Decision::Allow);

} // namespace proofs

} // namespace moha::tools::policy
