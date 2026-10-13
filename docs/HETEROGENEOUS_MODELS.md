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

### 2.2 Their model-layer vocabulary

Near their model layer's strings:

```
…tool_search_output econnrefused enotfound  output_index  call_id  custom_ …
```

`output_index` and `call_id` are **present** in that vocabulary.

> **Do not read more into this than presence.** An earlier version of this
> section said their Responses struct deserializes these two and *not*
> `item_id`. The presence half is sound; the absence half is not provable
> from a binary that interns string literals, and the run above also
> contains OAuth error prose — adjacency in a dedup pool is not structure.
> §8 has the full correction. The `output_index` fix was diagnosed from a
> user's log, not from this line.

Same vocabulary, a few hundred bytes on:

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

Credit where due: we already had the right ideas. We had them in the
wrong number of places.

Measured before the work, and after:

| Transport | attribution, before | after |
|---|---|---|
| `openai/transport.cpp` | keyed — `ToolCallTracker` | unchanged |
| `responses/codec.cpp` | keyed — `OpenCalls` | + `output_index` first |
| `anthropic/sse.cpp` | **single `current_tool_id`** | keyed — block index |
| `ollama/transport.cpp` | hand-rolled | **atomic** — verified, documented |

The `wire::` usage count was the real tell: it is the size of our neutral
core, and it was small — two headers, `tool_calls.hpp` and
`streamed.hpp`. Everything else that *could* be shared was copied or
absent.

Salvage was in 2 of 4 transports as two unrelated implementations, and
absent from `responses/` — exactly the combination that produced `args=0`
on 16 consecutive calls.

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

## 4. The gap, as of now

Five items. **All five have shipped** — kept here with what was actually
done, because the reasoning is worth more than the checklist.

### 4.1 One attribution seam — DONE (`059b9c1e`, `e59af900`)

Anthropic hand-rolled a single `current_tool_id` and routed every
`input_json_delta` to it. The wire puts an `index` on every
`content_block` frame and we ignored it in six places.

It was safe — Claude emits blocks one at a time, so "the last one
opened" happened to be right. A bet on emission order is not a property.
Now keyed by the wire's index through one named `tool_at()`, with no
spelling of "the most recent".

**The C++ part:** deleting the old fields turned "did I find every site?"
into a build error. The compiler found a sixth site in `transport.cpp`.
That is the argument for changing the *type* rather than the uses.

Ollama turned out **not** to need the tracker. Its NDJSON carries whole
calls in one frame, so `Start`→`Delta`→`End` go out back-to-back and two
calls are never in flight. Checked rather than assumed, and now written
down so nobody re-derives it.

`attribution_discipline_test` gained two cases: every transport must
**name** its strategy (`keyed` or `atomic`) with the mechanism that
proves it, and the spellings `current_tool_id` / `active_tool_id` /
`last_tool_id` / `latest_tool_item` are banned outright. The old scanner
could not catch those — no `*begin()`, no `.back()`, nothing that looks
like a pick. It looks like a variable, and it is an attribution decision.

### 4.2 Salvage, measured — DONE (`9a0a7807`)

The carrier that needed it is `output_item.done`: its arguments are
addressed by the item's **own** `id`, not through `addressed_item()`, so
the `sole()` fallback that rescues the other carriers never applies. A
rewritten id there and the payload has no owner.

Salvage fires only where it cannot be wrong — exactly one open call.
`sole()` answers that and nothing else, so it cannot decay into "pick the
most recent". With two open we still drop: a coin flip producing valid
JSON is worse than a visibly broken turn.

And it is **counted**. `salvaged_args` / `unroutable_args` on
`responses.completed`, so one line per turn says whether the wire is
healthy — instead of counting warnings in a 20 MB log, which is what the
Copilot diagnosis actually took.

> **A note worth more than the feature.** The first version of the
> salvage test passed *without salvage ever firing* — `addressed_item`
> already rescued that carrier. Checking the log instead of trusting the
> green run showed `salvaged_args=0`, and the test was rewritten against
> the carrier that genuinely lacks the fallback. A test that passes for
> the wrong reason is worse than no test: it converts "we don't know"
> into "we checked".

### 4.3 Promises asserted per dialect — DONE (`e59af900`)

Writing the contract found a live bug: **Responses never sent
`max_tokens`.** Chat, Anthropic and Ollama all did, under their own
spellings (`max_tokens`, `max_tokens`, `num_predict`). This dialect calls
it `max_output_tokens` and dropped the field. A user capping output got
the cap on three models of four, with nothing in the UI to tell them
apart — the reasoning toggle's shape exactly, found by a test this time
rather than a report.

`provider_conformance_test` now has a **request** half. Behavioural where
a pure body builder exists, structural elsewhere — the other three
assemble their body inside the stream function, so a behavioural
assertion cannot reach them. Coarse on purpose: it cannot prove the value
is used correctly, but it proves nobody deleted the only line that reads
it. A dialect that genuinely cannot express a field must now say so in
the table, with the reason.

### 4.4 Typed failures — DONE (`eb6f2fd4`)

The assumption going in was "we string-match everywhere". Measured, that
was wrong: `error_class.hpp` is already typed, `static_assert`-proven and
per-class, the retry ladder already consumes it, and only Kimi still
sniffs (a quota-message special case, correct there).

The real gap was narrower and worse. A mid-stream `event: error` arrives
inside a **200** body — the status line is long gone — so both dialects
fell back to sniffing the human `message`, while a machine-readable
`error.type` sat unused in the same JSON object.

That fallback is wrong in both directions:

- an overload phrased "unusually high demand" matches no substring we
  know → classifies **Terminal**, turn dies on a retryable failure
- an invalid request whose message contains "connection" → classifies
  **Transient**, six retries against something that can never succeed

Now `error.type` maps onto the equivalent HTTP status and goes through
the same `classify(HttpError)` table the header path uses. One table for
both dialects, because two lists drift. An unrecognised type returns `0`
— *no opinion* — which keeps the caller on its existing path rather than
inventing a classification.

> **The mutation that survived.** Deleting the typed status on the
> Anthropic side broke nothing: the golden renderer printed
> `Error(message)` without the status, so the field was unobservable
> from any test. Fixed the renderer, and the same mutation now fails two
> assertions. A field nothing can see is a field nothing is checking —
> and green would have said otherwise.

Still genuinely absent: a **circuit breaker**. GitHub has
`api_circuit_breaker.rs` with `CLOSED`/`HALF_OPEN`; our retry ladder is
per-turn and keeps no cross-turn memory of a dead endpoint. That is a
real difference, deliberately not built yet — it needs an endpoint-health
store above the transports, and inventing one with a single consumer
would be the ceremony this section warns about.

### 4.5 The fifth carrier — DONE (`8359ddc2`)

`response.custom_tool_call_input.delta` / `.done`, item type
`custom_tool_call`, payload under `input`. We handled none of it; a model
using custom tools got `{}` on every call. Routed through the same
`feed_tool_args` seam as the other four, so attribution and
snapshot-vs-fragment reconciliation are identical **by construction**.
The framing matrix went 5 → 7.

Also: `responses.unhandled_event` logged via `util::dbglog`, not
`AGT_LOG(Wire, …)`. Under `AGENTTY_LOG=wire=debug` — the filter you
actually run — an unknown event did not appear at all. It is on
`Wire/Warn` now.

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

8. **A field nothing can observe is a field nothing is checking.**
   Mutate the source and watch the test fail. Twice in this work a test
   passed for the wrong reason — once because the code path was never
   reached, once because the renderer dropped the field under assertion.
   Green is evidence only after you have seen red.

---

## 6. Where this landed

The order mattered: each step made the next cheaper and safer.

| # | What | Commit |
|---|---|---|
| 4.5 | fifth carrier + `unhandled_event` on `Wire` | `8359ddc2` |
| 4.1 | Anthropic keyed by block index; strategy named per transport | `059b9c1e`, `e59af900` |
| 4.3 | request-half conformance; `max_tokens` on Responses | `e59af900` |
| 4.2 | salvage where it cannot be wrong, and counted | `9a0a7807` |
| 4.4 | typed mid-stream errors via the proven table | `eb6f2fd4` |

All five closed. The one remaining difference from GitHub's runtime is a
circuit breaker (§4.4) — named, scoped, and left unbuilt on purpose.

None of it was a rewrite. The pieces existed; they were in two places
when they should have been in one, and in zero places when they should
have been in one.

The measured result: attribution discipline went from 2 of 4 transports
to 4 of 4, with the remaining hand-rolled state deleted rather than
documented. Salvage went from two unrelated implementations to one that
reports its own rate.

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

---

## 8. What the binary can and cannot prove

A correction, because getting this wrong produced a confident false claim
in an earlier commit message and the method matters more than the claim.

**Reliable.** Rust leaves three things intact that are worth reading:

| Artifact | Why it is trustworthy |
|---|---|
| `src/runtime/src/….rs` paths | panic-location table, one per source file |
| `struct X with N elements` | serde's `expecting()` string, verbatim |
| literal error prose | the sentence the server or client emits |

Those give module layout, struct arity, and exact wire vocabulary. The
architecture in §2 rests on them and stands.

**Not reliable: ABSENCE, and ADJACENCY.**

Rust interns and deduplicates string literals. A field name used by five
structs appears **once**, in a pool that also holds unrelated prose. So:

- *"`item_id` is not in their Responses struct"* — **unprovable this way.**
  `item_id` appears twice in the whole binary, and both are a
  window-reading tool's parameter schema. That does not mean the Responses
  decoder never names it; it means the string is shared or absent from the
  pool for reasons invisible from outside.
- *"`output_index` and `call_id` are adjacent, therefore same struct"* —
  **no.** That run also contains `econnrefused`, `enotfound` and OAuth
  error prose. Adjacency in a dedup pool is not structure.

So the honest form of §2.2 is: **`output_index` and `call_id` are present
in their model layer's vocabulary.** The rest was inference narrated as
reading.

**Does that change what shipped?** No. The `output_index` fix
(`16e09b35`) was diagnosed from a user's log — 48 unroutable frames, three
per call, ~440-char base64 blobs matching no announced item — and three
other projects reached the same fix independently. The binary was
corroboration that got over-weighted in a commit message. The code is
right; one sentence of its justification was not.

**The rule going forward:** presence is evidence, absence is not, and
adjacency is never structure. Where this document makes a claim the
binary cannot support, it says so.

---

## 9. Vision, and the layer a capability belongs to

The first thing found by applying §8's rule honestly — four literal error
strings, which is exactly the category that IS trustworthy:

```
not supported for vision
image media type not supported
exceeded maximum number of images
vision is not enabled for this organization
```

Four refusals of the same request, wanting three different reactions. The
last one is the interesting one.

### 9.1 Why it is not one boolean

"This model cannot see" and "your organisation forbids it" are both 400s
mentioning vision. They belong to **different layers**:

| | Scope | Fix | Where it belongs |
|---|---|---|---|
| model capability | the MODEL | pick another model | `ModelInfo::supports_vision` |
| org policy | the ACCOUNT | log into an entitled account | `entitlement::Fact` |

Recording an org block as a model capability is the worse error of the
two: it blames the model, persists against it for **every** account, and
survives the account switch that fixes it. The user moves to a working
login and finds the model still refusing to look at images, with nothing
anywhere explaining why.

That is precisely the failure `docs/IDENTITY_CAPABILITY_ENTITLEMENT.md`
was written about, and the reason the account-blind `context_1m_blocked`
bool became a keyed fact. The same trap, one capability later.

### 9.2 The ordering hazard

`"vision is not enabled for this organization"` **contains the word
"vision"**. A classifier testing model-capability patterns first matches
it and records the wrong layer.

So `classify_vision_rejection()` runs the narrow, most-specific test
first, and a test pins exactly that — moving the org arm after the model
arm fails two assertions.

### 9.3 What each kind does

| Kind | Recorded where | Scope |
|---|---|---|
| `OrgPolicy` | `Fact::VisionOrgPolicy`, empty model id | account-wide |
| `ModelCapability` | `ModelInfo::supports_vision = false` | that model |
| `MediaType` | nowhere | this request only |
| `TooManyImages` | nowhere | this request only |

The bottom two are deliberately not remembered: a PNG instead of a TIFF,
or two images instead of six, would succeed on that very model. Recording
them as capabilities would strip images that were never the problem.

All four then **strip the images and retry the turn**. That is the point
— the user asked a question and attached a picture, and an answer about
the text beats a dead turn with a 400 in it.

### 9.4 The tri-state default, restated

`supports_vision` is `optional<bool>`, and **unknown SENDS** — the same
asymmetry as `supports_tools`, for the same reason. Stripping on silence
would remove images from every model no catalog describes, and a vision
model whose screenshot we quietly dropped is indistinguishable from one
that looked and was unhelpful.

Only an explicit `false` withholds, because that is the one case we know.

---

## 10. Still open

- **`max_prompt_images`** — they carry a per-model image COUNT limit
  beside `supported_media_types`. We have no cap, so six screenshots to a
  model accepting two is a 400. The `TooManyImages` arm now recovers from
  it; a preflight cap would avoid it. Field names confirmed present;
  their values are not readable from the binary.
- **`supported_media_types`** — same: a declared allow-list we could
  validate against before sending.
- **Circuit breaker** — `api_circuit_breaker.rs`, `CLOSED`/`HALF_OPEN`.
  Our ladder is per-turn and forgets a dead endpoint between turns.
  Needs an endpoint-health store above the transports.

All three want a real catalog row or a real failure before being built
against. That is the §8 rule applied forward: build on what is verified,
not on what is plausible.
