---
title: "Implementing the Agent Client Protocol in C++: a codec is a fold, not a ritual"
date: 2026-06-10
author: agentty
tags: [c++, acp, protocol, deep-dive, design]
excerpt: "I wrote the first C++ implementation of the Agent Client Protocol — the thing that lets Zed drive Claude Code and Gemini. Most JSON-RPC libraries hand you stringly-typed blobs and a prayer. acp-cpp treats the protocol as what it actually is: a closed algebra of inductive types, where serialization is a fold over the type's shape and the compiler proves you handled every case. Here's the whole design, from the unit type up to the deferred-reply trick that keeps a single-reader transport from deadlocking mid-turn."
---

# Implementing the Agent Client Protocol in C++

When Zed talks to Claude Code or Gemini, it doesn't shell out and scrape a
terminal. It speaks the [Agent Client Protocol](https://agentclientprotocol.com)
— a JSON-RPC 2.0 dialect over stdio, where the editor is the *client* and the
LLM-side process is the *agent*. Streaming text, tool calls, inline diffs,
permission prompts, session reload: all of it is structured messages on a pipe.

I wanted [agentty](https://github.com/1ay1/agentty) — my C++ terminal coding
agent — to live inside Zed too. That meant speaking ACP. The reference
implementations are Rust and TypeScript; there was no C++ one. So I wrote
[`acp-cpp`](https://github.com/1ay1/acp-cpp), the first C++ implementation of the
protocol, and then made agentty consume it as a submodule.

This post is about the *library*, not the integration. Because the interesting
thing isn't "I parsed some JSON." It's the question every protocol library has
to answer and most answer badly: **what is a protocol, as a type?** Get that
right and the serialization writes itself, the compiler catches your missing
cases, and a 135-definition spec collapses into a few hundred lines of
declarations. Get it wrong and you spend the next year debugging `to_json`
functions that drifted from `from_json`.

> A protocol is a closed algebra of inductive types. Serialization is a *fold*
> over that algebra, not a hand-written ritual per type. Everything in `acp-cpp`
> is a consequence of taking that sentence literally.

## The wrong way (that everyone ships)

Here's the shape of almost every JSON-RPC binding I've used. You get a
`json` blob. You reach into it by string key. You remember — in your head, or in
a comment that's already stale — that `params["sessionId"]` is a string and
`params["prompt"]` is an array of content blocks where each block has a `"type"`
that's one of five magic strings.

```cpp
// the way that rots
auto sid = params.at("sessionId").get<std::string>();
for (auto& block : params.at("prompt")) {
    if (block.at("type") == "text") { /* ... */ }
    else if (block.at("type") == "image") { /* ... */ }
    // forget "audio" here and nothing tells you. ever.
}
```

Two `SessionId`s and a `ToolCallId` are all `std::string`, so nothing stops you
passing one where the other belongs. The set of content-block types lives in
your `else if` chain, so adding a sixth means grep-and-pray. The encode path and
the decode path are separate functions that must agree by discipline alone.
There is no single place that says *this is what a `ContentBlock` is*.

The protocol has structure. This code throws it away on the first line.

## A protocol is an algebra

Strip ACP — or any protocol — down to its constructors and you find a tiny,
closed set:

- **`1`** — the unit type. No information. (The reply to `logout` is this.)
- **`A × B`** — products. Records with named fields.
- **`A + B`** — coproducts. Tagged sums: "one of these, and a label says which."
- **`List A`** — finite sequences.
- **`Maybe A`** — optional values, which is just `A + 1`.
- **`Json`** — the free, untyped term we serialize into.

Every single type in the ACP spec is a closed expression in those six
constructors. `ContentBlock` is a sum of five products. `NewSessionParams` is a
product with one required field and two optional ones. `SessionId` is a
newtype over a string. There is nothing else.

So the first file in the library, `core.hpp`, does exactly one thing: it gives
these constructors names in C++.

```cpp
struct Unit { /* one inhabitant; encodes to {} */ };

template <class A> using Maybe = std::optional<A>;   // A + 1
template <class A> using List  = std::vector<A>;     // finite sequence
template <class... As> using Sum = std::variant<As...>;   // A₀ + … + Aₙ
```

These aren't new data structures — `Maybe` *is* `std::optional`, `Sum` *is*
`std::variant`. The point is to name them after what they **mean** in the
algebra. Optionality is a sum, not a sentinel; saying `Maybe<A>` instead of
`std::optional<A>` is a tiny act of honesty that pays off when you read the
schema types later and they line up one-to-one with the spec prose.

The eliminator for a sum is total case analysis, and C++ gives it to us with a
compile error if we miss a branch:

```cpp
template <class S, class... Fs>
constexpr decltype(auto) match(S&& s, Fs&&... fs) {
    struct overload : std::remove_reference_t<Fs>... {
        using std::remove_reference_t<Fs>::operator()...;
    };
    return std::visit(overload{std::forward<Fs>(fs)...}, std::forward<S>(s));
}
```

`std::visit` refuses to compile unless the overload set covers every alternative
of the variant. So `match` over a `ContentBlock` that forgets the `audio` arm is
a build error, not a 2 a.m. production surprise. That's the whole reason to model
the sum as a `variant` instead of a tagged struct: the exhaustiveness check is
free and load-bearing.

### Newtypes: same bytes, different type

On the wire, `SessionId` and `ToolCallId` are both strings. In the program they
must never be confused. C++ doesn't give us nominal typing over a string for
free, so I built it:

```cpp
template <class Phantom, class A>
struct Newtype {
    A value{};
    constexpr explicit Newtype(A v) : value(std::move(v)) {}
    constexpr operator const A&() const noexcept { return value; }
    friend constexpr bool operator==(const Newtype&, const Newtype&) = default;
    friend constexpr auto operator<=>(const Newtype&, const Newtype&) = default;
};
```

`SessionId` is `Newtype<struct SessionIdTag, std::string>`. It hashes and
compares like a string, costs nothing at runtime, and the type checker rejects
`session_close(toolCallId)` outright. The spec *implies* these are distinct
types; `Newtype` makes the implication real without a wrapper class per id.

## Codecs as a category

Here's the move that makes everything else fall out. Don't write `to_json` and
`from_json`. Instead, define a **`Codec<T>`** — a value that witnesses a partial
isomorphism between `T` and `Json`:

```cpp
template <class T>
struct Codec {
    std::function<Json(const T&)> encode;   // total
    std::function<T(const Json&)> decode;   // partial; throws on shape mismatch
};
```

The contract is one law: `decode ∘ encode = id`. Round-trip and you get back what
you put in. Encode and decode live in the *same value*, so they can't drift apart
the way two free functions can.

Now the magic: codecs **compose along the shape of the type**. I write a handful
of combinators, one per constructor in the algebra, and then every protocol type
is built by gluing combinators together — never by hand-writing field access.

The functorial lifts are the easy ones. A codec for `List<A>` is the `A` codec
mapped over an array:

```cpp
template <class A>
Codec<List<A>> list_codec(Codec<A> inner) {
    return {
        [enc = inner.encode](const List<A>& xs) -> Json {
            Json out = Json::array();
            for (const auto& x : xs) out.push_back(enc(x));
            return out;
        },
        [dec = inner.decode](const Json& j) -> List<A> {
            List<A> out;
            for (const auto& el : j) out.push_back(dec(el));
            return out;
        }};
}
```

`maybe_codec` is the same idea: `Nothing → null`, `Just(x) → enc(x)`. And
`newtype_codec<P, A>` just borrows `A`'s codec and re-wraps — nominal in C++,
structural on the wire. A dispatcher, `codec<T>()`, picks the right lift
automatically for `List`, `Maybe`, and `Newtype`, and otherwise looks up a
user-supplied `CodecOf<T>::get()`. That's the open recursion that lets the whole
thing bootstrap from a few primitives.

### Records are products

A record codec is built from fields, where a field is a triple: a wire key, a
pointer-to-member (the ergonomic C++ encoding of "project this field"), and a
codec for the field's type.

```cpp
return record<NewSessionParams>(
    required ("cwd",                   &NewSessionParams::cwd),
    defaulted("mcpServers",            &NewSessionParams::mcpServers, List<McpServer>{}),
    optional ("additionalDirectories", &NewSessionParams::additionalDirectories));
```

That's the *entire* codec for `NewSessionParams`. Read it against the spec and
it's a transcription: `cwd` is required, `mcpServers` defaults to empty if
absent, `additionalDirectories` is optional. The three field flavours —
`required`, `optional`, `defaulted` — cover every shape the spec actually uses,
and each knows its own absence behavior: a missing `required` throws, a missing
`optional` stays `Nothing`, a missing `defaulted` takes the default.

Encoding folds over the field tuple with a C++17 fold expression, and
`Maybe`-typed fields that are `Nothing` are simply omitted from the object — no
`"foo": null` noise on the wire:

```cpp
std::apply([&](const auto&... f) {
    ((/* per field */ [&] {
        using FT = std::decay_t<decltype(v.*(f.lens))>;
        if constexpr (detail::is_maybe<FT>::value) {
            if ((v.*(f.lens)).has_value())
                out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
        } else {
            out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
        }
    }()), ...);
}, fields);
```

There's one bespoke field combinator, `meta`, for the spec's `_meta` extension
object. The spec says `_meta` is optional and carries no meaning beyond "extra
keys live here," so its codec omits the field entirely when it's null or an empty
object, and decodes a missing key to `{}`. That's the one place the protocol's
own rules leak into a combinator — and it's still declarative.

### Sums are tagged

The payoff combinator is `sum_tagged`. ACP discriminates its sums with a string
key — `"type"` for content blocks, `"sessionUpdate"` for the eleven streaming
variants. So `sum_tagged` takes the discriminator key and one `arm` per
alternative, mapping a wire label to that alternative's codec:

```cpp
using ContentBlock = Sum<
    TextContent, ImageContent, AudioContent,
    ResourceLinkContent, ResourceContent>;

template <> struct CodecOf<ContentBlock> {
    static Codec<ContentBlock> get() {
        return sum_tagged<ContentBlock>("type",
            arm<ContentBlock, TextContent>        ("text"),
            arm<ContentBlock, ImageContent>       ("image"),
            arm<ContentBlock, AudioContent>       ("audio"),
            arm<ContentBlock, ResourceLinkContent>("resource_link"),
            arm<ContentBlock, ResourceContent>    ("resource"));
    }
};
```

Encode visits the active alternative, runs *its* arm's codec (which must produce
an object), and merges in `{"type": "<label>"}`. Decode reads the tag, finds the
matching arm, and dispatches. Add a sixth content type and you add one `arm`
line and one variant member; forget to handle it at a call site and `match`
won't compile. The set of content blocks now lives in *one* place — the type —
instead of scattered across every `if (block["type"] == ...)` in the codebase.

This is the whole library's character in one example. The spec says `ContentBlock`
is one of five things; the code says `Sum` of five things; the codec says
`sum_tagged` over five arms. Three restatements of the same sentence, and the
compiler keeps them in sync.

All of this — every combinator above — fits in about 200 lines of `codec.hpp`.
Then the *entire* schema layer (`ids`, `content`, `tools`, `session_types`,
`updates`, `caps`, `methods` — all 135 spec definitions) is nothing but
`record(...)`, `sum_tagged(...)`, and `enum_codec(...)` declarations. There is
not a single hand-written `to_json` anywhere downstream. When I verified the
library field-by-field against `schema/v1/schema.json`, I was checking
declarations against prose, not chasing serialization bugs.

## The engine: bidirectional JSON-RPC

With the codec algebra in place, the JSON-RPC engine is almost anticlimactic.
The wire is itself a sum:

```
RpcMessage ≅ Request      { id; method; params }
           + Notification {     method; params }
           + Response     { id; (result | error) }
```

`RpcEngine` keeps two handler tables (requests, notifications) and one table of
outstanding outbound requests keyed by id. Inbound, it classifies each envelope
by which fields are present and routes it. Outbound, it hands you typed calls:

```cpp
// typed request: Params → std::future<Result>
template <class Result, class Params>
std::future<Result> request(std::string_view method, const Params& params);

// typed inbound handler: Params → Result
template <class Params, class Result, class F>
void on(std::string method, F handler);
```

The codec algebra does the ser/de at the boundary, so a handler is just
`PromptParams → PromptResult` — no JSON in sight. Outbound requests return a
`std::future` whose `.get()` either yields the decoded `Result` or rethrows the
peer's error as an `RpcError`. The engine is thread-safe across registration,
dispatch, and outbound calls.

### The deadlock you only hit with a real agent

Here's the bug that the obvious design walks straight into, and the part I'm
proudest of fixing cleanly.

A naive request handler is synchronous: it returns the result, the engine writes
the reply. Fine for `initialize`. **Catastrophic for `session/prompt`.**

Driving a prompt turn isn't a quick computation. The agent streams text, runs
tools, and — crucially — makes its *own* outbound requests back to the editor
mid-turn: `session/request_permission` before a write, `fs/read_text_file` for
an unsaved buffer, `terminal/create` to run a command. Those are requests *from*
the agent, and their responses come back **over the same stdio pipe** the engine
is reading.

A single-reader transport has exactly one thread pulling lines off stdin. If the
`session/prompt` handler runs synchronously on that thread and blocks waiting for
a permission response, the reader is stuck inside the handler — so it can never
read the permission response that would unblock it. Classic deadlock. The agent
asks "may I write this file?", then refuses to listen for the answer.

The fix is a request handler that can **defer its reply**. A handler returns
`Maybe<Json>`:

- `Just(result)` — synchronous, reply now (the common case).
- `Nothing` — "I've taken ownership of this reply; I'll send it later, from
  whatever thread, using the request id I captured."

The deferred path hands back a one-shot `Responder<Result>` — a move-only handle
to the pending request that the handler stashes on a worker thread:

```cpp
template <class Params, class Result, class F>
void on_async(std::string method, F handler) {
    on_request(std::move(method),
        [this, h = std::move(handler)](const RpcId& id, const Json& j) -> Maybe<Json> {
            Params p = j.is_null() ? Params{} : from_json<Params>(j);
            h(p, Responder<Result>(this, id));   // hand off
            return Nothing;                       // reply travels later
        });
}
```

So `session/prompt` registers via `on_async`. The handler kicks the turn onto a
worker thread, captures the `Responder`, and **returns immediately** — freeing
the reader thread to keep pumping stdin. The worker drives the turn, and every
permission/fs/terminal request it makes gets its response read promptly by that
now-free reader. When the turn finishes, the worker calls `responder.ok(result)`
and the reply finally goes out. `Responder` is one-shot and move-only, so the
reply is sent exactly once even if the worker is sloppy.

This is the difference between a protocol library that passes a loopback test and
one that survives a real agent doing real work. The single-reader invariant is
the whole reason it matters, and `Maybe<Json>` — the same `Maybe` from the
algebra — is what models "synchronous or deferred" honestly.

## Two facades, one engine

The last layer is ergonomics. The engine is symmetric, but the two *roles*
aren't, so there are two thin facades over it:

- **`AgentConnection`** — what an *editor* (client) holds. It exposes outbound
  calls (`initialize`, `session_new`, `session_prompt`, `session_cancel`, …) and
  takes a `ClientHandlers` record of callbacks for the things the agent calls
  back: `on_session_update`, `on_request_permission`, `fs/*`, `terminal/*`.
- **`ClientConnection`** — what an *agent* (like agentty) holds. Mirror image:
  it exposes the inbound surface and the `session_update` push.

The handler records are products of `std::function`s, and an absent function
means "method not implemented" → the engine answers JSON-RPC `MethodNotFound`
automatically. Capability negotiation falls out of the same idea: after
`initialize`, you inspect `negotiated()` to see what the peer supports, and
sending an un-negotiated method gets a clean `MethodNotFound` rather than
undefined behavior.

A full, working agent is about 25 lines:

```cpp
AgentHandlers h;
std::shared_ptr<ClientConnection> conn;

h.on_session_prompt = [&](const PromptParams& p) {
    SessionUpdateMsg upd;
    upd.sessionId = p.sessionId;
    upd.update = SU_AgentMessageChunk{
        TextContent{"Hello!", Nothing, Json::object()}, Nothing};
    conn->session_update(upd);
    return PromptResult{StopReason::EndTurn};
};

StdioTransport tx(std::cin, std::cout);
conn = std::make_shared<ClientConnection>(tx.sink(), std::move(h));
tx.start(conn->engine());
tx.join();
```

That's the entire skeleton agentty fills in — except its `on_session_prompt`
defers via the async path above, drives the real model turn, and streams dozens
of `session_update` notifications with real tool cards and inline diffs.

## What the layering bought

The library is four layers, each smaller than the one below it:

| Layer | Headers | Job |
|-------|---------|-----|
| Algebra | `core.hpp`, `codec.hpp` | The six constructors + the codec combinators |
| Schema | `ids`, `content`, `tools`, `session_types`, `updates`, `caps`, `methods` | Every ACP type as a closed expression — no hand-written `to_json` |
| Engine | `rpc.hpp` | Bidirectional JSON-RPC 2.0, typed in/out, deferred replies |
| Facade | `stdio.hpp`, `agent.hpp`, `client.hpp` | Transport + the two role handles |

The whole thing is header-only, C++20, one dependency (`nlohmann/json`), and
covers ACP protocol version 1 completely: all eleven `session/update` variants,
all five content blocks, all ten tool kinds, the full session lifecycle, modes,
config options, slash commands, filesystem and terminal callbacks, MCP servers
across all three transports, `_meta` on every type. It passes a real loopback
integration test — two engines, two threads, one genuine protocol exchange — and
a `spec_round_trip` test that encodes and decodes every method's params and
result.

The lesson I keep relearning: the languages people call "low-level" are often the
best place to take types *seriously*, because the type system is strong enough to
make the structure load-bearing and the compiler is strict enough to enforce it.
A protocol is an algebra. Once you write the algebra down — `Unit`, `Maybe`,
`List`, `Sum`, `Newtype`, and a fold that follows their shape — the serialization
stops being code you maintain and becomes a consequence of the types you already
declared. Everything after that is just transcription, and the compiler proofreads
it.

The code is at [github.com/1ay1/acp-cpp](https://github.com/1ay1/acp-cpp) (Apache
2.0), and you can watch agentty drive it inside Zed on the
[ACP docs page](/docs/acp). It's early — the API will move until a few more
downstream users shake it out — but the protocol coverage is complete, and the
shape, I think, is right.
