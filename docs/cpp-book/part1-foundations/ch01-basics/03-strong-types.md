[← integers lie](02-integers-lie.md) · [chapter index](README.md) · [next: value categories →](04-value-categories.md)

# 3. Strong types: agentty's `Id<Tag>`

**Time:** 75 minutes. This is the big one.
**Real file:** [`include/agentty/domain/id.hpp`](../../../../include/agentty/domain/id.hpp)

We're going to write `Id<Tag>` from scratch, one line at a time, and at
every line I'll tell you what breaks if you leave it out. At the end
you'll open agentty's actual header and recognise all of it.

Open a file called `03_strong_types.cpp`.

---

## first, feel the bug

Type this:

```cpp
#include <cstdio>
#include <string>

namespace weak {

void cancel(const std::string& thread_id, const std::string& call_id) {
    std::printf("  cancel(thread=%s, call=%s)\n",
                thread_id.c_str(), call_id.c_str());
}

} // namespace weak

int main() {
    std::string tid = "thread-1";
    std::string cid = "call-9";

    weak::cancel(cid, tid);      // swapped. on purpose.
}
```

Compile it with every warning you have:

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic 03_strong_types.cpp -o 03 && ./03
```

```
  cancel(thread=call-9, call=thread-1)
```

Clean compile. No warning. The function cancelled the wrong thing and
nothing anywhere told you.

**This is the bug we're killing.** It isn't hypothetical. Any codebase
with two kinds of string id has shipped it. agentty has six kinds:
`ThreadId`, `ToolCallId`, `ModelId`, `CheckpointId`, `ToolName`,
`MessageId`. At runtime they're all "some hex in a string".

---

## build the fix, one line at a time

### the skeleton

Add this above `main`:

```cpp
template <typename Tag>
struct Id {
    std::string value;
};
```

That's the entire core idea, and it's worth stopping on.

`Tag` appears **nowhere in the body**. It's never stored, never used,
never even instantiated. It exists for one reason: to make the compiler
see two different types.

```cpp
struct ThreadIdTag   {};     // empty. no members. never constructed.
struct ToolCallIdTag {};

using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
```

`Id<ThreadIdTag>` and `Id<ToolCallIdTag>` have identical members,
identical layout, identical generated code. They are simply not the same
type, so one can't be passed where the other is expected.

This is called a **phantom type** — a type parameter carrying compile-time
information and zero runtime information.

Prove the "zero runtime" part right now. Add to `main`:

```cpp
std::printf("sizeof(std::string) = %zu\n", sizeof(std::string));
std::printf("sizeof(ThreadId)    = %zu\n", sizeof(ThreadId));
```

```
sizeof(std::string) = 32
sizeof(ThreadId)    = 32
```

Not "about the same". Identical. There's nothing to add, because the tag
is never a member.

> **Why a struct tag and not `template <int N>`?** You could write
> `Id<1>` and `Id<2>`. Don't. Tag structs are self-documenting, can't
> collide by accident, and they appear in compiler errors with a useful
> name: `cannot convert 'Id<ToolCallIdTag>' to 'Id<ThreadIdTag>'` versus
> `cannot convert 'Id<2>' to 'Id<1>'`. The first one you can fix without
> opening a file.

### the constructor, and the keyword that makes it all work

Add a constructor:

```cpp
template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
};
```

Four separate decisions in those two lines. Take them one at a time.

**`Id() = default;`**

Once you declare *any* constructor, the compiler stops generating the
default one. Without this line, `ThreadId id;` wouldn't compile, and
neither would `std::vector<ThreadId> v(10);`. `= default` asks for the
compiler's version back, explicitly.

**`explicit`** — this is the load-bearing keyword.

`explicit` means the compiler will never apply this constructor on its
own. You have to ask. Try it. Add to `main`:

```cpp
ThreadId a{"abc"};       // ok, you asked
// ThreadId b = "abc";   // uncomment this
```

Uncomment that line and compile:

```
error: conversion from 'const char [4]' to non-scalar type 'ThreadId'
{aka 'Id<ThreadIdTag>'} requested
```

Now **delete the word `explicit`** and try again. It compiles.

And that's the disaster. Without `explicit`, every `std::string`
implicitly converts to every `Id<T>`, so `cancel(call_id, thread_id)`
compiles again — the strings just convert to whatever the parameters
want. You'd have written all this machinery and bought exactly nothing.

Put `explicit` back.

> **The rule:** a single-argument constructor is `explicit` unless you
> have a specific reason to want the implicit conversion. `std::string`
> from `const char*` is one of those reasons. A strong id from a string is
> emphatically not.

**`std::string s` by value, then `std::move(s)`**

Why not `const std::string&`? Because taking by value lets the *caller*
decide:

```cpp
std::string s = compute();
ThreadId a{s};              // caller copies, ctor moves  -> 1 copy total
ThreadId b{std::move(s)};   // caller moves,  ctor moves  -> 0 copies
```

One signature, optimal in both cases. This is the **sink parameter**
idiom: when a function is going to *keep* the value, take it by value and
move it into place.

The alternative is writing both a `const T&` and a `T&&` overload, which
doubles for every parameter — a three-parameter constructor becomes eight
overloads. Nobody does that.

**`noexcept`**

Moving a `std::string` can't throw (it's three pointer writes), so the
whole constructor can't throw, so we say so. This is not decoration. It
has two concrete consequences:

- `std::vector<ThreadId>` will **move** elements when it grows instead of
  copying them. Section 8 shows this happening and measures the
  difference.
- `std::is_nothrow_move_constructible_v<ThreadId>` becomes true, which
  other generic code branches on.

### the accessors

```cpp
    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }
```

Three annotations on each. All three earn their place.

**`const`** — the member function promises not to modify the object, which
means it can be called *on* a const object:

```cpp
void log(const ThreadId& id) {
    if (id.empty()) return;     // works ONLY because empty() is const
}
```

Delete the `const` and that function stops compiling. And here's why it
matters more than it looks: the fix would be to change the parameter to
`ThreadId&`, which forces *that* function's callers to have a mutable
path, which forces *their* callers... This is **const poisoning**. One
missing `const` deep in a class propagates outward until nothing in the
codebase is const. Retrofitting it later is a week of work.

**Mark everything const that can be const, from the start.**

**`noexcept`** — `std::string::empty()` and `c_str()` are both
non-throwing, so saying so lets callers propagate the guarantee.

**`[[nodiscard]]`** — try this:

```cpp
ThreadId t{"x"};
t.empty();        // just this line, result ignored
```

```
warning: ignoring return value of 'bool Id<Tag>::empty() const
[with Tag = ThreadIdTag]', declared with attribute 'nodiscard'
[-Wunused-result]
```

`empty()` has no side effects. If you call it and throw away the answer,
you didn't mean to call `empty()` — you probably meant `clear()`. That
exact confusion is a classic bug with `std::string`, and `[[nodiscard]]`
turns it into a warning.

Use it on any function whose only purpose is to return something:
getters, predicates, factories, `operator+`. Don't use it on functions
called for effect where the return is just extra info.

### comparison: two lines that generate six operators

```cpp
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
```

That's it. Here's what those two lines give you:

- `==` and `!=` from the first
- `<`, `>`, `<=`, `>=` from the second (the **spaceship** operator, C++20)

All member-wise, all `constexpr` and `noexcept` when the member's
operations are.

Test it. Add to `main`:

```cpp
#include <algorithm>   // add this include up top
#include <vector>      // and this one

    ThreadId x{"c"}, y{"a"}, z{"b"};
    std::printf("x == x ? %s\n", (x == x) ? "yes" : "no");
    std::printf("y <  x ? %s\n", (y < x) ? "yes" : "no");

    std::vector<ThreadId> v{x, y, z};
    std::sort(v.begin(), v.end());       // works because <=> exists
    for (const auto& id : v) std::printf("%s ", id.value.c_str());
    std::putchar('\n');
```

```
x == x ? yes
y <  x ? yes
a b c
```

Because `<=>` exists, `Id` works as a `std::map` key, in `std::sort`, in
`std::set` — anywhere ordering is needed. Delete that one line and
`std::sort` fails with a wall of template errors.

Before C++20 this was six operators you hand-wrote and got subtly wrong.
If you ever see a class with a hand-written `operator<` that forgets a
member, you've found a bug.

### the convenience overload

```cpp
    [[nodiscard]] bool operator==(std::string_view sv) const noexcept {
        return value == sv;
    }
```

(Add `#include <string_view>`.)

This is purely ergonomic:

```cpp
if (tool_name == "bash") { }      // no ToolName{"bash"} ceremony
```

It's safe because it only goes **one way**. A `string_view` never becomes
a `ToolName` here — you just get to compare against a literal without
constructing anything. And `string_view` means no allocation and no
temporary string.

Critically, this does *not* weaken the main protection. Comparing is not
passing. `cancel(call_id, thread_id)` is still an error.

---

## watch it catch the bug

Now add the strong version of `cancel` and call both:

```cpp
static void cancel(const ThreadId& t, const ToolCallId& c) {
    std::printf("  cancel(thread=%s, call=%s)\n", t.c_str(), c.c_str());
}
```

In `main`:

```cpp
    ThreadId   t{"thread-1"};
    ToolCallId c{"call-9"};

    cancel(t, c);       // correct
    // cancel(c, t);    // uncomment this
```

Uncomment the swapped call:

```
error: invalid initialization of reference of type 'const ThreadId&'
{aka 'const Id<ThreadIdTag>&'} from expression of type 'ToolCallId'
{aka 'Id<ToolCallIdTag>'}
```

The runtime bug became a compile error. That's the whole point of the
section.

---

## now read the real thing

Here is agentty's actual `include/agentty/domain/id.hpp`. You just wrote
almost all of it:

```cpp
#pragma once
// agentty::Id<Tag> — zero-overhead strong newtype for string-shaped IDs.

#include <compare>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace agentty {

template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}

    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const noexcept { return value == sv; }

    friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
    friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
};

struct ThreadIdTag     {};
struct ToolCallIdTag   {};
struct ModelIdTag      {};
struct CheckpointIdTag {};
struct ToolNameTag     {};
struct MessageIdTag    {};

using ThreadId     = Id<ThreadIdTag>;
using ToolCallId   = Id<ToolCallIdTag>;
using ModelId      = Id<ModelIdTag>;
using CheckpointId = Id<CheckpointIdTag>;
using ToolName     = Id<ToolNameTag>;
using MessageId    = Id<MessageIdTag>;

[[nodiscard]] MessageId new_message_id();

} // namespace agentty
```

The only part you haven't written is the two json functions.

### `friend`, and how ADL finds it

```cpp
friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
```

nlohmann's json library serialises a type by calling an **unqualified**
`to_json(j, value)` and letting *argument-dependent lookup* find the right
overload. ADL searches the namespaces of the argument types — and also
their class scopes.

A `friend` function defined inside the class body:

- is **not a member** (it takes `Id` as a parameter; there's no `this`)
- is findable **only** through ADL on an `Id` argument
- has access to private members, if there were any

So it's exactly reachable from where nlohmann needs it, and invisible
everywhere else. You can't even call `agentty::to_json(j, id)` by
qualified name — and you don't want to. This is the **hidden friend**
idiom.

The payoff at the use site:

```cpp
nlohmann::json j = thread;   // every ThreadId inside serialises as a plain string
```

The json is `"abc123"`, not `{"value": "abc123"}`. The wire format doesn't
know the strong type exists, which is correct — the type safety is for
your code, not for the protocol.

### the comment worth reading twice

The real header has this note on `MessageId`:

```cpp
// Per-message stable identity. Generated at Message construction and
// persisted to disk so cache keys (and any future per-message indexing)
// stay valid across reloads. Lets the view-side render cache key by
// content identity rather than position — compacting / removing /
// reordering messages doesn't collide with stale cache entries the way
// (thread_id, msg_idx) keying would.
```

That's a *design* decision, not a type decision, and it's the kind of
thing that makes a strong type earn its keep.

The render cache needs a key per message. The obvious key is
`(thread_id, index)`. But indices **move**: compacting a thread removes
messages and everything after shifts down. Now index 7 names a different
message than it did a second ago, and the cache serves the wrong rendered
output.

A `MessageId` is content identity, not position. It survives compaction,
reordering, and reload.

---

## the cost, proven

Everybody says "zero overhead". Let's not take anyone's word for it.

Write this to a scratch file, `probe.cpp`:

```cpp
#include <string>

template <typename Tag>
struct Id {
    std::string value;
    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};
struct ThreadIdTag {};
using ThreadId = Id<ThreadIdTag>;

std::size_t weak_len(const std::string& s) { return s.size(); }
std::size_t strong_len(const ThreadId& id) { return id.value.size(); }

bool weak_empty(const std::string& s)      { return s.empty(); }
bool strong_empty(const ThreadId& id)      { return id.empty(); }
```

Compile to assembly and look:

```sh
g++ -std=c++23 -O2 -S probe.cpp -o probe.s

# pull out one function, dropping the .cfi/.loc bookkeeping directives
sed -n '/^_Z8weak_len/,/ret/p'   probe.s | grep -v '^\s*\.'
sed -n '/^_Z10strong_len/,/ret/p' probe.s | grep -v '^\s*\.'
```

The names look like line noise because they're **mangled** — C++ encodes
the full signature into the symbol so overloads don't collide.
`_Z8weak_len...` is `weak_len(std::string const&)`. Run the output
through `c++filt` if you want it readable.

Here's what you get:

```asm
; std::size_t weak_len(const std::string& s) { return s.size(); }
        movq    8(%rdi), %rax
        ret

; std::size_t strong_len(const ThreadId& id) { return id.value.size(); }
        movq    8(%rdi), %rax
        ret
```

```asm
; bool weak_empty(const std::string& s) { return s.empty(); }
        cmpq    $0, 8(%rdi)
        sete    %al
        ret

; bool strong_empty(const ThreadId& id) { return id.empty(); }
        cmpq    $0, 8(%rdi)
        sete    %al
        ret
```

Byte for byte the same. The template, the tag, the wrapper struct, the
`[[nodiscard]]`, the member function call — all of it evaporates before a
single instruction is emitted.

The full ledger:

| | cost |
|---|---|
| runtime | zero. proven above. |
| memory | zero. `sizeof` is identical. |
| compile time | a few template instantiations. unmeasurable here. |
| typing | `ThreadId{s}` instead of `s`. |
| **return** | **a whole bug class becomes a compile error** |

**Do this yourself, then break it:** add a `bool` member to `Id`, rerun
the assembly diff, and watch the sizes diverge and the code change.
*That's* what a non-free abstraction looks like. Now you can tell the
difference by looking instead of by guessing.

---

## when to reach for this

Wrap it in a strong type when **two values of the same underlying type
mean different things and could be confused at a call site.**

Good candidates:

- ids of different kinds (this section)
- units: `Meters` vs `Feet`, `Milliseconds` vs `Seconds`
- an index into *this* array vs an index into *that* one
- user input vs sanitised string

Don't bother when:

- there's only one thing of that type in the whole program
- the value is immediately consumed and never passed around
- it's a local in a ten-line function

---

## check yourself

1. What is `Tag` used for in `Id<Tag>`?
2. What breaks if you delete `explicit`?
3. Why is `sizeof(ThreadId) == sizeof(std::string)`?
4. What does `auto operator<=>(const Id&) const = default;` generate?
5. Why is `to_json` a `friend` instead of a free function in the
   namespace?
6. Why does agentty key its render cache by `MessageId` instead of by
   message index?

<details>
<summary>answers</summary>

1. Nothing at runtime. It makes `Id<A>` and `Id<B>` distinct types at
   compile time. It's a phantom type parameter.
2. Every `std::string` implicitly converts to every `Id<T>`, so swapped
   arguments compile again and the type safety is gone entirely.
3. The tag is never stored as a member. The only member is the
   `std::string`, so the layout is identical.
4. `<`, `>`, `<=`, `>=`, member-wise, plus it makes the type usable in
   ordered containers and `std::sort`.
5. So ADL finds it. A hidden friend is found only via an argument of that
   type, which is exactly how nlohmann dispatches.
6. Because indices shift when messages are compacted or reordered, so a
   position-based key can name a different message after an edit. A
   MessageId follows the message itself.

</details>

---

[next: value categories →](04-value-categories.md)
