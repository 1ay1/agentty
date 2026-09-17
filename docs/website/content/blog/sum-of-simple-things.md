---
title: "The sum of simple things should still be simple"
date: 2026-09-18
author: agentty
tags: [design, simplicity, architecture, deep-dive]
excerpt: "Simple does not mean small, and it does not mean fewer features. It means one fold. Which gives you a test most codebases fail: if you add up parts everyone agrees are simple and the result is complicated, then at least one of those parts was never simple — you just had not looked at where it touches the others."
---

# The sum of simple things should still be simple

Here is a claim I want to defend, because I think it is both obvious
once stated and almost never acted on:

> **Simple things add up to simple things. If your sum is complicated,
> something you added was not simple.**

That reads like a tautology. It is not. It is a *test* — and it is
sharper than almost anything else we have, because it catches the
failure mode that every other measure of simplicity misses.

Most teams never run it. They audit parts. Every part passes. The whole
is a swamp, and nobody can say which part is guilty, so they conclude
that complexity is emergent, inevitable, the cost of doing business.

It usually isn't. Complexity is rarely emergent. It is usually *smuggled*
— and the test tells you where to look.

## Simple is not small

First we have to kill the synonym that ruins the conversation.

Rich Hickey made the point definitively in **Simple Made Easy**: simple
comes from *sim-plex*, "one fold" or "one braid." Its opposite, complex,
means *braided together*. The word has nothing to do with size, count,
effort, or familiarity.

That last one matters most. **Simple is objective; easy is subjective.**
Easy means "near to my hand" — near my experience, my tooling, my
habits. A German speaker finds German easy. That tells you nothing about
German's structure. And Hickey's blade: we constantly choose easy,
mislabel it simple, and pay for the complexity later.

So the thesis is not "have fewer features." A program can have four
hundred features and be simple, if each one folds once. A program can
have three features and be a nightmare, if all three are braided into
the same mutable state.

Size and simplicity are independent axes. We conflate them because small
things are *easy to inspect*, and we mistake inspectability for
simplicity — which is precisely Hickey's error, one level up.

## Where the argument gets sharp

If simple means one fold, then here is the interesting consequence.

Folds are a property of a *thing*. But when you put two things together,
you create something that did not exist in either: **the relationship.**

And the relationship is where the extra folds hide. Not in either part.
In the join.

So "is this simple?" is not a question you can answer by looking at a
part in isolation — which is exactly what code review does, and exactly
what a unit test does. Both examine the part. Neither examines the join.

That is why the sum test has teeth. It is the only cheap way to
interrogate the joins, because **a bad join has nowhere to hide once you
add things up.**

## What "not simple" actually looks like

When you compose two simple things and get something complicated, the
guilty part is almost always guilty in one of four ways. These are worth
naming, because once you have the names you spot them in seconds.

**1. It has a hidden fold.** The part looks like it does one thing and
actually does two, with the second one invisible from outside. A
function that computes *and* caches. A parser that parses *and* logs. A
type that carries a value *and* a mode. On its own it seems fine,
because the second fold is unexercised. Compose it and the second fold
starts touching things.

**2. It leaks its implementation.** The interface describes less than
the thing actually requires. Callers end up needing knowledge that lives
outside the signature — call this before that, only on the main thread,
never with an empty list. Ousterhout calls this a **shallow module**:
one whose interface is nearly as complicated as its implementation, so
it hides nothing.

The deep/shallow distinction is the key one, because it explains why
splitting a big thing into many small things often makes life *worse*.
You added interfaces without hiding anything. Every new boundary is new
surface for callers to learn, and the complexity you were chasing just
got redistributed into the seams between your tidy little pieces. Small
is not simple. Small can be *many-folded*, and usually is.

**3. It is simple in the wrong dimension.** It folds once along an axis
nobody needed, and many times along the one that matters. A cache keyed
perfectly by time and arbitrarily by identity. A permissions model
elegant per-user and incoherent per-resource. This one is vicious
because the part is genuinely, demonstrably simple — just not about the
thing you have to compose along.

**4. It shares a substrate.** Two parts that never mention each other,
both touching the same global, the same clock, the same connection pool,
the same mutable buffer. Neither is braided *in its own file.* They are
braided through a third thing, and you will not see it by reading either
one.

Notice what all four have in common. **They are invisible while the part
is alone.** That is the entire reason the sum test earns its keep.

## The canonical counter-example, and why it agrees

The obvious objection: Unix. Thousands of tiny tools composing into
arbitrary pipelines. Surely that proves small things just *do* compose?

Read McIlroy's formulation again, and notice it is three clauses, not
one:

> Write programs that do one thing and do it well. Write programs to
> work together. **Write programs to handle text streams, because that
> is a universal interface.**

The third clause is doing nearly all the work, and it is the one people
drop when they quote it.

Unix does not compose because its parts are small. It composes because
somebody paid for a **universal interface** up front, then refused to
let anything escape it. Text streams are the fold. Every tool folds once
— against the stream — so composing two tools creates no new
relationship to reason about. The join was pre-decided.

That is the general shape of every system that actually composes:
somebody found the one interface, paid the cost of making everything
speak it, and held the line. Not "make the pieces small." **Make the
joins uniform, so the sum has the same shape as the parts.**

And where Unix stops composing is exactly where that breaks down. The
moment a tool needs structured data — not lines, but records with types
— the universal interface stops fitting, and you get `awk` incantations,
fragile field-splitting, and the thirty-year argument about whether
shells should carry objects. The philosophy did not fail. Its *fold* met
data it could not fold once.

## Why this is not the same as "complexity is emergent"

There is a fatalist reading of Brooks I want to argue against.

In *No Silver Bullet*, Brooks says something that sounds like it
contradicts everything above:

> In most cases, the elements interact with each other in some nonlinear
> fashion, and the complexity of the whole increases much more than
> linearly.

He is right. But look at what he attributes it to: **the elements
interact.** The nonlinearity lives in the interactions — in the joins —
not in the elements. Brooks is describing what happens when the joins
are unmanaged. He is not saying joins cannot be managed.

The distinction he actually draws is **essential** versus **accidental**
complexity. Essential complexity is the problem's own irreducible
difficulty. Accidental is what our tools and representations add.

Here is my sharpening of it, and the practical core of this post:

> **Essential complexity does not compound. Accidental complexity does.**

Essential complexity is additive. Two genuinely hard requirements are two
hard requirements. But accidental complexity *multiplies*, because it
lives in the joins, and joins grow with the square of the parts.

Which gives a rule of thumb worth more than most metrics:

- Sum grew roughly **linearly** → you are paying essential cost. Fine.
- Sum grew **superlinearly** → you are paying for joins. Something in
  there was not simple, and Brooks' nonlinearity is a symptom, not a law
  of nature.

Brooks says there is no silver bullet. He does not say there is no
cause.

## Running the test

It is a design review that takes an afternoon.

**Step one — find the pain.** Something in the system is disliked.
Changes there always break something. Nobody volunteers for it.

**Step two — decompose it, honestly.** Write down the parts. For each
one ask: *does this fold once?* Not "is it short." Not "do I understand
it." How many independent things does it braid?

**Step three — notice that every part passes.** It always does. This is
the moment teams give up and say "well, it's just complex." Don't.

**Step four — stop looking at parts and look at joins.** For each pair
that touches: what do they have to agree on? Every agreement is a fold.
List them. The list is your answer.

That list has a shape you will recognise immediately — it is a pile of
sentences starting *"you have to know that…"*. Knowledge required by the
composition and stated nowhere in it. (I wrote about
[that specific smell](/blog/weak-model-bad-design) recently: it is also
exactly where language models fall over, which makes them a fast way to
find these.)

**Step five — fix the part, not the sum.** This is where the discipline
matters, because fixing the sum is so much easier. Add a coordination
layer. Add a manager class. Add a facade that hides the mess.

Every one of those is a new part whose job is to absorb a bad join. It
works, briefly. And you have now made the thing *bigger* and *more
braided* to make it *look* simpler from one angle — which is the
accidental-complexity engine running in reverse. The join is still
there. You wrapped it.

## The uncomfortable implication

If you take this seriously, one thing follows that most codebases are
not ready for:

**"We'll refactor it later" is usually false, and this explains why.**

You can refactor a part later. Parts are local; the blast radius is the
file. But you cannot easily refactor a *join*, because a join is an
agreement between two things, and by the time you notice it, other
things have joined the agreement. That is Page-Jones' *degree* — how
many places share the assumption — and degree only ever goes up.

So the cost of a bad part is linear in its size, and the cost of a bad
join is quadratic in its age. Which means the sum test is not a
code-quality exercise. It is an early-warning system, and it stops
working roughly when you most want it.

## The one-sentence version

Simple means one fold. Folds hide in joins, not in parts. Adding up
things that each fold once must give you something that folds once — so
when the sum is a mess, stop blaming emergence and go find which part
was lying.

It is almost always the one everybody agreed was fine.

---

*Sources worth your time: Rich Hickey,
[Simple Made Easy](https://www.infoq.com/presentations/Simple-Made-Easy/)
(2011) — the talk this whole post is downstream of; Fred Brooks,
[No Silver Bullet](https://www.cgl.ucsf.edu/Outreach/pc204/NoSilverBullet.html)
(1987), for essential vs accidental; John Ousterhout,
*A Philosophy of Software Design* (2018), for deep vs shallow modules
and why splitting things up can make them worse; and Doug McIlroy's
[Unix philosophy](https://en.wikipedia.org/wiki/Unix_philosophy) — all
three clauses of it.*
