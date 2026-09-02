# Identity, capability, entitlement

> Three layers that look like one, and what goes wrong when you collapse them.
>
> This is the *theory* doc. Its concrete instances are
> [PROVIDER_HETEROGENEITY.md](PROVIDER_HETEROGENEITY.md) (how identity and
> capability are expressed as data) and
> `include/agentty/domain/entitlement.hpp` (how the third layer is keyed).

---

## The question that produced this

> *"A model has a provider, not an account. But is that a deeper good design?"*

It came up while designing the model picker. A multi-account user sees two
visually identical rows for the same provider and can't tell which identity a
switch will use — so the obvious move is to put the account on the model row.

The obvious move is wrong, and working out *why* it's wrong turned up a
misfiled fact that had been costing users real round trips for months.

---

## The three layers

| Layer | Key | Answers | Lifetime |
|---|---|---|---|
| **Identity** | `(provider, model)` | *What is this thing?* | as long as the model exists |
| **Capability** | `(provider, model)` | *What can it do?* | changes when the provider changes its dispatch table |
| **Entitlement** | `(provider, account, model)` | *What may **I** do with it?* | changes when a subscription changes |

They are routinely conflated because in the common case — one account per
provider — all three have the same cardinality, so any of the three keys
"works". The distinction only bites when a user has two accounts, which is
exactly when they are least able to debug it.

### Identity: `(provider, model)`

A model's identity is what the wire needs to dispatch: which endpoint,
which dialect, which slug. `claude-opus-4-5` is the same model — same
tokenizer, same context window, same tool grammar — on your work account
and your personal one.

**The reductio for adding the account.** If identity were
`(provider, account, model)`, then switching accounts would invalidate:

- the current model selection,
- the MRU / recents list,
- Smart Mode role pins,
- per-provider model recall (`Settings::provider_models`),
- every favourite.

You would re-pick your model every time you switched wallets. Nobody wants
that, and the reason is that **the account was never part of what the model
is**.

There is also a structural argument. At any instant a provider has exactly
one active credential — account is **1:1 with provider**, not 1:N. A key
component with cardinality 1 adds no discriminating power, only redundancy:
in a 150-model OpenRouter list you would paint one provider-level fact onto
150 rows, stealing width from the model name, which is the thing the user is
actually choosing between.

**Corollary for UI:** show a fact where the decision is made. The account is
chosen in the provider/account picker and displayed in the status bar, where
it is *actionable*. On a model row it would be decoration that implies an
affordance which doesn't exist — picking a model never picks an account
(`switch_to_model_ref` resolves the provider's **active** credential).

### Capability: `(provider, model)`

What the model can do — reasoning support, effort ladder, tool grammar,
context window, tier. Also account-blind, and for the same reason: it
describes the model as the provider serves it, not as your subscription
permits it.

Capability is *provider*-scoped rather than globally model-scoped because
the same model id can behave differently on different hosts (one gateway
enables a tool grammar another has disabled). That's the heterogeneity axis
[PROVIDER_HETEROGENEITY.md](PROVIDER_HETEROGENEITY.md) covers.

### Entitlement: `(provider, account, model)`

What **this subscription** may use. Not a property of the model at all — a
property of your relationship with the provider.

The canonical case: Anthropic's 1M-context beta. The model supports it. The
provider serves it. Whether *you* may use it depends on your plan, and the
OAuth token carries no entitlement field — **the only way to learn it is to
try and be rejected** (HTTP 400, "long context beta is not yet available for
this subscription").

---

## The failure mode: account-scoped truth in an account-blind box

Before this was named, that fact lived in `Settings::context_1m_blocked` —
a single global bool.

The code *knew* the box was wrong. The account-switch reducer manually
cleared the flag, with this comment:

> *"The 1M-context entitlement block was learned FOR the account being
> dropped; the next sign-in may be entitled. Re-arm discovery."*

That is a workaround wearing a comment as an apology. And **invalidation is
lossy by construction**:

```
Max account   →  1M works
switch to Pro →  400, learn "blocked"      (bool = true)
switch to Max →  reducer clears the bool   (fact destroyed)
switch to Pro →  400 again. Forever.
```

Every hop re-discovers the same rejection, because the answer is *thrown
away* rather than *filed under whose answer it was*. The user pays a failed
request plus a fallback round trip, repeatedly, for a fact the program had
already learned.

**The general shape of this bug:** when a fact's true scope is finer than its
storage key, the only available correction is deletion — and deletion is
lossy. You will recognise it by the presence of a *manual reset hook*. A
reset hook is the smell; a missing key axis is the disease.

---

## The fix: key it, don't reset it

`include/agentty/domain/entitlement.hpp` stores facts under

```
"<fact>\x1f<provider>\x1f<account>[\x1f<folded-model>]"
```

and the two reset hooks are **deleted**. Nothing needs re-arming on a
switch, because the outgoing account's facts were never in the incoming
account's way — and they survive for the switch back.

Four decisions in that key are worth stating, because each is a bug that
didn't happen:

**1. The separator is US (0x1f), not `/`.**
Provider ids are registry-controlled, but **account labels are user-typed**.
With `/`, an account named `work/claude-opus-4-5` would forge a model-scoped
key for account `work`. The separator must be something the label prompt
cannot produce. *(Test: "user-typed account labels cannot forge a key".)*

**2. The model component is `capkey::norm_model`-folded.**
Same discipline as every other capability registry: `mistral-medium-3.5`,
`-3-5` and case variants resolve to one key. Without it you learn the block
under one spelling and miss it under the other — the user eats the rejection
twice. *(This exact bug shipped once already, as two effort ladders for one
model.)*

**3. Storage is negative-only.** Absent ⇒ not blocked. Entitlement is
permissive by default and only ever learned by rejection, so "no entry" is
the correct answer for a fresh install, a fresh account, *and* a provider
that never rejects anything. One representation, no tri-state, no migration.

**4. `""` is a legitimate account component** meaning *"the only account"*.
Single-account users — almost everyone — key under `""` and keep working
with no migration at all.

And one behavioural rule:

**Forget on removal, never on switch.** Switching must remember (that is the
entire point). Removal must forget: those facts can never be consulted
again, and a later re-login may land on a different tier. The sweep is
separator-anchored so account `work` never sweeps `work2`.

---

## How to tell which layer a fact belongs to

Ask, in order:

1. **Would this fact change if the provider swapped which model it serves
   under this id?** → identity/capability, not entitlement.
2. **Would two users on the same provider, same model, same day, get
   different answers?** → entitlement. If the difference is *which plan
   they pay for*, it is definitionally account-scoped.
3. **Does the fact survive a re-login on the same account?** → if no, it is
   session state, not a stored fact at all.
4. **Is there a manual reset hook keeping it honest?** → it is misfiled.
   Find the axis the reset is compensating for and put it in the key.

Worked examples from this codebase:

| Fact | Layer | Why |
|---|---|---|
| context window, tokenizer | capability | provider-served property of the model |
| tool grammar / `supports_tools` | capability | the host either advertises it or doesn't |
| reasoning support, effort ladder | capability | dispatch-table fact, not a plan fact |
| learned effort rejection | capability (`provider/model`) | the provider's dispatch table said no, not your plan |
| 1M-context beta | **entitlement** | same model, same host, different answer per subscription |
| Copilot premium tier | **entitlement** | billing tier decides which model families are reachable |
| OAuth access token | session state | not stored as a fact; refreshed |

The `learned_effort_sets` row is the interesting judgement call. It is
capability-scoped today because an effort enum is a property of the
provider's dispatch table. **If a provider ever gates effort levels by
subscription tier, it moves to the entitlement layer** — and now there is a
layer for it to move to, with a key shape and a test suite already in place.
That is what makes this worth naming: the next misfiled fact has an obvious
home.

---

## The general principle

> **Key a fact by everything it actually varies over — no more, no less.**
>
> Too few axes and your only correction is deletion, which is lossy and
> needs a manual reset hook to paper over. Too many and you invalidate
> unrelated state on every change to an axis the fact never depended on.

Identity carries the minimum the wire needs. Entitlement carries the account
because it genuinely varies over it. Neither borrows the other's key — and
the model row in the picker stays clean, because the account was never part
of what a model *is*.
