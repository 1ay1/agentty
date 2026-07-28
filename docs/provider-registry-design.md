# Provider registry: single source of truth

agentty speaks to Anthropic, OpenAI (chat + Responses), the ChatGPT/Codex
native line, and any Ollama / raw `host:port` endpoint. Every one of those is a
**native C++ transport** — HTTP/2 + SSE / NDJSON straight through the in-house
`nghttp2` + OpenSSL stack, no Node shim, no per-provider slow path. The design
rule for the whole layer is the one agentty applies everywhere: **one fact, one
place.** A provider difference lives in exactly one declaration; the transports
read it, they never re-derive it.

Three seams enforce that. They are the whole "registry".

---

## 1. Model capabilities — `include/agentty/domain/catalog.hpp`

`ModelCapabilities::from_id(model_id)` is the *only* code that looks at a model
string and decides what the model can do. Family, generation, context window,
max output tokens, and whether the model takes a reasoning-effort knob are all
derived here and nowhere else. Transports, the picker, and the token accountant
all call `from_id` (or the free helpers `max_output_tokens_for` /
`effort_wire_for`) rather than string-matching model ids themselves.

The families:

| Family   | Ids                                              | Effort | Notes                                   |
|----------|--------------------------------------------------|--------|-----------------------------------------|
| Haiku    | `claude-haiku-*`                                 | no     | fast lane                               |
| Sonnet   | `claude-sonnet-*`                                | 4.5+   | workhorse                               |
| Opus     | `claude-opus-*`                                  | yes    | 1M ctx, 128k out                        |
| Fable    | `fable-5`, general-access 2026 flagship          | yes    | Opus-class specs, ranks above Opus      |
| Mythos   | `mythos-5`, restricted 2026 flagship             | yes    | same model as Fable, gated              |
| **Gpt**  | OpenAI `gpt-5.x` Responses line (`gpt-5.6-sol`…) | yes    | ChatGPT/Codex native; effort + big out  |
| Unknown  | `gpt-4o`, `gpt-4.1`, `gpt-3.5-turbo`, others     | no     | generic OpenAI-compat, 16k out cap      |

The load-bearing invariant, locked by `tests/model_caps_test.cpp`
(`test_gpt5_codex_caps`): **`gpt-5.x` is a first-class `Family::Gpt`** with
`supports_effort()` and a large output budget, while the legacy `gpt-4*` /
`gpt-3.5` ids stay generic (`!is_gpt()`, `!supports_effort()`, 16384 output).
Adding a model means adding one row to `from_id`, not touching four transports.

---

## 2. Turn termination — `include/agentty/provider/stream_epilogue.hpp`

Every streaming transport hits the same two problems once its body has been fed
to the HTTP client, and historically each open-coded both and drifted (the
ChatGPT path once reported a clean turn as *cancelled*). The epilogue header is
the single source of truth:

- **`finish_turn_once(terminated, sink, stop, err?, retry_after?, before_finish?)`**
  emits *exactly one* terminal event and latches `terminated`. Success →
  `StreamFinished{stop}` (running the transport's `before_finish` flush/salvage
  hook first); error → `StreamError`. A second call is a no-op.
- **`classify_stream_end(terminated, result_ok, http_status, cancel)`** maps a
  loop exit to one `StreamEnd` with fixed precedence: `AlreadyTerminated` →
  `UserCancelled` → `HttpError` → `TransportError` → `CleanClose`. The
  already-terminated case wins, so an intentional read-abort after a clean
  finish is never mistaken for a cancel.

Transport-specific work (close an open tool block, salvage leaked-JSON tool
args, flush held text) is injected as a callback, so the shared rule holds
without the shared code knowing any transport's internals. All four native
transports use it:

- `src/provider/anthropic/transport.cpp` — closes the tool block on both paths.
- `src/provider/openai/transport.cpp` — success-only salvage of leaked tool JSON.
- `src/provider/ollama/transport.cpp` — success-only drain/rescue.
- `src/provider/chatgpt/responses.cpp` — `classify_stream_end` on the Codex path.

> Forward note: `docs/internal-acp-backends.md` proposes folding this into a
> `TurnResult` return value + single send chokepoint, at which point
> `stream_epilogue.hpp` dissolves into the interface. Until that lands, this
> header is the source of truth and new transports get termination right for
> free by calling these two functions.

---

## 3. Provenance rendering — one node, one row

The RAG meta-strip (the dim provenance header above a turn) is emitted as a
*single* `TextElement` carrying per-segment `StyledRun`s with `TruncateEnd`,
not an hstack of separately-colored nodes. That makes it structurally
impossible to wrap onto a second row on a narrow (~30-col mobile-SSH) terminal:
the layout engine has one node to place, and it truncates rather than reflows.
Lives in `src/runtime/view/thread/turn/turn.cpp`.

---

## 4. The seams a new native provider touches — `include/agentty/provider/registry.hpp`

Everything above exists so that a *whole new backend* is mostly a data change.
The registry (`kProviders`) is the single ordered table of every backend: id,
label, blurb, `Kind` (wire dialect), `AuthStyle`, `is_local`, and the ordered
env vars its key comes from. One appended row and the picker lists it, the
badge names it, and `resolve_auth_for` knows how to authenticate it — no
if/else chain to hunt down.

A new native provider is built by satisfying the `Provider` concept
(`include/agentty/provider/provider.hpp`) — a single method
`stream(Request, EventSink)` — and nothing else in the codebase gets to know
its concrete type. The uniform, fast behaviour then comes for free from the
shared layers:

- **Requests are uniform**: the transport reads one `provider::Request`
  (model, system prompt, messages, tools, effort, context window, cancel
  token, typed `AuthHeader`). It never re-parses model strings — it asks the
  catalog. Effort is already clamped to the model's capability upstream in
  `cmd_factory.cpp` via `effort_wire_for`, so the transport just forwards
  `req.effort` onto its wire field.
- **Termination is uniform**: call `classify_stream_end` + `finish_turn_once`
  (§2). One terminal event, correct cancel/HTTP/clean-close precedence.
- **Native + fast is uniform**: SSE/NDJSON straight through the in-house
  `nghttp2` + OpenSSL client with `req.cancel` honoured; no per-provider shim,
  no emulation path, same speed on Linux/macOS/Windows.

### The recipe

1. **Catalog row** (§1) if the provider brings models with distinct caps
   (context, output, effort). Otherwise existing families cover it.
2. **Registry row** in `kProviders` — id, label, `Kind`, `AuthStyle`, env vars.
3. **Transport** implementing `stream(Request, EventSink)`, terminating via the
   epilogue. If it is OpenAI-compatible with a non-default path/port, add the
   matching arm in `openai::Endpoint::from_spec` keyed on the same id and
   you're done — no new transport at all.
4. **A `*_transport_test`** asserting the pure parse/emit behaviour with no
   network, mirroring `openai_transport_test` / `ollama_transport_test`.

### The two seams still imperative (and the target)

Routing — which concrete transport streams a turn — is a **single,
provider-agnostic function**, `provider::dispatch_stream`
(`src/provider/dispatch.cpp`). It names **no concrete transport type**: the
long-lived native providers (Anthropic + ChatGPT, which hold OAuth-token /
connection state) and the external-ACP driver are passed in as type-erased
`StreamFn`s via a `Routes` struct, bound once in `main()`. The short-lived
OpenAI-compat / Ollama transports are cheap value types built per call from the
active `Endpoint`. `main.cpp`'s `stream_fn` is a thin binder over `Routes`.

Because the seam is type-erased, it is **unit-tested**
(`tests/dispatch_route_test.cpp`): fake routes are injected and the test asserts
exactly which one each `Selection` reaches (Anthropic→anthropic route,
`is_chatgpt()`→chatgpt route) with no network and no concrete provider. A future
edit can't silently misroute a backend.

"This is the native OAuth Codex backend" is likewise **one predicate**,
`Selection::is_chatgpt()`, not a `label == "chatgpt"` literal re-derived at the
~6 sites (dispatch, prewarm, model-list, effort clamp, login gate, picker) that
used to compare the string themselves.

One hand-written spot remains for a genuinely new `Kind` (a new wire dialect,
not an OpenAI-compat endpoint): its arm in `dispatch_stream` (plus a `Routes`
field if it is long-lived). Prewarm still derives its host in
`prewarm_active_provider`; the registry deliberately does not own the concrete
`Provider` type (kept behind the type-erased `Deps::stream` seam), which is why
routing is a function over erased callables rather than a table of
constructors. **Remaining target:** a `prewarm_host(sel)` accessor so prewarm
reads the host off the row, closing the last uncorrelated seam. This is the
natural sibling of the epilogue's own end-state (folding into a `TurnResult`
return value); both are tracked as follow-ups.

Adding a native provider is now: one catalog row + one registry row + one
transport + one `dispatch_stream` arm (+ a `Routes` field if long-lived) + one
routing-test row — fully uniform, type-erased, and tested.

---

## Why this shape

DRY across providers is not cosmetic here — every duplicated "how a turn ends"
or "what can this model do" was a place two providers drifted and one grew a
bug the other didn't. Collapsing each onto one declaration means a new provider
or a new model is a *data* change (one catalog row, one registry row, two
epilogue calls), runs at full native speed on every platform, and inherits
correct behaviour instead of re-implementing it.
