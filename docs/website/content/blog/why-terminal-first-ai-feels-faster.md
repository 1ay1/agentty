---
title: "Why a terminal UI can out-render a browser: packed cells, SIMD diffing, and zero allocations"
date: 2026-08-05
author: agentty
tags: [terminal, performance, deep-dive, rendering]
excerpt: "A terminal frame is a grid of a few thousand cells, not a DOM tree — and that difference in data structure is why a well-built TUI renderer can diff, encode, and flush a frame with zero heap allocations and SIMD-width cell comparisons. Here's the actual rendering pipeline: 64-bit packed cells, open-addressing style interning, AVX/NEON row-skip, and why there's no GC pause waiting to happen."
---

# Why a terminal UI can out-render a browser

Not "terminal apps feel snappy" as received wisdom — the actual mechanics
of *why*, from the render loop of a TUI engine I work on
([maya](https://github.com/1ay1/maya)) that pushes real coding-agent
sessions to the screen at a few thousand frames without a stutter. None of
this is exotic; it's the natural consequence of what a terminal frame
*is* as a data structure, compared to what a browser has to maintain.

## The whole screen is a flat array

A terminal is `rows × cols` cells, and nothing else. No box model, no
flex/grid layout, no z-index stacking contexts, no CSS cascade to
resolve. So the canvas backing every frame is exactly that: a flat array
of packed 64-bit integers, one per cell —

```cpp
// Canvas is the rendering surface: a width x height grid of packed 64-bit
// cells. Each cell holds a Unicode code point, a style ID (interned via
// StylePool), a hyperlink ID, and a width marker for CJK/wide characters.
//
// The 64-bit packing enables O(1) cell comparison in the diff algorithm --
// two cells are identical iff their packed representations are equal.
```

A code point, a style ID, a hyperlink ID, and a wide-character marker,
bit-packed into one `uint64_t`. That one design decision is what makes
everything downstream cheap: comparing "did this cell change" is a single
integer equality check, not a deep-equal over a style object graph.

## Style comparison without comparing styles

A `Style` (foreground, background, bold, underline, ...) is a real struct
with several fields — expensive to compare field-by-field on every cell,
every frame. So styles aren't stored per-cell at all; they're interned
once into a pool and referenced by a 16-bit ID, open-addressed by hash:

```cpp
[[nodiscard]] MAYA_ALWAYS_INLINE uint16_t intern(const Style& s) {
    std::size_t h = hash_style(s);
    std::size_t idx = h & mask_;
    while (true) {
        auto& slot = slots_[idx];
        if (slot.hash == 0) [[unlikely]] {
            // Empty slot — insert new style.
            ...
```

Two cells with the same visual style always carry the same 16-bit ID, so
"did the style change" becomes another integer comparison, and transitioning
the terminal from one style to another becomes a lookup of a pre-built
escape sequence rather than re-deriving SGR codes on the fly:

```cpp
// Generates the minimal escape sequence to transition from one Style to
// another. This is critical for rendering performance: instead of resetting
// and re-applying every attribute on every cell, we only emit what changed.
```

Even the *overflow* case is deliberate: the ID space is a `uint16_t`
(65,535 styles), and if something pathological — an animation minting a
fresh color shade every frame, say — blows through that budget, new
styles collapse to the default style rather than silently aliasing onto
whatever the 65,534th style happened to be. Wrong-but-plain beats
wrong-and-misattributed, and it's a self-announcing failure instead of a
silent one.

## Diffing at SIMD width, not cell-by-cell

Two full-frame `uint64_t` arrays — front (what the terminal currently
shows) and back (what should be shown next) — get compared every frame.
The naive version of that is a scalar loop over a few thousand cells. The
actual implementation compares whole SIMD lanes at once and skips entire
runs of unchanged cells in one instruction:

```cpp
// Hardware-accelerated comparison of packed 64-bit cell arrays. Uses
// AVX-512F (8 cells/cycle), AVX2 (4 cells/cycle), SSE2 (2 cells/cycle,
// x86-64 baseline), or NEON (2 cells/cycle, ARM64). Scalar fallback for
// everything else.
```

Most of a terminal frame is unchanged between renders — a status line
tick, one line of new streamed text, a cursor blink — so a row-skip that
tests eight cells per cycle and moves on the instant they're all equal is
the difference between "diff cost scales with screen size" and "diff cost
scales with what actually changed."

## Writing the diff without allocating

The naive design for "turn a cell diff into terminal output" builds an
intermediate list of operations (move cursor, set style, write chars) and
then serializes that list to bytes. maya's differ skips the intermediate
representation entirely and writes ANSI bytes straight into the output
buffer as it walks the diff:

```cpp
// Key design: instead of building a vector<RenderOp> and then serializing,
// we write ANSI sequences directly into the caller's string buffer. This
// eliminates every heap allocation that the old RenderOp path incurred —
// one per changed cell for cursor positioning, one per style change, one
// per character batch. On a typical 80x24 frame with 10% changed cells,
// that's ~192 fewer allocations per render cycle.
```

~192 allocations is a small number in isolation. At 60fps sustained over
a session, it's the difference between a render loop the allocator never
has to think about and one that's doing real, avoidable work every 16ms —
work that, on a garbage-collected runtime, eventually has to be *collected*
too, on its own schedule, possibly mid-frame.

## No GC pause waiting to happen

That last point is the structural one, and it's not about any particular
language being "faster" — it's about what's *absent*. A render pipeline
built on manually-managed, stack- and pool-allocated buffers has nothing
for a garbage collector to walk, because there is no garbage collector.
There's no equivalent of a DOM node tree retained across frames that has
to be walked, diffed by a separate reconciler, and then thrown away. The
only persistent state is two flat arrays of integers and a style pool,
and the only per-frame work is: compare arrays at SIMD width, write bytes
for what changed, swap front and back. Nothing in that loop can pause
unpredictably, because nothing in that loop defers work to a scheduler
that isn't the one you wrote.

## The general point

None of the individual techniques here are novel — packed representations,
interning, SIMD comparison, and zero-allocation serialization are all old
ideas. What's worth noticing is how well they compound specifically for a
*terminal* frame: it's small (a few thousand cells, not an arbitrarily
deep DOM), flat (no layout pass, because a character grid has no box
model), and already discrete (cells either match or they don't — no
sub-pixel anti-aliasing, no partial repaint of a shape). A rendering
target that constrained is exactly the case where "compare integers, skip
what's equal, allocate nothing" stops being a micro-optimization and
becomes the entire architecture.

[Source](https://github.com/1ay1/maya) — `include/maya/render/canvas.hpp`
and `include/maya/render/diff.hpp` are short enough to read end to end in
one sitting, packed-cell layout and all.
