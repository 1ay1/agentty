[← capstone](10-capstone-imagecontent.md) · [chapter index](README.md) · [quick reference →](quick-reference.md)

# Exercises

Eight, easiest first. Do them in `code/`, next to the examples — the
Makefile picks up any `.cpp` in that directory if you add it to `PROGS`,
or just compile by hand:

```sh
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g ex1.cpp -o ex1 && ./ex1
```

Don't read a solution until you've made your version run.

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

## after these

You should be able to open `include/agentty/domain/` and read any file in
it without reaching for a reference. Try it — pick a header you haven't
seen and see how far you get.

[chapter index](README.md) · [quick reference](quick-reference.md) ·
[chapter 2 →](../ch02-memory/)
