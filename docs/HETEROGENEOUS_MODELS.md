# Heterogeneous models, one pipeline

What GitHub's Copilot runtime does to make many vendors behave like one,
what agentty does today, and the gap between them.

> **The thesis, stated once:**
> Robustness across vendors is not per-vendor correctness. It is a
> **narrow neutral core** that every vendor is forced through, with the
> vendor-specific part reduced to a leaf that cannot invent behaviour.

This is the companion to `docs/COPILOT_RESPONSES.md`. That document is
about one wire and four bugs. This one is about why those bugs were
possible at all, and what structure prevents the next four.

---

## 1. The mistake this document exists to correct

We fixed Copilot four times. Each fix was right. None of them was
structural, and that is why there was always a next one.

The framing was wrong. "Copilot is broken" is not a provider problem — it
is a **normalization** problem. Copilot itself is a multi-vendor router:
it serves OpenAI, Anthropic, Google and xAI models behind one endpoint, in
three different wire dialects, and its client makes them all look like one
thing to everything above it.

That is the same job agentty has. So the right question was never "how do
we fix Copilot", it is:

> How does a client absorb vendor differences so that the agent loop above
> never learns which vendor it is talking to?

GitHub answered that question in ~144 source files. Their answer is
visible in the binary, and it is worth reading carefully, because it is a
more disciplined version of what we already half-have.

---

## 2. What is actually in their runtime

`package/prebuilds/linux-x64/runtime.node` is 98 MB of Rust with source
paths intact. The model layer:

```
model/
  client.rs  resolver.rs  lookup.rs  policy.rs  provider_config.rs
  events.rs  completion_options.rs  available_model_lists.rs

  anthropic_transport.rs          anthropic_conversion.rs
  chat_completion_transport.rs    chat_completion_orchestrator.rs
  responses_transport.rs          responses_reasoning.rs

  interaction/
    driver.rs            model_call.rs        request_preparer.rs
    processors.rs        seams.rs             event_sink.rs
    anthropic_dispatcher.rs   responses_dispatcher.rs
    websocket_responses_dispatcher.rs         direct_transport.rs
    endpoint_refresh.rs  session_token_refresh.rs
    responses_size_recovery.rs
```

Read the shape, not the filenames.

**Three transports. One `interaction/`.** The transports are leaves. The
interaction layer is the spine, and it is where everything that is not
vendor-specific lives: the driver, the request preparer, the processors,
the event sink, the token refresh, the recovery.

A dispatcher per dialect sits at the boundary — `anthropic_dispatcher`,
`responses_dispatcher` — and its only job is to turn that dialect's frames
into the neutral vocabulary. Nothing above a dispatcher knows the dialect
exists.

### 2.1 The neutral vocabulary

Their struct table names it exactly:

```
PreparedModelCall          ModelIdentity            ModelHttpRequest
AssistantMessageStartData  AssistantMessageDeltaData
AssistantReasoningData     AssistantReasoningDeltaData
AssistantToolCallDeltaData AssistantStreamingDeltaData
AssistantTurnEndData       AssistantUsageData
ModelCallStartData         ModelCallFinishedData    ModelCallFailureData
```

Note what these are *not*. There is no `OpenAIDelta`, no
`AnthropicContentBlock`, no `ResponsesItem` above the dispatcher. By the
time an event is `AssistantToolCallDeltaData`, its vendor is gone.

`PreparedModelCall` is the other half: the request, normalized, before any
transport sees it. A dialect cannot decide to honour a field or not —
it receives a prepared call and encodes it.

That single property is what would have prevented our reasoning bug
(`COPILOT_RESPONSES.md` §6.1), where one transport of four quietly
ignored the toggle for months.

### 2.2 What they deserialize, and what they don't

serde emits field names contiguously. Their Responses event struct:

```
…tool_search_output econnrefused enotfound  output_index  call_id  custom_ …
```

`output_index` and `call_id`. **No `item_id`.** They do not deserialize
the field whose rewriting broke us, because they key on the identity a
proxy cannot forge.

Same table, a few hundred bytes on:

```
partial_json   copilot_salvaged_tool_input_ids   copilot_quota_snapshots
```

They **reconstruct** tool inputs that fail to route, **track which ids**
they salvaged, and **report it in telemetry**. You do not build that
unless it fires in production against your own servers.

The correct reading is not "they have a workaround we lack". It is:

> **Argument loss on a multi-vendor wire is expected, not exceptional.**
> Design for it as a normal state with a measurement, not as an error
> with a log line.

### 2.3 Resilience as a layer, not a habit

Also present, none of it inside a transport:

| Module / string | What it means |
|---|---|
| `model_bindings/api_circuit_breaker.rs`, `CLOSED`/`HALF_OPEN` | per-endpoint circuit breaking |
| `interaction/endpoint_refresh.rs`, `session_token_refresh.rs` | auth refresh mid-stream, above the wire |
| `interaction/responses_size_recovery.rs` | oversized response recovery |
| `ModelCallFailureRequestFingerprint` (7 fields) | failures fingerprinted, not just logged |
| `user_weekly_rate_limited`, `user_model_rate_limited`, … | rate limits typed, not string-matched |
| `AutoTierSwitchFailedData`, `recommended_auto_tier` | model tier switching as an event |

Every one of these is a cross-vendor concern implemented **once**. Ours
are either per-transport or absent. §4.

---

## 3. What agentty does today

Credit where due: we already have the right ideas. We have them in the
wrong number of places.

Measured, not assumed:

| Transport | lines | uses `wire::` | `ToolCallTracker` | salvage |
|---|---:|---:|:---:|---:|
| `openai/transport.cpp` | 2955 | 31 | yes | 90 |
| `ollama/transport.cpp` | 1843 | 24 | **no** | 69 |
| `responses/codec.cpp` | 881 | 13 | yes | **0** |
| `anthropic/sse.cpp` | 419 | 3 | **no** | **0** |

Read that table as a risk map.

- **Attribution discipline** (`ToolCallTracker`, which returns `Ambiguous`
  rather than guessing) is in **2 of 4**. Ollama and Anthropic each hand-roll
  their own tool-call state.
- **Salvage** is in **2 of 4**, and the two implementations are unrelated
  to each other — `openai` has one flavour, `ollama` another, both local.
- **`responses/`** has attribution but no salvage, which is exactly the
  combination that produced `args=0` on 16 consecutive calls.

The `wire::` column is the real tell. It is the size of our neutral core,
and it is small — two headers, `tool_calls.hpp` and `streamed.hpp`.
Everything else that *could* be shared is copied or absent.

### 3.1 The shape of every bug we shipped

Now the pattern is obvious. Each of the four Copilot bugs was a
**divergence between transports**, not an error in one:

| Bug | Divergence |
|---|---|
| reasoning toggle ignored | 3 transports honoured it, 1 did not |
| zero-arg calls malformed | main reducer seeded `{}`, subagent loop did not |
| `item_id` misrouting | `responses/` trusted a field, `openai/` validated |
| mid-stream error swallowed | decoder reported it, one consumer had an empty arm |

None is a hard problem. Each is what happens when the same question is
answered in four places. GitHub answers it in one and the question cannot
be reached from the leaves.

> **This is the whole lesson.** Robustness here is not cleverness at the
> wire. It is arranging things so a transport has *no opportunity* to be
> individually wrong.

---

## 4. The gap, concretely

Five items, ordered by how much silence they remove. Each one is real
today, not speculative.

### 4.1 One attribution seam, used by all four

`ToolCallTracker` already encodes the rule and makes guessing unwritable —
`attribute()` returns `Ambiguous`, `sole()` returns `nullopt` for zero *or
many*. It is used by two transports.

Ollama and Anthropic should not have their own tool-call state. The
Anthropic dialect addresses blocks by `index` and looks safe *today*;
"looks safe today" is precisely the reasoning that produced the previous
four bugs.

**Test:** `attribution_discipline_test` already scans for arbitrary picks.
Extend it to assert that every transport routes through the tracker — the
shape, not the instance.

### 4.2 Salvage as a shared, measured layer

Today: `openai` salvages one way, `ollama` another, `responses` and
`anthropic` not at all.

GitHub's position (§2.2) is that salvage is a normal path with telemetry.
Ours should be one helper, reachable from every dialect, that

- reconstructs what it can,
- records that it salvaged and for which call,
- and is **counted**, so we know the rate instead of guessing.

We already log `responses.tool_args_unroutable`. That is the measurement
hook. Use it before building the recovery, so the recovery is sized to
reality.

### 4.3 A prepared-call seam that transports cannot bypass

`PreparedModelCall` is the structural answer to §3.1's first row. If every
transport receives a neutral, already-resolved call, "this dialect ignores
`effort`" stops being expressible.

We have `provider::Request` and a conformance test that asserts the
reasoning gate across dialects — the idea is there. What is missing is
that the *request* is still assembled per-transport, so a field can be
dropped on the floor and only a test would catch it.

**Direction:** make the neutral call the only input a transport gets, and
extend `provider_conformance_test` from `reasoning` to every field that is
a promise to the user — `effort`, `max_tokens`, `context_window`, tool
advertisement.

### 4.4 Typed failures, not string matching

They have `ModelCallFailureRequestFingerprint` (7 fields) and typed rate
limits: `user_weekly_rate_limited`, `user_global_rate_limited`,
`user_model_rate_limited`, `integration_rate_limited`.

We have `error_class.hpp`, which is good, and a lot of per-transport
message sniffing, which is not. A fingerprint means two failures can be
recognised as *the same failure* across vendors — which is what a circuit
breaker and a retry ladder both need.

### 4.5 The carrier we drop

`response.custom_tool_call_input.delta` — GitHub deserializes it, we hit
`responses.unhandled_event` and drop the arguments.

Worth noting precisely: that path logs via `util::dbglog`, **not**
`AGT_LOG(Wire, …)`, so it does *not* appear under `AGENTTY_LOG=wire=debug`.
A dropped carrier is invisible on the channel where someone debugging this
would be looking. That is a small fix with an outsized effect on the next
report.

---

## 5. The principles worth keeping

Each is stated so it can be checked, not admired.

1. **The neutral core is the product; transports are leaves.**
   Everything that is not literally this vendor's bytes belongs above the
   dispatcher. If two transports both do it, it is in the wrong place.

2. **A transport must have no opportunity to be individually wrong.**
   Give it a prepared call and a sink. Do not give it the ability to
   decide whether a field applies.

3. **Key on what a proxy cannot forge.**
   Position over identifier, when the identifier passes through a rewriter.
   A present-but-unresolvable field outranks fallbacks while carrying no
   information, which makes it worse than absence.

4. **Guessing converts a visible failure into a silent corruption.**
   Wrongly-attributed arguments still parse and still dispatch. Fail the
   turn.

5. **Expected degradation deserves a measurement, not a warning.**
   If it fires in production, count it. A warning tells you it happened
   once; a counter tells you whether to build the recovery.

6. **Every promise the neutral call makes must be asserted per dialect.**
   A promise kept by three implementations of four is worse than none,
   because it looks arbitrary to the user and correct to the author.

7. **Diagnosis belongs on the channel someone will actually read.**
   A dropped event logged where nobody looks is a dropped event.

---

## 6. Where to start

The order matters, because each step makes the next one cheaper and
safer.

1. **§4.5** — route `unhandled_event` to the `Wire` channel and add the
   fifth carrier. Hours. Removes a silent drop and improves every future
   report.
2. **§4.1** — move Ollama and Anthropic onto `ToolCallTracker`, extend the
   discipline test to assert it structurally. This is the one that stops
   the recurring class.
3. **§4.3** — tighten the prepared-call seam and grow the conformance
   contract to the remaining promised fields.
4. **§4.2** — measure unroutable rate first, then build salvage to fit.
5. **§4.4** — typed failure fingerprints, once there is a retry/breaker
   consumer that needs them.

Nothing here is a rewrite. The pieces exist; they are in two places when
they should be in one, and in zero places when they should be in one.

---

## 7. Map

| Where | What |
|---|---|
| `include/agentty/provider/wire/tool_calls.hpp` | the attribution seam — the core, today |
| `include/agentty/provider/wire/streamed.hpp` | snapshot/fragment reconciliation |
| `include/agentty/provider/provider.hpp` | `Request` — the prepared call, such as it is |
| `src/provider/{openai,responses,anthropic,ollama}/` | the four leaves |
| `tests/provider_conformance_test.cpp` | promises asserted per dialect |
| `tests/attribution_discipline_test.cpp` | the source scan for arbitrary picks |
| `docs/COPILOT_RESPONSES.md` | the one-wire companion: four bugs in detail |
| `docs/TOOL_CALL_ATTRIBUTION.md` | why identity is `(id, index)` jointly |

**Provenance.** Everything attributed to GitHub's client here was read out
of `/usr/bin/copilot` (AUR `github-copilot-cli-bin` 1.0.86): a Node SEA
whose embedded tar sits at offset `108754132`, containing
`package/prebuilds/linux-x64/runtime.node`. Source paths, serde field
tables and struct names survive in the binary. No decompilation was
needed, and none of it is guesswork — where this document says "they do
X", the string is in the binary.
