[← references and const](06-references-and-const.md) · [chapter index](README.md) · [next: copy, move, elision →](08-copy-move-elision.md)

# 7. Lifetime

**Time:** 75 minutes. Type everything.

Every object is born and dies at a point the compiler knows. Use it
outside that window and you get undefined behaviour. This is the most
important section in the chapter.

Open `07_lifetime.cpp`.

---

## destruction is reverse of construction

```cpp
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct Loud {
    std::string tag;
    explicit Loud(std::string t) : tag(std::move(t)) {
        std::printf("  + %s\n", tag.c_str());
    }
    ~Loud() { std::printf("  - %s\n", tag.c_str()); }
};

static void destruction_order() {
    std::puts("-- scope: last in, first out --");
    Loud a{"a"};
    Loud b{"b"};
    {
        Loud inner{"inner"};
        std::puts("  (inner scope ending)");
    }
    Loud c{"c"};
    std::puts("  (function ending)");
}

int main() {
    destruction_order();
}
```

**Predict the output before running.**

```
-- scope: last in, first out --
  + a
  + b
  + inner
  (inner scope ending)
  - inner
  + c
  (function ending)
  - c
  - b
  - a
```

A stack. Last constructed, first destroyed. **Always**, with no
exceptions — including when an exception unwinds the stack.

That predictability is what makes RAII work. If `b` holds a lock and `c`
uses it, you *know* `c` dies first, so the lock is still held when `c`'s
destructor runs. Chapter 2 builds entirely on this.

---

## members, and the body-first rule

```cpp
struct Owner {
    Loud first{"member-first"};
    Loud second{"member-second"};
    ~Owner() { std::puts("  ~Owner body runs BEFORE members are destroyed"); }
};

static void member_order() {
    std::puts("\n-- members --");
    Owner o;
    std::puts("  (o going out of scope)");
}
```

```
-- members --
  + member-first
  + member-second
  (o going out of scope)
  ~Owner body runs BEFORE members are destroyed
  - member-second
  - member-first
```

Two rules, both worth knowing:

**Members construct in declaration order and destruct in reverse.** Not in
the order you wrote the init list — section 5 showed `-Wreorder` catching
that.

**The destructor body runs *before* members are destroyed.** So inside
`~Owner()` every member is still fully alive and usable. That's why you
can flush a buffer to a file member in a destructor body and it works.

---

## the four ways to dangle

Every dangling bug you will ever write is one of these four. Learn the
shapes.

### 1. returning a reference to a local

```cpp
const std::string& bad() {
    std::string s = "hello";
    return s;                 // s dies at the closing brace
}
```

Write it. gcc catches this one:

```
warning: reference to local variable 's' returned [-Wreturn-local-addr]
```

Return by value instead — section 8 shows it costs nothing.

### 2. a view into something temporary

```cpp
std::string_view sv = std::string("temp");    // dangling immediately
std::printf("%.*s\n", (int)sv.size(), sv.data());
```

The temporary `std::string` dies at the end of *that statement*.
`string_view` doesn't extend anything — it's a pointer and a length, and
now the pointer is stale.

Build that with `-fsanitize=address` and run it. Then read "finding these
bugs" below, because whether it *fires* depends on something worth
knowing.

The sneaky version:

```cpp
std::string_view name = get_config()["name"];    // if the config is a temporary
```

### 3. invalidation

Type this one, because the addresses make it concrete:

```cpp
static void dangling_catalogue() {
    std::puts("\n-- dangling, shape 3: invalidation --");
    std::vector<int> v{1, 2, 3};
    int* p = &v[0];
    std::printf("  &v[0] before reserve = %p\n", static_cast<void*>(p));
    v.reserve(1000);                    // may reallocate
    std::printf("  &v[0] after  reserve = %p\n", static_cast<void*>(&v[0]));
    std::puts("  if those differ, every saved pointer just went stale.");
}
```

```
  &v[0] before reserve = 0x7b92d41e0010
  &v[0] after  reserve = 0x7d82d41e1500
```

When a `vector` grows past its capacity it allocates a new buffer, moves
everything over, and frees the old one. Every pointer, reference, and
iterator into the old buffer is now dangling.

The rules per container:

| container | what invalidates |
|---|---|
| `vector` | any growth past capacity invalidates everything; erase invalidates from the erased point on |
| `deque` | insert/erase in the middle invalidates everything; at the ends, iterators but not references |
| `list`, `forward_list` | only the erased element |
| `map`, `set` | only the erased element |
| `unordered_map/set` | rehash invalidates iterators, not references |

`vector` is the one that bites, because it's the one you use most.

### 4. pointing into a moved-from object

```cpp
std::string a = "long enough to be heap allocated, definitely";
const char* p = a.c_str();
std::string b = std::move(a);      // b took the buffer
std::puts(p);                      // p pointed into a's buffer
```

This one is subtler than it looks, and ASan's behaviour on it is a lesson
in itself — see "finding these bugs" below.

---

## string_view: the sharpest of the four

```cpp
static std::string build() { return "built on the fly"; }

static void string_view_rules() {
    std::puts("\n-- string_view is a borrow, always --");

    std::string owned = "i own my bytes";
    std::string_view ok = owned;              // fine: owned outlives ok
    std::printf("ok  = '%.*s'\n", static_cast<int>(ok.size()), ok.data());

    // std::string_view bad = build();        // DANGLING. temp dies here.
    std::puts("std::string_view bad = build();   <- dangles immediately");

    std::string keep = build();               // keep it alive first
    std::string_view good = keep;
    std::printf("good = '%.*s'\n", static_cast<int>(good.size()), good.data());
}
```

Note `%.*s` — `string_view` is **not null-terminated**, so you must pass
the length explicitly. Using `%s` with `sv.data()` reads past the end.
That's a bug in itself and worth knowing.

`std::string_view` is a pointer and a length. It owns nothing. It's a
borrow with no borrow checker.

**Safe:**

```cpp
void log(std::string_view msg);       // parameter: the caller's string
                                      // provably outlives the call
```

Takes a `std::string`, a `const char*`, a literal, another view, with zero
allocations. This is the single best use of `string_view`.

**Dangerous, needs proof:**

```cpp
struct Bad  { std::string_view name; };   // outlives its owner?
std::string_view f();                      // a view of what?
```

Both *can* be correct, if the owner provably outlives the view. But now
**you** are maintaining that proof, not the compiler, and a refactor two
months from now will break it silently.

**The rule:** `string_view` parameters, freely. `string_view` members and
return values, only when you can state in a comment why the owner
outlives it.

---

## storage durations

```cpp
static int& counter() {
    static int n = 0;      // constructed on first call, destroyed at exit
    return ++n;
}

static void storage_durations() {
    std::puts("\n-- storage durations --");
    std::puts("automatic  : locals, die at end of scope");

    // NOTE: printing all three in ONE printf would be a trap. argument
    // evaluation order is UNSPECIFIED, so you might see 3, 2, 1.
    std::printf("static     : counter() -> ");
    std::printf("%d, ", counter());
    std::printf("%d, ", counter());
    std::printf("%d (survives calls)\n", counter());

    std::puts("dynamic    : new/delete, or better, a smart pointer (ch05)");
    std::puts("thread     : thread_local, one per thread");
}
```

```
-- storage durations --
automatic  : locals, die at end of scope
static     : counter() -> 1, 2, 3 (survives calls)
dynamic    : new/delete, or better, a smart pointer (ch05)
thread     : thread_local, one per thread
```

**That comment is a real bug I hit writing this.** The original was:

```cpp
std::printf("counter() -> %d, %d, %d\n", counter(), counter(), counter());
```

which printed `3, 2, 1`. Function argument evaluation order is
**unspecified** in C++ — the compiler may evaluate them in any order it
likes. When the calls have side effects, you must sequence them yourself
with separate statements. Try putting it back and see what your compiler
does.

The four durations:

**Automatic** — locals and parameters. Born at the declaration, die at the
closing brace. The one you use 95% of the time.

**Static** — globals, `static` members, `static` locals. Live for the
whole program.

```cpp
int& counter() {
    static int n = 0;      // constructed on FIRST call
    return ++n;
}
```

A function-local `static` is constructed the first time control reaches
the declaration, and since C++11 that construction is **thread-safe** —
the compiler emits a guard. This is the Meyers singleton and it's the
right way to do lazy global init.

**Dynamic** — `new`/`delete`, or what a smart pointer holds. Chapter 5.

**Thread** — `thread_local`, one instance per thread, destroyed at thread
exit.

---

## the static initialisation order fiasco

Two globals in different translation units:

```cpp
// a.cpp
Config g_config = load_config();

// b.cpp
Logger g_logger{g_config.log_path};   // is g_config constructed yet?
```

**Unspecified.** The order of dynamic initialisation across translation
units is not defined. `g_logger` might read an uninitialised `g_config`.

The fix is the function-local static, which constructs on first use:

```cpp
Config& config() {
    static Config c = load_config();
    return c;
}
```

Now `config()` always gives you a constructed object, whoever calls
first.

---

## the whole file

```cpp
// 07_lifetime.cpp — when objects die, and the four ways to outlive them.
//
// NOTE: the dangling examples are COMMENTED OUT on purpose. Uncomment one
// at a time and run under -fsanitize=address to watch it get caught.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct Loud {
    std::string tag;
    explicit Loud(std::string t) : tag(std::move(t)) {
        std::printf("  + %s\n", tag.c_str());
    }
    ~Loud() { std::printf("  - %s\n", tag.c_str()); }
};

// ── destruction order is reverse of construction ───────────────────────
static void destruction_order() {
    std::puts("-- scope: last in, first out --");
    Loud a{"a"};
    Loud b{"b"};
    {
        Loud inner{"inner"};
        std::puts("  (inner scope ending)");
    }
    Loud c{"c"};
    std::puts("  (function ending)");
}

// ── members die in reverse declaration order, after the body ───────────
struct Owner {
    Loud first{"member-first"};
    Loud second{"member-second"};
    ~Owner() { std::puts("  ~Owner body runs BEFORE members are destroyed"); }
};

static void member_order() {
    std::puts("\n-- members --");
    Owner o;
    std::puts("  (o going out of scope)");
}

// ── the four dangling shapes ───────────────────────────────────────────
static void dangling_catalogue() {
    std::puts("\n-- dangling, all four shapes (see source, they're commented) --");

    std::puts("1. reference to a local:");
    std::puts("     const std::string& f() { std::string s = \"x\"; return s; }");

    std::puts("2. view into a temporary:");
    std::puts("     std::string_view sv = std::string(\"temp\");");
    std::puts("   the string dies at the end of THAT statement. sv is garbage.");

    std::puts("3. iterator/reference invalidated by growth:");
    std::vector<int> v{1, 2, 3};
    int* p = &v[0];
    std::printf("     &v[0] before push_back = %p\n", static_cast<void*>(p));
    v.reserve(1000);                    // may reallocate
    std::printf("     &v[0] after  reserve   = %p\n", static_cast<void*>(&v[0]));
    std::puts("   if those differ, every saved pointer just went stale.");

    std::puts("4. pointer to a member of a moved-from object:");
    std::puts("     the buffer moved away; the pointer still aims at the old one.");
}

// ── string_view: the sharpest of the four ──────────────────────────────
static std::string build() { return "built on the fly"; }

static void string_view_rules() {
    std::puts("\n-- string_view is a borrow, always --");

    std::string owned = "i own my bytes";
    std::string_view ok = owned;              // fine: owned outlives ok
    std::printf("ok  = '%.*s'\n", static_cast<int>(ok.size()), ok.data());

    // std::string_view bad = build();        // DANGLING. temp dies here.
    std::puts("std::string_view bad = build();   <- dangles immediately");

    std::string keep = build();               // keep it alive first
    std::string_view good = keep;
    std::printf("good = '%.*s'\n", static_cast<int>(good.size()), good.data());

    std::puts("rule: a string_view parameter is fine. a string_view MEMBER");
    std::puts("or return value needs you to prove the owner outlives it.");
}

// ── static and thread_local ────────────────────────────────────────────
static int& counter() {
    static int n = 0;      // constructed on first call, destroyed at exit
    return ++n;
}

static void storage_durations() {
    std::puts("\n-- storage durations --");
    std::puts("automatic  : locals, die at end of scope");

    // NOTE: printing all three in ONE printf would be a trap. argument
    // evaluation order is UNSPECIFIED, so you might see 3, 2, 1. sequence
    // them with separate statements when the calls have side effects.
    std::printf("static     : counter() -> ");
    std::printf("%d, ", counter());
    std::printf("%d, ", counter());
    std::printf("%d (survives calls)\n", counter());

    std::puts("dynamic    : new/delete, or better, a smart pointer (ch05)");
    std::puts("thread     : thread_local, one per thread");
}

int main() {
    destruction_order();
    member_order();
    dangling_catalogue();
    string_view_rules();
    storage_durations();
}
```

---

## finding these bugs

**AddressSanitizer catches use-after-free and stack-use-after-scope at
runtime.** It's the single most valuable tool in C++ and costs about 2x
runtime:

```sh
g++ -fsanitize=address,undefined -g your.cpp -o your && ./your
```

### but it only catches what you actually touch

This is important and it surprised me while writing this section. I tried
to demo the dangling `string_view`:

```cpp
std::string_view sv = std::string("a long string not fitting in SSO");
std::printf("%.*s\n", (int)sv.size(), sv.data());
```

and ASan said **nothing**. Clean run, exit 0.

Then I read one byte explicitly:

```cpp
std::printf("char=%d\n", (int)sv.data()[0]);
```

```
==211094==ERROR: AddressSanitizer: heap-use-after-free on address 0x7ba867de0080
READ of size 1 at 0x7ba867de0080 thread T0
    #0 0x55b3e377a461 in main /tmp/a4.cpp:7
```

There it is. The first version didn't fire because `printf` with a
zero-ish length never dereferenced the stale pointer.

**The lesson is bigger than the demo.** A sanitizer is not a proof of
correctness. It's an observer of the paths your test actually executed.
Undefined behaviour that nothing touches stays invisible — which is
exactly how this class of bug survives to production, where a different
input touches it.

### what catches what

| shape | caught by |
|---|---|
| 1. return reference to local | **compiler**, `-Wreturn-local-addr` |
| 2. view into a temporary | ASan, *if you dereference it* |
| 3. invalidation after realloc | ASan, reliably |
| 4. pointer into moved-from | ASan, once the new owner also dies |

Shape 4 needs a second look. This does *not* fire:

```cpp
const char* p = a.c_str();
std::string b = std::move(a);
std::puts(p);                  // b still owns the buffer. memory is live.
```

because `b` took the buffer and is still holding it. The pointer is
*wrong* — it aims into an object that no longer logically owns it — but
the memory is valid, so there's nothing for ASan to see. Let `b` die
first and it fires immediately:

```cpp
const char* p = a.c_str();
{ std::string b = std::move(a); }   // b dies here, freeing the buffer
std::puts(p);                       // NOW it's a heap-use-after-free
```

**The compiler catches some statically:** `-Wreturn-local-addr`,
`-Wdangling-pointer` (gcc 12+), `-Wdangling-gsl` (clang). Keep `-Wall
-Wextra` on.

**Valgrind** catches more but is 20x slower. ASan first.

---

## now break it

1. Write the dangling `string_view` and print it with `%.*s`. Then read
   `sv.data()[0]` explicitly. Only the second one fires. Understand why
   before moving on — that gap is where real bugs live.
2. Write the `const std::string& f()` local-return version and read
   `-Wreturn-local-addr`.
3. In shape 3, replace `reserve(1000)` with `push_back(4)`. Do the
   addresses change? Run it a few times.
4. Write shape 4 both ways: with the new owner still alive, and with it
   scoped so it dies first. Only one fires.
5. Put the three `counter()` calls back into one `printf` and see which
   order your compiler picks.
6. Add a `Loud` member to `Owner` and predict the new destruction order.

---

## check yourself

1. Three locals `a, b, c` declared in that order. What order do they
   destruct?
2. Do members destruct before or after the destructor body runs?
3. `std::string_view sv = std::string("x");` — what's wrong?
4. `int* p = &v[0]; v.push_back(1); *p = 5;` — is this safe?
5. Why is a function-local `static` better than a global for lazy init?
6. What flag turns "mysterious crash next Tuesday" into "line 47,
   use-after-free"?

<details>
<summary>answers</summary>

1. `c`, `b`, `a`. Reverse of construction.
2. After. The body runs first, so members are alive inside it.
3. The temporary string dies at the end of that statement, leaving the
   view pointing at freed memory.
4. No. `push_back` may reallocate, which invalidates `p`.
5. It's constructed on first use, so there's no cross-translation-unit
   ordering problem, and since C++11 the construction is thread-safe.
6. `-fsanitize=address`.

</details>

---

[next: copy, move, elision →](08-copy-move-elision.md)
