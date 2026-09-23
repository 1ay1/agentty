[← auto and decltype](09-auto-and-decltype.md) · [chapter index](README.md) · [next: exercises →](exercises.md)

# 10. Capstone: rebuild `ImageContent`

**Time:** 90 minutes. Type all of it.
**Real files:** [`lazy_bytes.hpp`](../../../../include/agentty/domain/lazy_bytes.hpp),
[`conversation.hpp`](../../../../include/agentty/domain/conversation.hpp)

Everything from this chapter, in one class that ships in agentty. We're
building it from the problem statement up, and every decision traces back
to a section you've already done.

Open `10_imagecontent.cpp`.

---

## the problem, stated properly

agentty threads carry images: pasted screenshots, tool results, attached
files. Base64 in the thread file, raw bytes in memory.

The eager version decoded every image at load. Open a thread with forty
screenshots and you wait while forty base64 payloads decode into memory
you probably won't look at. Then you save the thread and it re-encodes all
forty to write them back.

Both of those are pure waste. Most images in a long thread are never
rendered again, and an untouched image doesn't need decoding *or*
re-encoding — the blob name can just be written straight back.

**The design has to:**

1. hold either bytes or a way to get bytes
2. materialise on first real use, once
3. stay callable through a `const Message&`, because that's how the
   renderer sees it
4. answer "is there anything here" **without** materialising
5. let the writer re-persist an untouched payload by reference
6. behave exactly like the eager version when a blob is missing

Keep that list next to you. We'll tick each one off.

---

## piece 1: the strong id (§3)

Start the file:

```cpp
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

template <typename Tag>
struct Id {
    std::string value;
    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }
    bool operator==(const Id&) const = default;
};
struct MessageIdTag {};
using MessageId = Id<MessageIdTag>;
```

Nothing new — that's section 3, trimmed to what we need here.

---

## piece 2: the source (§5)

```cpp
class LazyBytes {
public:
    struct Source {
        std::string blob;   // name in the content-addressed store
        std::string b64;    // or inline base64 from an old thread file
        [[nodiscard]] bool empty() const noexcept {
            return blob.empty() && b64.empty();
        }
    };
```

Two ways to get bytes, one type. It's an **aggregate** (§5), so it gets
built with designated initialisers:

```cpp
Source{.blob = std::move(name), .b64 = {}}
```

Without the field names you'd write `Source{name, {}}` and every reader
has to check which one is which.

`empty()` is `const noexcept [[nodiscard]]` for the §3 reasons: it can't
modify, it can't throw, and discarding the answer is a bug.

---

## piece 3: two ways to construct (§6)

```cpp
    LazyBytes() = default;

    // eager: a paste or a screenshot, bytes already in hand
    explicit LazyBytes(std::string bytes) noexcept
        : bytes_(std::move(bytes)), resolved_(true) {}

    // lazy: loaded from disk, nothing decoded yet
    explicit LazyBytes(Source src) noexcept
        : source_(std::move(src)), resolved_(false) {}
```

**Eager** — you already have the bytes. `resolved_` starts `true` because
there's nothing to resolve.

**Lazy** — you have a source. `resolved_` starts `false`.

Both take their argument **by value and move** — the §6 sink parameter —
so one signature is optimal whether the caller wants to keep their copy or
not. Both `noexcept`, because moving a string can't throw (§8, and it's
why a `vector<ImageContent>` will move rather than copy when it grows).

### a subtlety the real code gets right

The real header doesn't use a second constructor. It uses a named one:

```cpp
[[nodiscard]] static LazyBytes lazy(Source src) {
    LazyBytes v;
    v.source_   = std::move(src);
    v.resolved_ = v.source_.empty();   // nothing to resolve => already done
    return v;
}
[[nodiscard]] static LazyBytes from_blob(std::string name) {
    return lazy(Source{.blob = std::move(name), .b64 = {}});
}
```

Two reasons.

**Readability.** `LazyBytes(std::string)` and `LazyBytes(Source)` are two
overloads, and the call site doesn't say which you meant.
`LazyBytes::from_blob(name)` says it. That's the **named constructor**
idiom — reach for it whenever overloads would be ambiguous *to a reader*,
not just to the compiler.

**Correctness.** Look at that line again:

```cpp
v.resolved_ = v.source_.empty();
```

An **empty** source isn't pending, it's finished-with-nothing. Set it to
plain `false` and an empty source stays "unresolved" forever, calling the
resolver on every single access. That's a real bug, and it's one line.

Note `return v;` is NRVO (§8) — no copy, no move.

---

## piece 4: the mutable cache (§6)

This is the heart of it.

```cpp
    // const, because resolving is not a logical change.
    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            std::printf("    [resolve blob=%s b64=%zub]\n",
                        source_.blob.empty() ? "-" : source_.blob.c_str(),
                        source_.b64.size());
            bytes_    = resolve(source_);
            resolved_ = true;
        }
        return bytes_;
    }

private:
    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
```

**Why `bytes()` must be const.** The renderer holds a `const Message&`.
The provider serialiser holds a `const Thread&`. Neither is modifying
anything, and both need the pixels. Making this non-const would force a
mutable path to the conversation into the render layer — a far worse
problem than a `mutable` member.

**Why the `mutable` is honest.** The §6 test is *logical constness*: do
two const calls look different from outside? No, and the real header
states all three reasons as explicit invariants:

- **idempotent** — same bytes every call
- **copy-safe** — a copy copies `source_` too, so it resolves to the same
  bytes rather than to nothing
- **failure-equivalent** — a missing blob gives empty bytes, exactly what
  the eager loader produced for the same input

Being able to write those three down is what makes it reviewable instead
of a vibe.

**Note `resolved_ = true` as the default.** A default-constructed
`LazyBytes` is empty-and-done, not empty-and-pending.

---

## piece 5: the cheap questions (§6) — the actual win

```cpp
    [[nodiscard]] bool empty() const noexcept {
        return resolved_ ? bytes_.empty() : source_.empty();
    }
    [[nodiscard]] bool materialised() const noexcept { return resolved_; }
    [[nodiscard]] const Source& source() const noexcept { return source_; }
```

**`empty()` does not call `bytes()`.** If it did, every "does this message
have an image" check would decode the image and the whole design would buy
nothing.

The branch answers from whichever side is authoritative: resolved → ask
the bytes; not resolved → ask the source.

The persistence writer uses the other two:

```cpp
if (!img.materialised())
    write_blob_reference(img.source().blob);   // no decode, no re-encode
else
    write_blob(img.bytes());
```

Saving a thread you only scrolled through touches **zero** image bytes.
That's requirement 5 from the list, and it works only because the cheap
questions stayed cheap.

---

## piece 6: the resolver — dependency inversion

```cpp
    using Resolver = std::string (*)(const Source&);
    static void set_resolver(Resolver r) noexcept { resolver_ = r; }

private:
    static std::string resolve(const Source& s) {
        if (resolver_) return resolver_(s);
        return {};                       // null resolver == missing blob
    }

    inline static Resolver resolver_ = nullptr;
```

`lazy_bytes.hpp` knows nothing about the blob directory, about base64,
about the filesystem. It knows there are bytes that might not be here yet.
The persistence layer installs the function at startup.

That keeps the domain header dependency-free and makes it testable: a unit
test that never installs a resolver gets empty payloads, which is the same
as a missing blob, which is already a handled case.

**`inline static`** (C++17) lets a static data member be defined right in
the header with no separate definition in a `.cpp`. Before C++17 this
needed an out-of-line definition and was a classic linker-error source.

**Why a raw function pointer and not `std::function`?** A `std::function`
member would add bytes to every object and possibly allocate. There's
exactly one resolver process-wide, so there's nothing to store per object.

**The thread-safety caveat, stated plainly:** a raw pointer with no
atomics is fine here because it's installed once at startup before any
thread reads it. If agentty ever resolved from multiple threads this would
need a mutex or an atomic. Worth writing down next to the code rather than
discovering later.

---

## piece 7: the image wrapper

```cpp
class ImageContent {
public:
    using Source = LazyBytes::Source;

    std::string media_type;            // "image/png", "image/jpeg", ...
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    ImageContent() = default;

    ImageContent(std::string mt, std::string raw) noexcept
        : media_type(std::move(mt)), data_(std::move(raw)) {}

    ImageContent(std::string mt, Source src) noexcept
        : media_type(std::move(mt)), data_(std::move(src)) {}

    [[nodiscard]] const std::string& bytes() const { return data_.bytes(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] bool materialised() const noexcept { return data_.materialised(); }

private:
    LazyBytes data_;
};
```

`LazyBytes` knows about bytes. `ImageContent` knows about images.

`media_type`, `width` and `height` are **public** because they're plain
values with no invariant. `data_` is **private** because materialisation
has rules. That's the whole access-control decision, and it's worth making
consciously rather than defaulting to "everything private".

`std::uint32_t` for the dimensions (§1): an image is never 5 billion
pixels wide and never −40 pixels wide. The type says both.

**Rule of zero (§8):** `ImageContent` declares no destructor, no copy, no
move. Its only member manages itself, so the compiler's six are correct
and optimal. And since we never declare a destructor, the implicit moves
survive — which, as §8 showed, is not something to take for granted.

### why LazyBytes lives in its own header

The real header explains it:

> both `conversation.hpp` (`ImageContent`) and `composer_attachment.hpp`
> (`Attachment`) need it, and `conversation.hpp` already includes
> `composer_attachment.hpp` — defining it in either would be a cycle. It
> is also genuinely general: it knows nothing about images or
> attachments, only about bytes that may not be here yet.

---

## run it

Add a message type, a fake blob store, and a `main`:

```cpp
struct Message {
    MessageId                 id;
    std::string               text;
    std::vector<ImageContent> images;
};

static std::string fake_store(const LazyBytes::Source& s) {
    if (s.blob == "sha256-aa11") return std::string(4096, '\x89');
    if (!s.b64.empty())          return "decoded(" + s.b64 + ")";
    return {};
}

int main() {
    LazyBytes::set_resolver(&fake_store);

    std::puts("-- eager image (a paste): bytes are already here --");
    ImageContent pasted{"image/png", std::string(1024, '\x89')};
    pasted.width = 64;
    pasted.height = 64;
    std::printf("  materialised? %s\n", pasted.materialised() ? "yes" : "no");
    std::printf("  empty?        %s\n", pasted.empty() ? "yes" : "no");
    std::printf("  bytes:        %zu\n", pasted.bytes().size());

    std::puts("\n-- lazy image (loaded from a thread file) --");
    ImageContent loaded{"image/png",
                        ImageContent::Source{.blob = "sha256-aa11", .b64 = {}}};
    std::printf("  materialised? %s   <- nothing decoded yet\n",
                loaded.materialised() ? "yes" : "no");
    std::printf("  empty?        %s   <- answered WITHOUT resolving\n",
                loaded.empty() ? "yes" : "no");
    std::puts("  now ask for bytes:");
    std::printf("  bytes:        %zu\n", loaded.bytes().size());
    std::printf("  ask again:    %zu   <- no second resolve\n",
                loaded.bytes().size());

    std::puts("\n-- a missing blob resolves to empty, not a crash --");
    ImageContent gone{"image/png",
                      ImageContent::Source{.blob = "sha256-dead", .b64 = {}}};
    std::printf("  bytes: %zu\n", gone.bytes().size());
}
```

```
-- eager image (a paste): bytes are already here --
  materialised? yes
  empty?        no
  bytes:        1024

-- lazy image (loaded from a thread file) --
  materialised? no   <- nothing decoded yet
  empty?        no   <- answered WITHOUT resolving
  now ask for bytes:
    [resolve blob=sha256-aa11 b64=0b]
  bytes:        4096
  ask again:    4096   <- no second resolve

-- a missing blob resolves to empty, not a crash --
    [resolve blob=sha256-dead b64=0b]
  bytes: 0
```

Tick the requirements off: the resolve printed **once**, `empty()`
answered **without** resolving, and the missing blob gave 0 bytes with no
exception.

---

## the whole thing in use

```cpp
    std::puts("\n-- the whole thing inside a message --");
    Message m{
        .id     = MessageId{"msg-01"},
        .text   = "look at this",
        .images = {},
    };
    m.images.push_back(std::move(pasted));
    m.images.emplace_back("image/jpeg",
                          ImageContent::Source{.blob = {}, .b64 = "SGVsbG8="});

    std::printf("  message %s: '%s', %zu image(s)\n",
                m.id.c_str(), m.text.c_str(), m.images.size());
    for (const auto& img : m.images)
        std::printf("    %-10s resolved=%-3s bytes=%zu\n",
                    img.media_type.c_str(),
                    img.materialised() ? "yes" : "no",
                    img.bytes().size());

    std::puts("\n-- const message: you can still read the bytes --");
    const Message& cm = m;
    std::printf("  first image bytes through a const& : %zu\n",
                cm.images.front().bytes().size());
```

```
-- the whole thing inside a message --
  message msg-01: 'look at this', 2 image(s)
    image/png  resolved=yes bytes=1024
    [resolve blob=- b64=8b]
    image/jpeg resolved=yes bytes=17

-- const message: you can still read the bytes --
  first image bytes through a const& : 1024
```

**Count the sections in those ten lines:**

- `MessageId{"msg-01"}` — strong type, §3
- `.id = ... .text = ...` — designated initialisers, §5
- `push_back(std::move(pasted))` — value categories §4, move ctor §8
- `emplace_back(...)` — construct in place, §8
- `for (const auto& img : ...)` — no copies, §9
- `cm.images.front().bytes()` — const correctness §6, mutable cache §6

That's all of chapter 1, in ten lines of ordinary application code. None
of it is exotic. It's what careful C++ looks like when every piece is
doing its job.

---

## now break it

1. Change `v.resolved_ = v.source_.empty();` to `= false`. Construct a
   `LazyBytes` with an empty source and call `bytes()` twice. Watch the
   resolver run both times.
2. Make `empty()` call `bytes()`. Then call `empty()` on the lazy image
   and watch it resolve. That's the bug this design avoids.
3. Remove `mutable` from `bytes_`. Read the error.
4. Remove `const` from `bytes()` and try the `const Message& cm` block.
5. Add `~ImageContent() {}` — an empty destructor — then check
   `std::is_nothrow_move_constructible_v<ImageContent>`. §8 told you what
   happens. Confirm it.
6. Never call `set_resolver`. What do the lazy images do? (This is the
   unit-test path.)

---

## what to do now

1. Run your program and follow the resolve messages.
2. Open the two real headers and read them top to bottom. You now know
   every construct in them.
3. Do [the exercises](exercises.md). Exercise 6 asks you to rebuild
   `LazyBytes` from the spec with no reference, and exercise 7 asks
   whether it should be thread-safe.

---

## check yourself

1. Why does `bytes()` have to be `const`?
2. What would break if `empty()` called `bytes()`?
3. Why does `lazy()` set `resolved_ = source_.empty()` instead of `false`?
4. Why is the resolver a static function pointer instead of a virtual
   method or a `std::function` member?
5. Why does a missing blob resolve to empty instead of throwing?
6. Why does `LazyBytes` live in its own header?

<details>
<summary>answers</summary>

1. Because the renderer and serialiser both hold `const Message&` and both
   need the bytes. Non-const would force a mutable path to the
   conversation into the render layer.
2. Every "is there an image here" check would materialise the payload, and
   the lazy design would save nothing.
3. An empty source has nothing to resolve, so it's already finished. Set
   it to false and the resolver gets called on every access forever.
4. It keeps the domain header free of any dependency on the blob store,
   and there's exactly one resolver process-wide so there's nothing to
   store per object. A `std::function` member would add bytes to every
   image.
5. Because that's what the eager loader did for the same input, so every
   consumer already handles it. Choosing a failure mode the system already
   copes with beats adding a new one.
6. Both `conversation.hpp` and `composer_attachment.hpp` need it and
   `conversation.hpp` already includes the latter, so putting it in either
   creates a cycle. It's also general enough to stand alone.

</details>

---

[next: exercises →](exercises.md)
