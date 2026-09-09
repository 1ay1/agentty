# Thread Store — indexed head, async body

**Status:** proposal. Nothing here is implemented yet.
**Supersedes:** the ad-hoc laziness in `ImageContent` (commit `3d976a39`),
which this design absorbs and generalises.

---

## 1. The problem, measured

Across the 539 threads on a real machine — **596 MB** on disk:

| field | MB | share |
|---|---:|---:|
| `tool_calls` | 348.7 | 60.9% |
| `images` | 130.4 | 22.8% |
| `text` | 36.0 | 6.3% |
| `thinking_signature` | 29.1 | 5.1% |
| `thinking_blocks` | 13.3 | 2.3% |
| everything else | ~14 | 2.4% |

Two fields are **84%** of all bytes. Neither is needed to draw a frame.

Breaking `tool_calls` down further — 104 443 calls:

| part | MB |
|---|---:|
| `output` | 263.3 |
| `input` / args | 75.0 |
| metadata (name, id, status…) | 4.7 |

And the renderer does not draw that output. `tool_output_render_cap()`
caps `shell` at **4 rows**, `read` at 5, `git_*`/`grep`/`glob` at 7. A
500-line `git diff` renders as 7 rows. The bytes are loaded, parsed,
held in RAM for the life of the session — and 99% of them are never
looked at.

The current cost on the worst thread (29 MB, 2519 messages), measured in
an **optimized** build (this matters — `./build` is Debug `-O0`, where
nlohmann is ~16× slower and every measurement lies):

```
io            10 ms
json parse    89 ms
parse_thread 134 ms
render         4 ms     ← not the bottleneck, and never was
```

**Rendering is already fast.** `rehydrate_frozen` is bounded to ~2
screens. Making rendering lazier buys nothing; the 4 ms is not where the
230 ms went. The cost is *materialising data the frame does not use*.

---

## 2. What this design must also serve

The stated future requirement: **"later I might want to attach everything
to the thread files too."** Documents, audio, diffs, build logs, images,
whatever. That changes the shape of the answer.

If attachments grow without bound and every one of them lands in a single
JSON document, then *any* design that parses the whole document on open
is dead on arrival — lazy field access included. The fix cannot be "skip
the expensive fields." It has to be: **the thread file stops being one
document.**

So the requirement is not "make load faster." It is:

> Opening a thread must cost **O(what is on screen)**, not O(thread), and
> must stay that way as arbitrary large content is attached.

---

## 3. Design: a thread is an index plus a content-addressed store

Three layers, one owner each.

```
┌──────────────────────────────────────────────────────────────┐
│ ThreadStore            (io/thread_store/)                    │
│   the ONLY thing that reads or writes thread bytes           │
├──────────────────────────────────────────────────────────────┤
│  Index      <id>.idx.json    small, always fully parsed      │
│             everything the renderer needs, and nothing else  │
│                                                              │
│  Segments   <id>.seg/NNNN    message bodies, append-only     │
│             parsed on demand, in ~256-message chunks         │
│                                                              │
│  Blobs      blobs/<hash>     content-addressed payloads      │
│             images, tool output, attachments; never parsed   │
└──────────────────────────────────────────────────────────────┘
```

### 3.1 The index — the SSOT for rendering

One file, one parse, bounded size. It holds **exactly** the fields the
render path reads, which I established by walking every consumer:

```cpp
struct MessageIndex {
    MessageId   id;
    Role        role;
    Timestamp   timestamp;

    // Prose is small (6.3% of all bytes) and every renderer needs it.
    // Inline up to kInlineTextCap; longer bodies spill to a blob and
    // keep a head slice for the collapsed/frozen view.
    std::string text;              // full, or head slice if spilled
    std::optional<BlobRef> text_overflow;

    // Enough to draw the tool card WITHOUT its output: name, status,
    // and the head slice the card actually shows.
    //
    // Two populations, both measured over 104 443 real calls:
    //
    //   CAPPED (30 976 calls — shell 4 rows, read 5, git_*/grep/glob/
    //   web_* 7). The card can never show more than ~560 B at 80 cols,
    //   so a 1 KB slice is provably enough.
    //
    //   UNCAPPED (73 508 calls — bash, edit, write, process_*, todo…
    //   70% of all calls, so this is the COMMON case, not an edge one).
    //   These render full output, but they are SMALL: p50 = 506 B,
    //   p90 = 2.4 KB, p99 = 8.3 KB, and 13% exceed 40 lines and get
    //   folded to ~1 row by the frozen renderer anyway.
    //
    // So the cap is 4 KB, which renders ~p95 of all uncapped calls with
    // zero body loads, and the rest fold or ask for a body. Index size
    // for the 29 MB / 2519-message thread, measured:
    //
    //     preview cap   index size
    //        0 B          1.5 MB
    //      512 B          2.7 MB
    //     1024 B          3.6 MB
    //     4096 B          ~6 MB    ← chosen: still 5× smaller than today
    //
    // `truncated` tells the card whether `preview` IS the whole output.
    // When false the card draws from the index alone and never touches
    // a blob — which is what keeps the common path synchronous.
    struct ToolIndex {
        ToolId       id;
        ToolName     name;
        ToolStatus   status;
        std::string  preview;      // ≤ kToolPreviewCap bytes
        bool         truncated;    // false ⇒ preview is the FULL output
        std::size_t  output_len;   // for "1.2 MB" labels + token math
        BlobRef      output;       // fetched only when truncated
        BlobRef      input;
    };
    std::vector<ToolIndex> tools;

    // Images: dimensions and size, never bytes. The view draws a chip.
    struct ImageIndex {
        std::string media_type;
        std::size_t byte_len;
        int w = 0, h = 0;
        BlobRef     data;
    };
    std::vector<ImageIndex> images;

    // Opaque provider state (thinking_signature, reasoning_encrypted,
    // thinking_blocks — 8% of bytes). NEVER rendered; needed only when
    // this message is replayed onto the wire.
    BlobRef provider_state;

    std::uint64_t render_key;      // Message::compute_render_key(), precomputed
    SegmentRef    body;            // where the full Message lives
};
```

Measured on the real 29 MB / 2519-message thread: **~6 MB** at a 4 KB
preview cap, against 29.1 MB today — a 5× reduction in bytes parsed to
open, with **no body load for ~95% of tool cards**. (1.5 MB of that is
structure and prose; the rest is previews. A smaller cap shrinks the
index further but starts trading frames of latency on the common path,
which is the wrong direction — see the distribution above.)

`render_key` is stored rather than recomputed: it is a pure function of
the message, so persisting it lets the view cache hit *before* the body
is loaded.

### 3.2 Segments — bodies, in chunks

Full `Message` values, ~256 per segment, append-only. A thread that grows
by one turn appends to the last segment instead of rewriting 29 MB — which
also removes today's rewrite-everything-per-turn autosave cost.

Loaded when something genuinely needs a body: scrolling into an old
region, editing, forking, or building a wire request.

### 3.3 Blobs — the escape hatch that makes attachments viable

Already exists (`blobs/<fnv1a>`), already content-addressed, already
deduplicating. This design leans on it much harder: **tool output, images,
provider state, and future attachments are all blobs.** The index holds
references and sizes.

This is what makes "attach everything to the thread" safe: an attachment
is a blob reference in the index. Attaching a 2 GB file costs the index
~100 bytes, and opening the thread never touches the file.

### 3.4 Attachments — why this design, and not a smaller one

"Attach everything to the thread" is the requirement that rules out the
cheaper answers. A lazy-field reader would still be O(document) to open;
a compressed blob would still be O(document) to rewrite each turn. Once
the index and the content store are separate, an attachment is:

```cpp
struct AttachmentIndex {
    AttachmentId id;
    std::string  name;        // "build.log", "design.pdf"
    std::string  media_type;
    std::size_t  byte_len;
    BlobRef      data;        // content-addressed; deduped for free
    std::optional<BlobRef> thumbnail;   // pre-rendered, for the card
    MessageId    anchor;      // which message it hangs off, if any
};
```

That is ~100 bytes in the index no matter how large the payload is. The
properties fall out of the structure rather than needing new machinery:

- **Opening never touches the payload.** The card renders from `name`,
  `media_type`, `byte_len` and the optional thumbnail.
- **Attaching the same file twice costs one copy** — the blob name is its
  content hash, which is already true of images today.
- **Nothing has to stream.** The one place bytes are needed (the wire, an
  export, an external open) already goes through `blob()`, which is async
  by contract.
- **A 2 GB attachment cannot slow the UI down**, because no render path
  is allowed to ask for it.

This is the actual argument for the whole design: it is not primarily a
speed change, it is the change that makes unbounded attached content
*possible* without making thread switching a function of how much you
have attached.

---

## 4. The subsystem — one owner, one entry point

Today, thread bytes are touched in `persistence.cpp`, and the shape of a
`Message` is assumed in ~19 files. This design puts **one type** between
the app and storage:

```cpp
class ThreadStore {                        // io/thread_store/store.hpp
public:
    // Open: parses ONLY the index. Bounded, ~ms.
    [[nodiscard]] std::expected<ThreadHandle, StoreError>
    open(const ThreadId&);

    // Render path: index only, never blocks, never does I/O.
    [[nodiscard]] std::span<const MessageIndex> index() const noexcept;

    // Body access. Returns immediately:
    //   Ready(msg)  — resident
    //   Pending     — a load was scheduled; a ThreadBodyReady msg follows
    [[nodiscard]] BodyResult body(MessageId) const;

    // Blob access, same contract.
    [[nodiscard]] BlobResult blob(const BlobRef&) const;

    // The wire needs whole messages. Explicitly async, explicitly awaited
    // — this is the one place a caller may wait, and it is off the UI
    // thread by construction.
    [[nodiscard]] std::future<std::vector<Message>>
    materialise(std::span<const MessageId>);

    void append(const Message&);           // O(1): index + segment append
};
```

**The invariant that keeps this honest:** the render path may call
`index()` and nothing else. `body()` and `blob()` return a *status*, never
block. Any code that needs bytes now must say so, and gets them next
frame. That is enforceable in review and testable directly.

### 4.1 Async, without a new threading model

`ThreadStore` schedules loads on the existing persistence worker (the one
behind `save_thread`'s `async_writer`) and reports completion as a normal
`Msg` through the existing Elm loop:

```
ThreadBodyReady { ThreadId, MessageId }
ThreadBlobReady { ThreadId, BlobRef }
```

The reducer drops the matching view-cache entry; the next frame renders
with real content. **No new concurrency primitives, no locks on the UI
thread, no re-entrancy** — which is exactly how the crash we just fixed
happened, and worth not repeating.

---

## 5. Correctness — the part that matters more than the speed

Every one of these is a test, not a hope.

1. **Round-trip identity.** For all 539 real threads: `open → materialise
   → serialise` is byte-identical to `load → serialise` under today's
   code. This is the master test; it makes the migration provably
   lossless rather than plausibly lossless.

2. **Render equivalence.** For every thread and several widths, the frame
   rendered from the index+async path is byte-identical to the frame
   rendered from a fully-materialised thread, once quiescent. Guarantees
   "fast" never silently means "different."

3. **No blocking on the render path.** A debug build asserts if `body()`
   or `blob()` performs I/O on the UI thread. Cheap, and it catches the
   regression class permanently.

4. **The write hazard.** Saving a thread whose bodies were never
   materialised must not lose them. This already bit us once in
   `3d976a39` — an unmaterialised image would have serialised as empty
   bytes and destroyed every image on the next autosave. The store
   re-persists unloaded segments **by reference**; a test asserts a
   load→save cycle with zero materialisation is a no-op on disk.

5. **Crash safety.** Index and segments are written atomically
   (temp + rename, as today). A torn write must leave the previous
   consistent state, and a missing/corrupt index must be rebuildable from
   segments — the index is a cache, the segments are truth.

6. **Concurrency.** Two agentty instances on the same thread must not
   corrupt it. Segments are append-only and blobs are immutable, which
   makes this mostly structural; the index write takes the existing
   cross-process lock.

---

## 6. Migration — the risk, and how it stays boring

596 MB of real user data. The plan is strictly additive:

- **Phase 0.** `ThreadStore` reads today's format and presents the index
  API by parsing the whole file. No format change; no user-visible
  change. Everything downstream is written against the new API and
  tested against the old bytes. *This de-risks all later phases.*
- **Phase 1.** Writer emits the new layout. Reader accepts both, forever.
  Conversion happens on save, one thread at a time, as they are opened —
  same shape as the image migration already shipped.
- **Phase 2.** Render path moves to `index()`; bodies go async.
- **Phase 3.** Segment reads become genuinely lazy.

Old threads keep working at every phase. No big-bang rewrite, no
migration script the user has to trust, and every phase is independently
revertible.

---

## 7. Expected result

| | today | designed |
|---|---:|---:|
| bytes parsed to open the 29 MB thread | 29 MB | ~6 MB (measured) |
| open (optimized, warm) | 233 ms | ~50 ms (projected) |
| per-turn autosave | rewrite whole file | append |
| attach a 2 GB file | impossible | ~100 B in the index |
| tool cards needing an async body | — | ~5% |
| render | 4 ms | 4 ms |

Render stays 4 ms because it was never the problem.

The open figure is a projection, not a measurement: ~6 MB is measured,
and the ~50 ms assumes open scales with bytes parsed (today's 29 MB costs
~99 ms of io+parse, plus 134 ms of `parse_thread` walking every field).
Phase 0 makes it measurable for real before any format changes — and if
it does not hold, that is the moment to revisit the cap, not after the
format has shipped.

---

## 8. Open questions

1. **Segment size.** 256 messages is a guess. Should be measured against
   real scroll patterns before it is fixed.
2. **Index format.** JSON keeps it debuggable and consistent with the
   rest of the app; a binary format would parse faster but costs
   inspectability. Recommend JSON until measured to matter.
3. **Blob GC.** Deleting a thread should release blobs no other thread
   references. Needs a refcount or a mark-sweep; deferrable, but should
   not be forgotten — it is how the store slowly leaks.
4. **`text` inline cap.** Prose is only 6.3% of bytes; inlining all of it
   is probably right and keeps the common path allocation-free. Needs a
   distribution check on outliers.
5. **Skeleton rows.** ~5% of tool cards will need a body on first paint.
   Either they draw from `preview` with a "…" affordance until the body
   lands, or they show a skeleton row. This is the ONE place "async body"
   is visible to the user, so it deserves a deliberate answer rather than
   whatever falls out of the implementation.
