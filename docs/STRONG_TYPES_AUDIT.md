# Where else strong types pay for themselves

**Status:** audit. Findings and recommendations, no code changed yet.

This came out of `92cf9bc1`, where turning `Attachment::body` from a
`std::string` into `LazyBytes` immediately exposed a real bug the
compiler had been unable to see: `compute_render_key` mixed
`a.body.size()`, which on a lazy body would have read a 2 MB blob off
disk *to compute a hash*, on every frame that rebuilt a turn.

A plain string compiled fine and would have silently read empty. The type
made the compiler ask the question.

So: where else does that argument hold? The audit below is deliberately
evidence-first — every candidate was checked against how the code
actually uses the value, and **two of the four were rejected on the
evidence**, which is the useful part.

---

## What the codebase already does well

This is not a codebase that needs converting to strong types. It already
has the machinery, and uses it:

- **`Id<Tag>`** (`domain/id.hpp`) — zero-overhead newtype for
  string-shaped IDs, with `ThreadId`, `MessageId`, `ToolCallId`,
  `ModelId`, `CheckpointId`, `ToolName`. The header's own comment states
  the rule: *"never pass a raw String across an API boundary when the
  caller could plausibly confuse it with another String."*
- **`Refined<T, Predicate>`** (`domain/refined.hpp`) — values that
  carry a proven invariant.
- **Enums for closed sets** — `ModelRole`, `Effort`, `Complexity`,
  `Role`, `Attachment::Kind`, and the `ToolUse::Status` sum type.

The gaps below are places where that vocabulary exists but is **dropped**
at a boundary, not places where it is missing.

---

## Finding 1 — `Complexity` and `Effort` are enums that get stored as strings

**The strongest case, because the type already exists and is thrown away.**

`Message` carries the Smart-Mode routing card as four adjacent bare
strings:

```cpp
std::string smart_route_model;       // wire id
std::string smart_route_effort;      // "off" / "high" / …
std::string smart_route_complexity;  // "trivial" / "simple" / "standard" / "complex"
std::string smart_route_note;
```

`Complexity` and `Effort` are already enums. They are converted to
strings on the way in — and then **re-parsed by string comparison** in
the view:

```cpp
// src/runtime/view/thread/turn/turn.cpp
auto cx_color = [&](const std::string& c) -> maya::Color {
    if (c == "complex")  return status_warn;
    if (c == "standard") return status_info;
    return muted;
};
```

Two concrete hazards, neither hypothetical:

1. **A fifth `Complexity` value renders `muted` silently.** No warning,
   no failing test — the `else` branch swallows it. With an enum and a
   `switch`, `-Wswitch` catches it at compile time.
2. **A typo is undetectable.** `"stanadrd"` compiles, runs, and renders
   the wrong colour forever.

**Recommendation:** store the enums (`Complexity`, `Effort`) and convert
to string only at the JSON boundary, where `to_string` already exists.
The view switches on the enum. This is a contained change: one struct,
one writer, one reader, one view function.

---

## Finding 2 — adjacent same-typed strings that can be swapped

**SHIPPED.** `served_model` is now `ModelId`, `served_role` is
`std::optional<smart::ModelRole>`, and the same for the session's
`smart_turn_model` / `smart_turn_role`.

```cpp
// domain/session.hpp
std::string smart_turn_model;
std::string smart_turn_role;

// domain/conversation.hpp
std::string served_model;
std::string served_role;
```

Assigned in adjacent lines, in that order:

```cpp
last.served_model = m.s.smart_turn_model;
last.served_role  = m.s.smart_turn_role;
```

Swap those two lines and it compiles, runs, and mislabels every turn's
provenance. `ModelId` already exists and `ModelRole` is already an enum —
the values are typed *everywhere except where they are stored*.

**And the hazard is not hypothetical — the drift has already happened.**
The same concept is spelled three incompatible ways today:

| where | spelling |
|---|---|
| the only writer (`cmd_factory.cpp:841`) | `"strategic"` — and *only* this one |
| the view (`role_accent_for`) | `"strategic"`, `"implementation"`, `"utility"` |
| `role_label(ModelRole)` | `"strategic"`, **`"impl"`**, `"utility"` |

Two consequences, both live:

1. The view's `"implementation"` and `"utility"` branches are **dead
   code** — nothing ever writes those strings.
2. If someone "tidied" the writer to use the existing `role_label()`
   helper — the obvious thing to do — it would emit `"impl"`, the view
   would fail to match, and the accent colour would silently vanish for
   implementation turns. No compiler error, no failing test.

That is exactly the failure the `body` change caught, in a different
costume: a value whose meaning is carried by convention instead of by
type.

**Recommendation:** `served_model` → `ModelId`, `served_role` →
`std::optional<ModelRole>`. The view switches on the enum, and the three
spellings collapse into one `role_label()` used by both sides.

### What implementing it turned up

A second bug, in the same fields, that the audit had not predicted.
`compute_render_key` mixed **sizes**:

```cpp
mix(served_model.size());
mix(served_role.size());
```

The three roles are distinguished by length — 9, 14, 7 — **by luck**. Any
future 9-character role would have collided with `"strategic"`, and two
model ids of equal length collide today: one turn's header would reuse
another's cached Element and paint the wrong accent colour. It now mixes
an enum ordinal (no ambiguity possible) and hashes the model id's bytes
via a new `mix_str`.

That is the third time in this session that giving a value a type exposed
a latent bug in code that read it — `body.size()` reading a blob off the
render path, the three role spellings, and now this. The pattern is
consistent: **the bug is never in the field, it is in something that
quietly assumed the field was just a string.**

The persisted spelling is now `role_wire_name` / `role_from_wire_name`, a
proven bijection (`turn_provenance_test`), deliberately separate from
`role_label()` — which stays a UI abbreviation. An unknown role reads as
"no role tag" rather than failing the load, so a thread written by a
newer build still opens.

---

## Finding 3 — REJECTED: `ToolUse::output` should not be lazy

The obvious next candidate after `Attachment::body`: tool output is large
(263 MB across a real corpus) and already blob-backed above 8 KB.

**Checked, and it is not the same bug class.** The view reads
`tc.output()` in **41 places** — `bash_body`, `git_diff_body`,
`web_fetch_body`, `task_body` and the rest all render it. Attachments had
**zero** such reads, which is precisely why laziness was free there.

Making tool output lazy would move a blob read onto the render path —
turning a rendering optimisation into a rendering *regression*. The
existing arrangement (blob-backed on disk, materialised on load) is
correct.

**Recommendation:** leave it. Recorded here so the next person who spots
the size doesn't repeat the analysis.

---

## Finding 4 — REJECTED: mass `.value` unwrapping is mostly legitimate

798 `.value` unwraps across `src/`. That looks like type leakage, and
mostly isn't:

| shape | count | verdict |
|---|---:|---|
| `.value)` / `.value,` | 172 | passing to APIs that take strings — legitimate |
| `.value.empty()` | 13 | **gratuitous** — `Id<Tag>` has `empty()` |
| `.value ==` | 9 | **gratuitous** — `Id<Tag>` has `operator==` |
| `.value.size()`, `.c_str()` | 7 | legitimate |

Of the gratuitous ones, only **4** are on `Id<Tag>` types (the rest are
unrelated `.value` fields on `ApiKeyHeader`, HTTP headers, etc.).

**Recommendation:** not worth a commit on its own. Fix opportunistically
when touching those lines. Four call sites is churn, not a safety win.

---

## The rule this suggests

Not "make everything a type" — the audit rejected half its own
candidates. The rule that actually predicted the `body` bug:

> **Give a value a type when the compiler would otherwise be unable to
> catch a plausible misuse — a swap with a neighbour, a silent fallthrough,
> or an implicit expensive operation.**
>
> Do NOT add a type to a value whose only property is "it is a string",
> and do NOT make something lazy that the render path reads.

By that rule: Findings 1 and 2 are worth doing, 3 and 4 are not.

---

## Suggested order

1. ~~**Finding 2** first~~ — **done.** Mechanical, as predicted; the only
   surprise was the render-key size collision it exposed.
2. **Finding 1** next — slightly larger because the view switches from
   string comparison to a `switch`, which is the actual win.

Both are contained, both are testable by round-tripping the corpus, and
neither touches the storage format.
