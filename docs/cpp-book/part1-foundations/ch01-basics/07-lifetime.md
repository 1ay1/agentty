[← references and const](06-references-and-const.md) · [chapter index](README.md) · [next: copy, move, elision →](08-copy-move-elision.md)

# 7. Lifetime

**Time:** 75 minutes
**Code:** [`code/07_lifetime.cpp`](code/07_lifetime.cpp)

```sh
cd code && make 07_lifetime && ./07_lifetime
```

Every object is born and dies at a point the compiler knows. Use it
outside that window and you get undefined behaviour. This section is the
most important one in the chapter.

---

## destruction is reverse of construction

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

A stack. Last constructed, first destroyed. Always, with no exceptions,
including when an exception unwinds the stack.

That predictability is what makes RAII work. If `b` holds a lock and `c`
uses it, you *know* `c` dies first, so the lock is still held when `c`'s
destructor runs. Chapter 2 builds on exactly this.

---

## members, and the body-first rule

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

**Members construct in declaration order and destruct in reverse.** Not
in the order you wrote the init list. If you write them in a different
order, gcc's `-Wreorder` warns you, and you should listen — if one
member's initialiser reads another, the declaration order is load-bearing.

**The destructor body runs *before* members are destroyed.** So inside
`~Owner()` every member is still fully alive and usable. That's why you
can flush a buffer to a file member in a destructor body and it works.

---

## the four ways to dangle

```
-- dangling, all four shapes --
1. reference to a local:
     const std::string& f() { std::string s = "x"; return s; }
2. view into a temporary:
     std::string_view sv = std::string("temp");
   the string dies at the end of THAT statement. sv is garbage.
3. iterator/reference invalidated by growth:
     &v[0] before push_back = 0x7b92d41e0010
     &v[0] after  reserve   = 0x7d82d41e1500
   if those differ, every saved pointer just went stale.
4. pointer to a member of a moved-from object:
     the buffer moved away; the pointer still aims at the old one.
```

Every dangling bug you ever write is one of these four. Learn the shapes.

### 1. returning a reference to a local

```cpp
const std::string& bad() {
    std::string s = "hello";
    return s;                 // s dies at the closing brace
}
```

gcc and clang both warn (`-Wreturn-local-addr`). Return by value instead
— section 8 shows it costs nothing.

### 2. a view into something temporary

```cpp
std::string_view sv = std::string("temp");    // dangling immediately
```

The temporary `std::string` dies at the end of that statement.
`string_view` doesn't extend anything — it's a pointer and a length, and
now the pointer is stale.

The sneaky version:

```cpp
std::string_view name = get_config()["name"];    // if the config is a temporary
std::string_view first = full_name.substr(0, 5); // ok: substr of a string_view
                                                 // is a view of the SAME buffer
```

### 3. invalidation

```cpp
std::vector<int> v{1, 2, 3};
int* p = &v[0];
v.push_back(4);               // may reallocate
*p = 10;                      // maybe use-after-free
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
std::string a = "long enough to be heap allocated";
const char* p = a.c_str();
std::string b = std::move(a);      // b took the buffer
std::puts(p);                      // p pointed into a's buffer. gone.
```

---

## string_view: the sharpest tool here

```
-- string_view is a borrow, always --
ok  = 'i own my bytes'
std::string_view bad = build();   <- dangles immediately
good = 'built on the fly'
rule: a string_view parameter is fine. a string_view MEMBER
or return value needs you to prove the owner outlives it.
```

`std::string_view` is a pointer and a length. It owns nothing. It's a
borrow with no borrow checker.

**Safe:**

```cpp
void log(std::string_view msg);       // parameter: the caller's string
                                      // outlives the call, guaranteed
```

Takes a `std::string`, a `const char*`, a literal, another view, with zero
allocations. This is the single best use of `string_view` and you should
reach for it every time a function reads a string it won't keep.

**Dangerous, needs proof:**

```cpp
struct Bad  { std::string_view name; };   // outlives its owner?
std::string_view f();                      // returns a view of what?
```

Both can be correct, if the owner provably outlives the view — a view into
a string literal or a long-lived buffer is fine. But now *you* are
maintaining that proof, not the compiler, and a refactor two months from
now will break it silently.

**The rule:** `string_view` parameters, freely. `string_view` members and
return values, only when you can state in a comment why the owner
outlives it.

---

## storage durations

```
-- storage durations --
automatic  : locals, die at end of scope
static     : counter() -> 1, 2, 3 (survives calls)
dynamic    : new/delete, or better, a smart pointer (ch05)
thread     : thread_local, one per thread
```

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

Note the demo prints `1, 2, 3` only because the three calls are in
separate statements. Put them in one `printf` and argument evaluation
order is unspecified, so you might get `3, 2, 1`. That's a real trap, and
the source comments on it.

**Dynamic** — `new`/`delete`, or what a smart pointer holds. You decide
when it dies. Chapter 5.

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

Now `config()` always gives you a constructed object, whoever calls first.

---

## finding these bugs

**AddressSanitizer catches use-after-free and stack-use-after-scope at
runtime.** Every program in `code/` builds with it. It's the single most
valuable tool in C++ and it costs about 2x runtime:

```sh
g++ -fsanitize=address,undefined -g your.cpp -o your && ./your
```

Try it. Uncomment one of the dangling examples in `07_lifetime.cpp`,
rebuild, run, and read what ASan prints. It gives you the allocation
stack, the free stack, and the use stack. That output teaches lifetime
better than any paragraph.

**The compiler catches some statically:** `-Wreturn-local-addr`,
`-Wdangling-pointer` (gcc 12+), `-Wdangling-gsl` (clang). Keep `-Wall
-Wextra` on.

**Valgrind** catches more but is 20x slower. ASan first.

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
