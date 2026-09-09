# Thread Store — an append-only log with a byte-offset index

**Status:** proposal. Nothing here is implemented yet.
**Supersedes:** the ad-hoc laziness in `ImageContent` (commit `3d976a39`),
which this design keeps and generalises to attachments.

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
230 ms went. The cost is *parsing 29 MB to draw 60 messages*.

The field table above is the reason the file is 29 MB in the first place,
and it is why the first draft of this document tried to separate "hot"
from "cold" fields. That turned out to be the wrong conclusion — §3.1 has
the measurement. The right axis is *recent vs old*, not *hot vs cold*.

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

The corollary that shapes everything below: if opening is O(screen), then
it is already fast enough to be **synchronous**, and none of the
asynchronous machinery an O(thread) design would need has to exist.

---

## 3. Design: one line per message, plus a byte-offset index

The whole design is two files per thread and one idea:

> **A thread is an append-only log of messages. An index of byte offsets
> makes any message seekable. Neither file is ever rewritten.**

```
~/.agentty/threads/
  <id>.jsonl      one message per line, append-only, never rewritten
  <id>.ofs        one 8-byte offset per message — 20 KB for 2519 messages
  blobs/<hash>    images + attachments (exists today, unchanged)
```

That is it. **No database, no new dependency**, no segments, no container
format, no CRC, no schema migration. The `.jsonl` line is exactly the
per-message JSON `message_to_json()` already produces.

> **Constraint, not a preference: this subsystem adds no third-party
> dependency.** It is built from `<fstream>`, `<filesystem>` and the
> nlohmann codec already in the tree. SQLite was measured (§3.1) and
> rejected — same speed, real cost. If a future change to this design
> starts by adding a library, that is the signal it has gone wrong.

### 3.1 Why this and not the alternatives

I prototyped the alternatives against the real 29 MB / 2519-message
thread rather than reasoning about them. Measured (Python — a pessimistic
floor; the C++ path is faster):

| approach | file | full load | switch (tail 60) |
|---|---:|---:|---:|
| today: one JSON document | 29.2 MB | 226 ms | 226 ms |
| **JSONL + offset index** | 29.2 MB | 49 ms | **1.4 ms** |
| JSONL + zlib per line | 17.7 MB | 173 ms | — |
| whole-file gzip | 17.0 MB | 173 ms | — |
| **JSONL + payloads >4 KB in blobs** | **6.3 MB** | **20 ms** | **0.6 ms** |

Four conclusions, three of which killed an idea:

**Compression is a trap.** It halves the file and *triples* the load
(173 ms vs 49 ms). We are not short of disk; we are short of
milliseconds. It also destroys `grep`-ability and random access — you
cannot seek into a gzip stream. Rejected.

**Externalising big payloads dominates everything else.** Moving the 576
payloads over 4 KB into the blob store takes the file from 29.2 MB to
**6.3 MB** and the full load from 49 ms to **20 ms** — better than
compression on both axes at once, because those 22.2 MB are never parsed
rather than merely stored smaller.

**The blob threshold has a floor.** Lowering it below 4 KB buys little
and costs many files: 4 KB → 576 blobs / 2.6 MB inline; 1 KB → 1533 blobs
for 0.6 MB inline. Below ~4 KB an inode, an open and a read cost more
than they save. The existing `kOutputBlobMin` is 8 KB, which is in the
right region; 4 KB is a marginal improvement worth measuring in C++
before changing.

**Most of the win is already implemented and simply not applied.** The
current code already externalises tool output ≥ 8 KB and images — but
only when a thread is *written*. Across the 539 real threads: **14 are
fully blob-backed, 232 still carry 255 MB of legacy inline payload**,
re-parsed on every single open, because they have not been rewritten
since the blob store landed.

So the single highest-value change is not a new format at all. It is
**rewriting old threads into the format we already have** — which the log
migration (§6) does anyway, for free, the first time each thread is
opened.

#### Also rejected

**A hot/cold split file.** An earlier draft proposed extracting the
render-relevant fields into a separate index file. Built, it is 6 MB and
takes **29 ms** to parse — twenty times slower than seeking into the raw
log. A switch renders ~60 messages, so *any* design that reads a
whole-thread structure has already lost to one that reads 60 messages.
Hot-vs-cold is the wrong axis; recent-vs-old is the right one, and a byte
offset does that for 8 bytes per message.

**SQLite.** Measured **1.3 ms** against JSONL's 1.4 ms — within noise. It
would add transactions and SQL, neither of which this problem has: no
joins, no query beyond "messages N..M", one writer per thread. Paying a
250 KB C dependency and losing `grep`/`jq` for 0.1 ms is a bad trade.
(Zed uses SQLite, but for workspace state with genuinely relational
shape. Claude Code and Codex both store sessions as JSONL.)

**No index at all** (scan the last 1 MB backwards for newlines). Works,
but 4.5 ms vs 1.4 ms and fiddlier to get right at the boundary. The
offset file is 20 KB and rebuilds in 12 ms; it earns its place.

### 3.2 What each file does

**`<id>.jsonl` — the log.** One message per line, appended, never
rewritten. This alone fixes the second-worst property of today's format:
every turn currently rewrites the entire 29 MB file. Appending one line
is **0.01 ms**, and it is constant in thread size.

**`<id>.ofs` — the index.** A flat array of little-endian `uint64` file
offsets, one per message. Message `i` starts at byte `ofs[i]`. Rendering
the last 60 messages is `seek(ofs[n-60])` and parse to EOF.

It is deliberately the dumbest possible index:

- **Fixed-width, so no parsing.** `ofs[i]` is `read(8 bytes at i*8)`.
- **Append-only, like the log.** A new turn appends 8 bytes.
- **Derivable.** It is a *cache*, not truth. Rebuilding it is one pass
  counting newlines: measured **12 ms** for the 29 MB thread. So a
  missing, stale, or corrupt `.ofs` is never fatal — the store rebuilds
  it and continues. That single property removes most of the failure
  modes a bespoke format would have needed to handle.
- **Self-validating.** `size(.ofs)/8` must equal the line count implied
  by the log, and `ofs[last]` must be < `size(.jsonl)`. Cheap to check on
  open; on mismatch, rebuild.

### 3.3 Crash safety comes free

Append-only + line-delimited is why this needs no WAL and no CRC:

- A torn append leaves a **partial last line**. Verified by truncating
  mid-line: 2538 lines parse, 1 fails — the partial one. The reader drops
  a trailing unparseable line, which is exactly the correct recovery.
  Nothing earlier in the file can be damaged, because nothing earlier is
  ever written again.
- The `.ofs` file can only ever be *behind* the log (log is fsynced
  first). Extra log lines with no offsets are detected on open and the
  tail of the index is rebuilt.
- Ordering: append log → fsync → append offset. A crash between them
  costs a rebuild, not data.

Compare with today, where every turn rewrites the whole file: a crash
mid-write risks the entire thread, which is why `write_json_atomic` has
to do temp+rename of 29 MB.

### 3.4 Attachments — any file type, not just images

**Arbitrary files already attach today.** `Attachment::Kind` covers
`Paste`, `FileRef` (`@path`), `Symbol`, `Output` (captured command
output) and `Image`, and non-image kinds are persisted on the message.
So this is not a new feature to invent — it is an existing feature with a
storage problem.

The problem is that images and everything else took different paths:

| | images | every other kind |
|---|---|---|
| bytes live in | `blobs/<hash>` (since `3d976a39`) | **base64 inline in the thread JSON** |
| deduped | yes (content-addressed) | no |
| lazy | yes | no — decoded on every load |
| reaches the model as | a real image block | text spliced into the prompt |

Measured across the 539 real threads: 407 pastes (1.0 MB) and 26 captured
outputs (**4.2 MB**), all base64-inline. The largest single attachment is
a **2.0 MB build log** sitting in the middle of a thread file, re-decoded
on every open. That is precisely the cost this design exists to remove,
and it is already being paid — just by `Output` and `Paste` rather than
by images.

So the design does not need a new attachment concept. It needs the
**existing** one to use the same blob path images already use:

```json
{"id":"m47","role":"user","text":"look at \u0001ATT:0\u0001",
 "attachments":[
   {"kind":"output","name":"cargo build","media_type":"text/plain",
    "byte_count":2097152,"line_count":18422,"blob":"91cc…"}]}
```

The only change to the on-disk shape is `"body": "<base64>"` becoming
`"blob": "<hash>"`, with `body` still accepted for old threads — exactly
the migration images already went through. Everything else (the
placeholder protocol, `attachment::expand()`, the chip caption) is
unchanged.

**What this buys, per kind:**

- **Any file type works**, because a blob is bytes plus a `media_type`.
  A PDF, a video, a 2 GB core dump — the log line is ~100 bytes either
  way. Nothing in the store inspects the content.
- **Lazy by the same mechanism as images.** `Attachment::body` becomes a
  lazy handle like `ImageContent`: the chip renders from `name`,
  `media_type`, `line_count` and `byte_count`, all of which are already
  stored and are all the renderer ever reads. Bytes materialise only when
  `attachment::expand()` runs at request-build time.
- **Dedup for free.** The blob name is the content hash, so pasting the
  same log twice, or forking a thread, costs one copy.

**What is deliberately NOT solved here.** Whether the model can *use* a
given file type is a wire question, not a storage question, and the two
should not be conflated:

- Text-ish kinds (`Paste`, `FileRef`, `Symbol`, `Output`) are spliced
  into the prompt as text by `attachment::expand()`. That works for
  anything UTF-8 and is what ships today.
- Images go out as native image blocks, per dialect.
- A PDF or a video can be **stored and attached** by this design, but
  sending it to a model needs either a provider that accepts that
  content type (e.g. Anthropic's document blocks) or a local extraction
  step. That is a separate piece of work, and the storage layer is
  deliberately agnostic to it — it holds bytes and a media type, and lets
  the wire layer decide what it can do with them.

The useful consequence: attaching a 2 GB file cannot slow a thread switch
down, because switching reads log lines and no render path opens a blob.

### 3.5 What stays lazy

`ImageContent`'s lazy materialisation (shipped in `3d976a39`) stays as
is, and `Attachment` gets the same treatment: the log line carries the
reference plus the metadata the chip renders from (`name`, `media_type`,
`line_count`, `byte_count` — all already stored), and bytes are fetched
only when `attachment::expand()` builds a request or the user explicitly
opens the file.

One shared mechanism, two callers. `ImageContent::Source` already models
"a blob name or legacy inline base64, resolved on first use"; attachments
need exactly that, so the sensible move is to lift it into a small shared
`LazyBytes` type rather than write it twice.

Tool output does **not** need a separate mechanism. It lives in the
message line, and the line is only parsed when that message is loaded.
p99 tool output is 8.3 KB, so even a whole-thread load is dominated by
line count, not by any single output.

---

## 4. Expected result

| | today | §3 alone | + windowed read |
|---|---:|---:|---:|
| thread switch (29 MB thread) | 226 ms | **58 ms** | **1.4 ms** |
| per-turn save | rewrite 29 MB | **0.01 ms** append | same |
| index size | — | 20 KB | 20 KB |
| index rebuild if lost | — | 12 ms | 12 ms |
| attach a 2 GB file | impossible | ~100 B in the log | same |
| new dependencies | — | none | none |
| still `grep`-able / `jq`-able | yes | yes (better: per-line) | same |
| view files touched | — | **zero** | many (see §9.0) |

Every number is measured on your largest real thread, not projected.
Render stays ~4 ms and was never the problem.

The two columns matter: **§3 alone is a self-contained change** that makes
switching 4× faster and saving O(1), without touching a single view file.
The 1.4 ms column additionally requires unpicking the assumption that a
thread is fully resident in memory — 500+ call sites across 46 files.
§9.0 is honest about that being a separate project.

---

## 5. Correctness

Each of these is a test, not a hope.

1. **Round-trip identity.** For all 539 real threads: converting to
   `.jsonl` and reading back produces a `Thread` byte-identical to
   today's loader. This is the master test — it makes the migration
   provably lossless.

2. **Render equivalence.** For every thread and several widths, the frame
   rendered from the log matches the frame rendered from today's path.
   "Fast" must never quietly mean "different".

3. **Index is a pure cache.** Delete `.ofs`, and every thread still opens
   with identical results (just slower for one open). Corrupt it with
   garbage, and the store detects and rebuilds. Property-tested by
   mutating the index and asserting the loaded `Thread` is unchanged.

4. **Torn-write recovery.** Truncate a log mid-line at many offsets; the
   reader must return every complete message and drop exactly the partial
   tail. Already verified by hand (2538 good, 1 dropped).

5. **Append durability.** After `append()` returns, the message survives
   `kill -9`. Ordering (log fsync before offset append) is asserted.

6. **No whole-file rewrite.** A test asserts that appending a turn to a
   100-message thread writes O(1) bytes, not O(thread) — this is the
   property that silently regresses if someone later "simplifies" the
   writer.

---

## 6. Migration

Strictly additive, and reversible at every step:

- **Phase 0.** Reader accepts both formats. `<id>.json` still loads
  exactly as today. Nothing is written differently.
- **Phase 1.** On save, a thread is written as `<id>.jsonl` + `<id>.ofs`,
  and the old `<id>.json` is kept until the new pair reads back
  identically, then removed. One thread at a time, as they are opened —
  the same shape as the image-blob migration already shipped.
- **Phase 2.** The view switches to seeking the tail rather than loading
  the whole thread.
- **Phase 3.** Attachments.

At every phase old threads keep working, and any phase can be reverted
without touching user data.

---

## 7. The subsystem — one owner

Today thread bytes are touched in `persistence.cpp` (86 KB, ten concerns)
and the shape of a `Message` is assumed in ~19 files. This puts one small
type between the app and the disk:

```cpp
// include/agentty/store/thread_log.hpp
class ThreadLog {
public:
    // Opens the log: reads .ofs (20 KB), validates it against the log's
    // size, rebuilds it if stale/missing. Does NOT read the log body.
    [[nodiscard]] static std::expected<ThreadLog, StoreError>
    open(const ThreadId&);

    [[nodiscard]] std::size_t size() const noexcept;   // message count

    // Parse messages [from, to). This is the ONLY read path.
    //
    // Phase 1 uses range(0, size()) exclusively — the app still wants a
    // whole resident Thread (see §9.0), and even that is 4x faster than
    // today because 2519 small parses beat one 29 MB parse.
    //
    // The windowed call, range(size()-60, size()), is what reaches
    // 1.4 ms. The API is shaped for it from day one so the storage layer
    // never has to change again — but nothing calls it until the view
    // stops requiring residency, which is its own project.
    [[nodiscard]] std::vector<Message> range(std::size_t from,
                                             std::size_t to) const;

    // Append one message: one line + one 8-byte offset. O(1), 0.01 ms.
    void append(const Message&);

    // Rewrite the whole log. Needed only by edit/fork/compaction, which
    // genuinely change history — NOT by the per-turn save path.
    void rewrite(std::span<const Message>);
};
```

Four methods. No async machinery, no completion messages, no cache
invalidation, no re-entrancy — because at 1.4 ms (windowed) or 58 ms
(whole thread) there is nothing worth hiding behind a background thread.
That is the main practical benefit of the measurements in §3.1: they
delete an entire subsystem the earlier draft needed.

`range()` is synchronous on purpose. A background loader would add a
threading model, a `Msg` round-trip, and a "content not here yet" state
to every render path — to save tens of milliseconds on an operation the
user explicitly initiated. The previous draft proposed exactly that; the
measurement says don't.

### 7.1 How it plugs into the app

The seam already exists: `Deps` holds `std::function` store callbacks and
`store::Store` is a concept with `FsStore` as its model.

```cpp
// include/agentty/runtime/app/deps.hpp — today, unchanged
std::function<std::optional<Thread>(const ThreadId&)> load_thread;
```

Phases 0–1 change nothing here: `FsStore::load_thread` keeps returning a
whole `Thread`, now assembled from the log via `range(0, size())`. The
windowed dep below is what a future residency project would add — listed
so the API shape is already right, not because phase 1 needs it:

```cpp
// "give me messages [from,to)" — NOT used in phase 1; see §9.0
std::function<std::vector<Message>(const ThreadId&, std::size_t, std::size_t)>
    load_range;
```

---

## 8. Code layout

House convention is `include/agentty/<subsys>/` mirroring `src/<subsys>/`,
one concern per file, a subdirectory once it outgrows a few
(`provider/anthropic/`, `runtime/view/thread/turn/`).

### 8.1 What exists today

```
include/agentty/io/persistence.hpp    5 KB
src/io/persistence.cpp               86 KB   ten concerns in one file
```

Atomic writes, path resolution, the blob store, transcript-markdown
export, message↔JSON, typed deserializers, the SAX metadata parser, the
`index.json` sidecar, the async writer, settings. This design does not
get to add an eleventh — but it is also small enough not to need much.

### 8.2 Target layout

```
include/agentty/store/
  store.hpp          (exists) the Store concept — UNCHANGED
  thread_log.hpp     NEW  ThreadLog: open/size/range/append/rewrite

include/agentty/io/
  persistence.hpp    (exists) free functions + FsStore — UNCHANGED
  blob_store.hpp     NEW  put/get/exists over blobs/<hash>

src/io/
  thread_log.cpp     NEW  the log + offset index (~300 lines)
  blob_store.cpp     NEW  extracted from persistence.cpp (~70 lines)
  persistence.cpp    SHRINKS — blob code moves out; message↔JSON is
                     REUSED by thread_log.cpp, not duplicated
```

Two new files. The earlier draft proposed six plus a new subdirectory;
with no segments, no container format and no async loader, there is
nothing for them to hold.

Notes:

- **`message_to_json` / `parse_message` are reused verbatim.** A log line
  is exactly the per-message JSON persistence already emits, so the
  format is not new code and existing round-trip tests still cover it.
- **`blob_store.cpp` comes out first** as a pure refactor: self-contained,
  ~70 lines, already content-addressed, needed by both paths.
- **No `legacy.cpp`.** The old reader is `persistence.cpp`'s existing
  `load_thread_file`, kept as-is for `<id>.json`.

### 8.3 On-disk layout

```
~/.agentty/threads/
  index.json              (exists) picker metadata: id → title, mtime
  <id>.json               (exists) legacy whole-thread doc, still read
  <id>.jsonl              NEW  the message log
  <id>.ofs                NEW  byte offsets, 8 bytes/message
  <id>.transcript.md      (exists) human-readable export
  blobs/<fnv1a>           (exists) images; + attachments
```

`threads/index.json` (which threads exist) is unrelated to `<id>.ofs`
(where messages are inside one thread). Both are caches that self-heal by
falling back to a full parse — `.ofs` inherits that contract deliberately,
because `index.json` already proved it works here.

### 8.4 Tests

```
tests/thread_log_roundtrip_test.cpp   convert→read ≡ today's loader,
                                      over all 539 real threads
tests/thread_log_index_test.cpp       .ofs is a pure cache: delete it,
                                      corrupt it, truncate it — same result
tests/thread_log_append_test.cpp      append is O(1) bytes; survives kill -9
tests/thread_log_torn_test.cpp        truncate mid-line at many offsets;
                                      every complete message survives
```

`real_thread_render_probe` gains a `--log` flag that renders both paths
and diffs the frames — correctness property #2, run against real threads
rather than fixtures.

### 8.5 Commit sequence

Each lands green and revertible:

1. `blob_store` extracted from `persistence.cpp` — pure refactor.
2. `thread_log.cpp` + round-trip test over all 539 threads. Nothing uses
   it yet.
3. `FsStore` reads `.jsonl` when present, `.json` otherwise.
4. Save writes the log; keep `.json` until the pair reads back identical.
5. Attachment bodies move to blobs (`body` → `blob`), reusing the lazy
   mechanism images already have. This is where "any file type" lands,
   and it removes the 4.2 MB of base64-inline captured output currently
   sitting in real threads.

Steps 1–2 change no on-disk bytes and no behaviour. Steps 1–5 touch no
view code and deliver the 58 ms switch + O(1) save.

The windowed read — which takes 58 ms to 1.4 ms — is deliberately **not**
on this list. It is a separate project with its own design doc; see
§9.0.

---

## 9. What's next, measured

Commits 1–4 shipped. This section is the profile of what is left, taken
after the log landed rather than before — the earlier numbers in this
document are pre-migration and no longer describe the system.

### 9.1 Where the remaining time goes

The 29 MB thread, migrated (7.5 MB log), RelWithDebInfo:

```
load   36 ms      ← all of it is parsing 2519 messages
render  6 ms      ← bounded by rehydrate_frozen, already constant
```

Breaking the load down further:

| stage | cost |
|---|---:|
| read the file | 8 ms |
| split into lines | 15 ms |
| **JSON-parse every message** | **30 ms** |

And what is still inline in those 7.5 MB:

| field | MB |
|---|---:|
| `tool_calls` | 6.34 |
| `thinking_signature` | 0.75 |
| `text` | 0.67 |
| everything else | 0.11 |

### 9.2 Two candidate directions, both measured

**A. Lower the blob threshold.** `kOutputBlobMin` is 8 KB, and the
remaining inline tool output is 3.37 MB across 2164 calls (p50 930 B,
p99 7.3 KB). Pushing more of it into blobs:

| threshold | log size | parse | new blob files |
|---|---:|---:|---:|
| 8 KB (today) | 7.97 MB | 28.7 ms | — |
| 4 KB | 6.49 MB | 24.8 ms | +232 |
| 2 KB | 5.15 MB | 20.1 ms | +588 |
| 1 KB | 4.32 MB | 19.1 ms | +1035 |

**Verdict: not worth it.** 28.7 → 19.1 ms costs 1035 extra files *per
thread* — tens of thousands across the corpus, each an inode and an open
when the wire needs it. It also inverts the blob store's own rule of
thumb (SQLite's measurement: payloads under ~100 KB belong inline). A
modest 8 KB → 4 KB move is defensible (+232 files for 4 ms) but it is a
rounding error, not a design.

**B. Parse only what is displayed.** The same log, parsing a suffix:

| messages parsed | time |
|---:|---:|
| 60 (one switch's worth) | **0.61 ms** |
| 200 | 1.9 ms |
| 500 | 5.2 ms |
| 2519 (today) | 29.0 ms |

**47×**, and it is the only direction with real headroom. Everything
else is trimming a constant.

### 9.3 The obstacle, and why it is smaller than it looks

The app holds `m.d.current.messages` as a resident `std::vector<Message>`
and reads it in 500+ places. But the reason is narrower than that number
suggests: **the WIRE needs every message** (`wire_messages_for_impl`
walks the whole transcript to build a request), and the view is simply
helping itself to the same vector.

So the split is not "500 call sites" — it is two consumers with genuinely
different needs:

| consumer | needs | frequency |
|---|---|---|
| view | ~60 messages | every switch, every frame |
| wire | all of them | once per turn you send |

And the wire already does its work **on a background thread**:
`launch_stream` copies the Thread into a worker ("the Thread copy is the
one unavoidable cost") and builds the request there. That is the part
that makes this tractable — the expensive read is already off the UI
thread, so it can become an expensive *load* without anyone waiting.

### 9.4 The shape that follows

Not "make the view async". The measurement says the opposite: at 0.61 ms
for a screenful, the view should stay **synchronous** and simply ask for
less.

```
switch:  meta() + range(size()-N, size())      0.6 ms, on the UI thread
send:    range(0, size()) on the worker        30 ms, nobody waiting
scroll:  range(from-N, from)                   sub-ms, on demand
```

The storage layer already supports all three — `ThreadLog::range` was
shaped for it in commit 2. What is missing is a `Thread` that can hold a
window instead of everything, and that is where the care goes: the
tempting version (make `messages` lazily materialise behind `operator[]`)
hides I/O behind innocuous indexing, which is precisely the class of bug
that produced the crash in `1f7cdc3`. The honest version is an explicit
window the view asks for and the wire bypasses.

That is a separate project with its own design pass. The numbers that
justify it: **36 ms → ~7 ms** on a switch (0.6 ms parse + the io/split
floor), against render's 6 ms — at which point the two are balanced and
there is nothing left worth chasing.

### 9.5 Smaller items

1. **`kOutputBlobMin` 8 KB → 4 KB.** +232 files, −4 ms. Marginal; do it
   only if something else touches that path anyway.
2. **Compaction / edit / fork call `rewrite()`** — O(thread), but rare
   and user-initiated. Confirm no *frequent* path does.
3. **Blob GC.** Deleting a thread should release blobs nothing else
   references. Deferrable, but it is how the store slowly leaks.
4. **`.ofs` endianness** is fixed little-endian so a threads directory
   stays portable; worth a test if a big-endian target ever appears.
5. **Window size.** "60 messages" is a stand-in for two screens — derive
   it from terminal height and scroll position rather than fixing it.

### 9.6 The residency obstacle, in full

The design above quietly assumes the app can render from a *window* of
messages. It cannot, today. Two hard facts, both checked:

- **`m.d.current.messages` is read in 500+ places across 46 files** as a
  resident `std::vector<Message>`. Indexing, `erase`, `remove_if`,
  reverse iteration, `.back()`, range-for — all of it assumes the whole
  thread is in RAM.
- **Every turn already needs the whole thread anyway.**
  `launch_stream()` does `Thread thread_snapshot = m.d.current;` and
  hands it to `wire_messages_for_impl()`, which walks all messages to
  build the request. The comment there even calls the copy "the one
  unavoidable cost".

So the honest position is:

> The **storage** change (§3) is ready to implement and delivers the
> per-turn append win on its own. The **windowed read** (§5 of the commit
> sequence) is a separate, much larger project, and the 1.4 ms figure is
> only realised once it lands.

That is not an argument against the design — the log is a prerequisite
for windowing either way, and it stands on its own merits (append instead
of rewriting 29 MB per turn, crash safety, attachments). It is an
argument against pretending phase 5 is a step rather than a project.

What the intermediate state actually buys, with no view changes at all
— measured, not estimated:

| | today | after §3 only | + windowed read |
|---|---:|---:|---:|
| thread switch (29 MB) | 226 ms | **58 ms** | **1.4 ms** |
| per-turn save | rewrite 29 MB | 0.01 ms append | same |
| crash mid-save | risks whole thread | loses partial last line | same |
| attachments | impossible | ✓ | ✓ |

The 58 ms is the pleasant surprise: parsing 2519 small documents beats
parsing one 29 MB document by **4×**, even loading every message. Line
delimiters let the parser start fresh per message instead of maintaining
one deep nesting context across 29 MB, and each line's allocations are
small and short-lived.

So §3 alone is worth doing on its own merits: a 4× faster switch, an
O(1) per-turn save, real crash safety, and attachments — without touching
a single view file. The remaining 58 → 1.4 ms needs the residency work
below.

Three candidate routes, in increasing order of honesty and cost:

1. **Wire-only residency.** Keep `messages` resident for the wire, render
   from a window. Saves nothing on switch, since loading still parses
   everything.
2. **Lazy `Thread`.** Make `messages` a type that materialises on
   demand behind the existing `operator[]` / iterator surface. Touches
   few call sites, but hides I/O behind innocuous-looking indexing —
   exactly the class of bug that caused the crash in `1f7cdc3`.
3. **Explicit windows.** The view asks for `range(from,to)`; the wire
   asks for everything. Most invasive, and the only one that is honest
   about where I/O happens.

(3) is right, and it should be its own design document with its own
measurements. It is not a bullet in a commit list. §9.4 sketches the
shape; §9.5 holds the smaller items.
