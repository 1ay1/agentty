[← capstone](10-capstone-imagecontent.md) · [chapter index](README.md) · [quick reference →](quick-reference.md)

# Exercises

Twelve, easiest first. Do them in `code/`, next to the examples — or just
compile by hand:

```sh
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g ex1.cpp -o ex1 && ./ex1
```

Don't read a solution until you've made your version run.

The last four are different from the rest. 1–8 check that you *learned the
chapter*. 9–12 check that you can **work without it** — derive an answer
the text never gave you, and judge a design rather than apply a rule.
Those are the ones that matter.

---

## 1. break it on purpose

**Sections 1, 2, 7. About 30 minutes.**

Write one program with five functions, each containing one bug from this
chapter:

1. a signed/unsigned comparison that gives the wrong answer
2. `size() - 1` on an empty vector
3. a narrowing conversion that silently truncates
4. a dangling `string_view` into a temporary
5. a pointer into a vector, used after `push_back` reallocates

For each, write down *before compiling*: does the compiler catch it, does
ASan catch it, or does it silently do the wrong thing?

Then compile with `-Wall -Wextra -fsanitize=address,undefined` and check
your predictions.

**What you're learning:** which of your tools catches what. That map is
the difference between a ten-minute debug and a two-day one.

---

## 2. make the compiler refuse

**Section 3. About 30 minutes.**

Write a `Meters` and a `Feet` strong type over `double`, with:

- `explicit` constructors
- `operator+` that only adds same units
- a `to_feet(Meters)` conversion function
- `<=>` so they sort

Then write:

```cpp
Meters m{100.0};
Feet   f{328.0};
auto   bad = m + f;      // this must NOT compile
auto   ok  = m + to_meters(f);
```

Confirm `bad` is a compile error and read the message.

Then measure the cost: write a loop summing a million `Meters` and a
million `double`s, compile at `-O2`, and time both. They should be
identical. If they're not, you added a member you didn't mean to.

**What you're learning:** the zero-overhead claim is checkable, so check
it.

---

## 3. trace every operation

**Sections 4, 8. About 45 minutes.**

Take `Tracked` from `08_copy_move.cpp`. Before running each of these,
write down the exact sequence of operations you expect:

```cpp
std::vector<Tracked> v;
v.push_back(Tracked{"a"});
v.push_back(Tracked{"b"});
v.push_back(Tracked{"c"});
```

then:

```cpp
std::vector<Tracked> v;
v.reserve(3);
v.emplace_back("a");
v.emplace_back("b");
v.emplace_back("c");
```

then:

```cpp
std::vector<Tracked> a;
a.emplace_back("x");
std::vector<Tracked> b = a;
std::vector<Tracked> c = std::move(a);
```

Run them and compare. Where you were wrong, work out why.

Then remove `noexcept` from `Tracked`'s move constructor and rerun the
first one. Count the difference.

**What you're learning:** to predict copies instead of guessing at them.

---

## 4. find the lifetime bug

**Section 7. About 45 minutes.**

This compiles with no warnings and is broken in three places:

```cpp
#include <string>
#include <string_view>
#include <vector>

struct Entry {
    std::string_view key;
    int value;
};

class Table {
public:
    void add(const std::string& k, int v) {
        entries_.push_back({k, v});
    }

    std::string_view first_key() const {
        return entries_.front().key;
    }

    const Entry& biggest() const {
        Entry best{"", -1};
        for (const auto& e : entries_)
            if (e.value > best.value) best = e;
        return best;
    }

private:
    std::vector<Entry> entries_;
};

int main() {
    Table t;
    t.add("alpha", 1);
    t.add(std::string("beta"), 2);
    auto k = t.first_key();
    const Entry& b = t.biggest();
    return static_cast<int>(k.size()) + b.value;
}
```

Find all three. For each: which of the four dangling shapes is it, and
what's the fix?

Then build with ASan and confirm it catches what you expected it to.

<details>
<summary>hints</summary>

1. `Entry::key` is a view. Who owns the characters after `add` returns?
2. `add(std::string("beta"), 2)` — how long does that argument live?
3. `biggest()` returns a reference to what, exactly?

</details>

---

## 5. give `Id` a `starts_with`

**Section 3. About 45 minutes.**

agentty checks id prefixes in a few places. Add this to `Id<Tag>`:

```cpp
[[nodiscard]] bool starts_with(std::string_view prefix) const noexcept;
```

Requirements:

- `const`, `noexcept`, `[[nodiscard]]` — and be able to say why each
- no allocation, no temporary string
- works on an empty id and an empty prefix

Write a test with at least six cases including both empties and a prefix
longer than the value.

Then answer: should this be a member, or a free function taking
`const Id<Tag>&`? Argue both sides in three lines.

<details>
<summary>solution sketch</summary>

```cpp
[[nodiscard]] bool starts_with(std::string_view prefix) const noexcept {
    return std::string_view{value}.starts_with(prefix);
}
```

`std::string_view` has `starts_with` since C++20, and constructing a view
from the member is free — no allocation, so `noexcept` holds.

Member versus free: a member is discoverable from the type and matches
`empty()`/`c_str()`. A free function keeps the class minimal and works
uniformly over anything string-shaped. For a type this small the member
wins on ergonomics; for a large class, prefer free functions (Scott
Meyers' argument: they improve encapsulation because they can't touch
privates).

</details>

---

## 6. rebuild `LazyBytes` from the spec

**Sections 6, 10. About 60 minutes.**

Don't look at `10_imagecontent.cpp` or the real header. Write a class from
this spec:

- holds either bytes or a `Source{blob, b64}`
- `bytes()` returns `const std::string&`, materialises on first call
- callable on a `const` object
- `empty()` and `materialised()` never materialise
- a missing blob gives empty bytes, no exception
- copying gives an object that resolves to the same bytes
- `set_bytes(std::string)` replaces the payload and clears the source

Write tests for each bullet. The copy one is the interesting test: copy an
*unmaterialised* object, resolve the copy, and check it got the right
bytes.

Then diff yours against the real header. Every difference is either a bug
in yours or a decision you should be able to explain.

---

## 7. make the mutable cache thread-safe

**Sections 6, 10. About 60 minutes. Harder.**

`LazyBytes` is not thread-safe: two threads calling `bytes()` on the same
unmaterialised object both see `resolved_ == false` and both run the
resolver, with a data race on `bytes_`.

Write a version that is. Then answer, with reasoning:

1. What does the mutex cost when the object is *already* resolved? (Hint:
   measure it. A million `bytes()` calls, with and without.)
2. Can you make the already-resolved path lock-free with an atomic flag?
   What memory ordering do you need, and why?
3. Is `std::call_once` a better fit? What does it cost per object?
4. **Should agentty actually do this?** Where is the resolver called from?
   What's the real cost of paying for synchronisation that nothing needs?

Question 4 is the one that matters. The answer in agentty's case is no,
and being able to say *why* is the skill.

---

## 8. profile the lazy design

**Section 10. About 90 minutes. Hardest.**

Build a benchmark that compares eager and lazy loading:

- generate a synthetic thread: 100 messages, 40 with 100KB images
- **eager:** decode every image at load
- **lazy:** decode on first `bytes()` call
- measure: load time, peak RSS, and time-to-first-render where render
  touches only the last 10 messages
- then measure a **save** of a thread that was loaded and scrolled but
  never had images accessed

Report a table. Then answer:

1. At what fraction of images-actually-viewed does lazy stop winning?
2. What's the worst case for lazy, and how much worse is it?
3. The save path is where lazy wins biggest. Why?
4. Is the extra complexity worth it for a 10-message thread? For a
   500-message one? Where's the crossover?

**What you're learning:** to justify a design decision with numbers. "It's
faster" is not an argument until you can say how much, under what load,
and where it stops being true.

---

## 9. predict before you compile

**Sections 1, 2, 4, 8, 9. About 45 minutes. This is the one that tests
whether you can derive.**

For each expression below, write down **on paper** (a) the resulting type
and (b) the value, before compiling anything. Then check with
`TypeIs<decltype(expr)>` (§11) and a `printf`.

```cpp
short s = 30000;
std::uint8_t b = 200;
unsigned u = 1;
int i = -1;
long l = 1;
std::size_t n = 0;

1.  s + s
2.  b + b
3.  b * 2
4.  i + u
5.  u + l
6.  n - 1
7.  i < u
8.  (b + b) > 255
9.  static_cast<std::uint8_t>(b + b) > 255
10. sizeof(b << 1)
```

Score yourself. **Anything under 8/10 means go back to §2's "the machine
underneath" and work through the four conversion rules again** — you're
still recalling traps instead of deriving from the rule.

Then the harder half. Same drill, but these are about value categories and
copies:

```cpp
std::vector<std::string> v{"a", "b", "c"};

11. how many string copies does `for (auto x : v)` make?
12. how many does `for (const auto& x : v)` make?
13. how many does `std::vector<std::string> w = v;` make?
14. how many does `auto w = std::move(v);` make?
15. how many does `v.push_back("d")` make, if capacity is 3?
```

Verify 11–15 with a `Tracked`-style counting type, not by reasoning alone.

<details>
<summary>answer key — measured, not asserted</summary>

**Types and values.** `short s = 30000; uint8_t b = 200; unsigned u = 1;
int i = -1; long l = 1; size_t n = 0;`

| # | expression | type | value |
|---|---|---|---|
| 1 | `s + s` | `int` | 60000 — promoted, so no overflow |
| 2 | `b + b` | `int` | 400 — promoted, **not** 144 |
| 3 | `b * 2` | `int` | 400 |
| 4 | `i + u` | `unsigned` | 0 — `-1` became `UINT_MAX`, plus 1 wraps |
| 5 | `u + l` | `long` | 2 — **on 64-bit Linux.** `unsigned long` on Windows |
| 6 | `n - 1` | `unsigned long` | 18446744073709551615 |
| 7 | `i < u` | `bool` | **false**, and it warns |
| 8 | `(b + b) > 255` | `bool` | **true** — 400 > 255 |
| 9 | `(uint8_t)(b+b) > 255` | `bool` | **false, always** — warns `-Wtype-limits` |
| 10 | `sizeof(b << 1)` | `size_t` | **4** — promoted to `int` before shifting |

The two that catch most people are 8 and 9. Same arithmetic, one cast
apart, opposite answers — and 9 can *never* be true, which is why gcc
warns that the comparison is pointless.

**Copies**, with `vector<T> v` holding 3 elements, capacity 3:

| # | expression | copies | moves |
|---|---|---|---|
| 11 | `for (auto x : v)` | **3** | 0 |
| 12 | `for (const auto& x : v)` | 0 | 0 |
| 13 | `std::vector<T> w = v;` | **3** | 0 |
| 14 | `auto w = std::move(v);` | 0 | **0** |
| 15 | `v.push_back(T{"d"})` at capacity | 0 | **4** |

14 is the interesting one: **zero moves, not three.** Moving a `vector`
moves the vector's three pointers, not its elements. The elements never
learn it happened.

15 is four moves, not one: one for the new element, plus three to
relocate the existing ones into the bigger buffer. And they're *moves*
only because `T`'s move constructor is `noexcept` (§8). Take that
`noexcept` off and you get 4 copies.

</details>

---

## 10. the design review

**Sections 3, 6, 8, 10. About 60 minutes. No code required — this is
judgement.**

Here's a real class from a real codebase. It compiles, passes its tests,
and is wrong in five ways you now know how to name.

```cpp
class Session {
public:
    Session(std::string id, std::string user) {
        id_ = id;
        user_ = user;
    }

    ~Session() { log_close(); }

    std::string id() { return id_; }
    bool expired() { return expiry_ < now(); }

    void add_event(std::string e) { events_.push_back(e); }

    std::string_view last_event() { return events_.back(); }

private:
    std::string id_;
    std::string user_;
    std::vector<std::string> events_;
    Time expiry_;
};
```

Write one paragraph per problem: **what's wrong, which section covers it,
what it costs, and the fix.** Then rewrite the class.

<details>
<summary>the five, once you've found them</summary>

1. **The constructor copies twice** (§6). Takes by value implicitly? No —
   it takes by value and then *copy-assigns* in the body. Should be
   `: id_(std::move(id)), user_(std::move(user))`. Two allocations saved
   per session, and §5 explains why body-assignment is worse than an init
   list.

2. **The destructor kills the implicit moves** (§8). `~Session()` is
   user-declared, so `Session` has no move constructor — every
   `vector<Session>` reallocation deep-copies every string in every event
   list. This is the expensive one.

3. **`id()` returns by value and isn't const** (§6). Should be
   `const std::string& id() const`. As written, every caller allocates,
   and no caller holding a `const Session&` can call it at all — const
   poisoning.

4. **`expired()` isn't const** (§6). Same problem, and this one is a
   predicate so it should also be `[[nodiscard]]`.

5. **`last_event()` returns a dangling-prone view** (§7). It's a
   `string_view` into a `vector` element. Any `add_event` that reallocates
   invalidates it. Return `const std::string&`, or document the
   invalidation contract loudly.

Bonus: `expiry_` has no default member initialiser, so a `Session` whose
constructor throws midway leaves it indeterminate (§5).

</details>

---

## 11. find the real bug in agentty

**Sections 3, 7, 11. About 60 minutes. Uses the actual codebase.**

Open `include/agentty/domain/` and pick a header you haven't read.

1. For every member function, state whether it should be `const`,
   `noexcept`, `[[nodiscard]]`, and whether it is. Find one that's
   missing something.
2. For every type, run
   `static_assert(std::is_nothrow_move_constructible_v<T>)` in a scratch
   file. Does every one pass? If one fails, work out which of §8's rules
   caused it.
3. Find one place where a `std::string` parameter could be a
   `std::string_view` (§6) and one where it couldn't. Explain the
   difference.

Write up what you find. If it's a genuine improvement, that's a patch.

---

## 12. teach it back

**All sections. About 60 minutes. The real test.**

Pick the single concept from this chapter you found hardest. Write an
explanation of it for someone who knows Python but not C++.

Constraints:

- under 500 words
- exactly one runnable program, under 40 lines, that demonstrates it
- at least one thing the reader is told to **break on purpose**, with the
  expected error
- no hand-waving: every claim either shows output or cites a rule

If you can't do this, you don't know the concept yet — and finding out
which one that is, is the point of the exercise.

The list of things worth trying: why `std::move` doesn't move; why a named
`T&&` is an lvalue; why `~T(){}` makes your vector slow; why
`LazyBytes::empty()` must not call `bytes()`; why `-1 < 1u` is false.

---

## after these

You should be able to open `include/agentty/domain/` and read any file in
it without reaching for a reference. Try it — pick a header you haven't
seen and see how far you get.

If exercises 9 and 10 went well, you're ready for chapter 2. If they
didn't, the gap they exposed is worth closing first — it'll only get more
expensive later.

[chapter index](README.md) · [quick reference](quick-reference.md) ·
[chapter 2 →](../ch02-memory/)
