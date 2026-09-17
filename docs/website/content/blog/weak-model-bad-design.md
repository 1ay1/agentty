---
title: "If a weak model can't implement your feature, your design is bad"
date: 2026-09-17
author: agentty
tags: [design, llm, api-design, deep-dive]
excerpt: "We keep treating 'the small model couldn't do it' as a fact about the model. It is usually a fact about the code. A weak model is a design review that runs in thirty seconds — it fails exactly where your API requires knowledge that isn't in the API, and that missing knowledge is what your next hire will miss too."
---

# If a weak model can't implement your feature, your design is bad

There is a reflex I want to argue against. You hand a task to a small
model, it produces something broken, and you conclude: *I need a better
model.* Sometimes that is true. Often it is a misread of what just
happened.

A language model writing code against your API has exactly one advantage
over a new hire: it has read more code than any human ever will. It has
exactly one disadvantage: it cannot ask you a question, cannot read your
Slack, and has no memory of the incident that made you write that
function the way you did.

So it is a very specific instrument. It fails precisely where **correct
usage requires knowledge that is not present in the code**. That is not
a model limitation to route around. That is a design review, and it runs
in thirty seconds.

## The tell

Here is the shape. You ask for something small. The model writes code
that compiles, passes review at a glance, and is wrong. When you look at
why, the answer is always some version of:

> *Well, you have to know that…*

Stop there. That sentence is the finding. Everything after "you have to
know that" is a fact your API required and did not supply, and the model
just proved it is not discoverable from the code.

A strong model often *does* know — because it has seen a thousand
codebases with the same trap and pattern-matched past yours. That is not
a reason to be pleased. It means your API is survivable only by someone
with a thousand codebases of context, which is a bad property to ship.

## A real one

Here is a bug we shipped in agentty. I want to be specific, because the
abstract version of this argument is worthless.

agentty renders in the terminal. Colours resolve through a theme into a
`LitColor` — a colour ready to paint:

```cpp
LitColor c = theme.resolve(Color::slot(ThemeSlot::Muted));
```

Now blend two of them, which is what a fade does:

```cpp
LitColor lerp(LitColor a, LitColor b, double t) {
    return LitColor::rgb(mix(a.r(), b.r()),
                         mix(a.g(), b.g()),
                         mix(a.b(), b.b()));
}
```

Read that. It is obviously correct. Any model would write it. So did we.

It painted text you could select but not read.

The reason is that `LitColor` is *paintable* but not *numeric*. It has
five kinds, and only one of them has channels:

| Kind | what `r()`, `g()`, `b()` actually hold |
|---|---|
| `Rgb` | real channels |
| `Named` | a **palette index** in `r_`; `g_`/`b_` are zero |
| `Indexed` | a **palette index** in `r_`; `g_`/`b_` are zero |
| `Default` | nothing — it means "the terminal's own colour" |

Our default theme states its slots as `Named`, deliberately, so the
user's own terminal palette shows through. So `bright_black` is
`Named(8)`, and the blend above read that 8 as a red channel:

```
bright_black → Named(8) → rgb(8, 0, 0) → "38;2;8;0;0"
```

Near-black. On a black terminal. For every line of every reasoning
block.

## Whose fault is that?

Tempting to say the caller should have checked the kind. But look at
what the type offered:

```cpp
uint8_t r()     const { return r_; }   // "red channel"
uint8_t index() const { return r_; }   // "palette index"
```

**The same byte, behind two names, and nothing to tell you which one
applies.** `r()` is right some of the time. It compiles all of the time.
No signal at the call site, no diagnostic, nothing in the signature. The
only thing between you and the bug was knowing.

A weak model doesn't know. Neither does a new hire. Neither did we — we
wrote it.

## The fix is the point

The interesting part is what fixing it looked like. Not "be careful with
`r()`." The type grew a question it could answer for itself:

```cpp
bool has_channels() const { return kind_ == Kind::Rgb; }

LitColor lerp(LitColor a, LitColor b, double t) {
    if (!a.has_channels() || !b.has_channels())
        return t < 0.5 ? a : b;        // snap; never invent a triple
    /* … mix channels … */
}
```

Now the hazard is *in the code*. A model reading `has_channels()` in one
blend will write it in the next, because local imitation is the thing
models are best at. The knowledge moved out of our heads and into the
file.

That is the whole move. **Every "you have to know that" is state living
in someone's head, and the fix is to move it into the artefact** — a
guard, a named type, an assertion, a function that cannot be called
wrong.

## The stronger claim

I would go further than "weak models find bad designs." I think they
find them *better than code review does*, for a slightly uncomfortable
reason:

**A reviewer shares your context.** They were in the meeting. They
remember the incident. They read `r()` and their eyes slide over it,
because they know. That knowledge makes them worse at this specific job
— it is exactly the thing being tested, and they have it.

A weak model is a reviewer with your codebase and none of your history.
That is an unusual and useful thing to be able to summon on demand.

Same logic explains why documentation is a weak substitute. A doc says
"remember to check the kind." The model must read the doc, connect it to
this call site, and apply it — three steps, each optional. A type that
won't compile is one step and mandatory.

## What this does not mean

I am not claiming every failure is your fault. The honest boundaries:

**Genuinely hard problems are genuinely hard.** If a task needs an
insight — a novel algorithm, a real trade-off — a small model failing
tells you about the task, not the design. Be careful here though: much
less code is *genuinely* hard than we like to believe. "Hard" is often
"underspecified," which is a design property.

**Large context isn't a flaw by itself.** Some changes touch forty files
because the feature does. But if a *small* change needs forty files of
context to be safe, that coupling is the finding.

**Some failures are just failures.** Models hallucinate APIs and lose
the thread. The signal isn't one bad attempt — it is *repeated,
consistent* failure at the same spot, and especially failures that look
plausible. A confidently wrong answer means the wrong thing looked
right, and that is a property of your API.

## How to use it

Nothing elaborate:

1. Hand a real task to a model deliberately weaker than your daily one.
2. Don't help. No hints, no "remember that we…". The hints are the data.
3. When it fails, write down the sentence starting "you have to know
   that."
4. Fix the *design* so that sentence is unnecessary. Then throw the
   model's code away — you were never trying to keep it.

The output is not the code. The output is the list of things your
codebase required and did not say.

## The uncomfortable bit

This works not because models are clever, but because they are
**relentlessly literal**. They do exactly what the API appears to
license, at scale, without the instinct that makes a careful human pause
at a line that *feels* off.

Your codebase is full of lines that feel off to you and read fine to
everyone else. That feeling is undocumented knowledge, and it does not
survive you changing teams — or, frankly, a long enough weekend.

We found seven bugs in one session this way. Not because anything was
clever. Because the obvious thing kept being written, and the obvious
thing kept being wrong.

That was never a fact about the model.
