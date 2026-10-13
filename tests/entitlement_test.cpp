// entitlement_test — ACCOUNT-scoped facts, keyed properly.
//
// The bug this layer retires: `Settings::context_1m_blocked` was a single
// global bool holding an ACCOUNT-scoped truth (Anthropic's 1M-context beta
// is a subscription entitlement). Because the box was account-blind, the
// account-switch reducer had to CLEAR it — which meant ping-ponging between
// an entitled (Max) and an unentitled (Pro) account re-discovered the same
// HTTP 400 on every hop, forever, because the answer was thrown away rather
// than remembered per account.
//
// These tests pin the properties that make keying correct:
//   • facts are per (provider, account, model) and never leak across any axis
//   • a switch AWAY and BACK remembers — the ping-pong case, stated directly
//   • the account component is a USER-TYPED string, so key building must be
//     injection-proof (the reason the separator is US, not '/')
//   • forgetting happens on account REMOVAL only, and is surgical

#include "agtest.hpp"

#include "agentty/domain/entitlement.hpp"

#include <string>

using namespace agentty;
using ent = domain::entitlement::Fact;
namespace E = domain::entitlement;

TEST_CASE("entitlement: absent ⇒ not blocked (permissive default)") {
    E::Store s;
    // A fresh install, a fresh account, and a provider that never rejects
    // anything must all behave identically: nothing is blocked until a
    // rejection teaches us otherwise.
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work", "claude-opus-4-5"));
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "", "claude-opus-4-5"));
}

TEST_CASE("entitlement: a fact is scoped to its account, not the provider") {
    E::Store s;
    E::record_blocked(s, ent::Context1M, "anthropic", "pro", "claude-opus-4-5");

    CHECK(E::blocked(s, ent::Context1M, "anthropic", "pro", "claude-opus-4-5"));
    // The OTHER account on the same provider is unaffected — this is the
    // whole point. The old global bool answered `true` here and stripped
    // the [1m] rows from a Max account that was entitled to them.
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "max", "claude-opus-4-5"));
    // …and so is a different provider, and a different model.
    CHECK(!E::blocked(s, ent::Context1M, "openai", "pro", "claude-opus-4-5"));
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "pro", "claude-sonnet-4-6"));
}

TEST_CASE("entitlement: switch away and back REMEMBERS (the ping-pong bug)") {
    E::Store s;
    // Turn 1 on the Pro account: the wire rejects the beta, we learn.
    REQUIRE(E::record_blocked(s, ent::Context1M, "anthropic", "pro",
                              "claude-opus-4-5"));
    // Switch to Max — nothing is cleared (no reset hook exists any more),
    // and Max is correctly unblocked.
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "max", "claude-opus-4-5"));
    // Switch BACK to Pro. Pre-fix this was a clean slate and the user ate
    // another 400 + fallback round trip. Now it is remembered.
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "pro", "claude-opus-4-5"));
}

TEST_CASE("entitlement: model component is capkey-folded") {
    E::Store s;
    // Spelling variants of ONE model must resolve to ONE key — the same
    // discipline the learned-effort registry uses. Without this, learning
    // the block under "mistral-medium-3.5" would miss "mistral-medium-3-5"
    // and the user would eat the rejection twice.
    E::record_blocked(s, ent::Context1M, "anthropic", "a", "Claude-Opus-4.5");
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "a", "claude-opus-4-5"));
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "a", "CLAUDE_OPUS_4_5"));
}

TEST_CASE("entitlement: account-wide facts are distinct from model-scoped") {
    E::Store s;
    E::record_blocked(s, ent::Context1M, "anthropic", "work");   // no model
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "work"));
    // An account-wide fact must NOT answer a model-scoped question (and
    // vice versa) — they are different keys with different meanings.
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work", "claude-opus-4-5"));
}

TEST_CASE("entitlement: user-typed account labels cannot forge a key") {
    E::Store s;
    // Account labels are USER-TYPED ("work/personal", "acct #2"), so a
    // separator the user can type would let one account's label spill into
    // the model component of another's key. The separator is US (0x1f),
    // which cannot be typed into the label prompt.
    E::record_blocked(s, ent::Context1M, "anthropic", "work/claude-opus-4-5");
    // The slash-bearing label is its OWN account, and did not accidentally
    // create a model-scoped fact for account "work".
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "work/claude-opus-4-5"));
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work", "claude-opus-4-5"));
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work"));
}

TEST_CASE("entitlement: forget_account is surgical and prefix-anchored") {
    E::Store s;
    E::record_blocked(s, ent::Context1M, "anthropic", "work",  "claude-opus-4-5");
    E::record_blocked(s, ent::Context1M, "anthropic", "work",  "claude-sonnet-4-6");
    E::record_blocked(s, ent::Context1M, "anthropic", "work2", "claude-opus-4-5");
    E::record_blocked(s, ent::Context1M, "anthropic", "home",  "claude-opus-4-5");
    E::record_blocked(s, ent::Context1M, "openai",    "work",  "gpt-5");

    E::forget_account(s, "anthropic", "work");

    // Both of "work"'s model-scoped facts are gone…
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work", "claude-opus-4-5"));
    CHECK(!E::blocked(s, ent::Context1M, "anthropic", "work", "claude-sonnet-4-6"));
    // …but "work2" must NOT be swept by a naive prefix match (the reason
    // forget_account anchors on the separator), nor another account, nor
    // the same label on a different provider.
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "work2", "claude-opus-4-5"));
    CHECK(E::blocked(s, ent::Context1M, "anthropic", "home",  "claude-opus-4-5"));
    CHECK(E::blocked(s, ent::Context1M, "openai",    "work",  "gpt-5"));
}

TEST_CASE("entitlement: record_blocked reports novelty") {
    E::Store s;
    // Callers skip a settings write when nothing changed — a rejection that
    // repeats every turn must not rewrite settings.json every turn.
    CHECK(E::record_blocked(s, ent::Context1M, "anthropic", "a", "m"));
    CHECK(!E::record_blocked(s, ent::Context1M, "anthropic", "a", "m"));
}

TEST_CASE("entitlement: the empty account label is a legitimate key") {
    E::Store s;
    // A single-account provider has no named account; "" means "the only
    // account". This is what makes the migration free for single-account
    // users: their facts key under "" and keep working.
    E::record_blocked(s, ent::Context1M, "ollama", "", "llama3");
    CHECK(E::blocked(s, ent::Context1M, "ollama", "", "llama3"));
    CHECK(!E::blocked(s, ent::Context1M, "ollama", "named", "llama3"));
}
