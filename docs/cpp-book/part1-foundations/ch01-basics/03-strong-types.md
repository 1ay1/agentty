[← integers lie](02-integers-lie.md) · [chapter index](README.md) · [next: value categories →](04-value-categories.md)

# 3. Strong types: agentty's `Id<Tag>`

**Time:** 75 minutes
**Code:** [`code/03_strong_types.cpp`](code/03_strong_types.cpp)
**Real file:** [`include/agentty/domain/id.hpp`](../../../../include/agentty/domain/id.hpp)

```sh
cd code && make 03_strong_types && ./03_strong_types
```

This is the first section where you read real agentty code. By the end
you'll understand `id.hpp` completely — all 60 lines of it.

---

## the bug we're preventing

agentty passes identifiers everywhere. A thread id. A tool call id. A
model id. A checkpoint id. At runtime they're all "some hex in a string".

Write the API with raw strings and this compiles:

```cpp
void cancel(const std::string& thread_id, const std::string& call_id);

// somewhere, three files away:
cancel(call_id, thread_id);      // swapped. compiles. runs. wrong.
```

```
-- the bug --
weak version, arguments swapped, compiles fine:
  cancel(thread=call-9, call=thread-1)
```

No warning. No error. The function cancels nothing, or cancels the wrong
thing, and you find out from a user report.

This class of bug is *the* reason to reach for strong types. It isn't
theoretical and it isn't rare. Any codebase with more than two kinds of
string id has shipped this bug.

---

## the fix, in full

Here is the whole thing, from `include/agentty/domain/id.hpp`:

```cpp
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
```

Now:

```cpp
void cancel(const ThreadId& thread_id, const ToolCallId& call_id);

cancel(call_id, thread_id);   // ERROR: no conversion from ToolCallId to ThreadId
```

The bug is now a compile error. Let's take the header apart line by line.

---

## `template <typename Tag>` — the phantom

`Tag` appears nowhere in the body. It's never stored, never used, never
instantiated. It exists purely so the compiler sees two different types.

```cpp
struct ThreadIdTag {};     // empty. no members. never constructed.
using ThreadId = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
```

`Id<ThreadIdTag>` and `Id<ToolCallIdTag>` are separate types with separate
overload sets. They have identical members, identical layout, and
identical generated code. They just aren't interchangeable.

This is called a **phantom type** — a type parameter that carries
compile-time information and zero runtime information.

```
-- the wrapper is free --
sizeof(std::string) = 32
sizeof(ThreadId)    = 32   <- identical
sizeof(ModelId)     = 32
a phantom tag adds no member, so it adds no byte.
```

Not "roughly the same". Identical. There is nothing to add, because the
tag is never a member.

### why a struct and not an enum value

You could imagine `template <int N> struct Id`. Don't. Tag structs are
self-documenting (`ThreadIdTag` reads as what it is), they can't collide
by accident the way magic numbers can, and they show up in compiler
diagnostics with a useful name:

```
error: cannot convert 'Id<ToolCallIdTag>' to 'Id<ThreadIdTag>'
```

versus

```
error: cannot convert 'Id<3>' to 'Id<1>'
```

The first one you can fix without opening a file.

---

## `explicit` — the door stays shut

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

`explicit` means the compiler will never apply this constructor on its
own. You have to ask.

```cpp
ThreadId t{"abc123"};       // ok, you asked
ThreadId t = "abc123";      // ERROR
send(ThreadId{"abc"});      // ok
send("abc");                // ERROR
```

```
-- explicit --
ThreadId t{"abc123"}  -> abc123
ThreadId t = "abc123"   is a COMPILE ERROR.
without explicit, any string would silently become a ThreadId
and the whole point of the type would be gone.
```

**Drop `explicit` and the entire design collapses.** Without it, any
`std::string` implicitly converts to any `Id<T>`, so `cancel(call_id,
thread_id)` compiles again — the strings convert to whatever the
parameters want. You'd have written all this machinery and bought nothing.

Rule: a single-argument constructor is `explicit` unless you have a
specific reason for the implicit conversion. `std::string` from `const
char*` is one of those reasons. A strong id from a string is not.

---

## `noexcept` — two different promises

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

The constructor takes the string **by value** and then **moves** it into
the member. Taking by value means the caller decides:

```cpp
std::string s = compute();
ThreadId a{s};              // copies s (you still want s)
ThreadId b{std::move(s)};   // moves s (you're done with it)
```

One constructor, both cases optimal. That's the "sink parameter" idiom and
it shows up constantly.

And since moving a `std::string` can't throw, the whole constructor can't
throw, so it's marked `noexcept`. That's not decoration:

- `std::vector<ThreadId>` will **move** elements when it grows instead of
  copying them, because it knows the move can't leave it in a broken
  state. Section 8 shows this happening.
- `std::is_nothrow_move_constructible_v<ThreadId>` is true, which other
  generic code branches on.

The `[[nodiscard]]` accessors are `noexcept` for the same reason —
`empty()` and `c_str()` on a `std::string` are both non-throwing, so
saying so lets callers propagate the guarantee.

---

## `[[nodiscard]]` — the answer matters

```cpp
[[nodiscard]] bool empty() const noexcept { return value.empty(); }
```

```cpp
ThreadId t{"x"};
t.empty();          // warning: ignoring return value
```

```
-- [[nodiscard]] --
a bare `t.empty();` warns. empty() asks a question and
throwing away the answer is always a bug.
```

`empty()` has no side effects. If you call it and throw the result away,
you didn't mean to call `empty()` — you probably meant `clear()`. That
confusion is real (it's the classic `std::string::empty()` vs `clear()`
mixup) and `[[nodiscard]]` turns it into a warning.

**When to use it:** any function whose only purpose is to return
something. Getters, predicates, factories, `operator+`. Anything where
discarding the result means you called the wrong function.

**When not to:** functions called for effect, where the return is extra
information. `std::vector::insert` returns an iterator but you usually
don't care.

---

## `const` and where it goes

```cpp
bool empty() const noexcept
//           ^^^^^ this const
```

A `const` member function promises not to modify the object. Which means
it can be called *on* a const object:

```cpp
void log(const ThreadId& id) {
    if (id.empty()) return;          // works because empty() is const
    id.value.clear();                // ERROR: id is const
}
```

Forget the `const` on the member function and every `const ThreadId&` in
your codebase becomes unusable. This is *const poisoning* and it
propagates: one missing `const` deep in a class forces callers to drop
`const`, which forces *their* callers to drop it, until nothing is const
anymore.

**Mark everything const that can be const, from the start.** Retrofitting
const into an existing codebase is miserable.

---

## `= default` on comparison — 60 lines for free

```cpp
bool operator==(const Id&) const = default;
auto operator<=>(const Id&) const = default;
```

Two lines. Here's what they generate:

- `==` and `!=` from the first one
- `<`, `>`, `<=`, `>=` from the second (the *spaceship* operator, C++20)

All of them compare `value` member-wise, all of them are `constexpr` and
`noexcept` when the member's operations are.

```
-- defaulted == and <=> --
a == a2 ? yes
a <  b  ? yes   <- <=> gives you <, >, <=, >= for free
a == "aaa" (string_view overload) ? yes
sortable: a b c
```

Because `<=>` exists, `Id` works as a `std::map` key, in `std::sort`, in
`std::set`, anywhere ordering is needed. Without it you'd write a
comparator at every call site.

Pre-C++20 this was six operators you hand-wrote and got subtly wrong. If
you ever see a class with a hand-written `operator<` that forgets a
member, you've found a bug.

### the extra overload

```cpp
[[nodiscard]] bool operator==(std::string_view sv) const noexcept { return value == sv; }
```

This one is for ergonomics:

```cpp
if (tool_name == "bash") { }              // no ToolName{"bash"} needed
```

It's safe because it only goes one way. A `string_view` never becomes a
`ToolName` here; you just get to compare against a literal without
ceremony. And `string_view` means no allocation and no temporary string.

Note it doesn't break the main protection — comparing is not the same as
passing. `cancel(call_id, thread_id)` is still an error.

---

## `friend` to_json — how ADL finds it

```cpp
friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
```

nlohmann's json library serialises a type by calling an unqualified
`to_json(j, value)` and letting **argument-dependent lookup** find it.
ADL searches the namespaces *and the class scope* of the argument types.

A `friend` function defined inside the class body:

- is not a member (it takes `Id` as a parameter, there's no `this`)
- is only findable through ADL on an `Id` argument
- has access to the private parts, if there were any

So it's exactly reachable from where it needs to be, and invisible
everywhere else. You can't call `agentty::to_json(j, id)` by qualified
name — and you don't want to.

The payoff at the use site:

```cpp
nlohmann::json j = thread;     // serialises every ThreadId inside as a plain string
```

The json is `"abc123"`, not `{"value": "abc123"}`. The wire format doesn't
know the strong type exists, which is correct: the type safety is for your
code, not for the protocol.

---

## `MessageId` and why it exists

```cpp
// Per-message stable identity. Generated at Message construction and
// persisted to disk so cache keys (and any future per-message indexing)
// stay valid across reloads. Lets the view-side render cache key by
// content identity rather than position — compacting / removing /
// reordering messages doesn't collide with stale cache entries the way
// (thread_id, msg_idx) keying would.
using MessageId    = Id<MessageIdTag>;

[[nodiscard]] MessageId new_message_id();
```

Worth reading twice, because it's a design decision not a type decision.

The render cache needs a key per message. The obvious key is
`(thread_id, index)`. But indices *move*: compacting a thread removes
messages, and every message after the removed one shifts down. Now index 7
names a different message than it did a second ago, and the cache hands
back the wrong rendered output.

A `MessageId` is content identity, not position. It survives compaction,
reordering, and reload. The cache key stays correct because the key names
the message rather than the slot.

That's the sort of thing a strong type makes it easy to reason about:
the type tells you that this identifier follows the *message*, and the
compiler won't let you accidentally key by something else.

---

## the cost, measured

- **Runtime:** zero. Same size, same instructions, same codegen. Verify it
  yourself: `g++ -O2 -S` the strong and weak versions and diff the
  assembly. They're identical.
- **Compile time:** a few extra template instantiations. Unmeasurable at
  this scale.
- **Typing:** you write `ThreadId{s}` instead of `s`. That's the whole
  price.
- **Return:** a whole category of bug becomes a compile error.

---

## when to reach for this

Wrap it in a strong type when **two values of the same underlying type
mean different things and could be confused at a call site**.

Good candidates:

- ids of different kinds (this chapter)
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
   arguments compile again and the type safety is gone.
3. The tag is never stored as a member. The only member is the
   `std::string`, so the layout is identical.
4. `<`, `>`, `<=`, `>=`, member-wise, plus it makes the type usable in
   ordered containers and `std::sort`.
5. So ADL finds it. A hidden friend is found only via an argument of that
   type, which is exactly how nlohmann dispatches.
6. Because indices shift when messages are compacted or reordered, so a
   position-based key can name a different message after an edit. A
   MessageId follows the message.

</details>

---

[next: value categories →](04-value-categories.md)
