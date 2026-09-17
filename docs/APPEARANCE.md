# Appearance — settings that apply forward, never backward

**Status: SHIPPED** (`d8d66b22`, `26fa31dd`, `ac23790b`, `766e67ad`,
`33f3d026`; maya `db8356e`, `b66e1f9`, `f07bd55`).

The Appearance pane answers one question — *how should agentty look?* —
across eleven knobs, live, with no apply step. This document is the
design: what the invariants are, which file owns each one, why a live
setting cannot touch already-rendered output, and which alternatives
were rejected.

Vocabulary, in one sentence:

> **Prefs** are what you asked for; **Resolved** is what the terminal
> can give you; the **pane** shows both; the **seam** carries them to
> the renderer. Color is applied at paint time, structure at build time
> — which is the whole reason the theme behaves differently from every
> other knob.

---

## 1. The three rules

Everything below follows from three constraints, in priority order.

1. **Structural settings apply to FUTURE renderings only.** How tall an
   element is, and what content it contains, is decided when it is built
   and never revisited. The theme is the exception, and only because
   color is applied at *paint* time rather than build time — see §2.
   This is not a limitation to work around; it is the invariant that
   keeps scrollback uncorrupted.
2. **There is no apply step.** A theme is judged by *looking* at it, so
   every row writes through on the keystroke that changes it. Between
   choosing and seeing there must not be a restart.
3. **The Model is the truth; everything else is a projection.** The
   prefs live on `Model::Domain::ui`. The form rows, the published seam
   and the rendered output are all derived from it, every frame, and
   nothing writes back.

Rule 1 is a hard boundary. Rule 2 is what makes the pane worth opening.
Rule 3 is what stops the three copies disagreeing.

---

## 2. Rule 1 in detail: what changes, and what cannot

This is the rule that shapes every other decision, so it is worth being
precise about — including about the one setting that behaves
differently.

agentty's transcript is **inline scrollback** (see
`docs/INLINE_SCROLLBACK.md`): settled turns are rendered once and sealed
as immutable `maya::Element` values in a `ScrollbackLedger`. Those
elements are then either (a) still on screen, repainted from the ledger
every frame, or (b) committed to the terminal's own scrollback and gone
from agentty's reach forever.

### 2.1 Structure is fixed at build time; color is not

The distinction that matters is **which part of a sealed element a
setting touches**.

A sealed `Element` is a tree of nodes carrying *semantic* color slots —
`Color::red()`, `Color::default_color()`, `theme.muted`. It does **not**
carry escape bytes. Those are produced by the paint pass, against the
theme live *at paint time*.

So:

| A setting that changes… | Effect on a sealed, on-screen element |
|---|---|
| **Color** (theme) | Repaints in the new palette. The tree is unchanged; only the emitted bytes differ. |
| **Structure** (height, width, what content exists) | Nothing. The tree was built once and is never rebuilt. |

**The theme is the only setting in the color column.** Every other knob
in this pane — density, compact turns, prose width, tool output,
thinking, timestamps — changes *structure*: how many rows an element
occupies, or what content it contains. Those are decided at build time
and frozen there.

This is why a theme switch restyles the visible transcript and a
compact-turns switch does not. It is not an inconsistency — the two act
on different layers.

### 2.2 Why structure must not change retroactively

The ledger records each sealed block's **real laid-out height**, stamped
by the paint pass every frame — same layout pass, same width, same frame
as the bytes on the wire. Those recordings mint the `ScrollbackDebt`
token that tells the terminal how many rows to commit.

If a rebuild changed an element's height, the recorded height and the
committed count would disagree, and every subsequent row would be off by
the difference. The canvas tears. (`INLINE_SCROLLBACK.md` §2 lists three
historical bugs of exactly this shape.)

Color changes are safe against this machinery precisely because they are
**height-preserving**: a repaint in a different palette lays out
identically and re-stamps the same height.

### 2.3 Rows already committed to the terminal

Once `drop_front` + `harvest` commit a block to native scrollback,
agentty cannot touch it at all — not its structure, not its colors. It
belongs to the terminal now.

So a long session that switches themes shows the new palette for
everything still in the ledger, and the old palette for everything
scrolled past the commit boundary. **That is correct behaviour**: the
alternative is not "one consistent look", it is rewriting bytes we do
not own.

### 2.4 Summary

| Surface | Theme change | Structural change |
|---|---|---|
| Committed to native scrollback | unchanged | unchanged |
| Sealed in the ledger, on screen | **restyled next frame** | unchanged |
| Live tail (streaming turn) | restyled | rebuilt → changes |
| Panels, pickers, composer | restyled | rebuilt → changes |
| Every future turn | new palette | new structure |

### 2.5 The one exception, and why it is not one

`rehydrate_frozen()` rebuilds the entire frozen prefix from the
transcript. It runs *only* on a fresh full repaint — thread load, fork,
rewind, `Ctrl-L` — never mid-session. At those moments there is no
committed prefix that must stay byte-accurate, because the screen is
being cleared and redrawn from scratch. So reloading an old thread *does*
pick up your current structural settings for the whole transcript.

Mid-session rebuilding is forbidden for the same reason mid-stream
freezing is forbidden (`INLINE_SCROLLBACK.md` §4).

### 2.6 The height coupling

Two settings change how *tall* a rendered thing is, which is where the
tearing risk concentrates:

- **Compact turns** changes the inter-turn seam from 3 rows to 1.
- **Tool output** changes how many body lines a tool card shows.

Frozen scrollback seals each element with an explicit row estimate:

```cpp
push_frozen(m, gap_row(), static_cast<std::size_t>(gap_rows()), true);
```

If `gap_row()` and `gap_rows()` ever disagree, the estimate and the
element diverge. They are therefore **a function of the same pref,
evaluated at the same instant** — `gap_rows()` is a function, never a
constant someone has to remember to update alongside the element.
`kGapRows` survives only as the documented non-compact height for code
that reasons about the roomy layout specifically.

The same hazard in a different costume: **tool output must enter the
cache key**, not be applied after the lookup. A body built under
"preview" and cached must not be served once the user asks for "full".
Applying the pref post-lookup produces a bug that appears only on the
*second* render, which is strictly worse than one that appears on the
first.

---

## 3. Prefs vs Resolved

Two structs, deliberately distinct.

**`ui_prefs::Prefs`** (`include/agentty/domain/ui_prefs.hpp`) is the
*wish*. It is what you selected, it persists to the user store, and it
is never modified by detection. `theme = "Dracula"` means you asked for
Dracula.

**`ui_prefs::Resolved`** (`include/agentty/domain/ui_theme.hpp`) is what
the terminal actually gets, after detection and fallback. On a 16-colour
terminal, `resolve()` returns `native` — because your own 16 colours
beat a quantised Dracula, every time.

Keeping them separate is what lets a row show **both**:

```
Colors      auto — detected: truecolor
Theme       Dracula — needs 256 colors, using native
```

A settings screen that showed only `auto` would be hiding the one fact
you opened it to learn. A screen that showed only the resolved value
would silently discard your choice the moment you moved to a poorer
terminal.

### 3.1 The detection stack

`resolve()` applies, in order:

1. `NO_COLOR` set → mono. (The standard; it wins over everything.)
2. `TERM=dumb` → mono.
3. `COLORTERM=truecolor|24bit` → truecolor.
4. `TERM` conventions (`*-256color` → 256, etc.).
5. Otherwise 16 colours — the safe floor.

Background polarity reads `COLORFGBG` and reports **"not reported"**
rather than guessing. Guessing was the original bug (issue #37): a dark
theme assumed on a light terminal is unreadable, and the assumption is
invisible to the user. Reporting ignorance is better than confident
wrongness.

### 3.2 Why native is the default

`native` emits only `39`/`49` (the terminal's own foreground and
background) and named ANSI slots — never an RGB literal. It is the only
choice that is correct on a light terminal, a dark terminal, a
16-colour terminal and a monochrome one simultaneously.

Every built-in scheme is a *guess* about your terminal. Native is the
absence of a guess.

### 3.3 Writing a widget that survives native

Native is the theme most likely to break and least likely to be noticed
breaking, because every developer runs a scheme. Issue #45 was one
report that turned out to be **seven** separate bugs, all of the same
family, all invisible under any other theme.

The root fact: under native the 23 theme slots collapse onto the 16 ANSI
colours the *terminal* owns. Two different slots can be the same colour,
and we cannot read what any of them actually look like.

**A `LitColor` is paintable, not numeric.** Only `Kind::Rgb` has
channels. `Named`/`Indexed` keep a palette *index* in the `r_` byte with
`g_`/`b_` zero; `Default` has nothing at all. Reading `r()/g()/b()` off
one is how `bright_black` became `rgb(8,0,0)` — near-black, invisible,
selectable. Guard arithmetic with `has_channels()`, and let a blend
degrade to **no effect** (snap to an endpoint), never to a `to_rgb()`
projection: projecting substitutes the standard table for whatever the
user actually remapped.

**A slot may not be distinct from its neighbour.** `success` and
`diff_added` are both SGR 32 here. Pairing them paints green on green —
both perfectly legitimate palette colours, and unreadable. Never assume
two slots differ; if a fg/bg pair must contrast, one side has to be
stated rather than named.

**`inverse_text` is not ink for a badge.** Under native it is `Default`,
the same colour as ordinary text, because there is no second canvas to
invert against. A filled chip using it paints normal foreground on a
coloured block. For a filled element, either state both sides (see
`maya::diff_palette`) or use **reverse video** (SGR 7) and let the
terminal swap its own pair — it is the only party that knows them.

**Effects that cannot be computed must become no effect.** The reveal
animation, the diff bands and the status banner each carried their own
hardcoded RGB, which is a widget overruling the user's palette. Name a
role (`Color::slot`) unless the colour is genuinely content rather than
chrome — a diff's green, a language brand colour, a syntax deck — in
which case state it *with its ink*, because contrast is a property of
the pair.

The guards for this live in `tests/native_visibility_test.cpp`: render
every surface under native and assert no cell paints a truecolor value,
and no cell paints text in its own background colour. Chasing individual
widgets found three instances and missed the fourth every time.

---

## 4. The seam: how a pref reaches the renderer

The consumers are awkward. `panel_viewport_h()` is a free function that
twenty panel builders call. The `StreamingMarkdown` setup is buried in
`turn.cpp`. Neither has a `Model`, and neither's callers do either —
threading one through would touch dozens of signatures that have no
other business with appearance.

So the prefs are **published once per frame**:

```cpp
// view.cpp, before building anything
ui_prefs::publish(m.d.ui);
maya::anim::set_reduce_motion(m.d.ui.motion == ui_prefs::Motion::Off);
```

and consumers read `ui_prefs::current()`
(`include/agentty/domain/ui_live.hpp`). This is exactly how maya
publishes its theme slot (`app_set_theme`), and for exactly the same
reason.

**What this is not:** a place to keep mutable UI state. It holds a copy
of one Model field, refreshed from the Model every frame, and nothing
writes to it except `publish()`. A reducer calling `publish()` would put
the truth in two places and let them drift — that is the bug this design
is shaped to prevent, so it is worth stating as a rule: **reducers write
the Model; only `view()` publishes.**

**Implementation note.** The slot is a `shared_mutex` + value, not a
`std::atomic<Prefs>`: `Prefs` holds a `std::string` (the theme name) so
it is not trivially copyable. The lock is uncontended in practice —
published and read on the render thread — and exists for the background
workers that build tool previews off-frame. They need a consistent
snapshot, not a *particular* frame's snapshot.

---

## 5. The knobs

Eleven settings in five groups. Each row shows its value and, where
detection is involved, what was resolved.

### Theme

| Row | Values | Drives |
|---|---|---|
| Scheme | native + 615 built-ins | The whole palette |

A `Pick`, not a `Choice` — 615 options is precisely the case
`form.hpp` says belongs in a searchable picker rather than a dropdown.
See §6 for the browser's design.

### Color

| Row | Values | Drives |
|---|---|---|
| Colors | auto / truecolor / 256 / 16 / mono | Palette depth; schemes below 256 fall back to native |
| Background | auto / dark / light | Polarity assumptions; `auto` reports what `COLORFGBG` said |

### Layout

| Row | Values | Drives |
|---|---|---|
| Density | compact 10 / normal 14 / roomy 22 | `panel_viewport_h()` ceiling |
| Prose width | 0–200, 0 = no cap | Width cap on assistant body text |
| Compact turns | on / off | Inter-turn seam, 3 rows → 1 |

**Density is a ceiling, not a height.** A roomy setting on a 20-row
terminal still yields what the terminal has; the pref can never push a
panel off-screen.

**Prose width caps the body only** — not tool panels, diffs or tables.
Those are structured output whose columns mean something; squeezing them
to a reading measure would be actively worse. It is a `Number` rather
than a `Choice` because the useful values (72, 80, 100) are a continuum,
not an enum anyone could name.

### Motion

| Row | Values | Drives |
|---|---|---|
| Motion | full / reduced / off | Streaming reveal, spinners, every stepped animation |

Motion is **two questions**, not one:

- *May anything move?* → `animations_on()`
- *May the DECORATIVE layer move?* → `reveal_decoration_on()`

`Reduced` keeps the progressive reveal — text walking in is information
about progress — and drops the glyph churn, which is not. `Off` stops
both.

This is an accessibility switch before it is a preference. Vestibular
disorders make a typewriter reveal genuinely unpleasant, and "mostly
stopped" is not an accommodation. See §7.

### Content

| Row | Values | Drives |
|---|---|---|
| Syntax highlighting | on / off | Code fences |
| Tool output | collapsed / preview / full | Tool card bodies |
| Thinking | shown / collapsed / hidden | The reasoning block |
| Timestamps | off / relative / absolute | The turn meta strip |

**Timestamps default to off**, which is the right default: in a live
session "when" is always "just now", so a clock in the gutter of every
turn is noise. `Relative` ("4m ago") is what you want re-reading a long
thread — the *distance* between turns, not the hour. `Absolute`
("14:32") is for correlating with a log or a colleague.

Relative times are coarse on purpose: one unit, no decimals. A turn
reading "4m 12s ago" invites arithmetic nobody wanted to do.

**Thinking: `collapsed` tickers the LIVE block to its newest 3 lines.**
Settled reasoning still renders in full, and this is deliberate — see
§8.

---

## 6. The theme browser

Enter on the Scheme row opens a floating list. Three properties, each
chosen against an obvious alternative:

**It floats OVER the pane, which stays painted.** A theme picker that
covers the screen it is restyling asks you to judge a scheme by its
name. Keeping the pane, the transcript and the composer visible behind a
narrow list means the list is a *caption on its own preview*.

**Moving the highlight APPLIES the scheme.** Not on commit — on
movement. A picker that only previews when you accept it makes you take
the theme to find out what it looks like. Typing narrows *and* previews:
`dra` shows you Dracula without a second keystroke.

**Esc is a true cancel.** The theme you opened on is stashed in
`picker.restore` and put back — in the model *and* on disk. A browse you
abandoned leaves nothing behind. (`restore` is exempt from the visual
hash: it is never drawn, so hashing it would wake a frame for an
invisible value.)

Search is fuzzy subsequence, case-insensitive: `gvd` → Gruvbox Dark. An
empty query lists everything, native first — catalogue mode. A query
that clearly means a scheme does not drag native along at the top.

The browser routes through `NavSpec` rather than a hand-rolled switch,
so it inherits PageUp/PageDn and Home/End for free, and a printable is a
query character rather than a form verb.

---

## 7. Motion is gated at the bottom of the stack

The gate lives in maya, under the stepped-animation primitives
(`anim::frame_index`, `anim::blink`, `anim::wave`) — not in each widget.

**Why there.** "Nothing moves" has to be *true*, not true of the widgets
someone remembered to gate. Every spinner, blink and stepped counter in
maya funnels through those three functions, so gating at the source
covers widgets that have never heard of the setting — including ones
written later.

**It suppresses the frame REQUEST too**, which matters beyond
aesthetics. A paused animation that still asks to be repainted 11× a
second keeps the render loop hot and the laptop warm for a glyph that
never changes. Stopping means stopping the wakeups.

**Frozen values are chosen to read as deliberate, not broken:**

| Primitive | Frozen at | Why not the obvious choice |
|---|---|---|
| `frame_index` | 0 | The resting glyph of every frame set maya ships |
| `blink` | `true` | Held OFF would lose the cursor entirely |
| `wave` | 0.5 | Frozen at the trough, a "breathing" highlight reads as a rendering bug |

Motion also re-applies to an *existing* streaming widget when the pref
changes, which is the one place a live setting deliberately beats the
"configure on construction" rule: a user who turns motion off mid-stream
is saying *stop now*, not *stop on the next message*.

---

## 8. Rejected alternatives

**A flat settings list.** Appearance shipped first as eleven cyclic rows
in the Ctrl+K settings list. Wrong twice: cycling an enum four times to
*see* its options is worse than a list of them, and eleven ungrouped
toggles have no shape to read. It is a form pane now, which is where
grouping, per-row help and provenance already live.

**A Ctrl+K palette entry.** Appearance had both a palette command and a
Settings row. Two doors to one pane makes the palette the place people
learn it from, which is the wrong place — a theme is not something you
reach for mid-turn, and its neighbours (profile, Smart Mode, retrieval)
all live in Settings. Now: **Ctrl+K → Settings → Appearance**.

**A theme dropdown.** 615 options in a `Choice` would be a worse picker
than the picker. `form.hpp` draws this line explicitly: if you cannot
name every option in a header comment, it is a `Pick`.

**Expandable settled content.** Rejected outright, and worth recording
why: a "▸ click to expand" affordance on a settled reasoning block or
tool card would have to *grow* an element already sealed in the ledger
with a recorded height. That is the exact drift §2.2 exists to prevent —
and note that it is a STRUCTURAL change, so the theme's paint-time
escape hatch does not apply to it. Everything that collapses in agentty
collapses at **build time**, as a property of how the element was
constructed, and never changes afterward.

**Per-project appearance.** Rejected: a light terminal is a property of
your eyes, not of the repo. Appearance persists to the user store and
follows you between checkouts. (Contrast permission profile, which *is*
per-project — that one really is a property of the code you are in.)

**An apply button.** See rule 2.

---

## 9. File map

| Concern | File |
|---|---|
| Prefs struct, enums, labels, cycles | `include/agentty/domain/ui_prefs.hpp` |
| Resolution + detection + fallback | `include/agentty/domain/ui_theme.hpp` |
| The published seam | `include/agentty/domain/ui_live.hpp` |
| Pane state + row ids | `include/agentty/runtime/panel/appearance.hpp` |
| Form rows + theme search | `src/runtime/panel/appearance_form.cpp` |
| Reducer (live writes, browser) | `src/runtime/app/update/appearance.cpp` |
| View (form + floating browser) | `src/runtime/view/panels/appearance.cpp` |
| Key routing | `src/runtime/app/subscribe.cpp` (`on_appearance`) |
| Per-frame publish | `src/runtime/view/view.cpp` |
| Persistence (JSON) | `src/io/persistence.cpp` |
| Hydration at startup | `src/runtime/app/init.cpp` |
| Motion gate | `maya/include/maya/core/motion.hpp` |
| Built-in schemes (generated) | `maya/include/maya/style/schemes.hpp` |
| Scheme generator | `maya/scripts/gen_themes.py` |

Themes are generated from the iTerm2 colour-scheme corpus. Red → error,
green → success, yellow → warning, cyan → info; diffs and chrome are
derived by blending toward the background. Re-run with `--all` for the
full 609.

---

## 10. Tests

`tests/ui_prefs_test.cpp` — 14 cases:

- Every knob has a row; the pane opens on a setting, not a header
- Adjusting in place wraps; a toggle toggles
- A change is persisted, not just held (no apply step, ever)
- The theme row hands off to a browser, not a dropdown
- Moving in the browser previews live; Enter keeps; Esc reverts both
  model and disk
- Typing filters and previews the top match
- Fuzzy subsequence: `gvd` → Gruvbox Dark, case-insensitive
- Resolution never leaves the user unable to read (tier fallback,
  unknown names)
- **Publishing a pref reaches its consumers** (the seam)
- **Reduce-motion freezes maya's primitives** at their documented
  resting values
- **Compact turns changes the seam's height AND its rows together**

The three scrollback tests — `reveal_scrollback_test`,
`scrollback_wire_fuzz`, `frozen_invariant_fuzz` — are what actually
prove the variable-height seam is safe. Run them after touching
anything in §2.2.

`tests/native_visibility_test.cpp` guards §3.3. It renders every major
surface under native — reasoning (settled, live, and mid-reveal), tool
calls including all three diff paths, and the status banner in each of
its kinds — and asserts:

- no cell paints a truecolor value (the diff palette is the one listed
  exemption, colour by colour, so a *new* literal still fails)
- no cell paints text in its own background colour
- every native slot resolves to `Named`/`Default`, never RGB
- no pairwise blend of two slots fabricates a triple

The pair matters: the first question is "did anyone invent a colour",
the second is "can you read it", and neither implies the other. Fixing
the first by moving widgets onto theme slots is what *caused* the
second.

`maya/tests/test_blend_safety.cpp` pins the same rule one layer down,
and both repos' `theme_discipline` tests grep the source for colour
literals and unguarded channel reads.

> **Run maya's own suite too.** agentty does not compile maya's tests or
> examples, so a change to a maya header can be green here and red in
> maya's CI. `cmake -B build -DMAYA_BUILD_TESTS=ON -DMAYA_BUILD_EXAMPLES=ON`
> in `maya/` is what CI builds.
