# Stats — one pass, one vocabulary, one table

**Status: PROPOSED.** Supersedes the single-tab Smart Mode viewer shipped
in `ea4591e1` / `88004849`.

The stats subsystem answers "what did this session actually do?" across
ten groups of questions, in a tabbed read-only panel. This document is the
design: what the invariants are, which file owns each one, why it cannot
slow the stream down, and which alternatives were rejected.

Vocabulary, in one sentence:

> A **fold** turns messages into **Facts**; a **section** projects Facts
> into **Metrics**; a **Viz** draws Metrics. A **tab** is a row in a table
> naming which sections it shows.

---

## 1. The three rules

Everything below follows from three constraints, in priority order.

1. **The stream path pays nothing per token.** Not one branch, not one
   add. Stats are a *projection*, never an accumulator sitting in the hot
   loop.
2. **The transcript is the SSOT.** A statistic about a turn is a property
   *of that turn*, stored on it — so it survives save/reload, fork, and
   rewind, and so nothing has to be recomputed from volatile session
   state that was already thrown away.
3. **A new statistic is data, not code.** Adding one touches a struct
   field, a fold line, and a table row. It touches no view code, no key
   handling, and no panel code.

Rule 1 is a hard budget. Rule 2 is what makes rule 1 affordable. Rule 3
is what stops ten tabs becoming ten bespoke renderers.

---

## 2. Why the current shape does not scale to ten tabs

`smart_stats()` is pure, `O(turns)`, called from the view, cached behind
a `Stamp{message_count, thread_id}`. For one projection that is fine.
Ten projections on that pattern gives ten walks, ten caches, ten stamps,
and ten hand-rolled renderers. It also has two faults that only become
visible at scale:

- **The stamp is blind to the live tail.** `message_count` does not
  change while the last message is being streamed into — which is
  precisely when the numbers are moving. Today's single tab gets away
  with it because a turn's routing provenance is written once at the
  start. A token counter would freeze mid-stream.
- **`Row{label, count, share}` only expresses tallies.** A duration, a
  ratio, a byte count, or a cache hit rate is not a tally, so each would
  arrive with its own formatter and its own bar code.

---

## 3. Layer 1 — per-turn telemetry, sealed once

The stream path's counters are *volatile*: `transient_retries`,
`mid_stream_failures`, `no_progress_failures` and friends live on
`phase::Active`, which is reset every turn. By the time the panel opens
they are gone. A purely-derived-from-transcript projection therefore
cannot report stream health at all.

The fix is the one this codebase already made for `served_model`:

> A turn's provenance is a property OF THE TURN, so it lives on the turn.
> — `domain/conversation.hpp`

So `finalize_turn` seals a small record onto the assistant `Message`:

```cpp
// domain/conversation.hpp, on Message
struct Telemetry {
    std::uint32_t ttft_ms          = 0;  // request → first content byte
    std::uint32_t stream_ms        = 0;  // first byte → stop
    std::uint32_t input_tokens     = 0;
    std::uint32_t output_tokens    = 0;
    std::uint32_t reasoning_tokens = 0;
    std::uint32_t cache_read       = 0;
    std::uint32_t cache_creation   = 0;
    std::uint16_t transient_retries    = 0;
    std::uint16_t mid_stream_failures  = 0;
    std::uint16_t no_progress_failures = 0;
    StopReason    stop = StopReason::Unspecified;
};
std::optional<Telemetry> telemetry;   // absent on old / user messages
```

**Cost: one struct write per turn.** Nothing per token, nothing per
frame. 48 bytes on an assistant message that already carries kilobytes of
text.

Three properties fall out, and they are the reason this is worth a field:

| property | why |
|---|---|
| stats survive reload | it is persisted with the message |
| stats survive fork/rewind | it travels with the turn it describes |
| cache ratios stop being lies | `cache_hit_ratio` is computed every turn today and **discarded** — this is the first time it is recoverable |

`std::optional` rather than a zeroed struct: "this turn predates
telemetry" and "this turn used no cache" are different statements, and a
sentinel that conflates them puts fake zeroes in the denominator.

---

## 4. Layer 2 — `Facts`: one incremental pass

```cpp
Facts::refresh(const Thread&, const Session&);   // amortised O(1) per frame
```

One struct holds every group's counters. Its shape mirrors the tab table
one-to-one, which is what keeps "where does this number live" answerable:

```cpp
struct Facts {
    struct Session   { ... } session;
    struct Models    { Tally by_model, by_role; } models;
    struct Smart     { std::size_t routed, unrouted; ... } smart;
    struct Tokens    { ... } tokens;
    struct Cache     { ... } cache;
    struct Tools     { Tally by_name; Hist latency; ... } tools;
    struct Reasoning { ... } reasoning;
    struct Stream    { ... } stream;
    struct Context   { ... } context;
    struct Retrieval { ... } retrieval;
};
```

### 4.1 The incremental cursor

`Facts` is **not** recomputed per frame. It carries `consumed` — how many
messages have been folded — and folds only what is new:

- messages `[0, size-1)` are **sealed**: folded exactly once, ever.
- message `size-1` is the **live tail**: folded into a separate scratch
  total that is recomputed each refresh and added on read.

The tail rule is unconditional — the last message is *always* treated as
volatile, never sealed. That is one message of work per frame, and it is
what makes the panel live during a stream without the stale-stamp bug.

On a settled 800-turn thread, a frame costs **zero folds**. During a
stream it costs **one**.

### 4.2 Invalidation — the part that must not be clever

Three operations mutate history rather than appending to it, and each
silently corrupts an incremental fold:

| operation | detection | response |
|---|---|---|
| fork / thread switch | `thread_id` differs | full rebuild |
| rewind to checkpoint | `messages.size() < consumed` | full rebuild |
| compaction | `compactions.size()` differs | full rebuild |

All three are rare and user-initiated, so a full rebuild — one `O(n)`
walk of integer adds — is the right answer. The guard is a 3-field
epoch compared by value; anything that does not match resets `consumed`
to 0. **Nothing here tries to patch a mutated prefix in place.** That is
the class of optimisation that produces numbers which are wrong and
believed.

### 4.3 Data structures chosen for the sizes actually seen

- **`Tally`** — `vector<pair<string, uint32>>`, linear scan. Distinct
  models per session is `< 20` and tools `< 40`; at that size a linear
  scan beats a hash map and keeps insertion order stable, so rows do not
  reshuffle frame to frame.
- **`Hist`** — 24 log2 buckets, `1ms … ~4h`, 96 bytes. `add()` is
  `O(1)`, `quantile()` is `O(24)`. Tool latency p50/p95 without storing
  a duration per call, which is the version that grows without bound.

---

## 5. Layer 3 — `Metric`: one number vocabulary

```cpp
enum class Unit : std::uint8_t {
    Count, Tokens, Bytes, Millis, Ratio, Usd, Rate
};

struct Metric {
    std::string label;          // SSO: no allocation for real labels
    Unit        unit  = Unit::Count;
    double      value = 0;
    double      of    = 0;      // denominator; 0 = no share bar
};
```

`format(Unit, double)` is the **single** place a number becomes text. So
tokens are `12.4k` everywhere, durations are `3.2s` everywhere, and no
tab can invent its own spelling of a byte count.

`of` is carried, never recomputed at the view — the existing code already
learned this: two consumers dividing independently disagree in the last
digit, and the panel then shows shares that do not sum to one.

One `label` field, not `string_view label` + `string name`. Two fields
describing one mutually-exclusive fact is the shape this codebase keeps
deleting; real labels are under 23 chars, so SSO means the "allocation"
argument for splitting them is not real either.

---

## 6. The widget — `maya::StatSheet`

**Status: SHIPPED**, maya `38d6dbb`. This is what §5's `Metric`s are
drawn by, and it is one widget rather than three renderers because what
makes a stats tab legible is not the bar — it is that every row lines up
with every other row.

The library already had `BarChart`, `Sparkline` and `Gauge`, and none of
them could draw a stats tab. A tab is a dozen *heterogeneous* rows that
must align **with each other**, and that property cannot live in a row
widget: a row cannot right-align its number against the other rows'
numbers, because it does not know they exist. So the sheet owns the
measurement — it scans every entry, derives one label column, one track,
one value column and one detail column, and paints all rows against that
geometry. Digits line up on their right edge, which is what lets you
compare two numbers without reading them.

```cpp
StatSheet s;
s.hero("62%", "of routed turns ran below the Strategic model");
s.heading("By role");
s.entry({.label = "Strategic", .value = "12", .detail = "38%", .share = 0.38});
s.entry({.label = "Output",    .value = "1.2k/s", .spark = rate_history});
s.entry({.label = "Context",   .value = "62%", .share = 0.62, .wide = true});
```

```
 62% of routed turns ran below the Strategic model

 By role
 Strategic          █████▍────────      12  38%
 Implementation     ██████▏───────      14  44%
 Utility            ██▌───────────       6  18%

 Throughput
 Output rate        ▂▄▃▇▃▆█▅▆▇▄█    1.2k/s
 Context            ███████████████▎──────────     62%
```

**One entry type, not four.** `label` / `value` / `detail` / `share` /
`spark` / `wide`, where what is absent simply does not draw. A key-value
row, a ranked bar, a trend strip and a full-width meter are one struct
and one renderer instead of four of each. Bars and sparklines occupy the
**same columns**, so a tab may mix them and still align.

### 6.1 Two more graph forms

Shipped in maya `1ec853d`. Each answers a question ranked bars answer
badly, which is why each is a row kind rather than a flag.

**`band`** — one full-width track split into coloured segments that sum
to the whole, with a one-line legend.

```
 input tokens by origin
 ████████████████████████████████████████████████████
 ■ read   ■ write   ■ miss
```

Ranked bars answer *how big is each one* — you compare lengths. A band
answers *what is this made of* — one bar, proportions read directly.
Cache read/write/miss and accepted/rejected/pending are band questions:
the parts **are** a whole, and three separate bars hide that they sum to
one.

Segments are apportioned by **largest remainder**, never by rounding each
independently — independent rounding leaves the total a column or two
short, so the right edge wobbles with the data and the bar visibly is not
the whole it claims to be. A non-zero segment always gets at least one
column, the same rule that stops a 1% bar rounding to empty.

**`plot`** — a braille line chart over the full width.

```
 output tokens, last 40 turns
 ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠸⠧⡄⠀⠀⠀⠀⠖⡆⡀⠀⠀⠀⠀⠰⠲⡀⠀⠀⠀⠀⠀⠠⠤⠀⠀ 1.5k
 ⠀⠀⠀⠀⠀⠁⠸⠹⠀⠀⠀⠀⠸⠀⠇⠀⠀⠀⠖⠃⠀⠇⡀⠀⠀⠸⠉⠀⠸⡀⠀⠀⠀⠠⠼⠉⠹⠀
 ⠀⠀⠸⠹⠁⠸⠀⠘⠲⠀⠀⠸⠉⠀⠉⠇⡀⠤⠇⠀⠀⠀⠇⡀⠀⠞⠀⠀⠀⠸⠤⠀⠠⠼⠀⠀⠈⠹
 ⠁⡀⠸⠘⠚⠀⠀⠀⠈⠹⠠⠼⠀⠀⠀⠀⠓⠃⠀⠀⠀⠀⠀⠇⠏⠁⠀⠀⠀⠀⠈⠹⠼⠀⠀⠀⠀⠀
 ⠚⠈⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀    0
```

A sparkline is one row and answers *is it going up*; a plot is several
rows with a labelled scale and answers *by how much, and when*. One
braille cell is a 2×4 dot matrix, so it carries **eight times** a block
chart's resolution — a real 40-turn curve fits in five terminal rows.

Consecutive samples are **joined** by a vertical stroke. Without it a
steep move leaves a gap and the eye reads two unrelated marks instead of
one falling line — a scatter plot nobody asked for. The join asserts
continuity *between samples*, not values between them: resampling stays
nearest-neighbour, because these are measured points and interpolating
invents a curve the data never had.

Scale labels ride the first and last rows, in a gutter reserved *before*
the plot is sized — a curve with no peak label is a shape without units,
and a plot that overruns its labels is worse than one two columns
narrower.

Both degrade to **nothing** on degenerate input (no segments, all-zero,
no samples) rather than to a divide-by-zero or a stripe of garbage.

### 6.2 Mapping `Viz` onto the sheet

A `Section`'s `Viz` therefore maps onto sheet calls rather than onto
separate widgets — `Bars` sets `share`, `Spark` sets `spark`, `Kv` sets
neither, `Band` and `Plot` emit their own row kinds. `extract` fills
`Metric`s; the panel formats each through `format(Unit,…)` into a
`StatEntry`. The view still has no per-tab branches.

Two rendering rules that are corrections rather than decisions:

- **A non-zero share never floors to an empty bar.** "Almost no work"
  and "no work" are different readings; the eighth-block ramp keeps them
  distinct, so a 1% share draws `▏`.
- **The unfilled remainder is drawn** (`─`), because an empty tail makes
  a short bar read as a *missing* bar rather than as a position on a
  scale.

**Degradation ladder.** Narrow surfaces shed in the order that loses
least: `detail` first (redundant with the value), then the bar shrinks
and only then vanishes (it is a comparison aid), and the label truncates
rather than dropping. The value is never shed — a picture of a statistic
with the statistic removed is not a fallback.

**Units stay in the domain.** The sheet takes numbers pre-formatted; it
never sees a token count or a duration, only `"12.4k"` and `"3.2s"`.
Baking `format()` in would mean two owners of "how big is a kilotoken"
and a widget enum growing every time a host learns a unit — which is
exactly the `Unit` table in §5, one layer down where it belongs.

Shipped alongside it: `unicode::truncate_to_width`, the column-safe cut
every aligned layout needs and that `substr()` cannot do, since bytes
are not columns and a sequence sliced in half renders as a replacement
glyph — destroying the alignment the caller was truncating to preserve.

---

## 7. Layer 4 — the tab table (the SSOT)

```cpp
enum class Viz : std::uint8_t { Kv, Bars, Spark, Band, Plot };

struct Section {
    std::string_view heading;
    Viz              viz;
    void (*extract)(const Facts&, std::vector<Metric>& out);
};

struct TabDesc {
    Tab                      id;
    std::string_view         title;      // strip label
    std::string_view         subtitle;   // panel subtitle
    std::span<const Section> sections;
    bool (*available)(const Facts&);     // hide a tab with nothing to say
};

inline constexpr std::array kTabs = { /* one row per tab */ };
```

`kTabCount`, `tab_title`, `tab_subtitle` and `tab_step` all become
**derived from `kTabs`** instead of four parallel `switch`es that drift
apart. This is the same pattern as `ProviderDescriptor` and `kCommands`,
for the same reason: heterogeneity belongs in data, not in code
branches.

`extract` writes into a **caller-owned scratch vector** that the panel
reuses, so a steady-state frame allocates nothing.

`available()` is what stops empty tabs: a session that ran no tools does
not show a Tools tab, and — the case that matters — a session that never
ran Smart Mode hides the Smart tab instead of rendering today's empty
state explaining why it is empty. `tab_step` skips unavailable tabs, so
Tab/Shift-Tab never lands on a dead view.

---

## 8. The tabs

Grouped by the *question the user is asking*, which is the only grouping
that survives contact with a tenth statistic.

| Tab | Question | Source |
|---|---|---|
| **Session** | what happened at all? | turns, wall-clock, errors, thread age |
| **Models** | who served my turns? | `served_model` (descriptive tally) |
| **Smart** | is routing delegating? | `served_role`, routed vs unrouted |
| **Tokens** | what am I paying? | `Telemetry` in/out/reasoning |
| **Cache** | why is it slow? | `Telemetry` cache read/creation — **band** |
| **Tools** | what did the agent *do*? | `ToolUse` name × status, latency `Hist` |
| **Reasoning** | is thinking earning its keep? | `reasoning_ms`, reasoning tokens |
| **Stream** | is the transport healthy? | retry / failure counters |
| **Context** | how close to the wall? | `est_prefix_tokens`, compactions — **plot** |
| **Retrieval** | is RAG helping? | `ProactiveContext` hits + confidence |

### 8.1 Models and Smart are not the same tab

They read the same two fields and answer different questions, and merging
them loses information:

- **Models** is *descriptive*. Denominator = every assistant turn.
- **Smart** is *a verdict on a feature*. Denominator = **routed turns
  only**. `unrouted_turns` is its own bucket precisely so a Smart-Mode-off
  turn cannot be counted as strategic and overstate the flagship's share.

Fold them together and `delegated_share` becomes uncomputable — you are
dividing by the wrong denominator. `delegated_share` is also not a
measurement but an *answer* ("is my expensive model still doing all the
work?"), and a model tally structurally cannot produce it, because it
does not know which model was supposed to be cheap.

What *does* get shared is the layer below: `served_model` and
`served_role` are folded once, into counters both tabs project from. The
DRY win lands at the data layer rather than by collapsing two questions
into one view.

---

## 9. File layout

```
include/agentty/domain/stats/
  unit.hpp        Unit + format()            — the number vocabulary
  metric.hpp      Metric, Viz, Section       — the row vocabulary
  facts.hpp       Facts + Tally + Hist       — the counters
  tabs.hpp        Tab, TabDesc, kTabs        — the table (SSOT)
src/domain/stats/
  fold.cpp        one message → Facts        — the only place that counts
  extract.cpp     Facts → Metrics            — one function per section
  format.cpp      Unit + double → string
include/agentty/runtime/panel/stats.hpp
                  Open{tab} + the Facts cache and its epoch guard
src/runtime/view/panels/stats/
  stats.cpp       the panel: table-driven, no per-tab branches
  viz.cpp         bars() / kv() / spark()    — three renderers, total
```

`domain/` knows nothing about maya; `view/` knows nothing about how a
number was counted. The existing single-file `domain/stats.hpp` +
`src/domain/stats.cpp` is replaced wholesale.

---

## 10. Cost model

| event | cost |
|---|---|
| one token streamed | **0** — no stats code runs |
| one turn finalised | one 48-byte struct write |
| frame, panel closed | **0** — `Facts` is never built |
| frame, panel open, thread settled | 0 folds; reuse cached `Metric` buffer |
| frame, panel open, streaming | 1 message fold + 1 extract (< 40 rows) |
| panel opened on an 800-turn thread | one `O(n)` walk of integer adds |
| fork / rewind / compaction | one `O(n)` rebuild |

The design has no always-on accumulator, no hook in the stream path, and
no allocation per token *or* per frame. That is what "does not come in
the way of speed" has to mean concretely.

---

## 11. Rejected

**11.1 An always-on `Metrics` accumulator updated from the stream.**
The obvious design, and it fails rule 1: every token pays for a
statistic nobody is looking at, and the counters then need their own
persistence, their own reset semantics on fork/rewind, and their own
consistency story against the transcript. Deriving from the transcript
means there is exactly one source of truth and it is already saved.

**11.2 Keeping `smart_stats()` and adding nine siblings.** Ten walks, ten
caches, ten stamps. Rejected in §2.

**11.3 A hash map for tallies.** Measured sizes are `< 20` models and
`< 40` tools. A map costs more per lookup at that size, and loses stable
ordering — rows reshuffling frame to frame during a stream reads as a
rendering fault.

**11.4 Storing every tool-call duration for exact percentiles.** Unbounded
memory for a number nobody reads to three digits. A 96-byte log2
histogram gives p50/p95 within a bucket, permanently.

**11.5 Patching the fold in place after compaction.** Compaction rewrites
a prefix; reconciling that incrementally is the optimisation that
produces numbers which are wrong *and* believed. A rare `O(n)` rebuild
is correct and cheap.

**11.6 A `Row` per tab.** One `Metric` + one `Unit` + one `format()` is
what stops the eleventh statistic arriving with its own spelling of
`12.4k`.

**11.7 A `StatRow` widget the host stacks itself.** The decomposition
that looks right and cannot work: a row cannot align against rows it
cannot see, so every host ends up measuring the columns and threading
widths down — which is `StatSheet`, written once per host instead of
once. See §6.

**11.8 Reusing `BarChart` / `Sparkline` / `Gauge` directly.** Each draws
its own chart against its own geometry, so a tab mixing them produces
three unrelated column layouts stacked vertically. They remain right for
what they are — a chart on its own — and wrong for a row in a sheet.

---

## 12. Open questions

- **Cost in USD.** `Unit::Usd` is in the vocabulary, but `catalog.hpp`
  carries no per-token pricing — only a coarse routing hint. Real dollar
  figures need a price table that will silently go stale, and a stale
  cost display is worse than none.
- **Cross-session history.** Everything above is *this thread*. "Cache
  hit ratio over the last 30 days" needs a stats log on disk, which is a
  separate design with its own retention and privacy questions.

---

## 13. Build order

1. `Telemetry` on `Message` + the seal in `finalize_turn` (nothing reads
   it yet — it starts accumulating on real threads immediately).
2. `Unit` / `format()` / `Metric` — the vocabulary, with tests.
3. `Facts` + `fold` + the epoch guard, with tests for the three
   invalidation paths.
4. `kTabs` + the three renderers; port the Smart tab onto them and
   delete `SmartStats`, `smart_stats()` and the `Stamp` cache.
5. The remaining tabs, one table row at a time.

Step 4 is the proof: if porting the existing tab does not delete more
code than it adds, the abstraction is not paying for itself.
