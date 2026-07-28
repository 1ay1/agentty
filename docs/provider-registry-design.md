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
without the shared code knowing any transport's internals. And rather than each
transport hand-rolling the `if (!result) … if (!is_success) … else finish`
ladder around those two primitives, the epilogue exposes the **whole post-loop
as one call**:

- **`finish_stream(StreamOutcome) -> StreamResult`** is the entire tail of a
  streaming transport, taken as ONE argument. It classifies the exit and emits
  exactly one terminal event with the right message and precedence, **and
  returns a `StreamResult`** naming how the turn ended. `StreamOutcome` bundles
  everything and states each fact ONCE — `terminated` is a *reference* to the
  transport's own latch (not a bool copied in twice) and `sink` is the
  transport's `EventSink`, both carried in the struct so there is nothing to
  duplicate or mismatch at the call site. The rest: `result_ok`, `http_status`,
  `cancel`, `stop`, and the hooks — `http_error_message()` /
  `transport_error_message()` build the user message from the buffered error
  body; `before_finish` runs success-only (salvage / flush); `on_any_end` runs
  on success *and* error (close an open tool block). All four transports end a
  turn through this one call and `return` its result — no transport
  re-implements the classify-and-emit ladder.

- **`classify_stream_end(terminated, result_ok, http_status, cancel)`** is the
  precedence heart underneath it: `already-terminated > user-cancel > http-error
  > transport-error > clean-close`, fixed and identical for every provider.
  That ordering is load-bearing — reversing cancel and http would turn a user's
  Esc during a 500 into a spurious "HTTP 500" — so it is locked in isolation by
  `test_classify_stream_end_precedence` in `tests/dispatch_route_test.cpp`.

### The outcome is a value, not just a side effect — `StreamResult`

A turn's terminal state used to be observable *only* as a `StreamFinished` /
`StreamError` Msg pushed into the sink; a caller that wanted to know "did this
error? was it a user cancel? what was the retry hint?" had to intercept and
re-decode events. That is now a first-class **return value**:

- The `Provider` concept is `stream(Request, EventSink) -> StreamResult` — the
  outcome is part of the *type*. `run_stream_sync` (native transports),
  `stream_responses` (ChatGPT), the four `Provider` adapters, `dispatch_stream`,
  and the erased `StreamFn` / `Routes` all thread it through, so the value
  survives the type-erased routing boundary instead of being silently dropped.
- `StreamResult{end, stop, error?, retry_after?, http_status}` with
  `ok()` / `cancelled()` / `already_terminated()` accessors. The emitted
  terminal Msg is **derived from the same classification** as the returned
  value, so the two can never disagree — Msg-reactive callers (the runtime
  reducer) keep seeing `StreamFinished` / `StreamError`; value-reactive callers
  read a field.
- Pre-flight bail-outs a transport handles itself (not authenticated / body
  encode failure) return `StreamResult::failed(msg)` after emitting their own
  `StreamError`, so the value still reflects reality.
- The ACP arm (`stream_external_acp`) folds the ACP backend's own per-round
  `provider::TurnResult` (a richer, adjacent-layer type in `acp_backend.hpp`,
  carrying `TurnError{auth_expired, user_cancel, …}`) onto a `StreamResult` at
  the boundary — the two describe adjacent layers and stay distinct types.
- Locked by `tests/dispatch_route_test.cpp`
  (`test_dispatch_propagates_stream_result`): dispatch hands back the transport's
  `StreamResult` (clean-close `ok()` vs. a transport error with its message)
  through the real erased seam, no network.

### The retry path consumes the outcome, not the prose

The payoff of carrying the outcome as data: the runtime's retry decision
(`src/runtime/app/update/stream.cpp`) no longer reverse-engineers the failure
class from the human error string. On an HTTP-status failure the transport
stamps the exact status onto the emitted `StreamError` Msg
(`StreamError::http_status`, set by `finish_stream` from the same
`StreamOutcome`). The reducer calls **`provider::classify_stream_error(message,
http_status)`**, which routes to the *typed*, compile-time-proven
`classify(HttpError)` path whenever the status is known and only falls back to
the substring sniff for the wire shapes that genuinely have none (SSE
`event: error` bodies, transport/socket failures, user cancels, the synthetic
stall). So a 429 phrased "Too Many Requests" (no digits) still backs off as
`RateLimit`, and a terminal 400 whose message happens to contain "connection"
is no longer mis-retried as `Transient` — the int the transport already had wins
over a re-parse of the English. Locked by `tests/error_class_test.cpp`.

All four native transports use it, differing only in their hooks:

- `src/provider/anthropic/transport.cpp` — `on_any_end` closes the tool block.
- `src/provider/openai/transport.cpp` — `before_finish` salvages leaked tool JSON.
- `src/provider/ollama/transport.cpp` — `before_finish` drains/rescues held text.
- `src/provider/chatgpt/responses.cpp` — no hooks; the plain success/error split.

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
`stream(Request, EventSink) -> StreamResult` — and nothing else in the codebase
gets to know its concrete type. The uniform, fast behaviour then comes for free
from the shared layers:

- **Requests are uniform**: the transport reads one `provider::Request`
  (model, system prompt, messages, tools, effort, context window, cancel
  token, typed `AuthHeader`). It never re-parses model strings — it asks the
  catalog. Effort is already clamped to the model's capability upstream in
  `cmd_factory.cpp` via `effort_wire_for`, so the transport just forwards
  `req.effort` onto its wire field.
- **Termination is uniform**: fill in a `StreamOutcome` and `return
  finish_stream(…)` (§2). One terminal event, correct cancel/HTTP/clean-close
  precedence, the whole post-loop in one call — and the `StreamResult` it hands
  back is the transport's return value.
- **Native + fast is uniform**: SSE/NDJSON straight through the in-house
  `nghttp2` + OpenSSL client with `req.cancel` honoured; no per-provider shim,
  no emulation path, same speed on Linux/macOS/Windows.

### The recipe

1. **Catalog row** (§1) if the provider brings models with distinct caps
   (context, output, effort). Otherwise existing families cover it.
2. **Registry row** in `kProviders` — id, label, `Kind`, `AuthStyle`, env vars.
3. **Transport** implementing `stream(Request, EventSink) -> StreamResult`,
   ending the turn by filling a `StreamOutcome` and `return`ing `finish_stream`.
   If it is OpenAI-compatible with a non-default path/port, add the matching arm
   in `openai::Endpoint::from_spec` keyed on the same id and you're done — no new
   transport at all.
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
used to compare the string themselves. And that predicate is now itself
**data-driven**: `Selection::is_oauth_native()` reads an `oauth_native` bool off
the `ProviderPreset` row ("signs in with the provider's own OAuth + rides a
dedicated long-lived transport", set only on the ChatGPT row today), so a second
OAuth-native provider becomes special by setting one flag — no new label
predicate. `dispatch_route_test` asserts the flag lights up `is_oauth_native()`
for chatgpt and stays false for a bearer-key OpenAI row.

One hand-written spot remains for a genuinely new `Kind` (a new wire dialect,
not an OpenAI-compat endpoint): its arm in `dispatch_stream` (plus a `Routes`
field if it is long-lived). **Prewarm is no longer one of them.** The
connection-warming host is now a `prewarm_host` column on the `ProviderPreset`
row (Anthropic → `api.anthropic.com`, ChatGPT → `chatgpt.com`; empty for every
other row — hosted OpenAI-compat backends warm their own `Endpoint` host, local
/ ACP backends warm nothing). `prewarm_target(Selection)` is a **pure,
registry-driven function** returning the `{host, port}` to warm (or empty to
skip); `prewarm_active_provider()` is a three-line wrapper that resolves the
target and opens the socket. The whole warm-routing table is unit-tested in
`dispatch_route_test` with no socket opened — Anthropic, ChatGPT, hosted
OpenAI-compat, local, ACP, and the port-0 sentinel each assert their target.

The registry deliberately does not own the concrete `Provider` type (kept
behind the type-erased `Deps::stream` seam), which is why *routing* is a
function over erased callables rather than a table of constructors — but every
static *fact* about a backend (auth, env vars, warm host) now lives on its row.
The epilogue's end-state — folding the turn outcome into a `StreamResult` return
value on the `Provider` concept itself — is now shipped (§2), so the terminal
outcome is a value threaded through every layer rather than a sink-only side
effect.

Adding a native provider is now: one catalog row + one registry row (id, label,
auth, env, warm host, oauth_native) + one transport + one `dispatch_stream` arm
(+ a `Routes` field if long-lived) + one routing-test row — fully uniform,
type-erased, data-driven, and tested.

---

## Why this shape

DRY across providers is not cosmetic here — every duplicated "how a turn ends"
or "what can this model do" was a place two providers drifted and one grew a
bug the other didn't. Collapsing each onto one declaration means a new provider
or a new model is a *data* change (one catalog row, one registry row, one
`finish_stream` return), runs at full native speed on every platform, and
inherits correct behaviour — including a typed `StreamResult` outcome — instead
of re-implementing it.
