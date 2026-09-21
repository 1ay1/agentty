# The Responses dialect, and what GitHub's own client does

Everything needed to make agentty's Copilot path as robust as the real
client. Written after four bugs on this one transport, and after pulling
the official CLI apart to check the answers.

> **The rule, stated once:**
> On the Responses dialect, a tool call's identity is its `output_index`.
> `item_id` is a hint. A hint that resolves to nothing is noise, not
> authority.

---

## 1. Why this document exists

Copilot was reported broken four times in a row. Each time the symptom was
different, each time the cause was one layer deeper, and each time the
previous fix had been correct but insufficient. That pattern means the
problem was never the individual bugs — it was that nobody had written down
how this dialect actually behaves.

So this is the writeup. It covers what the wire looks like, what goes wrong,
what GitHub's own client does about it, and what we still owe.

The distinguishing feature of every bug here: **the failure was silent.**
A tool ran with `{}` and reported success. A reasoning summary was billed
and discarded. Arguments arrived three times and were dropped three times.
Nothing crashed. That is the class of bug this document is meant to end.

---

## 2. The dialect map

Copilot is not one wire. It routes by model family:

| Model | Wire | Our transport |
|---|---|---|
| `gpt-*` | Responses | `src/provider/responses/codec.cpp` |
| `claude-*` | Anthropic Messages | `src/provider/anthropic/` |

This split is the single most useful fact about Copilot bugs. "Copilot is
broken" is never true on its own — it is broken *on one dialect*, and which
one tells you where to look. A user on Haiku and a user on GPT-5.4 are
exercising entirely different code.

The same split exists in GitHub's client (§5), which is a good sign the
shape is right rather than an accident of our layout.

---

## 3. What the wire actually looks like

### 3.1 Four ways to send the same arguments

A Responses server may deliver one tool call's arguments through any of
these, and all are spec-legal:

| # | Event | Shape | Who does this |
|---|---|---|---|
| 1 | `response.output_item.added` | whole string, up front | some backends |
| 2 | `response.function_call_arguments.delta` | fragments | Codex |
| 3 | `response.function_call_arguments.done` | complete snapshot | **Copilot** |
| 4 | `response.output_item.done` | item snapshot | most |

We handle all four. `wire_fragmentation_test` enumerates the space and
asserts the one property that must hold: *however the server chose to frame
the arguments, the decoded arguments are identical.*

That test exists because an earlier codec test encoded **one** server's
choice — deltas — as though it were the protocol. It passed while Copilot
was completely unusable, because Copilot only ever sends carrier 3. Every
argument-taking tool got `{}`; every zero-argument tool worked. That
asymmetry made it look like a flaky model instead of a dropped event.

**The lesson worth keeping:** a passing test that encodes one server's
choice is worse than no test, because it converts "we don't know" into
"we checked". Test the space, not the transcript.

### 3.2 A fifth carrier we do not handle

GitHub's client deserializes one we don't:

```
response.custom_tool_call_input.delta
```

Today it falls through to `responses.unhandled_event` and the arguments are
dropped — silently, in exactly the way this document is about.

Worse: that path logs through `util::dbglog`, **not** `AGT_LOG(Wire, …)`, so
it does not appear under `AGENTTY_LOG=wire=debug`. A dropped carrier is
invisible on the one channel someone debugging this would be reading. See
§8.

---

## 4. The bug: identity on this wire

### 4.1 What we believed

`item_id` names the item. Argument frames carry `item_id`. Look it up, append
the bytes. That is what the API reference says, and it is correct for every
well-behaved Responses server.

### 4.2 What Copilot actually sends

From the user's log, the line that ended a week of guessing:

```
responses.tool_args_unroutable: item_id=+SqMJNRKVsofdaNo…  known_items=4 bytes=91
responses.tool_closed:          call_id=call_4D9…          args=0
```

Read carefully, three facts fall out:

1. **The server sent the arguments.** `bytes=91` is real content, not an
   empty frame. This was never Copilot withholding anything.

2. **It sent them three times.** 48 unroutable frames for 16 calls, in
   groups of three with identical byte counts — one per carrier. The server
   was being *generous*, and we dropped all three.

3. **The `item_id` was a ~440-character base64 blob** matching nothing the
   server had announced.

That blob is Copilot's **encrypted reasoning content**. Its proxy rewrites
`item_id` on argument frames, and the rewrite does not round-trip. The item
it announced as `fc_abc` refers to itself later as 440 characters of
ciphertext.

This is not our bug. It is ours to survive.

### 4.3 Independently confirmed

Three other projects hit the identical wall:

| Report | Wording |
|---|---|
| `openclaw#72602` | "Encrypted content `item_id` did not match the target item id" |
| `github/copilot-sdk#615` | multi-turn tool calls broken with reasoning enabled |
| `jcode#1336` | landed on output_index-first keying |

When four independent implementations hit the same wall, it is a property of
the wire, not a mistake any of them made.

### 4.4 Why it was silent — the part that matters

The rewriting alone was survivable. What made it invisible was this:

```cpp
// before
if (item_id non-empty) return item_id;   // authoritative
if (exactly one call open) return that;  // fallback
```

A rewritten `item_id` **is non-empty**. So it won. And because it won, the
`sole()` fallback never ran — the fallback that would have routed a
single-call turn correctly, for free, with no knowledge of Copilot at all.

We had a working recovery path and suppressed it by treating presence as
authority.

> **Generalise this.** A field that is present but unresolvable is not
> better evidence than no field at all — it is *worse*, because it
> outranks every fallback while carrying no information. Validate before
> you trust, and let an unresolvable value fall through to the paths that
> can still answer.

The same shape bit us in the Chat transport (`tool_args_unattributable`) and
in `salvage_args`. It is a class, not an incident.

### 4.5 The fix

```cpp
// 1. output_index — the item's slot in output[].
//    A proxy cannot rewrite this without renumbering the array it is
//    describing. Tried FIRST.
// 2. item_id — but only if it names a call we actually opened.
// 3. sole() — exactly one open call means there is no order to be wrong
//    about. Returns nullopt for zero or many; never picks arbitrarily.
```

Shipped in `16e09b35`, pinned by `tests/copilot_item_id_test.cpp` with the
real frame shape. Five cases, chosen so that reverting either half of the
fix fails at least one:

- rewritten id **with** position → routes
- parallel calls, out of order, both rewritten → stay separate
- rewritten id, **no** position, one call open → `sole()` rescues it
- a **good** id that disagrees with position → id wins
- unknown position, several calls open → **stays unrouted**

That last one is not an oversight. See §7.

---

## 5. What GitHub's own client does

Not inference. The binary.

### 5.1 Getting at it

`/usr/bin/copilot` (AUR `github-copilot-cli-bin`) is a 167 MB Node SEA:

```python
import zlib
d = open('/usr/bin/copilot','rb').read()
tar = zlib.decompressobj(31).decompress(d[108754132:])   # 165 MB tar
```

`package/app.js` is 7.7 MB of TypeScript output — and contains **zero**
mentions of `output_index`, `function_call_arguments`, or even
`githubcopilot.com`. The model layer is not JavaScript.

It is `package/prebuilds/linux-x64/runtime.node`: 98 MB of **Rust**, with
source paths left in the binary:

```
src/runtime/src/model/responses_transport.rs
src/runtime/src/model/interaction/responses_dispatcher.rs
src/runtime/src/model/interaction/responses_size_recovery.rs
src/runtime/src/model/chat_completion_transport.rs
src/runtime/src/model/anthropic_transport.rs
```

Same dialect split as ours (§2), arrived at independently.

### 5.2 Their deserializer settles it

serde emits struct field names contiguously in the binary. Theirs:

```
…tool_search_output econnrefused enotfound  output_index  call_id  custom_ …
```

**`output_index` and `call_id`, adjacent. No `item_id` in the struct at
all.** GitHub's own client does not deserialize the field whose rewriting
broke us. They key on position and `call_id` — which is what `16e09b35`
changed us to, reached from the log before we had looked at their binary.

### 5.3 They ship a salvage path

A few hundred bytes along, in the same table:

```
partial_json   copilot_salvaged_tool_input_ids   copilot_quota_snapshots
```

Read that carefully. GitHub's client:

- **reconstructs** tool inputs that fail to route
- **tracks which ids** it salvaged, per response
- **reports it in telemetry**, alongside quota

You do not build that unless it fires in production, often, on your own
servers. The correct reading is not "they have a workaround we lack" — it is
**"argument loss on this wire is expected, not exceptional."**

We treat it as exceptional: drop the payload, log a warning. That is the
honest thing to do with no recovery path, and it is now clearly not enough.
§8.

### 5.4 Also in there

- `response.custom_tool_call_input.delta` — the fifth carrier (§3.2)
- `gpt-5.4` present in their model list — the reported model is current
- `responses_size_recovery.rs` — a whole module for oversized responses

---

## 6. The other three bugs

Same dialect, same silence, different mechanism.

### 6.1 Reasoning was requested with the toggle off

The picker read `reasoning ‹off›`. Every request went out `reasoning=1`.
Thinking blocks appeared and tokens were billed.

Three separate inputs could answer "do we want reasoning":

| Input | State | Reachable? |
|---|---|---|
| `m.d.effort` — the strip | `off` | **yes**, the control the user uses |
| `m.d.show_reasoning` — old `^R` | `true` | **no.** `^R` is review now |
| `ui.thinking != Hidden` | `Shown` | Appearance only |

`ModelsToggleShowReasoning` still had a reducer arm, but nothing dispatched
it. The flag was frozen at whatever `settings.json` last held. It could only
ever contradict the strip.

Then the wire bug underneath:

```cpp
if (!req.effort.empty())
    body["reasoning"] = {{"effort", req.effort}, {"summary", "auto"}};
else
    body["reasoning"] = {{"summary", "auto"}};   // ← asks anyway
```

No tier did not mean "no opinion". It was an explicit request for a
reasoning summary. With every toggle off, every request still asked.

**Fix (`aeb4f5fb`):** effort alone decides. No tier → no `reasoning` field.
`ui.thinking` governs *rendering* of what comes back, which is what
Appearance is for.

> **The invariant:** a DISPLAY preference must never change what we
> REQUEST. That is how tokens get paid for and thrown away. One question,
> one home — and if a control is unreachable, delete it rather than leave
> it able to disagree.

Pinned as a biconditional in `reasoning_ssot_test`: *the field is present
iff a tier is set.* A future "omit the tier but keep the summary" edit — the
exact shape of this bug — fails the test.

### 6.2 Zero-argument calls reported as malformed

A tool with no required arguments (`git_status`, `list_dir`) streams zero
argument deltas. The main reducer seeds `args = {}` at tool start. The
subagent loop did not, so `args` stayed `null` — and the dispatcher reads
`null` as its *parse failure* sentinel:

> "tool args failed to parse — re-emit the call with complete, valid JSON"

The model re-sent the identical correct call, got the identical error, and
burned its turn budget. In the log it reads like a model that cannot write
JSON.

**Fix (`2bb5875c`):** absent arguments are an empty object. `null` means a
failed parse and nothing else.

> **The invariant:** a sentinel must mean exactly one thing. The
> dispatcher's error message was written assuming `null` meant "parse
> failed"; the moment a second condition produced `null`, that message
> became a lie.

`empty_tool_args_test` asserts the seed at every site that builds a
`ToolUse` from a stream start, so a third agent loop cannot repeat it.

### 6.3 The log could not answer the question

The Chat transport dumps its request and decode decisions. The Responses
codec logged **only failures**. A turn that produced empty arguments left no
trace, so three very different bugs looked identical:

- we never sent the tools
- the server sent no arguments
- we received them and failed to route them

**Fix (`1c78ec7b`):** four lines covering a tool call's whole life.

```
responses.request            what we asked for, incl. tool count
responses.tool_open          item_id ↔ call_id ↔ name ↔ output_index
responses.tool_args_routed   which carrier delivered, how many bytes
responses.tool_closed        what the call ended up with
```

This is what turned the next report into a ten-minute diagnosis instead of a
week. §4.2 is just those lines being read.

> **The invariant:** log the decision, not just the failure. A warning tells
> you something went wrong. A trail tells you *where*, and the difference is
> a week.

---

## 7. Discipline: when not to guess

The unrouted case in §4.5 is deliberate, and it is the hardest rule here to
hold under pressure.

When a fragment cannot be attributed and several calls are open, we **drop
it and say so**. The alternative — pick the most recent call, pick the only
one that looks plausible — is tempting because it "usually works".

It does not fail loudly when it is wrong. It produces **valid JSON**. One
call's bytes land on another, the result parses, passes schema validation,
and dispatches. So `edit` runs with a path that belonged to `shell`, and
nothing looks wrong until it has already written.

> **Guessing converts a visible failure into a silent corruption.** One
> broken turn is strictly better than one plausible wrong action.

This is why `OpenCalls::sole()` returns `nullopt` for zero *or many* rather
than handing back an arbitrary item, and why `ToolCallTracker::attribute()`
returns `Ambiguous` instead of picking. The types are built so "the most
recent call" is unwritable — see `docs/TOOL_CALL_ATTRIBUTION.md` and
`attribution_discipline_test`, which scans sources for the shape because we
shipped it four times across two decoders.

GitHub reached the same conclusion from the other direction: rather than
guess, they salvage and *record that they salvaged*.

---

## 8. What we still owe

Two gaps, both known, neither speculative.

### 8.1 The fifth carrier

`response.custom_tool_call_input.delta` hits `responses.unhandled_event` and
the arguments are dropped. GitHub deserializes it. Any model using custom
tool calling loses its arguments today, silently, in precisely the way §4
was about.

Two parts to the fix, and the second is the one that compounds:

1. Another carrier arm routing through the same `feed_tool_args` seam, plus
   a case in `wire_fragmentation_test` so the matrix covers five framings.
2. Move `responses.unhandled_event` from `util::dbglog` onto the `Wire`
   channel. An unknown event is the single most useful line in a bug
   report, and right now it is the one line a `wire`-scoped filter does not
   show.

### 8.2 Salvage

§5.3 is the uncomfortable one. GitHub's client reconstructs unroutable tool
inputs and reports how often it does. We drop and warn.

With `output_index` keying landed, the unroutable case should now be rare —
but "should be" is exactly the reasoning that produced the previous four
bugs. The honest position: we now have `responses.tool_args_unroutable` in
the log, so we can *measure* whether it still fires before deciding what to
build. Measure first, then decide.

---

## 9. Debugging this dialect

```
AGENTTY_LOG=wire=debug agentty
```

Log lands at `~/.agentty/logs/agentty.log`. Add
`AGENTTY_LOG_FILE=/tmp/x.log` to redirect. Use `wire=trace` for raw SSE
frames and full request bodies.

**Scope it to `wire`.** A bare `debug` or `trace` sets the default for every
channel, and the render loop emits two trace lines per frame — a real run
produced 9,416 UI lines against 25 wire lines. The signal is there and
unreadable. Channel-scoped specs silence everything else.

### Reading a healthy turn

```
responses.tool_open:        item_id=fc_1 call_id=call_9 name=read output_index=0
responses.tool_args_routed: item_id=fc_1 call_id=call_9 total=1 bytes=16 fresh=16 have=16
responses.tool_closed:      item_id=fc_1 call_id=call_9 args=16
```

### Diagnosing `args=0`

Look for `tool_args_routed` between the open and the close:

| Observation | Meaning | Where to look |
|---|---|---|
| `routed` present, `args=0` at close | we got bytes and lost them | the codec |
| **no** `routed` line | no carrier delivered | `responses.request tools=N`, then `wire=trace` for the body |
| `tool_args_unroutable` | addressing failed | §4 — compare the ids |
| `unhandled_event` | a carrier we don't know | §8.1 — **not on the `wire` channel yet**, use `AGENTTY_LOG=debug` |

`item_id` and `call_id` are both on every line because they differ on
Copilot, and a mismatch between them is where arguments go missing.

---

## 10. The invariants, together

Each one cost at least one shipped bug.

1. **Identity is what a proxy cannot rewrite.** On Responses that is
   `output_index`. `item_id` is a hint.

2. **A present-but-unresolvable field is worse than an absent one.** It
   outranks fallbacks while carrying no information. Validate, then trust.

3. **Guessing converts a visible failure into a silent corruption.** Fail
   the turn instead.

4. **A sentinel means exactly one thing.** `null` args = parse failed.
   Absent arguments are `{}`.

5. **A display preference must never change what we request.** One
   question, one home.

6. **If a control is unreachable, delete it.** A frozen flag can only
   disagree with the live one.

7. **Log the decision, not just the failure.** A warning says something
   broke. A trail says where.

8. **Test the space, not the transcript.** A test encoding one server's
   choice converts "we don't know" into "we checked".

---

## 11. Map

| Where | What |
|---|---|
| `src/provider/responses/codec.cpp` | the dialect: `addressed_item`, `feed_tool_args`, carriers |
| `src/provider/openai/transport.cpp` | Chat Completions, same class of bug |
| `include/agentty/provider/wire/tool_calls.hpp` | `ToolCallTracker` — attribution that cannot guess |
| `tests/copilot_item_id_test.cpp` | §4, real frame shapes |
| `tests/reasoning_ssot_test.cpp` | §6.1, the biconditional |
| `tests/empty_tool_args_test.cpp` | §6.2, every construction site |
| `tests/responses_log_test.cpp` | §6.3, healthy vs empty distinguishable |
| `tests/wire_fragmentation_test.cpp` | §3.1, the framing matrix |
| `tests/attribution_discipline_test.cpp` | §7, source scan for arbitrary picks |
| `docs/TOOL_CALL_ATTRIBUTION.md` | the Chat-side companion |
| `docs/HETEROGENEOUS_MODELS.md` | why these bugs were possible: the neutral-core argument |

| Commit | What |
|---|---|
| `16e09b35` | output_index keying |
| `aeb4f5fb` | reasoning SSOT |
| `1c78ec7b` | the log trail |
| `2bb5875c` | zero-arg calls + unattributable chunks |

---

## 12. Closing

The through-line is not "Copilot is buggy". It is that **every one of these
failures was silent**, and each was found only once something was written
down that could not be true and false at once — a log line, a biconditional,
a source scan.

The wire will break again. It is a proxy in front of a model, and both
change. What should not break again is our ability to see it within ten
minutes of a user saying "it doesn't work".
