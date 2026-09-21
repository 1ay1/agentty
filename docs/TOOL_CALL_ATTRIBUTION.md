# Attribution: which call do these bytes belong to?

The rule, and why it took four bugs across two decoders to state it.

> **Identity is `(id, index)` jointly. A chunk carrying neither FAILS
> rather than guesses.**

---

## 1. The problem

A streaming chat protocol spreads one tool call across many chunks, and
several calls can be in flight at once. Attributing a chunk to a call *is*
the job. Chat Completions gives you two handles and neither is identity:

| handle | how it breaks |
|---|---|
| `id` | absent on continuations; restated as `""`; repeated across parallel calls |
| `index` | absent entirely; **reused** once the call at that index finishes |

Every harness that treated one as identity shipped the same bug:
openai-python #3377 (keyed on arrival order), opik #8360, strands #3950,
crewAI, azure-ai, Theia, llm-gateway #136, Zed #42584.

We shipped it twice.

## 2. Why guessing is worse than failing

This is the part that isn't obvious, and it's why "just pick the most recent
call" feels reasonable and is not.

**A wrong guess produces valid JSON.** Append one call's bytes to another and
you get an object that parses, passes schema validation, and dispatches. So
`edit` runs with a path that belonged to `shell`, and nothing looks wrong
until it has already written.

**A refusal produces a visible failure.** Drop the fragment and the turn fails
on a missing required field. One broken turn, clearly attributed.

Nothing about the wire lets you avoid choosing between those. The second is
strictly better, and it is the only one that stays true when you are wrong.

## 3. The four bugs

| # | where | shape | symptom |
|---|---|---|---|
| 1 | chat | fragment equal to a prefix of the buffer | `{"` opening a nested object was dropped as a "retransmission" — issue #48 |
| 2 | chat | `id` restated as `""` on continuations | identity lost → call never closed → turn hangs |
| 3 | chat | `index` reused for a new call | two parallel calls merged, both corrupted |
| 4 | responses | no `item_id`, several calls open | fragment routed by **hash order** to an arbitrary call |

Bug 4 is the instructive one. The fallback was named `latest_tool_item`,
which sounds ordered. It was refreshed from `*open_tool_items.begin()` on an
`unordered_set` — "latest" meant *whichever item the hash yielded*. The name
asserted a guarantee the code did not provide, which is why it survived
review.

## 4. Why a seam

Before: attribution was ~39 lines inlined in a 243-line `handle_delta` that
was also walking JSON and emitting events. Two consequences:

- **Testing needed a full SSE fixture.** Every bug report became a debugging
  session before it became a test.
- **The two decoders drifted.** A fix to one was invisible to the other. Bug
  4 sat in `responses/` while bugs 1–3 were fixed in `openai/`.

After: `wire::ToolCallTracker` (`include/agentty/provider/wire/tool_calls.hpp`)
— no I/O, no JSON, no events. Hand it what the chunk said about identity, it
tells you which call that is:

```cpp
auto who = tracker.attribute({id, index});
if (!who) { /* Ambiguous — drop and log, never guess */ }
```

A bug report is now four lines:

```cpp
auto a = t.attribute(both("call_a", 0));
auto b = t.attribute(both("call_b", 0));   // index reused
CHECK(b->is_new);
CHECK(*b->displaced == a->call);            // and close the old one
```

That ratio is the whole point: the next report becomes a test *before* it
becomes a debugging session.

## 5. The rule, stated for implementers

```
(id, index)  both present  →  id wins. Known id continues that call even if
                              its index moved. Unknown id starts a new call,
                              DISPLACING whoever held that index.
(id, -)      id only       →  match by id, else new call.      (MiniMax)
(-, index)   index only    →  match by index, else new call.   (ordinary)
(-, -)       neither       →  legal iff exactly one call is in flight.
                              Otherwise Ambiguous — fail.
```

Two corollaries that cost us bugs:

- **An empty string is absent, not a value.** `id: ""` must not open a call
  whose id is empty; the end-of-turn sweep skips those, so it never closes.
- **Displacing requires closing.** When an index is reused, the displaced
  call must be closed or it leaves a `tool_use` with no `tool_result` and
  hangs the turn. Splitting without closing trades one bug for a worse one.

## 6. Where the array-position fallback fits

`index` absent does not always mean "no evidence". Chat Completions lists
parallel calls in one delta, and the continuation delta lists their fragments
in the **same order**. Position is then real evidence, and using it is not a
guess.

This was found by regression, not by design: migrating to the tracker
initially dropped it, and `test_sse_parallel_tool_calls_without_index` began
correctly reporting Ambiguous for fragments a well-ordered server *had* given
us enough to place. Correct behaviour, wrong outcome.

The distinction: **position within one payload is stated by the payload.
"Whichever call I saw last" is not.**

## 7. What is pinned

- `tests/tool_call_identity_test.cpp` — 9 cases against the seam, one per
  shape someone actually hit. No SSE, no HTTP.
- `tests/openai_transport_test.cpp` — the same shapes end to end through the
  chat decoder.
- `tests/codex_responses_test.cpp` — both directions of the Responses case:
  ambiguous must drop, single-call must still land.
- `tests/provider_conformance_test.cpp` — hostile framing, asserted against
  every dialect that can express it.

All mutation-verified: reverting a fix fails its test. A test that passes
against the bug it was written for is decoration, and this session produced
two of those before I started checking.

## 8. The general lesson

**A fallback is a guess with a confident name.**

`latest_tool_item` survived because it read as ordered. The fix was not a
better fallback — it was deleting the fallback and making the absence of
evidence representable:

```cpp
enum class AttributionError : std::uint8_t { Ambiguous };
```

When you cannot know, say so in the type. The caller can then tell the user
something true, which is the one thing a silent guess can never do.
