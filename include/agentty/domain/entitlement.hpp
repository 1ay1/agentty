#pragma once
// agentty::domain::entitlement — ACCOUNT-scoped facts, keyed properly.
//
// THEORY: docs/IDENTITY_CAPABILITY_ENTITLEMENT.md explains the three-layer
// split this header implements, the failure mode it retires (a manual reset
// hook compensating for a missing key axis), and how to decide which layer a
// new fact belongs to. Read that first if you are adding a Fact.
//
// ── The layering this header exists to make explicit ─────────────────────
//
// agentty separates three things that are easy to conflate:
//
//   IDENTITY     (provider, model)            — what a model IS.
//   CAPABILITY   (provider, model)            — what the model CAN do.
//   ENTITLEMENT  (provider, account, model)   — what THIS SUBSCRIPTION may do.
//
// Identity and capability are account-blind ON PURPOSE. `claude-opus-4-5` is
// the same model, with the same tokenizer, context and tool grammar, on your
// work account and your personal one. Folding the account into model identity
// would invalidate your model selection, MRU, Smart Mode pins and per-provider
// recall on every account switch — obviously wrong, and the reason
// docs/PROVIDER_HETEROGENEITY.md keeps the account off the model row.
//
// But some facts genuinely ARE account-scoped: a subscription tier decides
// what you may USE, not what exists. Anthropic's 1M-context beta is the
// canonical case — the OAuth token carries no entitlement field, so the only
// way to learn it is to try and be rejected (HTTP 400 "long context beta is
// not yet available for this subscription").
//
// ── Why a registry instead of a bool ─────────────────────────────────────
//
// This started as `Settings::context_1m_blocked`, one global bool. That is
// account-scoped truth in an account-blind box, and the code knew it: the
// account-switch reducer manually CLEARED the flag with the comment "the
// block was learned FOR the account being dropped". Invalidation-on-switch
// is lossy by construction — ping-pong between a Max account (1M works) and
// a Pro account (blocked) re-discovers the same 400 on every hop, forever,
// because the answer is thrown away each time instead of remembered per
// account.
//
// Keyed storage fixes that AND deletes the special case: there is nothing to
// reset on a switch, because the other account's facts were never in the way.
// Same discipline the learned-effort registry already uses (capkey-folded
// keys, so spelling variants can't miss) — one more axis on the key.
//
// ── Key shape ────────────────────────────────────────────────────────────
//
//     "<provider>\x1f<account>\x1f<folded-model>"      model-scoped fact
//     "<provider>\x1f<account>"                        account-wide fact
//
// US (0x1f) as the separator, not '/': provider ids are registry-controlled
// but ACCOUNT LABELS ARE USER-TYPED ("work/personal", "acct #2"), and a
// separator a user can type is a key-collision bug waiting to happen. US is
// not typeable into the account-label prompt and never appears in a model id.
//
// The account component is the registry's active label for that provider
// (auth::accounts::active_label). An unnamed / single-account provider
// resolves to "" — a perfectly good key component that simply means "the
// only account", so single-account users get the same behaviour they always
// had with zero migration.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "agentty/domain/capkey.hpp"

namespace agentty::domain::entitlement {

// Key separator — see the header note. Deliberately not typeable.
inline constexpr char kSep = '\x1f';

// The entitlement facts agentty can learn. One enumerator per fact, so a
// new one is an enum entry + a `key_for` call site, never a new Settings
// field and never a new manual reset.
//
// Adding one: extend this enum, extend `tag`, and record it at the site
// that learns it. Storage, keying, persistence and account-switch
// behaviour all come for free.
enum class Fact : std::uint8_t {
    // Anthropic's context-1m-2025-08-07 beta was rejected for this
    // subscription (HTTP 400). Model-scoped: entitlement is per model line,
    // and a future account might be entitled for one model but not another.
    Context1M,
};

[[nodiscard]] constexpr std::string_view tag(Fact f) noexcept {
    switch (f) {
        case Fact::Context1M: return "ctx1m";
    }
    return "?";
}

// Build the storage key for a fact.
//
// `model_id` empty ⇒ an ACCOUNT-WIDE fact (no model component). The model
// component is capkey::norm_model-folded so "3.5" / "3-5" / case variants
// resolve to ONE key — the same rule every other capability registry uses,
// which is what makes spelling-based misses structurally impossible.
//
// `account` empty is legal and meaningful: "the only account".
[[nodiscard]] inline std::string key_for(Fact f,
                                         std::string_view provider,
                                         std::string_view account,
                                         std::string_view model_id = {}) {
    std::string k;
    k.reserve(tag(f).size() + provider.size() + account.size()
              + model_id.size() + 3);
    k += tag(f);
    k += kSep;
    k += provider;
    k += kSep;
    k += account;
    if (!model_id.empty()) {
        k += kSep;
        k += capkey::norm_model(model_id);
    }
    return k;
}

// The persisted store: key → blocked. Only NEGATIVE facts are recorded —
// "this subscription may not" — because entitlement is permissive by
// default and we only ever learn by rejection. An absent key means "not
// known to be blocked", which is exactly the right default for a fresh
// account, a fresh install, and a provider that never rejects anything.
using Store = std::map<std::string, bool>;

[[nodiscard]] inline bool blocked(const Store& s, Fact f,
                                  std::string_view provider,
                                  std::string_view account,
                                  std::string_view model_id = {}) {
    const auto it = s.find(key_for(f, provider, account, model_id));
    return it != s.end() && it->second;
}

// Record a learned rejection. Returns true when this is NEW information
// (so callers can skip a settings write when nothing changed).
inline bool record_blocked(Store& s, Fact f,
                           std::string_view provider,
                           std::string_view account,
                           std::string_view model_id = {}) {
    auto [it, inserted] = s.insert_or_assign(
        key_for(f, provider, account, model_id), true);
    return inserted;
}

// Drop every fact for one (provider, account) — the SIGN-OUT hook, not the
// account-switch hook. Switching accounts must NOT forget: that is the
// whole point of keying. But signing an account OUT means its facts can
// never be consulted again, and a later re-login may land on a different
// subscription tier, so the slate is genuinely stale.
inline void forget_account(Store& s, std::string_view provider,
                           std::string_view account) {
    std::string prefix;
    prefix += kSep;
    prefix += provider;
    prefix += kSep;
    prefix += account;
    std::erase_if(s, [&](const auto& kv) {
        // Match "<tag>\x1f<provider>\x1f<account>" exactly or as a prefix
        // of a model-scoped key ("…\x1f<model>"). Anchor on the separator
        // so account "work" never matches account "work2".
        const std::string& k = kv.first;
        const auto at = k.find(prefix);
        if (at == std::string::npos) return false;
        const auto end = at + prefix.size();
        return end == k.size() || k[end] == kSep;
    });
}

} // namespace agentty::domain::entitlement
