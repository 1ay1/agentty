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

That is it. No database, no dependency, no segments, no container format,
no CRC, no schema migration. The `.jsonl` line is exactly the per-message
JSON `message_to_json()` already produces.

### 3.1 Why this and not the alternatives

I prototyped the alternatives against the real 29 MB / 2519-message
thread rather than reasoning about them. Measured (Python — a pessimistic
floor; the C++ path is faster):

| approach | thread switch | notes |
|---|---:|---|
| today: one JSON document | 233 ms | parses 29 MB to draw 60 messages |
| **JSONL + offset index** | **1.4 ms** | seek to `ofs[-60]`, parse 60 lines |
| JSONL, no index (scan last 1 MB) | 4.5 ms | works, but 3× slower and fiddly |
| hot/cold split file | 29 ms | *worse* — see below |
| SQLite | 1.3 ms | same speed, new dependency |

Two of those results changed the design:

**The hot/cold split lost.** My §2 draft proposed extracting the
render-relevant fields into a separate index file. Built, it is 6 MB and
takes **29 ms** to parse — twenty times slower than seeking into the raw
log. The reason is obvious in hindsight: a thread switch renders ~60
messages, so *any* design that reads a whole-thread structure has already
lost to one that reads only 60 messages. Separating hot from cold fields
optimises the wrong axis. Separating *recent* from *old* is the axis that
matters, and a byte offset does that for 8 bytes per message.

**SQLite is exactly as fast and costs a dependency.** It measured 1.3 ms
against JSONL's 1.4 ms — within noise. It would also give us transactions
and SQL, neither of which this problem needs: there are no joins, no
queries beyond "give me messages N..M", and one writer per thread. Paying
a 250 KB C dependency and losing `grep`-ability for 0.1 ms is a bad
trade. (Worth noting Claude Code and Codex both store sessions as JSONL;
Zed uses SQLite, but for workspace state with genuinely relational shape.)

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

### 3.4 Attachments

Unchanged from the existing blob store, which already works: payloads
live in `threads/blobs/<hash>`, content-addressed and deduped. A message
line references them by name.

```json
{"id":"m47","role":"user","text":"look at this",
 "images":[{"media_type":"image/png","blob":"a3f2…","w":1600,"h":900}],
 "attachments":[{"name":"build.log","media_type":"text/plain",
                 "byte_len":2147483648,"blob":"91cc…"}]}
```

An attachment costs ~100 bytes in the log regardless of payload size, and
no render path opens the blob. Attaching a 2 GB file cannot slow a thread
switch, because switching only reads 60 lines of `.jsonl`.

Images already work this way as of `3d976a39`; attachments are the same
mechanism with a name and a media type.

### 3.5 What stays lazy

`ImageContent`'s lazy materialisation (shipped in `3d976a39`) stays as
is, and generalises to attachments: the log line carries the reference
and the metadata, and bytes are fetched only when the wire or an explicit
open needs them.

Tool output does **not** need a separate mechanism. It lives in the
message line, and the line is only parsed when that message is on screen
— which is the whole point of seeking. p99 tool output is 8.3 KB, so 60
messages of tail is a few hundred KB of parse.

---

## 4. Expected result

| | today | designed |
|---|---:|---:|
| thread switch (29 MB thread) | 233 ms | **1.4 ms** (measured) |
| per-turn save | rewrite 29 MB | **0.01 ms** append |
| index size | — | 20 KB |
| index rebuild if lost | — | 12 ms |
| attach a 2 GB file | impossible | ~100 B in the log |
| new dependencies | — | none |
| still `grep`-able / `jq`-able | yes | yes (better: per-line) |

The 1.4 ms is a real measurement on your largest real thread, not a
projection. Render stays ~4 ms and was never the problem.

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

    // Parse messages [from, to). This is the ONLY read path: the view
    // asks for the visible window, the wire asks for everything.
    // Synchronous and cheap by construction — 60 messages is ~1.4 ms.
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
invalidation, no re-entrancy — because at 1.4 ms there is nothing to hide
behind a background thread. That is the main advantage of the measurement
in §3.1: it deletes an entire subsystem the earlier draft needed.

`range()` is synchronous on purpose. A background loader would add a
threading model, a `Msg` round-trip, and a "content not here yet" state
to every render path, to save a millisecond. The previous draft proposed
exactly that; the measurement says don't.

### 7.1 How it plugs into the app

The seam already exists: `Deps` holds `std::function` store callbacks and
`store::Store` is a concept with `FsStore` as its model.

```cpp
// include/agentty/runtime/app/deps.hpp — today, unchanged
std::function<std::optional<Thread>(const ThreadId&)> load_thread;
```

Phases 0–1 change nothing here — `FsStore::load_thread` keeps returning a
whole `Thread`, now assembled from the log. Phase 2 adds one dep
*alongside* it, so the view can migrate one surface at a time:

```cpp
// "give me messages [from,to)" — the windowed read
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
5. View switches to windowed `range()` reads.
6. Attachments.

Steps 1–2 change no on-disk bytes and no behaviour.

---

## 9. Open questions

1. **Window size.** 60 messages is "two screens" — it should be derived
   from the terminal height and the scroll position rather than fixed.
2. **Scroll-up.** Reading further back is `range(from-60, from)`, but the
   view currently assumes the whole `Thread` is resident. That assumption
   is what phase 5 has to unpick, and it is the only genuinely invasive
   part of this plan.
3. **Compaction / edit / fork.** These rewrite history, so they call
   `rewrite()` — O(thread), but rare and user-initiated. Worth confirming
   no *frequent* path needs it.
4. **Blob GC.** Deleting a thread should release blobs nothing else
   references. Deferrable, but it is how the store slowly leaks.
5. **`.ofs` endianness.** Fixed little-endian, so a thread directory
   stays portable across machines; worth an explicit test on a big-endian
   target if one is ever supported.
