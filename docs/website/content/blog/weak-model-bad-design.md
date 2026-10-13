---
title: "If a weak model can't implement your feature, your design is bad"
date: 2026-09-17
author: agentty
tags: [design, llm, api-design, coupling, deep-dive]
excerpt: "We read 'the small model couldn't do it' as a fact about the model. It is usually a fact about the code. A weak model is an analyzability probe — it fails exactly where correct usage requires knowledge that isn't in the code, and Page-Jones, Berners-Lee and Parnas all told us why that knowledge is the expensive kind."
---

# If a weak model can't implement your feature, your design is bad

There is a reflex I want to argue against. You hand a task to a small
model, it produces something broken, and you conclude: *I need a better
model.* Sometimes that is true. Often it is a misread of what just
happened.

A language model writing code against your API has exactly one advantage
over a new hire: it has read more code than any human ever will. It has
exactly one disadvantage: it cannot ask you a question, cannot read your
team chat, and has no memory of the incident that made you write that
function the way you did.

So it is a very specific instrument. It fails precisely where **correct
usage requires knowledge that is not present in the code.** That is not
a model limitation to route around. It is a measurement — and it is one
the field has wanted for fifty years without having a cheap way to take
it.

## The tell

Here is the shape. You ask for something small. The model writes code
that compiles, passes review at a glance, and is wrong. When you look at
why, the answer is always some version of:

> *Well, you have to know that…*

Stop there. That sentence is the finding. Everything after "you have to
know that" is a fact your API required and did not supply, and the model
just demonstrated it is not recoverable from the code.

A strong model often *does* know — it has seen a thousand codebases with
the same trap and pattern-matched past yours. That is not a reason to be
pleased. It means your API is survivable only by someone carrying a
thousand codebases of context, which is a poor property to ship.

## A real one

Here is a bug we shipped. I want to be specific, because the abstract
version of this argument is worthless — and you should not need to know
anything about our codebase to follow it.

Some background, briefly. Terminals do not accept arbitrary colours.
They accept *three different kinds* of colour instruction, and this is a
forty-year-old accident rather than a design:

- **truecolor** — "paint red 200, green 40, blue 90." Actual numbers.
- **palette** — "paint colour number 8." The terminal decides what 8
  looks like. Yours might be grey; mine might be pink. There are 16 of
  these, and the user configures them.
- **default** — "paint whatever your normal text colour is." No colour
  named at all.

A terminal program has to model all three. We have a type called
`LitColor` that means "a colour resolved and ready to paint," and it can
be holding any of those kinds.

Now: blend two of them, which is what a fade does.

```cpp
LitColor lerp(LitColor a, LitColor b, double t) {
    return LitColor::rgb(mix(a.r(), b.r()),
                         mix(a.g(), b.g()),
                         mix(a.b(), b.b()));
}
```

Read it. It is obviously correct — take the red of each, mix them, same
for green and blue. Any model writes this. So did we.

It painted text you could select but not read.

Because `LitColor` is *paintable* but not necessarily *numeric*. Only
one of its kinds actually has channels:

| Kind | what `r()`, `g()`, `b()` really hold |
|---|---|
| truecolor | real channels |
| palette | a **palette index** in the red byte; green and blue are zero |
| default | nothing at all |

There is only one byte of storage. For a truecolor value it means red.
For a palette value it means *which of the 16*. Same byte, two meanings,
and nothing in the code says which one you have.

Our default theme uses palette colours deliberately — that is how a
user's own carefully-chosen terminal colours show through instead of
being painted over. So the "muted grey" we use for secondary text was
palette index 8, and the blend above read that 8 as a red channel:

```
palette index 8  →  rgb(8, 0, 0)  →  "38;2;8;0;0"
```

Which is near-black. On a black terminal. For every line of secondary
text, in the default theme, for everyone who had not picked a different
one.

## This bug has a name

It is not a novel failure. Meilir Page-Jones catalogued it in 1992 as
**connascence of meaning** — when two pieces of code must agree on the
*meaning* of a value that carries no such meaning in itself.

("Connascence" is his term for "these two things are born together":
change one and you must change the other. It is a finer-grained
vocabulary for what most people call coupling.)

His taxonomy ranks these by how hard they are to fix. Connascence of
*name* is weak: rename a thing and the compiler finds every use.
Connascence of *meaning* is strong, because semantic agreement is
invisible to every tool you own. The canonical example is a function
returning `None` for "not found" and also `None` for "the database
exploded" — same value, two meanings, and only the author knows which is
which.

Our byte was that, in miniature. And Page-Jones' other two axes explain
why it hurt so much:

- **Degree** — how many places share the assumption. Every blend, fade,
  gradient and pulse in the program.
- **Locality** — how far apart they sit. The convention was established
  in one header; the damage happened in files that never mention it.

High degree, poor locality, strongest category. That is a bug you ship
and keep shipping.

## Why the model was always going to fail

Tim Berners-Lee has a principle for this, aimed at web architecture but
general: the **Rule of Least Power**. Given a choice, use the *least*
powerful language adequate for the job — because

> the less powerful the language, the more you can do with the data
> stored in that language.

The reasoning is that power and **analyzability** trade against each
other. A declarative format can be indexed, checked, transformed and
reasoned about by tools that never anticipated your use. Arbitrary code
can do anything, which means nothing outside it can say anything about
it. The W3C finding puts it sharply: data in a Turing-complete language
is data no analyzer can understand.

Now look at what our API offered:

```cpp
uint8_t r()     const;   // "red channel"
uint8_t index() const;   // "palette index"
```

Both return the same byte. `r()` is right some of the time and compiles
all of the time. The correctness rule — *only call this when the kind is
truecolor* — existed in exactly one place: our heads.

That is a maximally powerful, minimally analyzable interface. It permits
every call and validates none. And a language model is, at bottom, an
**analyzer**: it reads the artefact and predicts what belongs. Give it an
interface that permits everything, and it will confidently write the
thing that was never checked.

The model did not fail to understand our code. It understood it exactly
as written. We had written the wrong thing.

## The fix is the point

The interesting part is what fixing it looked like. Not "be careful with
`r()`." The type grew a question it could answer for itself:

```cpp
bool has_channels() const { return kind == Kind::Rgb; }

LitColor lerp(LitColor a, LitColor b, double t) {
    if (!a.has_channels() || !b.has_channels())
        return t < 0.5 ? a : b;        // snap; never invent a triple
    /* … mix channels … */
}
```

In connascence terms, meaning was promoted to **name**. The rule stopped
being semantic agreement and became a symbol the compiler, the grep, and
the model can all see. Degree collapses, because you no longer need the
rule in every blend — you need the *call*, and calls are visible.

Note the failure behaviour too. When the blend cannot be computed it
returns an endpoint unchanged. **An effect that cannot be computed
becomes no effect, never a computed wrong answer** — a fade that
silently doesn't fade is a cosmetic disappointment; a fade that invents
a colour is invisible text.

This is also David Parnas' 1972 argument, which is older than most of us
and still the whole game. Modules should hide *decisions likely to
change*, and expose interfaces that do not leak them. "Which byte means
what, depending on kind" is precisely such a decision. We had published
it as two accessors and hoped.

## The stronger claim

I would go further than "weak models find bad designs." I think they
find them *better than code review does*, for a slightly uncomfortable
reason:

**A reviewer shares your context.** They were in the meeting. They
remember the incident. They read `r()` and their eyes slide over it,
because they know. That knowledge makes them worse at this particular
job — it is exactly the thing under test, and they have it.

This is why fresh eyes find different bugs than expert eyes, and why the
best bug reports come from people who have used your software for a
week. A weak model is a reviewer with your entire codebase and none of
your history, available on demand. That combination did not previously
exist.

It also explains why documentation is a weak substitute. A doc says
"remember to check the kind." The model must read the doc, connect it to
this call site, and apply it — three steps, each optional, each skipped
under pressure. A type that will not compile is one step and mandatory.
Berners-Lee again: prefer the construct that can be *checked* over the
one that must be *understood*.

## What this does not mean

I am not claiming every failure is your fault. The honest boundaries:

**Genuinely hard problems are genuinely hard.** If a task needs an
insight — a novel algorithm, a real trade-off between two goods — a
small model failing tells you about the task, not the design. Be careful
here though: far less code is *genuinely* hard than we like to believe.
"Hard" is frequently "underspecified," and underspecified is a design
property.

**Large context is not automatically a flaw.** Some changes touch forty
files because the feature spans forty files. But if a *small* change
needs forty files of context to be made safely, that is Page-Jones'
degree-and-locality problem wearing a hat, and it is the finding.

**Some failures are just failures.** Models hallucinate APIs and lose
the thread. The signal is not one bad attempt — it is *repeated,
consistent* failure at the same spot, and especially failures that look
plausible. A confidently wrong answer means the wrong thing looked
right, and "looked right" is a property of your API, not of the reader.

**Least power has limits.** Sometimes you need the powerful construct,
and a rule that forbids it is worse than the trap. The point is not to
always choose weakness; it is to know you are spending analyzability
when you choose power, and to have gotten something for it.

## How to actually use this

Nothing elaborate:

1. Hand a real task to a model deliberately weaker than your daily one.
2. Don't help. No hints, no "remember that we…". **The hints are the
   data.**
3. When it fails, write down the sentence starting "you have to know
   that."
4. Fix the *design* so that sentence is unnecessary. Then throw the
   model's code away — you were never trying to keep it.

The output of this exercise is not code. It is a list of things your
codebase required and did not say. That list is also, not
coincidentally, your onboarding backlog.

## The uncomfortable bit

This works not because models are clever, but because they are
**relentlessly literal**. They do exactly what the interface appears to
license, at scale, without the instinct that makes a careful human pause
at a line that *feels* off.

Your codebase is full of lines that feel off to you and read fine to
everyone else. That feeling is undocumented knowledge. It does not
survive you changing teams, and it does not survive a long enough
weekend.

We found seven bugs in one session this way. Not because anything was
clever — because the obvious thing kept getting written, and the obvious
thing kept being wrong.

That was never a fact about the model.

---

*Further reading: Page-Jones on [connascence](https://connascence.io/)
(1992); the W3C TAG finding on the [Rule of Least
Power](https://www.w3.org/2001/tag/doc/leastPower.html) (2006); Parnas,
["On the Criteria To Be Used in Decomposing Systems into
Modules"](http://sunnyday.mit.edu/16.355/parnas-criteria.html) (1972) —
fourteen pages, still the best thing written on the subject. The colour
bug is [agentty #45](https://github.com/1ay1/agentty/issues/45), and the
fix is in [maya](https://github.com/1ay1/maya), the TUI engine
underneath it.*
