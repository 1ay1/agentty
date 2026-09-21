# Late binding: why a theme switch is free

A design note about one bug class, three shipped instances of it, and the
enforcement that retires it. The rule is one line; the rest is why.

> **Resolution happens at PAINT. Everything above paint stays symbolic.**

---

## 1. The bug class

**A derived value was cached under a key that did not name everything it
derives from.**

Every theme bug this project has shipped is that sentence. A rendered
`Element` derives from `(source, theme)`. Its cache key was `source`. The
theme moved, the key did not, and the stale value was served forever.

It is not really a theming bug. It is a cache-key omission, and theming is
just what makes it visible — because theming is the one input that changes
*without* changing any content.

It is not ours either. Same shape, same year, elsewhere:

- **OpenSCAD #7053** — `toPolySet()` resolves colours from render settings and
  stores them in the `PolySet`. The `PolySet` is cached; `setColorScheme()`
  does not invalidate it.
- **react-native-linux #52** — stated generally: *"a dynamic colour is
  resolved once, cached in the render tree, and the theme change has no path
  to invalidate it."*

## 2. Our three instances

| Where | What was baked | Why nothing caught it |
|---|---|---|
| `md_block_to_element` | a committed block stored a fully RENDERED `Element` with ~58 colour slots resolved at COMMIT time | the bytes never change, so `set_content` no-ops, nothing goes dirty, every cache tier faithfully replays the same `Element` |
| `colors::` accessors | the markdown palette held `LitColor`, so every read handed back an already-resolved colour | legal C++, and the header explained why it was `LitColor` — the reason had simply expired |
| `ui::Slot` | `operator Color()` returned `read()`, i.e. resolved | a token that *looks* late-bound at ~700 call sites while being early-bound at every one |

All three are the same move: **deciding a colour earlier than necessary.**

## 3. Why it looked intermittent

This is the part worth internalising, because "intermittent" is what made it
survive five rounds of fixes.

Two things are on screen and they behave differently:

- the **live tail** is rebuilt from the Model every frame, so it re-reads the
  tokens and always tracks the current theme
- the **frozen ledger** and **committed blocks** are stored `Element`s, so
  they keep whatever palette was resolved into them

So a theme switch visibly worked on everything that was repainting anyway and
silently failed on everything settled. And because an unrelated animation
(welcome screen, spinner, live stream) keeps frames flowing, any recovery
that needed a second frame *got* one — for free, within 16 ms. Idle is the
only state where a missing frame is actually missing.

**A bug that hides whenever something else is animating is not a colour bug.
It is a bug about which layer owns the decision.**

## 4. Why late binding, and not better invalidation

We tried invalidation first. It worked and it was still wrong.

Re-rendering every committed block on a theme change is `O(transcript)` per
keystroke. Measured: per-keypress render went **2.42 ms → 4.65 ms on a
100-message thread**, scaling with conversation length. Past ~33 ms (one
key-repeat slot at 30/s) keys outrun frames, frames coalesce, and the browser
visibly updates on every *second* theme you arrow past.

That reads as two bugs — "slow" and "skips entries" — and is one.

Late binding removes the work rather than optimising it:

```
invalidate-and-rebuild   O(transcript)   per switch
late binding             O(distinct styles) per switch
```

`StylePool::retheme()` re-derives the cached SGR bytes for each interned
style and, in its own words, *"ids stay valid and no canvas cell needs
rewriting."* Stored `Element`s, component caches and cell caches all stay
correct, untouched. **Nothing was baked, so there is nothing to invalidate.**

This is the CSS-variable discipline, and it is why a browser theme switch
needs no invalidation anywhere.

## 5. How it works here

maya already had every piece:

| Piece | Role |
|---|---|
| `Color` (`Res::Sym`) | symbolic — a tag plus a slot index |
| `LitColor` (`Res::Lit`) | resolved — what a `Theme` field holds, what gets emitted |
| `Color::slot(ThemeSlot)` | names a colour **without** naming a theme |
| `Style::fg` / `bg` | `std::optional<Color>` — symbolic, by design |
| `Style::to_sgr()` | resolves via `live_color_resolver()` **at emit time** |
| `StylePool::retheme()` | re-derives SGR bytes on a swap; ids stay valid |

The type system was already telling the truth. Two call sites just opted out
of it.

One consequence worth stating: `Color::slot()` touches no `Theme`, so it is
safe on the detached markdown parse worker. The original reason the palette
held `LitColor` — *"the parse worker cannot reach a Theme"* — was real when
written and is dissolved by slots.

## 6. Why a trait and not a rule

maya had already solved this bug class **three times, well** — and every
solution was opt-in:

- **`visual.hpp`'s `mix_any`** derives a hash *from the type*, so a field
  added tomorrow is hashed tomorrow, and an unknown type does not compile.
- **`theme::projected<P>()`** makes deriving *be* the read path, so "forgot
  to subscribe" is not expressible.
- **`Color` vs `LitColor`** puts resolution state in the type.

Each is excellent. None was mandatory. The bug walked in through the gap.

`visual.hpp`'s own header describes this exact failure — *"a PARALLEL
DESCRIPTION of the view's dependency set with nothing keeping the two
equal"* — and fixes it by inverting the dependency so the hash derives from
the type. That is precisely the cure the block cache needed; it just was not
applied there.

So the answer was never a fourth mechanism. It was: **make the existing
discipline unavoidable.**

`maya/style/binding.hpp` does that. A `LitColor` reaching storage that
outlives a frame is now a compile error:

```
error: static assertion failed: decltype(BadPalette::text) stores a RESOLVED
colour (LitColor), so anything built from it is pinned to whatever theme was
live at build time and cannot follow a theme switch. Store a symbolic `Color`
(Color::slot(...)) instead and let paint resolve it. If you genuinely need
channels, resolve locally and keep the result short-lived — never in storage.
```

The message is an instruction, not a diagnosis. The person who trips it is
mid-edit and needs to know what to do.

Asserted at the seams that matter: `Style::fg` / `Style::bg` (every stored
`Element` carries Styles), the markdown `Palette` (where the shipped bug
lived), and `ui::Slot`'s `Color` conversion.

## 7. The rules

- **Build-time code** — anything producing an `Element`, a `Config`, a
  `Style` — names a **slot**. It never calls `resolve()`.
- **Paint-time code** — `to_sgr`, the renderer, `StylePool` — resolves. It is
  the only layer allowed to.
- **Need channels?** (a blend, a contrast ratio) Resolve explicitly and keep
  the result **short-lived**. Putting it in anything stored is the bug.
- **Adding a widget?** Name a token (`ui::accent`), never a literal.
  `theme_discipline_test` greps for literals; `binding.hpp` catches resolved
  colours in storage. Between them there is no third way to get this wrong.

## 8. Still open

**A `PaintScope` witness.** Today `resolve()` is *discouraged* above paint.
It could be made *impossible*: make it take a token constructible only inside
the renderer, and build-time code cannot call it at all — no argument to
pass. That is the strongest form (remove the capability, don't police it),
but it touches every resolve site, so it is deliberately not bundled here.

## 9. The general lesson

Ask of every cached value: **what is this a function of?** Then make the key
that exact set, *mechanically* — never by hand-listing fields, because
hand-listed keys rot the moment someone adds an input.

And prefer deleting the cache. The fix that works is usually the one that
**deletes more than it adds**: this change removed the `parsed_blocks`,
`built_theme_epoch_` and per-block re-render machinery an earlier attempt
introduced, and ended up with fewer moving parts than before the bug was
found.
