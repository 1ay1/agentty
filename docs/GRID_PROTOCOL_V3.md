# Grid protocol v3 — design notes

Goal: make the grid wire format **self-describing and host-agnostic**, so a
non-terminal host (a VS Code webview building DOM, a GPUI view, a canvas) can
render it **correctly by construction** — without independently reimplementing
maya's wcwidth tables, palette resolution, or scrollback semantics.

This is a strict superset negotiated by version: hosts that only speak v2 keep
working; a host advertises `AGENTTY_GRID_PROTO=3` (or similar) and maya emits v3.

---

## What made v2 hard for a DOM host (the friction, concretely)

1. **Width is implicit.** `run.len` counts codepoints but `run.col` is a display
   column. The host must run its OWN wcwidth to advance columns, and it must
   match maya's exact Unicode tables or every run after a wide/zero-width glyph
   drifts and overruns the row. (Cost us an 84k-codepoint table port.)

2. **Resize semantics are implicit.** A `Resize` frame states the new height but
   NOT that survivors are preserved and won't be re-stated. Reallocating on
   resize silently wiped content.

3. **No document/viewport contract.** Nothing says "this is a growing document,
   render it all and scroll" vs "this is a fixed viewport." The host reverse-
   engineers it from row growth.

4. **Colors are unresolved.** `Named16` / `Indexed256` require the host to own a
   palette; theme-matching is guesswork.

5. **Overloaded fields.** `Commit`'s count rides the `rows` header field; frame
   semantics are spread across prose comments.

---

## v3 changes

### A. Resolve width on the emitter side (biggest win)

maya already knows each cell's display width (it lays the Canvas out with
`unicode::char_width`). Put it on the wire so the host never computes width:

- Each run gains an explicit **`u16 cols`** = the number of GRID COLUMNS the run
  occupies (sum of cell widths). The host advances its column cursor by `cols`,
  not by codepoint count. Wide glyph → the run's `cols` already accounts for 2.
- For a run that contains wide glyphs, the emitter also encodes, per glyph, how
  many columns it spans — OR, simpler and sufficient: the run text is emitted so
  that **each grid column maps to exactly one cell**, with the trailing half of a
  wide glyph sent as an explicit **U+0000 → "continuation" sentinel** the host
  renders as nothing. Then `run.text`'s codepoint count == `run.cols`, and the
  host needs zero width logic.

  Net: the host loop becomes `for each cp in run.text: cell[col++] = cp` with no
  wcwidth at all. This alone removes friction #1 and #4 entirely.

### B. Make frame semantics explicit and self-describing

Add a **surface-model byte** to the header (or a one-time Hello frame):

- `model`: `0 = document` (growing surface, host renders all + scrolls) ·
  `1 = viewport` (fixed height, host shows exactly `rows`).
- On `Resize`: a `preserve` bit — `1` = survivors are kept and NOT re-stated
  (v2's actual behaviour, now explicit); `0` = the following Full re-states all.

Give `Commit` its own explicit `u16 count` field instead of overloading `rows`.

### C. Resolve colors to RGB (with a fallback tag)

Emit colors as **RGB triples resolved against the ACTIVE theme** maya already
knows, PLUS a `role` tag (e.g. `fg.default`, `accent`, `error`) so a themed host
can re-map if it wants. Named16/Indexed become just an optimisation the emitter
may still use, but the host is never REQUIRED to own a palette.

### D. A `Hello` frame (protocol handshake)

First frame maya emits announces: `proto_version`, `model`, `cell semantics`
(width-resolved? colors-resolved?), and any capability flags. The host reads one
frame and knows exactly how to interpret the rest — no guessing, no version
sniffing mid-stream.

### E. Keep the good parts of v2

- APC + u32 length prefix framing (binary-safe, split-safe). ✓
- Per-frame deduped style table (interning is cheap and correct). ✓
- Diff = changed rows only; Full = whole surface. ✓
- OSC 5379 follow-along hint. ✓

---

## Header, v3 (sketch)

```
frame := header [style_table] [runs] [cursor]

header:
  u8  ver      = 3
  u8  type     0 DIFF · 1 FULL · 2 RESIZE · 3 CURSOR · 4 CLEAR · 5 BELL
               · 6 COMMIT · 7 HELLO
  u16 flags    bit0 style_table · bit1 cursor · bit2 preserve (resize)
               · bit3 width_resolved · bit4 color_resolved
  u8  model    0 document · 1 viewport            (meaningful on HELLO/RESIZE)
  u16 cols
  u16 rows
  u16 base_row
  u16 count    (COMMIT: rows scrolled off; else 0)

run (width-resolved form):
  u16 row · u16 col · u16 cols · u16 style_id · <utf8, one entry per column,
  wide-glyph trailing column = U+0000 sentinel>
```

The host render loop, v3:

```
col = run.col
for cp in decode_utf8(run.text):
    if cp == 0: col++; continue        # wide-glyph continuation, render nothing
    grid[run.row][col] = { ch: cp, style: run.style }
    col++
```

No wcwidth. No palette. No implicit resize rules. Correct by construction.

---

## Migration

1. Add the v3 emitter behind a capability check; keep v2 as the default.
2. agentty-vscode advertises v3, drops its ported width tables + palette guessing.
3. agentty-mode (Emacs) can stay on v2 (its C module already has wcwidth), or
   adopt v3 to shed its own width code too.
