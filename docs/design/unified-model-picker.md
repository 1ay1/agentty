# Unified cross-provider model picker

Status: design → implementation (staged)
Author: agentty
Scope: `runtime/app/update/picker.cpp`, `runtime/view/pickers.cpp`, `domain/catalog.hpp`, `provider/selection.*`, `runtime/msg.hpp`

## The problem

Switching model and switching provider are two separate journeys today:

* `^/` opens the **model picker** — but it lists only the *currently active*
  provider's catalog (`Model::d.available_models`, refetched on every
  provider switch and cleared in between).
* `^P` opens the **provider picker** — pick a provider, *then* its models
  load, *then* you pick again.

But a user never thinks "first Anthropic, then Sonnet." They think **"I want
Sonnet"** or **"I want the cheapest 200k model"** or **"GPT-5 for this one."**
The provider is a *property* of the model they want, not a prior decision.
The two-step, two-list dance is the single biggest friction in multi-provider
use, and it's why cross-provider switching feels bolted-on rather than
first-class.

## The idea

Make the **model the primary object** and the provider a **facet** of it. One
flat, fuzzy-searchable list spanning **every authenticated provider**, where
each row is `provider · model` and selecting a row switches provider **and**
model **and** account **atomically**.

```
  /model                                              gpt

  ⭐ RECENT
   ●  Anthropic   Claude Sonnet 4.6     $3/$15   200k   ← active
      OpenAI      gpt-5-codex           $2/$8    400k

  ── all providers ─────────────────────────────────────
      OpenAI      gpt-5                 $2/$8    400k
      OpenAI      gpt-4o                $2.5/$10 128k
      Kimi        kimi-k2-0905          ¢        256k
      Copilot     gpt-4o                incl.    128k
   +  Sign in to Groq …                          (not authed — Enter to add)
```

Type `gpt` → every GPT across every provider you're signed into. `Enter` →
one atomic switch. This collapses the mental model to the one users actually
carry, and turns "use a different provider for this task" from a chore into a
reflex.

## Why this is the right shape for *this* codebase

The plumbing already exists; we are **composing** it, not rebuilding:

1. **`commit_provider_switch(m, spec, auth, label)`** (modal.cpp) is the ONE
   funnel every switch already flows through — provider select + per-provider
   model recall + effort re-clamp + auth swap + refetch + toast. The fused
   picker becomes just a new *caller* of it, with the target model pre-chosen
   so we skip the "recall/default" step and set the model explicitly.

2. **`provider::list_models_for(Selection, auth)`** is the ONE catalog router,
   dispatching on the same axes as the stream path. We call it once per authed
   provider to build the merged catalog — no new per-provider fetch code.

3. **`ModelInfo` already carries `provider`** — the row's facet is free.

4. **Auth-presence predicates are cheap and stat-cached**:
   `auth::anthropic_signed_in()`, `chatgpt::responses_available()`,
   `copilot::signed_in()`, `kimi::signed_in()`, plus `accounts::list_for()`
   for saved API-key/custom-host providers. Enumerating "which providers can I
   switch to right now" is a fast, network-free pass over `providers()`.

5. **`ProviderDescriptor` (registry.hpp) is the SSOT** — one row per provider,
   with `id`/`label`/`wire`/`auth`. "Add a provider" stays "append a row"; the
   fused picker iterates `providers()` and reads capability fields, never
   switches on identity.

## State model

A new domain type carries the merged, multi-provider catalog and its
per-provider load state. It lives beside `available_models` (which stays the
*active* provider's catalog — the wire path and context-window math still read
it) rather than replacing it, so the change is additive and the freeze/scroll
invariants are untouched.

```cpp
// domain/catalog.hpp
struct ProviderCatalog {
    std::string provider_id;                 // "anthropic", "openai", …
    std::string label;                       // "Anthropic"
    enum class State { Idle, Loading, Ready, Failed } state = State::Idle;
    std::vector<ModelInfo> models;           // list_models_for() result
    std::string account_label;               // active account for this provider
};

// A row in the FUSED picker: a concrete (provider, model) pair, or an
// un-authed provider offer that routes to login.
struct FusedRow {
    std::string provider_id;
    std::string label;                       // provider display name
    ModelInfo   model;                       // empty id ⇒ "sign in" offer
    bool        authed  = true;
    bool        active  = false;             // == current provider+model
    bool        recent  = false;             // in the MRU section
};
```

`Model::d` gains:

```cpp
std::vector<ProviderCatalog> provider_catalogs;   // merged, multi-provider
std::vector<ModelRef> recent_models;              // MRU (provider,model), cap 6
```

`ModelRef{provider_id, model_id}` persists to `Settings` as `recent_models`
(a bounded list, most-recent-first), so quick-swap and the RECENT section
survive restarts.

## Message model

The fused picker gets its own `Msg` leaf domain — parallel to
`ModelPickerMsg` / `ProviderPickerMsg`, so the exhaustiveness static_asserts
and the domain-count invariants extend cleanly:

```cpp
struct FusedPickerMsg {                 // opened with `/model` (default) and ^/
    struct Open {};
    struct Close {};
    struct Move { int delta; };
    struct Jump { enum class Where { Home, End, PageUp, PageDown } where; };
    struct FilterChar { char32_t c; };
    struct FilterBackspace {};
    struct Select {};                   // atomic switch to highlighted row
    struct Favorite {};                 // ^F on the highlighted row
    // fired per-provider as each catalog resolves; merges into the model
    struct CatalogLoaded { std::string provider_id;
                           std::vector<ModelInfo> models; bool ok; };
    using Variant = std::variant<Open, Close, Move, Jump, FilterChar,
                                 FilterBackspace, Select, Favorite,
                                 CatalogLoaded>;
    Variant v;
};
```

**The atomic switch.** `Select` resolves the highlighted `FusedRow` and:

* If `row.authed` and `row.provider_id == active` → this is a same-provider
  model change: reuse the existing model-switch path (set `model_id`,
  re-clamp effort, persist, `fetch_models`), no provider hop.
* If `row.authed` and a *different* provider → resolve that provider's auth
  (exactly as `ProviderPickerSelect` does today) and call
  `commit_provider_switch(m, spec, auth, label)` **with the target model
  pre-stashed** so step (3) of the funnel installs `row.model.id` instead of
  the recalled default. One code path, one toast, one atomic transition.
* If `!row.authed` → this is a "sign in to X" offer: open the existing login
  flow for that provider (`ApiKeyInput` / device / ChatGPT OAuth), with
  `origin::FusedPicker{}` so completing auth returns to the fused picker with
  the provider now authed and its catalog loading. Un-authed → authed is one
  continuous flow, not a separate journey.

To pre-stash the target model without widening `commit_provider_switch`'s
signature at all call sites, we add ONE optional param
(`std::string_view desired_model = {}`) defaulted empty — every existing
caller is unchanged; the fused picker passes the chosen id. Inside the funnel,
step (3) becomes: `desired_model` if non-empty, else recall, else default.

## Fetch strategy — fast, bounded, non-blocking

Opening the picker must feel instant even with 4 providers signed in:

* **Active provider's catalog is already in memory** (`available_models`) —
  render it immediately, no spinner.
* **Other authed providers** load **lazily and concurrently**: on `Open`, fan
  out one `cmd::fetch_models_for(provider_id)` per authed provider whose
  `ProviderCatalog.state == Idle`. Each resolves into a `CatalogLoaded` that
  merges in place; the row list re-sorts without disturbing the cursor's
  logical selection (we track selection by `(provider,model)` identity, not
  index, so an async insert never moves what's under the cursor).
* **Cached-first**: providers with a cached catalog (`chatgpt::
  list_models_cached`, the Anthropic/OpenAI built-in lists) seed `Ready`
  synchronously; the async fetch only *refreshes*. Cold providers show a dim
  `Anthropic  loading…` group header that resolves in place.
* **Stale-guard**: `CatalogLoaded.provider_id` is checked against the still-
  authed set (a provider signed out mid-fetch is dropped) — the same guard
  discipline `ModelsLoaded` already uses.

Blocking network calls **never** run on the reduce/render thread — every fetch
is a `Cmd`. The picker is usable (search + switch to the active provider)
the instant it opens; other providers' rows fill in within a frame or two.

## Ranking & sections

The list is one fuzzy-scored stream with lightweight section headers:

1. **RECENT** (MRU) — up to 6 `(provider,model)` you actually switch between,
   most-recent first. This is what makes the common A↔B toggle a two-keystroke
   action. The active row is marked `●` and pinned at the top of RECENT.
2. **all providers** — every authed provider's catalog, fuzzy-ranked by the
   query. Ties break by: favorite → provider registry order → context window.
3. **sign in to …** — un-authed providers as dim offers at the bottom (only
   when the query is empty or matches the provider name), so discovery is
   present but never noisy.

Favorites (`^F`) float within their section. The existing shared fuzzy scorer
(`filter_provider_indices`'s ranker, generalized to score `label + model`)
keeps reducer and view in agreement — the SSOT discipline this codebase holds.

## Quick-swap (`^Tab`)

Beyond the picker: a global `SwitchToPrevious` message bound to `^Tab` jumps
straight to `recent_models[1]` (the previous provider+model), the
editor-tab-switch idiom. No overlay, one keystroke, instant. It reuses the
same atomic-switch resolution as `FusedPickerMsg::Select`, so behaviour and
toast are identical.

## Status-bar identity

The active `provider · model · account` is always visible (extend the existing
`model_badge`). Every switch fires the switch toast (`→ OpenAI · gpt-5-codex`)
already produced by `commit_provider_switch`, so a switch always *feels* like
it happened — the confirmation is the affordance.

## What we deliberately keep

* `^P` provider picker stays — it's the right surface for **managing**
  providers/hosts/accounts (add a custom host, remove one, switch accounts on
  one provider). The fused picker is for **choosing what runs**; the provider
  picker is for **administering** the backends. Cross-hints keep them linked.
* `available_models` stays the active catalog. The wire path, context math,
  and effort clamp are unchanged.
* `commit_provider_switch` stays the one funnel — we add a caller and one
  optional param, we don't fork it.

## Invariants & tests

* **Atomicity**: a `Select` on a cross-provider row must leave provider,
  model, effort, auth, and persisted settings mutually consistent (no
  `codex → claude-opus` cross-provider stale-model bug — the pre-stashed
  `desired_model` closes exactly the hole step (3) was written to guard).
* **Selection stability**: async `CatalogLoaded` merges must not move the
  logical selection (identity-keyed cursor).
* **Auth honesty**: an un-authed row can never switch — it can only open
  login; a provider signed out mid-session drops from the list on next open.
* **No blocking on the UI thread**: every catalog fetch is a `Cmd`.
* Tests land in `provider_model_switch_test` (atomic compound switch, MRU
  recall, un-authed→login routing, stale CatalogLoaded drop) and the adapter
  render test (row facets, section headers, active marker).

## Rollout

1. Additive state (`ProviderCatalog`, `recent_models`) + `Settings` field.
2. `FusedPickerMsg` domain + reducer, reusing `commit_provider_switch`
   (+`desired_model`) and `list_models_for`.
3. Fused row builder + renderer (shared scorer, sections, badges).
4. Bind `/model` and `^/` to the fused picker; keep `^P` as-is.
5. `^Tab` quick-swap + MRU persistence.
6. Tests, then docs (`docs/website/interface.md` + a keybindings note).
