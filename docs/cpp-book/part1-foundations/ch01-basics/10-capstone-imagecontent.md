[← auto and decltype](09-auto-and-decltype.md) · [chapter index](README.md) · [next: exercises →](exercises.md)

# 10. Capstone: rebuild `ImageContent`

**Time:** 90 minutes
**Code:** [`code/10_imagecontent.cpp`](code/10_imagecontent.cpp)
**Real files:** [`lazy_bytes.hpp`](../../../../include/agentty/domain/lazy_bytes.hpp),
[`conversation.hpp`](../../../../include/agentty/domain/conversation.hpp)

```sh
cd code && make 10_imagecontent && ./10_imagecontent
```

Everything from this chapter, in one class that ships in agentty.

---

## the problem, stated properly

agentty threads carry images: pasted screenshots, tool results, attached
files. Base64 in the thread file, raw bytes in memory.

The eager version decoded every image at load. Open a thread with forty
screenshots and you wait while forty base64 payloads decode into memory
you probably won't look at. Then you save the thread and it re-encodes all
forty to write them back out.

Both of those are pure waste. Most images in a long thread are never
rendered again, and an untouched image doesn't need decoding *or*
re-encoding — the blob name can just be written straight back.

The design that fixes it needs to:

1. hold either bytes or a way to get bytes
2. materialise on first real use, once
3. stay callable through a `const Message&`, because that's how the
   renderer sees it
4. answer "is there anything here" **without** materialising
5. let the writer re-persist an untouched payload by reference
6. behave exactly like the eager version when a blob is missing

---

## the real code

```cpp
// include/agentty/domain/lazy_bytes.hpp
class LazyBytes {
public:
    struct Source {
        std::string blob;   // content-addressed blob name (preferred)
        std::string b64;    // legacy inline base64 fallback
        [[nodiscard]] bool empty() const noexcept {
            return blob.empty() && b64.empty();
        }
    };

    LazyBytes() = default;
    explicit LazyBytes(std::string raw)
        : bytes_(std::move(raw)), resolved_(true) {}

    [[nodiscard]] static LazyBytes lazy(Source src) {
        LazyBytes v;
        v.source_   = std::move(src);
        v.resolved_ = v.source_.empty();   // nothing to resolve => done
        return v;
    }
    [[nodiscard]] static LazyBytes from_blob(std::string name) {
        return lazy(Source{.blob = std::move(name), .b64 = {}});
    }
    [[nodiscard]] static LazyBytes from_base64(std::string b64) {
        return lazy(Source{.blob = {}, .b64 = std::move(b64)});
    }

    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            bytes_    = resolver_ ? resolver_(source_) : std::string{};
            resolved_ = true;
        }
        return bytes_;
    }

    void set_bytes(std::string raw) {
        bytes_    = std::move(raw);
        source_   = {};
        resolved_ = true;
    }

    [[nodiscard]] const Source& source() const noexcept { return source_; }
    [[nodiscard]] bool materialised() const noexcept { return resolved_; }

    [[nodiscard]] bool empty() const noexcept {
        return resolved_ ? bytes_.empty() : source_.empty();
    }

    using Resolver = std::string (*)(const Source&);
    static void set_resolver(Resolver r) noexcept { resolver_ = r; }

private:
    inline static Resolver resolver_ = nullptr;

    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
};
```

```cpp
// include/agentty/domain/conversation.hpp
class ImageContent {
public:
    using Source = LazyBytes::Source;

    std::string media_type;  // "image/png", "image/jpeg", "image/webp", "image/gif"

    ImageContent() = default;
    ImageContent(std::string mt, std::string raw_bytes)
        : media_type(std::move(mt)), data_(std::move(raw_bytes)) {}

    static ImageContent lazy(std::string mt, Source src) {
        ImageContent img;
        img.media_type = std::move(mt);
        img.data_      = LazyBytes::lazy(std::move(src));
        return img;
    }

    [[nodiscard]] const std::string& bytes() const { return data_.bytes(); }
    void set_bytes(std::string raw) { data_.set_bytes(std::move(raw)); }
    [[nodiscard]] const Source& source() const noexcept { return data_.source(); }
    [[nodiscard]] bool materialised() const noexcept { return data_.materialised(); }

    using Resolver = LazyBytes::Resolver;
    static void set_resolver(Resolver r) noexcept { LazyBytes::set_resolver(r); }

private:
    LazyBytes data_;
};
```

Now every decision in it, and which section of this chapter it came from.

---

## `struct Source` — aggregate, section 5

```cpp
struct Source {
    std::string blob;
    std::string b64;
    [[nodiscard]] bool empty() const noexcept { return blob.empty() && b64.empty(); }
};
```

Two ways to get bytes, one type. It's an aggregate, so it gets built with
designated initialisers:

```cpp
Source{.blob = std::move(name), .b64 = {}}
```

Without the field names you'd write `Source{name, {}}` and every reader
has to check which one is which. §5.

`empty()` is `const noexcept [[nodiscard]]` for the reasons in §3: it
can't modify, it can't throw, and discarding the answer is a bug.

---

## two constructors, two situations — section 6

```cpp
explicit LazyBytes(std::string raw)
    : bytes_(std::move(raw)), resolved_(true) {}

[[nodiscard]] static LazyBytes lazy(Source src) { ... }
```

**Eager** — you already have the bytes. A paste, a screenshot, a tool
result. `resolved_` starts true because there's nothing to resolve.

**Lazy** — you have a source. A thread loaded from disk. `resolved_`
starts false.

Both take their argument **by value and move** (§6's sink parameter), so
one signature is optimal whether the caller wants to keep their copy or
not.

Why is `lazy` a named static function instead of a second constructor?
Because `LazyBytes(std::string)` and `LazyBytes(Source)` would be two
overloads and the call site wouldn't say which one you meant.
`LazyBytes::from_blob(name)` says it. That's the named-constructor idiom,
and it's worth reaching for whenever overloads would be ambiguous *to a
reader*, not just to the compiler.

Note the subtlety in `lazy`:

```cpp
v.resolved_ = v.source_.empty();   // nothing to resolve => done
```

An empty source isn't pending, it's finished-with-nothing. Getting this
wrong would mean an empty source stays "unresolved" forever and calls the
resolver on every access.

---

## `mutable` + const `bytes()` — section 6

```cpp
mutable std::string bytes_;
mutable bool        resolved_ = true;

[[nodiscard]] const std::string& bytes() const {
    if (!resolved_) {
        bytes_    = resolver_ ? resolver_(source_) : std::string{};
        resolved_ = true;
    }
    return bytes_;
}
```

```
-- lazy image (loaded from a thread file) --
  materialised? no   <- nothing decoded yet
  empty?        no   <- answered WITHOUT resolving
  now ask for bytes:
    [resolve blob=sha256-aa11 b64=0b]
  bytes:        4096
  ask again:    4096   <- no second resolve
```

The resolve printed once. The second call hit the cache.

`bytes()` **must** be const, because the renderer holds a `const Message&`
and needs pixels. Making it non-const would force a mutable path to the
conversation into the render layer, which is a far worse problem.

The `mutable` is honest because the three invariants hold, and the real
header states them:

- **idempotent** — same bytes on every call, so two const calls are
  indistinguishable
- **copy-safe** — a copy copies `source_` too, so the copy resolves to the
  same bytes rather than to nothing
- **failure-equivalent** — a missing blob resolves to empty, which is
  exactly what the eager loader produced for the same input, and every
  consumer already handles empty

```
-- a missing blob resolves to empty, not a crash --
    [resolve blob=sha256-dead b64=0b]
  bytes: 0
```

No exception, no crash, no special case. The failure mode was chosen to be
one the rest of the system already handles.

---

## `empty()` doesn't materialise — the actual win

```cpp
[[nodiscard]] bool empty() const noexcept {
    return resolved_ ? bytes_.empty() : source_.empty();
}
```

If `empty()` called `bytes()`, every "does this message have an image"
check would decode the image, and the entire design would buy nothing.

The branch answers from whichever side is authoritative: if we've
resolved, ask the bytes; if not, ask the source. An unmaterialised payload
with a source is not empty, whatever it eventually resolves to.

Same for `source()` and `materialised()`. The persistence writer uses
them:

```cpp
if (!img.materialised())
    write_blob_reference(img.source().blob);   // no decode, no re-encode
else
    write_blob(img.bytes());
```

Saving a thread you only scrolled through touches zero image bytes. That's
the payoff, and it exists only because the cheap questions stayed cheap.

---

## the static resolver — dependency inversion

```cpp
using Resolver = std::string (*)(const Source&);
inline static Resolver resolver_ = nullptr;
static void set_resolver(Resolver r) noexcept { resolver_ = r; }
```

`lazy_bytes.hpp` knows nothing about the blob directory, about base64,
about the filesystem. It knows there are bytes that might not be here yet.

The persistence layer installs the function at startup. That keeps the
domain header dependency-free and makes it testable: a unit test that
never installs a resolver gets empty payloads, which is the same as a
missing blob, which is already a handled case.

`inline static` (C++17) lets a static data member be defined right in the
header with no separate definition in a `.cpp`. Before C++17 this needed
an out-of-line definition and was a common linker-error source.

**The thread-safety caveat, stated plainly:** a raw function pointer with
no atomics is fine here because it's installed once at startup before any
thread reads it. If agentty ever resolved from multiple threads this would
need a mutex or an atomic. That's the kind of thing worth writing down
next to the code, not discovering later.

---

## `ImageContent` wraps it — separation of concerns

```cpp
class ImageContent {
public:
    using Source = LazyBytes::Source;
    std::string media_type;
    // ...
private:
    LazyBytes data_;
};
```

`LazyBytes` knows about bytes. `ImageContent` knows about images. The
`media_type` is public because it's a plain value with no invariant. The
`data_` is private because materialisation has rules.

The header comment says why `LazyBytes` is its own file: both
`conversation.hpp` (`ImageContent`) and `composer_attachment.hpp`
(`Attachment`) need it, and `conversation.hpp` already includes
`composer_attachment.hpp`, so defining it in either would be a cycle. It's
also genuinely general — it knows nothing about images or attachments.

**Rule of zero, §8:** `ImageContent` declares no destructor, no copy, no
move. Its only member manages itself, so the compiler's six are correct
and optimal.

---

## the whole thing in use

```
-- the whole thing inside a message --
  message msg-01: 'look at this', 2 image(s)
    image/png  resolved=yes bytes=1024
    [resolve blob=- b64=8b]
    image/jpeg resolved=yes bytes=17

-- const message: you can still read the bytes --
  first image bytes through a const& : 1024
```

```cpp
Message m{
    .id     = MessageId{"msg-01"},       // §3 strong type, §5 designated init
    .text   = "look at this",
    .images = {},
};
m.images.push_back(std::move(pasted));   // §4 move, §8 move ctor
m.images.emplace_back("image/jpeg",      // §8 construct in place
                      Source{.blob = {}, .b64 = "SGVsbG8="});

for (const auto& img : m.images)         // §9 no copies
    use(img.bytes());                    // §6 const, §6 mutable cache
```

Count the sections. Strong types, move semantics, designated initialisers,
const correctness, `mutable`, rule of zero, `emplace_back`, `const auto&`.
All of chapter 1, in ten lines of ordinary application code.

That's the point. None of this is exotic. It's what careful C++ looks
like when every piece is doing its job.

---

## what to do now

1. Run `./10_imagecontent` and follow the resolve messages.
2. Open the two real headers and read them top to bottom. You now know
   every construct in them.
3. Do [the exercises](exercises.md). Exercise 8 asks you to extend
   `LazyBytes` for real.

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
   it to false and the resolver gets called on every single access forever.
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
