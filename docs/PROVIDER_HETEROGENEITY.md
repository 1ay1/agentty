# Provider heterogeneity: data, not branches

> How agentty absorbs the fact that every provider is different — and why
> adding a provider is a table row, not a code review.
>
> Companion to [provider-registry-design.md](provider-registry-design.md)
> (the original registry write-up) and §6 of
> [ARCHITECTURE.md](ARCHITECTURE.md). Where they disagree, the code wins;
> this doc names the code.
>
> For the LAYERING question underneath all of this — why a model is keyed
> `(provider, model)` and never `(provider, account, model)`, and which
> facts genuinely do need the account — see
> [IDENTITY_CAPABILITY_ENTITLEMENT.md](IDENTITY_CAPABILITY_ENTITLEMENT.md).

---

## The problem, honestly stated

There is no clean abstraction over LLM providers. The industry has at least
three response-endpoint dialects (OpenAI Responses, Anthropic Messages,
Mistral Conversations), the same model id can stream through *different*
dialects on different hosts (`gpt-*` over `/responses` on Copilot but
`/chat/completions` on Azure), effort enums differ per host, reasoning
visibility is account-tier-gated, and hosts occasionally ship a model with
its tool grammar disabled ("they launched the backend with grammar set to
none"). Every attempt at one "good" generalized abstraction hits this wall
— which is why agentty accepts the
[worse-is-better](https://dreamsongs.com/RiseOfWorseIsBetter.html)
conclusion: **every provider gets a thin, ugly adapter**.

What we refuse to accept is the usual *cost* of that conclusion: N adapters
that each re-derive the same facts, drift apart silently, and turn every
provider quirk into an archaeology project. The design rule is:

> **Heterogeneity is expressed as data on a small number of tables, each
> with a compile-time coherence proof. Adapters read the tables; they never
> re-derive a fact.**

A turn is the product of three orthogonal axes, each owned by exactly one
table:

```
turn = ROUTING (registry row) × MODEL FACTS (catalog) × WIRE MECHANICS (scaffold + epilogue)
```

---

## Axis 1 — Routing: `ProviderDescriptor` (`include/agentty/provider/registry.hpp`)

`kProviders` is a `constexpr` array; one row per backend. A row carries the
provider's *entire* identity: wire dialect, lifetime, auth style + env vars,
endpoint columns (host/path/models_path), auth capability flags
(`device_login`, `method_menu`, `oauth_native`, …), a default model — and,
since commit `14440a2d`, its **routing slot**:

```cpp
enum class RouteSlot : std::uint8_t { None, Anthropic, ChatGpt, Copilot, Kimi };

struct ProviderDescriptor {
    // ...
    RouteSlot route = RouteSlot::None;   // which long-lived transport serves this row
};
```

A `LongLived` provider holds cross-turn state (a refreshed OAuth session, a
warm HTTP/2 connection), so it is constructed once in `main()` and reused;
`RouteSlot` is the index into that router. Dispatch is now **one field
read**:

```cpp
LongLived long_lived_slot(const Selection& sel) {
    return sel.row ? sel.row->route : RouteSlot::None;
}
```

No label compares. No `is_chatgpt()` ladder. A row-less selection (a custom
`host:port` that matched no preset) routes to the per-call transports *by
construction* — it has no row to read.

### The proofs

The reason a one-field read is safe to trust blindly is that the table is
**proven coherent at compile time**. `registry.hpp` ends in a block of
`static_assert`s over `constexpr` predicates:

| Proof | Catches |
|---|---|
| `routing_consistent()` | a routed row that isn't `LongLived`; a `LongLived` row with no slot (unreachable transport); an `oauth_native` row with no slot (**the silent custom-host degradation bug**); two rows sharing a slot |
| `endpoints_consistent()` | a row whose `Wire` dialect disagrees with the path it dials (this bug shipped: the `openai` row claimed `Responses` while dialling `/v1/chat/completions`) |
| `auth_caps_consistent()` | auth flags that contradict the row's `AuthStyle` (a method menu with only one method, device-login on an API-key row) |
| `ids_unique()` | a duplicate primary key making `preset_for` ambiguous |

Every one of these encodes a bug that *actually happened* (or its obvious
neighbour). The pattern: when a field bug traces to two facts drifting
apart, don't fix the call site — merge the facts into one row and add the
`static_assert` that makes the drift a compile error.

`slot_tag(RouteSlot)` lives in the same header and supplies the `route=`
vocabulary of the `dispatch.turn` log line, so the code, the log, and the
troubleshooting docs literally cannot disagree about what a route is
called.

### Identity is a value, not a derivation

`Selection` (the resolved active provider) carries `const ProviderPreset*
row`, resolved **once** by `parse_selection`. Identity used to be re-derived
per query by comparing `openai_endpoint.label` against literals — but a
custom base URL *overwrites* that label, so a Copilot-backed custom host
silently degraded to the generic OpenAI path (empty model picker, no
error; a real field report). Carrying the row makes the identity immune to
endpoint overrides: `https://api.githubcopilot.com` as a custom spec still
adopts the copilot row, still routes to `RouteSlot::Copilot`, still gets
the Copilot OAuth session.

**Testing rule that falls out:** tests must build selections with
`parse_selection("...")`, never by hand-setting `.kind`. A row-less
`Kind::Anthropic` is a state production cannot produce; three older tests
that fabricated one were passing vacuously and failed loudly the moment
routing became row-driven. That is the "make invalid states
unrepresentable" payoff working as intended — on the tests themselves.

---

## Axis 2 — Model facts: the catalog (`include/agentty/domain/catalog.hpp`)

A **model fact** describes how a model was trained to behave — identically
on every host that serves it. These live in the catalog, next to each
other, `constexpr` where possible, pinned by `static_assert`s:

| Fact | Function | Example |
|---|---|---|
| family / generation / effort ladder | `ModelCapabilities::from_id` | Opus 4.5+ takes `effort`, Haiku doesn't |
| weak tool user (needs slim prompt, doom-loop cap, salvage) | `is_weak_model` | small local models |
| takes top-level `reasoning_effort` on the Chat wire | `infer_reasoning_compat` | mistral-medium yes, **magistral no (422!)** |
| begins output in reasoning with **no** opening think-tag | `reasons_by_default` | Magistral, DeepSeek-R1 |
| dialect can carry reasoning text at all | `wire_shows_reasoning` | Chat Completions can't for o-series |

`reasons_by_default` is the instructive one: it used to be an inline
lowercase-substring sniff inside the OpenAI-compat transport. That worked —
until you notice the Ollama transport serves the same models and would
have needed the same sniff, copied, and drifting. A model fact in a
transport is a drift bomb; moving it to the catalog beside its siblings is
what makes the *next* transport correct by default.

Resolution precedence for capabilities (highest wins):

```
user override  >  learned-from-rejection  >  live catalog  >  models.dev  >  from_id inference
```

The **learned** tier is the runtime's answer to facts no static table can
know: when a provider rejects an effort value, the reducer parses the
rejection (`parse_effort_rejection`), writes the learned set under a
provider-scoped key (`caps.effort_learned` at Warn — persisted facts are
never written silently), persists it, and retries once. Tomorrow's session
starts correct. Key discipline is `capkey::norm_model` — one spelling for
`Kimi-K2.5` / `kimi-k2-5` / `kimi k2.5` — because the
mistral-medium-3.5-vs-3-5 two-ladders bug was exactly two spellings of one
model resolving to different facts.

---

## Axis 3 — Wire mechanics: scaffold + epilogue + Site

Everything all streams share, stated once:

- **`StreamScaffold`** (`stream_scaffold.hpp`) — how a turn *runs*: status
  capture, Retry-After parsing, heartbeat/buffered-wait forwarding,
  dialect-tagged wire dump, 64 KB-capped error-body accumulation, the
  uniform end-of-turn log pair, and `stream_timeouts()` (connect 10 s /
  total 30 min / ping 15 s / idle 90 s — idle being the one legitimate
  per-transport knob: local servers grind silently during prompt
  processing, so plaintext-OpenAI gets 10 min and Ollama 120 s +
  unbounded total).
- **`finish_stream`** (`stream_epilogue.hpp`) — how a turn *ends*: exactly
  one terminal event, with fixed exit precedence
  (already-terminated > user-cancel > http-error > transport-error > clean).
- **`http::append_sse_no_buffer`** — the anti-buffering header trio.

A transport supplies **only** its dialect tag, its sink, and a parser-feed
lambda. The Responses dialect goes one step further with `Site`
(`responses/responses.hpp`): ChatGPT and Copilot share one codec and differ
only in `authorize` (where + which credentials), `decorate_body` (host
extras on the neutral body), and `explain_http_error` (host-phrased
failures). A third Responses host is one `Site`, zero codec changes — a
claim since exercised: `openai/responses_site.cpp` added api.openai.com to
the dialect without touching the codec.

### Which dialect does a turn use?

A row's `wire` is its DEFAULT, not its only dialect. The same OpenAI key
reaches `/v1/chat/completions` and `/v1/responses`; the same Copilot session
serves `claude-*` on chat and `mai-code-*` on Responses only. So the dialect
is a property of the **(provider, model) pair**, and `provider::dialect_for()`
(`provider/dialect.hpp`) is its single authority — consulted both by the
transport picking a URL and by the UI deciding whether the thinking pane may
promise output. Those were once two separate guesses, and they drifted: the
`openai` row claimed `Wire::OpenAIResponses` while dialling chat, so the UI
advertised reasoning the wire never sent.

This is not a preference. Per OpenAI's reasoning guide, Chat Completions
rejects tool calling for GPT-5.4+ at any `reasoning_effort` other than `none`,
and GPT-6-class models drop chat function calling entirely — agentty always
sends tools, so for those models the dialect decides whether a turn runs at
all.

A row advertises its second dialect with `responses_path`, checked by
`endpoints_consistent()` exactly like `path`. Because the model-family tables
are a prior about names — and names move — `note_dialect_rejected()` lets a
live 404 demote one (provider, model) for the rest of the process, so a stale
guess self-corrects instead of stranding the user. There is deliberately **no
user-facing surface**: no picker row, no flag, no config key. The user selects
a model; the protocol is agentty's problem.

When the scaffold migration ran, it *found* drift, which is the argument
for it: Ollama had never wired `on_buffered_wait` (buffered sends showed
as unattributed dead air) and logged nothing at end of turn; the error
predicate and connect timeouts had each forked three ways.

---

## Enforcement: proofs at compile time, conformance at test time

Compile-time (`static_assert`, free): table coherence — see the proofs
table above, plus the catalog's pinned family facts
(`static_assert(reasons_by_default("deepseek-r1:70b"))`).

Test-time (the conformance suites, inherited by every provider for free):

- **`provider_conformance_test`** — ONE tool-call contract instantiated
  against every dialect (fragments / snapshot / both framing; empty `{}`
  args survive; exactly one start/end per call). Written after the same
  bug shipped twice in different dialect clothes (Responses dropped
  `arguments.done` → Copilot `[invalid args]`; Chat double-concatenated
  proxy-coalesced fragments). Also pins the scaffold contract itself:
  exact 64 KB cap, success/error routing, feed-return propagation, both
  liveness callbacks wired, the timeout ladder.
- **`provider_identity_test`** — identity survives endpoint overrides;
  every id maps to its slot; slot tags match the log vocabulary.
- **`capability_conformance_test` / `model_caps_test`** — the capability
  ladder and effort gates.
- **`anthropic_sse_golden_test` / `codex_responses_test` /
  `ollama_transport_test` / `openai_transport_test`** — per-dialect
  parsers against recorded wire bytes.

And at runtime, the third leg: **observability** (see
[website/logging.md](website/logging.md), "Debugging model
heterogeneity"). Every turn is bracketed by `dispatch.turn` (the
fingerprint: route, model, effort, tool count, protocol flags) and
`stream.result` (status, transport verdict, `thinking_deltas`); every
adapter decision names itself (`salvage.*`, `caps.effort_learned`,
`*.frame_unparseable`). When a quirk isn't in the tables yet, the log is
how it gets found and promoted into one.

---

## What adding a provider costs

| You are adding… | You write | Forgetting something is… |
|---|---|---|
| an OpenAI-compat host (llama.cpp, vLLM, a gateway) | **one registry row** | `endpoints_consistent` build error |
| a long-lived OAuth provider | RouteSlot enumerator + `.route` on the row + one `router` bind in `main()` | `routing_consistent` build error |
| a Responses-dialect host | one `Site` (authorize / decorate_body / explain_http_error) | conformance suite failure |
| a second dialect on an EXISTING host | one `responses_path` column | `endpoints_consistent` build error |
| a model family with a reasoning quirk | one line in the relevant catalog function | `static_assert` on the family |
| a new failure kind | one enum entry + one `classify` arm (`error_class.hpp`) | exhaustive-switch build error |

Everything else — streaming, retries, Retry-After, salvage, capped error
bodies, terminal-event discipline, logging, the conformance suite — is
inherited.

---

## The design in one sentence

Adapters stay thin and ugly (worse-is-better is right about that), but
their variability lives in three inspectable tables — registry row, catalog
fact, wire scaffold — each guarded by a compile-time proof, exercised by a
shared conformance suite, and narrated by the `model`/`wire` log channels,
so a provider difference is a *declaration*, never a derivation.
