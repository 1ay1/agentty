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
// The whole policy is constexpr, so we can verify it cell-by-cell at
// compile time. If anyone ever tweaks the table in a way that breaks
// these properties, the build fails — no test run required.
//
// Reading guide: each `static_assert` is one cell of the trust matrix
// or one structural property the policy must hold. Adding a new Effect
// or a new Profile arm: extend this block with the new expectations.
namespace proofs {

inline constexpr EffectSet kPure  {};
inline constexpr EffectSet kRead  {Effect::ReadFs};
inline constexpr EffectSet kWrite {Effect::WriteFs};
inline constexpr EffectSet kNet   {Effect::Net};
inline constexpr EffectSet kExec  {Effect::Exec};
inline constexpr EffectSet kEdit  {Effect::ReadFs, Effect::WriteFs};

// Property 1 — Profile::Write is fully autonomous: every effect is
// allowed, no prompt is ever shown.
static_assert(permission(kPure,  Profile::Write) == Decision::Allow);
static_assert(permission(kRead,  Profile::Write) == Decision::Allow);
static_assert(permission(kWrite, Profile::Write) == Decision::Allow);
static_assert(permission(kNet,   Profile::Write) == Decision::Allow);
static_assert(permission(kExec,  Profile::Write) == Decision::Allow);
static_assert(permission(kEdit,  Profile::Write) == Decision::Allow);

// Property 2 — Profile::Ask trusts read-only inspection (otherwise an
// agent loop is unusable: every read-file/grep/glob would prompt). It
// gates everything that mutates state, runs code, or hits the network.
static_assert(permission(kPure,  Profile::Ask)   == Decision::Allow);
static_assert(permission(kRead,  Profile::Ask)   == Decision::Allow);
static_assert(permission(kWrite, Profile::Ask)   == Decision::Prompt);
static_assert(permission(kNet,   Profile::Ask)   == Decision::Prompt);
static_assert(permission(kExec,  Profile::Ask)   == Decision::Prompt);
static_assert(permission(kEdit,  Profile::Ask)   == Decision::Prompt);

// Property 3 — Profile::Minimal prompts for *anything* that touches the
// outside world (including reads). Only Pure tools auto-allow.
static_assert(permission(kPure,  Profile::Minimal) == Decision::Allow);
static_assert(permission(kRead,  Profile::Minimal) == Decision::Prompt);
static_assert(permission(kWrite, Profile::Minimal) == Decision::Prompt);
static_assert(permission(kNet,   Profile::Minimal) == Decision::Prompt);
static_assert(permission(kExec,  Profile::Minimal) == Decision::Prompt);

// Property 4 — Exec is the maximal capability: any tool that has it
// prompts under any non-Write profile, regardless of what other effects
// it carries.
static_assert(permission(EffectSet{Effect::Exec, Effect::ReadFs},  Profile::Ask)
              == Decision::Prompt);
static_assert(permission(EffectSet{Effect::Exec, Effect::WriteFs, Effect::Net},
                         Profile::Ask) == Decision::Prompt);

// Property 5 — Profile::Write *cannot* be made to prompt by adding more
// effects. The policy short-circuits on profile alone.
static_assert(permission(EffectSet{Effect::Exec, Effect::WriteFs,
                                   Effect::Net, Effect::ReadFs},
                         Profile::Write) == Decision::Allow);

} // namespace proofs

} // namespace moha::tools::policy
