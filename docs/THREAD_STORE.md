# Thread Store — an append-only log with a byte-offset index

**Status: SHIPPED.** Commits `0f11cb4b` → `92cf9bc1`, on `master`.
Every number below is measured on a real 597 MB / 549-thread store, in a
RelWithDebInfo build — never in `./build`, which is Debug `-O0` and
inflates parse costs ~16×.

This document is both the design and the record of how it was arrived at,
including the **six** ideas that were measured and rejected (§6). Those
are kept deliberately: the rejections are most of the value, because each
one is a plausible-sounding optimisation that the data killed — including
the one this project was originally supposed to end with.

---

## 1. What a thread looks like on disk

```
~/.agentty/threads/
  <id>.jsonl        one message per line, appended, never rewritten
  <id>.ofs          one 8-byte LE offset per message  (20 KB for 2519)
  <id>.meta.json    title, timestamps, fork provenance, compactions
  blobs/<hash>      images, tool output, attachments — content-addressed
  index.json        picker cache: which threads exist  (pre-existing)

  <id>.json         LEGACY whole-document form — still read, forever
```

Four properties follow from that shape, and they are the whole design:

| property | mechanism |
|---|---|
| a turn costs O(1) to save | append one line + 8 bytes |
| a torn write costs one message | line-delimited, nothing earlier is rewritten |
| the index can never be wrong-and-believed | it's a cache, validated on open, rebuilt in 12 ms |
| a 2 GB attachment costs ~40 bytes | payload lives in `blobs/`, referenced by hash |

**No new dependency.** `<fstream>`, `<filesystem>`, and the JSON
libraries already in the tree. SQLite was measured and rejected (§6.2).

---

## 2. The problem, as measured before any of this

Across 549 real threads, 597 MB:

| field | MB | share |
|---|---:|---:|
| `tool_calls` | 348.7 | 60.9% |
| `images` | 130.4 | 22.8% |
| `text` | 36.0 | 6.3% |
| `thinking_signature` | 29.1 | 5.1% |
| everything else | ~27 | 4.9% |

Two fields were **84% of all bytes**, and neither is needed to draw a
frame. Meanwhile:

- **Opening a thread parsed the whole document.** A 28 MB thread meant
  28 MB of JSON before a single row could be drawn.
- **Saving a turn rewrote the whole document.** Every turn. On that same
  thread: 28 MB written to append one message.
- **Nothing ever reclaimed a blob.** 283 of 486 (11.2 MB) were
  unreferenced.

---

## 3. What shipped, and what each part bought

### 3.1 The log — `io/thread_log.{hpp,cpp}`

`ThreadLog` is five methods: `open`, `range(from,to)`, `append`,
`rewrite`, and the `meta` pair. Lines are produced and consumed by
`persistence::message_to_json` / `message_from_json` — **the same codec
the legacy format uses**, deliberately, because two codecs would drift
and the drift would surface as silently mangled history.

Measured, same threads, warm cache, RelWithDebInfo:

| thread | legacy load | log load | file |
|---|---:|---:|---:|
| 2519 msgs | 114 ms | **22 ms** | 28 MB → 7.2 MB |
| 3567 msgs | 101 ms | **30 ms** | 22 MB → 7.6 MB |
| 1238 msgs | 50 ms | **17 ms** | 13 MB → 3.9 MB |

Part of that 4–5× is line-delimiting itself: a parser handed 2519 small
documents starts fresh on each, with short-lived allocations, instead of
carrying one deep nesting context across 28 MB.

### 3.2 O(1) append — the property that matters most long-term

Appending a turn writes **one line plus 8 bytes**, regardless of thread
length. Against rewriting 13–28 MB per turn, that is the difference
between a cost that is constant and one that grows without bound.

This was got wrong once, in this very subsystem: `append()` originally
called `write_index_()`, which atomically rewrote *every* offset — 20 KB
per turn at 2519 messages, 400 KB at 50k. `append_offset_()` writes the
one new offset with `O_APPEND` instead.

The test for it took two attempts, and the first was worthless: file
**size** cannot distinguish "rewrote n offsets" from "appended the nth",
since both leave `n*8` bytes. The signal that works is that
`write_json_atomic` publishes by **rename**, so a whole-index write
replaces the file object while an append modifies it in place — observed
with a hard link and `fs::equivalent`. Verified by reintroducing the bug:
the test fails.

### 3.3 simdjson on the read path — `4f3f9fe8`

Parsing dominates a load. simdjson is already a dependency (the Anthropic
SSE reader uses it) and reads the same bytes **7.6× faster** than
nlohmann.

**We deliberately took 3.9×, not 7.6×.** `message_from_json` is 170 lines
of accumulated tolerance for every shape a `Message` has ever had on
disk, across 28 access sites. Porting that to a second JSON API would be
the riskiest change in the project for a pure speed win, and the failure
mode is losing a field nobody notices until history is already saved
without it. So the line is parsed with simdjson and **converted** to
nlohmann, leaving the reader untouched. nlohmann remains the fallback: a
parser disagreement must not make someone's history unreadable.

### 3.4 Lazy payloads — `LazyBytes`, `domain/lazy_bytes.hpp`

Images and attachment bodies are large, opaque, and read by almost
nothing. The renderer never touches either: an image draws from its media
type and dimensions, an attachment from `name` / `byte_count` /
`line_count`. The one real consumer is the wire, when a message is
re-sent.

So both hold a `Source` (a blob name, or legacy base64) and materialise
on first `bytes()`. Measured: image payload dropped from 17 MB to zero on
the load path of one thread; attachment bodies took another real thread
from 3.5 MB → 0.7 MB.

**Making it a type, not a `std::string`, is what caught a real bug.**
`compute_render_key` mixed `a.body.size()` — which on a lazy body would
read a 2 MB blob off disk *to compute a hash*, on every frame that
rebuilds a turn. A plain string would have compiled and silently read
empty. It mixes `byte_count` now: stored metadata, already what the chip
displays. (See `docs/STRONG_TYPES_AUDIT.md` for where else this argument
holds — and the two places it was checked and rejected.)

### 3.5 Blob GC — `io/blob_gc.{hpp,cpp}`

**Mark-and-sweep, not refcounting**, and that is a safety decision rather
than a performance one. Blobs are content-addressed, so the same
screenshot in two threads is *one file* — 9 of 203 live blobs have
multiple referrers. "Delete this thread's blobs when the thread is
deleted" would blank images in threads still open, invisibly, until
someone scrolled back.

A refcount fixes that in principle and is worse in practice: updated
transactionally on every save, rewrite, fork, compaction and delete,
where one missed decrement leaks forever and one spurious decrement
**destroys data**.

Two rules make it safe:

- **Reference detection is structural**, not a key list. `put_or_inline()`
  generates `<field>_blob` dynamically, so `thinking_blob`,
  `signature_blob` and `text_blob` already exist and a new one appears the
  moment someone calls it with a new field. A fixed list would silently
  stop protecting those. So: any key that is `"blob"` or ends in
  `"_blob"`.
- **If any thread file is unreadable, the sweep deletes nothing.**
  Unknown references cannot be assumed absent, and a corrupt thread file
  is precisely when payloads matter most.

Dry run by default. On the real store: 283 orphans / 11.2 MB, and after
reclaiming, all 542 threads still round-tripped verbatim.

---

## 4. Migration — how 597 MB moved without a migration script

**Verify before delete, one thread at a time, as they are used.**

Saving a thread writes the log, **reopens it from disk**, compares
field-by-field against the in-memory `Thread`, and only then removes
`<id>.json`. Any failure at any step falls back to the legacy writer and
leaves the old file exactly where it was — so the worst case is a thread
that did not migrate this turn, not one that was damaged.

The comparison is field-by-field rather than a byte diff, because
re-serialising is not stable: blob promotion moves a payload out of line
on the way in, so bytes would differ where no data did.

There is **no migration pass and no flag day**. A thread never opened
again stays legacy forever and still loads — the legacy reader is ~105
frozen lines, which is a trivial price for being unable to lose history.

Rehearsed on a full copy before it ever touched real data:

```
540 threads, 597 MB, migrated in 61.6 s
0 lost, 0 changed, 0 still legacy, 0 listing problems
```

### 4.1 Bugs the migration work surfaced

Four, none of which would have thrown an error:

1. **Appending onto a torn tail merged two messages.** A crash mid-append
   leaves a line with no `\n`; the next append concatenated onto it,
   producing one unparseable line and losing *both* messages. `append()`
   truncates a torn tail first, and `open()` detects the tear in both
   paths — from the scan when it rebuilds, and from a single byte (does
   the log end in `\n`?) when it trusts the index.
2. **`ThreadLog` persisted Smart Mode routing cards.** The document
   writer had always skipped them, so a migrated thread would have grown
   a row per reload. The save-time verification caught this itself and
   refused to migrate — the system working as designed.
3. **`exists()` used `fs::exists`**, so a *directory* at the log path
   counted as a log: the loader found nothing and returned an empty
   thread while a good `.json` sat beside it. Now `is_regular_file`, plus
   a guard that refuses to prefer an empty log when a legacy document has
   content.
4. **`delete_thread` only removed `<id>.json`**, and `load_all_threads`
   only globbed `*.json` — so a migrated thread would survive deletion
   and vanish from the picker respectively.

---

## 5. Correctness — how this is actually verified

Unit tests cover the logic; **probes cover reality**. The distinction
matters, because the shapes in 549 real threads are ones nobody would
think to invent: tool calls that failed mid-stream, three providers'
image formats, half-migrated blob references, text in every encoding a
shell can produce.

| probe | what it proves |
|---|---|
| `thread_log_corpus_probe` | every real thread round-trips **verbatim** — every field, including *materialised* image bytes and attachment bodies |
| `thread_migration_probe` | the real save path migrates one real thread losing nothing |
| `thread_migration_bulk_probe` | a whole directory migrates; content fingerprints unchanged |
| `thread_switch_prof_probe` | splits a switch into worker-thread vs UI-thread cost |
| `blob_gc_probe` | dry-run sweep of a real store |
| `real_thread_render_probe` | renders a real thread at several widths, either format |

All take an explicit path and no-op when absent, so they cost nothing in
CI and can be pointed at a copy by hand.

Current: **655 tests / 9516 assertions green**, and 542 threads /
113,202 messages / 598 MB round-tripping verbatim.

---

## 6. The rejected alternatives

The most useful section, because each of these sounds right.

### 6.1 A hot/cold split file — REJECTED

The first draft proposed extracting render-relevant fields into a
separate index. Built, it was 6 MB and **29 ms** to parse — twenty times
slower than seeking into the raw log. A switch renders ~60 messages, so
*any* design that reads a whole-thread structure has already lost to one
that reads 60 messages. Hot-vs-cold is the wrong axis; recent-vs-old is
the right one, and a byte offset does that for 8 bytes per message.

### 6.2 SQLite — REJECTED

Measured **1.3 ms** against JSONL's 1.4 ms — within noise. It would add
transactions and SQL, neither of which this problem has: no joins, no
query beyond "messages N..M", one writer per thread. Paying a 250 KB C
dependency and losing `grep`/`jq` on your own history for 0.1 ms is a bad
trade.

(For contrast: Zed uses SQLite for workspace state, which is genuinely
relational. Claude Code and Codex both store sessions as JSONL.)

### 6.3 Compression — REJECTED

Halves the file and **triples** the load (173 ms vs 49 ms), and
forecloses seeking entirely — you cannot seek into a gzip stream. We are
not short of disk; we are short of milliseconds.

### 6.4 A lower blob threshold — REJECTED

`kOutputBlobMin` is 8 KB. Dropping it moves more tool output out of the
parse:

| threshold | parse | extra files **per thread** |
|---|---:|---:|
| 8 KB (today) | 28.7 ms | — |
| 4 KB | 24.8 ms | +232 |
| 1 KB | 19.1 ms | +1035 |

Tens of thousands of new inodes across the corpus, each an open when the
wire wants it, for 9 ms. It also inverts the blob store's own rule of
thumb (SQLite's measurement: payloads under ~100 KB belong inline).

### 6.5 Making `ToolUse::output` lazy — REJECTED

The obvious next `Attachment::body`. It is **not the same bug class**:
the view reads `tc.output()` in **41 places** (`bash_body`,
`git_diff_body`, `web_fetch_body`, `task_body`…). Attachments had *zero*
such reads, which is exactly why laziness was free there. Making tool
output lazy would move a blob read onto the render path — a rendering
regression dressed as an optimisation.

### 6.6 The windowed read — REJECTED, and this one is the lesson

The plan's final step was to have the view read a *window* of messages
instead of the whole thread: 0.61 ms for 60 messages against 29 ms for
2519, a 47× win.

Then the switch was actually profiled, and the premise collapsed. Every
earlier number measured **load**, which runs on an isolated worker
(`cmd_factory::load_thread_async` uses `task_isolated`) and dispatches
`ThreadLoaded` when it finishes. **Nobody waits for it.** What the user
waits for is the reducer plus the first frame:

| | 2519-msg thread |
|---|---:|
| worker: load + parse | 19.4 ms |
| UI: model swap | 0.00 ms |
| UI: `rehydrate_frozen` | 0.40 ms |
| UI: build + render | 0.13 ms |
| **UI total — the perceived cost** | **0.53 ms** |

And it does not scale with thread length (0.53 ms at 2519 messages,
0.12 ms at 1238). `rehydrate_frozen` is bounded by `frozen_row_budget()`
— roughly three viewports — and everything downstream is per-visible-row,
so **a switch was already O(screen), not O(thread)**.

The windowed read would have taken an imperceptible number to a slightly
smaller imperceptible number, in exchange for unpicking a residency
assumption held in 500+ places across 46 files, and introducing I/O
behind innocuous-looking indexing — the exact hazard that caused the
crash in maya `1f7cdc3`.

**The lesson: measure the thing the user waits on, not the thing that is
easy to time.** The design is preserved in §9.5 with the trigger that
would revive it — the UI number growing, not the load number.

---

## 7. What the load work was still worth

Since §6.6 says the perceived switch cost was never the parse, it is fair
to ask what 114 ms → 22 ms bought:

- it is the window in which a switch can feel unresponsive on a slow disk
  or a cold cache;
- it is CPU and allocator pressure on a shared machine;
- it bounds how fast the thread picker can preview;
- and **the wire pays the same parse before every turn you send**.

Plus the parts that were never about load at all: O(1) saves instead of
rewriting 28 MB per turn, crash safety from append-only, and attachments
of unbounded size.

---

## 8. Code layout

```
include/agentty/domain/
  lazy_bytes.hpp        LazyBytes — a payload that fetches itself   (121)

include/agentty/io/            src/io/
  thread_log.hpp  (196)          thread_log.cpp   (518)   the log + index
  blob_store.hpp   (53)          blob_store.cpp    (72)   content-addressed store
  blob_gc.hpp      (73)          blob_gc.cpp      (172)   mark and sweep
  persistence.hpp                persistence.cpp          codecs, legacy format,
                                                          async writer, settings
```

`LazyBytes` needed its own header rather than living in
`conversation.hpp`: that file already includes `composer_attachment.hpp`,
so defining it in either would have been a cycle — and it is genuinely
general, knowing nothing about images or attachments, only about bytes
that may not be here yet.

---

## 9. The profile that ended the project

Kept as the working record of the measurements summarised in §6.6 and
§7. It is the profile taken after the log shipped, and it is what turned
the windowed read from "the next commit" into "do not build this".

### 9.1 The switch is already O(1) on the UI thread

Every earlier number in this document measured *load*, which is the wrong
thing. `cmd_factory::load_thread_async` runs the parse on an isolated
worker (`task_isolated`) and dispatches `ThreadLoaded` when it finishes,
so **the user never waits for it**. What they wait for is the reducer and
the first frame, both on the UI thread.

Split apart on the migrated 28 MB / 2519-message thread, measured after
simdjson (`4f3f9fe8`) and the row-budget fix (`ea216da4`):

| | |
|---|---:|
| **worker thread** — load + parse | 19.4 ms |
| model swap + cache drop | 0.00 ms |
| `rehydrate_frozen` | 0.40 ms |
| `conversation_config` | 0.00 ms |
| element build | 0.00 ms |
| render | 0.13 ms |
| **UI thread total — the perceived cost** | **0.53 ms** |

And it does not scale with thread length:

| messages | worker load | **UI thread** |
|---:|---:|---:|
| 3567 | 24 ms | **0.35 ms** |
| 2519 | 19 ms | **0.53 ms** |
| 1238 | 9 ms | **0.12 ms** |

The UI cost is uncorrelated with thread size because `rehydrate_frozen`
is bounded by `frozen_row_budget()` — `max(48, term_rows * 3)`, i.e. about
three viewports — and everything downstream is per-visible-row. Observed
on the 2519-message thread: **11 entries, 119 rows**. **A switch is
already O(screen), not O(thread).**

### 9.2 So the windowed read is not worth building

§9.5 argued for making the view read a window instead of the whole
thread, projecting 36 ms → 7 ms. That projection was against the wrong
baseline. The real UI-thread cost is 0.53 ms, and a windowed read would
take it to perhaps 0.3 ms — a saving nobody can perceive, in exchange for
unpicking a residency assumption held in 500+ places and introducing the
exact hazard (I/O behind innocuous-looking indexing) that caused the
`1f7cdc3` crash.

**Recommendation: don't.** The design is written down in §9.5 if the
situation ever changes; the trigger would be the UI-thread number
growing, not the load number.

What the load time still buys, and why it was worth fixing anyway:

- it is the window in which a switch can be *cancelled* or feel
  unresponsive on a slow disk;
- it is CPU and allocator pressure on a shared machine;
- it bounds how fast the thread PICKER can preview things;
- and it is the same parse the wire pays before every turn.

Going 114 ms → 19 ms was real, and §9.3's first suggestion (simdjson)
shipped as `4f3f9fe8` and delivered most of it.

### 9.3 If more is wanted, it is on the worker

The remaining ~19 ms is io, line-split and parse. Three options were
considered, in order of value per unit of risk — (2) shipped:

1. **Parse lazily per message.** The log already stores one JSON document
   per line, so a `Message` could keep its line as bytes and parse on
   first access. The wire touches every message, so it would pay the same
   total — but spread across the turn rather than in one burst.
2. **A faster parser on this path — SHIPPED (`4f3f9fe8`).** simdjson was
   already a dependency (the SSE reader uses it) and reads these
   documents 7.6× faster. Taken as 3.9× by parsing with simdjson and
   converting to nlohmann, so `message_from_json` — 170 lines of
   tolerance for every historical shape — stayed untouched. See §3.3.
3. **Parallel parse.** Lines are independent, so a parallel-for over the
   2519 of them is embarrassingly parallel. More machinery than (2) for
   a similar win.

None of these change the file format, and none touch the UI thread.

### 9.4 Smaller items

1. **`kOutputBlobMin` 8 KB → 4 KB.** +232 files, −4 ms of parse.
   Marginal; do it only if something else touches that path.
2. **Compaction / edit / fork call `rewrite()`** — O(thread), but rare
   and user-initiated. Confirm no *frequent* path does.
3. **Blob GC.** Deleting a thread should release blobs nothing else
   references. Deferrable, but it is how the store slowly leaks.
4. **`.ofs` endianness** is fixed little-endian so a threads directory
   stays portable; worth a test if a big-endian target ever appears.

### 9.5 The residency obstacle, for the record

Kept because §9.2 decides against acting on it, and the reasoning should
outlive the decision. The design below assumes the app can render from a
*window* of messages. It cannot, today. Two hard facts, both checked:

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
measurements. It is not a bullet in a commit list — and §9.2 explains why
it is not on any commit list at all.
