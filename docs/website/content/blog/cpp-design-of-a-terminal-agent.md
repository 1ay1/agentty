---
title: "The C++ design of a terminal coding agent: a state machine that can't lie to you"
date: 2026-06-09
author: agentty
tags: [c++, architecture, deep-dive, design]
excerpt: "An LLM agent loop looks like the worst possible fit for C++ — async, streaming, full of stringly-typed events. It is actually one of the best. Here's how agentty turns the whole app into a closed sum of events, a single std::visit reducer, and a permission matrix the compiler proves correct over all 48 cells."
---

# The C++ design of a terminal coding agent

The instinct is *"C++ for a chat client? masochism."* You picture dangling
pointers in the streaming path, a use-after-free every time a tool call
overlaps a redraw, and a `switch` over event types with a `default:` that
quietly swallows the case you forgot to handle.

That picture is wrong, and it's wrong in an interesting way. An agent loop — the
thing under every terminal coding assistant — is almost *suspiciously*
well-suited to modern C++. Not C++ as a faster Python. C++ as a language where
you get to make illegal states **unrepresentable** and then have the compiler
check, at build time, that you handled every legal one.

This post is the long version of that claim. I'm going to walk through how
[agentty](https://github.com/1ay1/agentty) is actually built — the real types,
the real reducer, the real permission table — and show you where the type system
is doing load-bearing work that a dynamic language would have to discover at
runtime, in production, at 2 a.m.

> The whole app is a pure function of `(Model, Msg)`. **The compiler is a
> proof that you handled every event.** Everything below is a consequence of
> taking that one sentence literally.

## The shape of the thing: Model, Msg, Cmd

Strip an agent down and it's an Elm program. There are exactly three nouns:

- **`Model`** — all the state. The conversation, the in-flight request, the UI
  (composer, pickers, modals). One value. No globals, no singletons, no
  "current session" hiding in a static.
- **`Msg`** — every event that can ever happen, as a *closed* set. A keystroke,
  a streamed token, a tool finishing, a permission decision, a timer tick.
- **`Cmd`** — a *description* of a side effect to run next (fire an HTTP request,
  spawn a subprocess, write a file). The reducer returns these; it never
  performs them.

The entire program is one pure function:

```
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
```

Give it the current state and one event, get back the next state and a
description of what to do. The runtime loop is dumb on purpose: read an event,
call `update`, render the new `Model`, execute the returned `Cmd`, repeat. No
business logic lives in the loop. All of it lives in a pure function you can call
in a test with a hand-built `Model` and a hand-built `Msg` and assert on the
result — no network, no terminal, no mocking.

That's the architecture. The rest of this post is what C++ specifically brings
to it.

## Msg: a closed sum the compiler counts for you

Here's the part dynamic languages can't give you. In JavaScript, an action is an
object with a `type` string. Nothing stops you from dispatching
`{type: "STRAEM_DELTA"}` with a typo, and nothing stops a reducer from quietly
falling through to `default`. The set of events is *open* — it's whatever
strings happen to flow through at runtime.

In agentty, `Msg` is a `std::variant`. The set of events is **closed and known
at compile time**:

```
using Msg = std::variant<
    msg::ComposerMsg,      // typing, cursor, paste, undo, history…
    msg::StreamMsg,        // token deltas, tool-use blocks, finish
    msg::ToolMsg,          // tool exec output, permission results
    msg::ModelPickerMsg,
    msg::ThreadListMsg,
    msg::CommandPaletteMsg,
    msg::MentionPaletteMsg,
    msg::SymbolPaletteMsg,
    msg::TodoMsg,
    msg::LoginMsg,
    msg::DiffReviewMsg,
    msg::MetaMsg
>;
```

Each of those is itself a `std::variant` of leaf events. `ComposerCharInput`,
`ComposerBackspace`, `ComposerEnter`, `StreamTextDelta`, `ToolExecOutput`,
`AcceptAllChanges`, `Tick` — around eighty leaves total, each a tiny struct that
carries exactly the data that event needs and nothing more. `ComposerCharInput`
holds a `char32_t`. `ComposerPaste` holds a `std::string`. The shape of the
payload is part of the type.

This grouping is not cosmetic — it started as a flat 79-arm variant and *that*
version measurably hurt. `sizeof(Msg)` was pinned by the single heaviest leaf no
matter which event was live; one `std::visit` instantiated a 79-arm dispatch
table; and touching any one leaf forced the whole reducer translation unit to
recompile (about 15+ seconds). Grouping into twelve domain sub-variants fixed all
three: each domain's reducer lives in its own `.cpp`, so editing a composer event
recompiles the composer TU and nothing else.

And the flat construction syntax still works, because `std::variant`'s converting
constructor finds the unique domain that accepts a given leaf:

```
Msg m1 = ComposerEnter{};        // → Msg{ComposerMsg{ComposerEnter{}}}
dispatch(StreamTextDelta{"x"});  // → dispatch(Msg{StreamMsg{...}})
```

"Unique" is load-bearing. If a leaf accidentally appeared in two domain
variants, the converting constructor would be **ambiguous** and the build would
fail — but it would fail at every call site, scattered across the codebase, with
the inscrutable error message `std::variant` is famous for. So the discipline is
pinned in *one* place:

```
template <class L>
consteval int leaf_domain_count() {
    return int{in_variant_v<L, msg::ComposerMsg>}
         + int{in_variant_v<L, msg::StreamMsg>}
         + /* … every domain … */;
}

static_assert(leaf_domain_count<StreamTextDelta>() == 1,
              "StreamTextDelta must belong to exactly one Msg domain");
```

If someone files an event into two domains, *this line* fails to compile, with a
message that tells you exactly what went wrong — instead of a wall of variant
spew at some unrelated dispatch site. There's even a guard so the dispatcher and
the variant can't drift apart:

```
static_assert(std::variant_size_v<Msg> == 12,
              "Msg domain count changed — update the dispatcher");
```

Add a thirteenth domain and forget to wire it into the reducer? The build stops
and tells you. That is the whole pitch of this style: **the mistakes you'd
otherwise make become things that don't compile.**

## The reducer: one std::visit, no default case

The top-level `update` is twelve arms. That's the entire dispatch:

```
auto step = std::visit(overload{
    [&](msg::ComposerMsg cm)    { return composer_update(std::move(m), std::move(cm)); },
    [&](msg::StreamMsg sm)      { return stream_update  (std::move(m), std::move(sm)); },
    [&](msg::ToolMsg tm)        { return tool_update    (std::move(m), std::move(tm)); },
    [&](msg::ModelPickerMsg pm) { return model_picker_update(std::move(m), std::move(pm)); },
    // … nine more, one per domain …
}, msg);
```

Look at what's *not* there: no `default:`. No `else`. No "unknown message type"
branch. `std::visit` over a `std::variant` is **exhaustive by construction** — if
you don't handle every alternative, the code doesn't compile. The day someone
adds a thirteenth domain to `Msg`, this `std::visit` stops compiling until they
add the arm. You cannot ship a build that silently ignores an event, because
there is no syntactic place for "the event I forgot" to hide.

> In a dynamic agent, "unhandled action type" is a runtime surprise you find in
> a log. Here it is **a build error you find before lunch.**

Each per-domain reducer is the same pattern one level down: a `std::visit` over
that domain's leaves, again with no default. The compiler walks you through every
event in the system and refuses to let you forget one. A coding agent has a *lot*
of events — every keystroke, every streaming token, every tool lifecycle
transition — and the exhaustiveness check scales to all of them for free.

## The dangerous parts are compile-time

Now the part I'm proudest of. A coding agent's whole reason to be careful is the
permission boundary: *which tools may touch the filesystem, the shell, the
network, and under what circumstances do they get to do it without asking?*

The naive design — the one agentty used to have — gives every tool a
`needs_permission(Profile)` lambda. It's easy to write and impossible to keep
honest. Read-only `git status` historically said "never ask" while `bash` said
"always ask," even though both spawn subprocesses; the inconsistency lived in two
different lambdas nobody read side by side.

The typeful inversion: a tool doesn't get to decide its own trust level. It
declares **what it does to the world** as a set of capability tags, and a single
pure function maps `(effects × profile) → decision`.

```
enum class Effect : std::uint8_t {
    ReadFs   = 1u << 0,   // open, stat, readdir, scan
    WriteFs  = 1u << 1,   // create / modify / delete files
    Net      = 1u << 2,   // send or receive bytes over the network
    Exec     = 1u << 3,   // run an arbitrary, MODEL-chosen program
};
```

The distinction that matters isn't "does this shell out" — `git status` shells
out to `git` but it's tagged `ReadFs`, because *we* picked the command. `bash` is
`Exec` because the *model* picked it. "We know what this subprocess does" versus
"the model chose it" is exactly the line the permission flow cares about, and
it's right there in the tag.

The policy is one `constexpr` function with no state, no IO, no `Model`
dependency:

```
constexpr Decision permission(EffectSet effects, Profile profile) noexcept {
    if (profile == Profile::Write) return Decision::Allow;   // autonomous

    if (effects.has(Effect::Exec))    return Decision::Prompt;
    if (effects.has(Effect::WriteFs)) return Decision::Prompt;
    if (effects.has(Effect::Net))     return Decision::Prompt;

    if (profile == Profile::Minimal) {
        if (effects.has(Effect::ReadFs)) return Decision::Prompt;
        return Decision::Allow;   // Pure only
    }
    return Decision::Allow;       // Ask: ReadFs + Pure are trusted
}
```

You can read the entire trust model of the agent top to bottom in ten lines. But
here's the magic trick. `EffectSet` is a four-bit bitset — sixteen possible
sets — and there are three profiles. That's exactly **48 cells** in the trust
matrix. Because the policy is `constexpr`, I can *prove the whole table at
compile time*:

```
constexpr bool exhaustive_matches_spec() noexcept {
    for (every EffectSet e of the 16)
        for (Profile p : {Write, Ask, Minimal})
            if (permission(e, p) != expected_decision(e, p))
                return false;
    return true;
}
static_assert(exhaustive_matches_spec(),
              "policy disagrees with the spec on at least one cell");
```

`expected_decision` is an *independent restatement* of the same rule, written as
a parallel expression. The `static_assert` walks all 48 cells and demands the two
agree. Change one cell of the policy wrong — flip an `Allow` to a `Prompt`, drop
a profile check — and **the build breaks**. Not a test nobody runs nightly. Not a
production incident. The compiler refuses the binary.

> The most security-sensitive logic in the program is the part that *cannot ship
> wrong*, because shipping it wrong is a compile error.

There's a second `static_assert` that pins the bitset width, so if anyone adds a
fifth `Effect` tag the proof reminds them to extend both the policy and the spec
— the new capability can't slip through unscanned.

## Capabilities also schedule the parallelism

The same effect tags do double duty. When the agent wants to run tools
concurrently, *which ones are safe to overlap?* That's not a separate heuristic
sprinkled through the runtime — it's derived from the capability model by another
pure `constexpr` function:

```
constexpr bool is_parallel_safe(EffectSet active, EffectSet want) noexcept {
    constexpr auto exclusive = [](EffectSet e) {
        return e.has(Effect::WriteFs) || e.has(Effect::Exec);
    };
    if (exclusive(active) || exclusive(want))
        return active.empty();   // exclusive work runs alone
    return true;                 // Pure / ReadFs / Net compose freely
}
```

Two reads never race. A network call touches neither the filesystem nor process
state. But a `WriteFs` tool might mutate a file a sibling is reading, and `Exec`
is maximal — the model chose the command, so assume the worst and serialize it.
This rule, too, ships with compile-time spot-checks: `static_assert` that a read
composes with a read, that a write blocks a read, that two execs never overlap.
Rewrite the implementation wrong and the build tells you before you ever launch.

## Strong types kill whole bug classes

A `ThreadId` and a `ToolCallId` are both "some hex string" at runtime. At compile
time they're different types, and that difference catches the bug where you pass
one where the other is expected:

```
template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};

using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
using ModelId    = Id<ModelIdTag>;
using MessageId  = Id<MessageIdTag>;
```

It's a zero-overhead newtype — the same `std::string` at runtime, a distinct type
at compile time. Swap a `ThreadId` and a `ToolCallId` argument and you get a
compile error, not a 2 a.m. debugging session staring at two hex strings that
look identical in the logs. The cost is one `struct` template; the payoff is a
class of "wrong ID threaded through five call sites" bugs that simply can't be
written.

## Where C++ is genuinely better, not just safer

Two things fall out of this design that a Node or Python agent has to fight for:

- **No GC pauses mid-stream.** When tokens are flowing in from the API, there is
  no garbage collector waking up to walk the heap. The redraw loop is a
  `poll(2)` over the model stream and your input fd; every SSE chunk lands on the
  very next frame. The latency profile is flat because there's nothing that
  periodically isn't running your code.
- **Subprocess is the hard part, and C++ is good at it.** `posix_spawn` +
  `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX,
  `CreateProcessW` on Windows. No `popen` quoting hazards, no shelling out to GNU
  `timeout`, no interpreter between you and the kernel. The `Exec` capability is
  dangerous precisely because this layer is so direct — and the type system is
  what keeps that danger gated.

The streaming render path deserves its own post (the trick is an append-only
*frozen* prefix of pre-built rows that the terminal redraws zero-copy, so a
thousand-line transcript costs the same per frame as a short one). But the theme
is the same as everything above: pick a representation that makes the expensive
or dangerous thing structurally impossible, then let the machine enforce it.

## So: masochism, or not?

Modern C++ — `std::variant`, `std::visit`, `constexpr`, `consteval`,
`static_assert`, strong newtypes — turns out to be a calm, boring place to write
an agent. The unsafe stuff is unrepresentable. The exhaustive stuff is checked.
The dangerous stuff is a compile error when it's wrong. The fast stuff is free.

The reducer is one pure function. The events are a closed set the compiler
counts. The permission matrix is proven correct over all 48 cells before the
binary exists. None of that is masochism — it's the language doing the worrying
so I don't have to.

> If you've ever shipped an agent that did something it shouldn't have because a
> permission check had a typo: this one *can't compile* with that typo.

The source is MIT and reads like what I described here — one `std::visit`
reducer, one permission table, a handful of `static_assert`s standing guard.
[Browse the repo](https://github.com/1ay1/agentty), or just install it and run
`time agentty --version` to see what the no-runtime design buys you:

```
curl -fsSL https://agentty.org/install.sh | sh
```
