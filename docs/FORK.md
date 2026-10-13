# Fork — escape a full context window for near-zero tokens

**Status:** SHIPPED (master `3d6f8c1..4599582`).

A long agent session eventually fills the context window. The usual answers
are *compaction* (summarize the old turns) or *start a new thread* (lose
everything). agentty adds a third, better answer for the "the window is spent
but I still want continuity" case:

> **Fork** branches the current thread into a **fresh** one that carries
> **near-zero context**. The parent's full transcript is written to disk; the
> fork holds only a ~200-byte pointer to it. The model **reads that transcript
> on demand** — pulling just the slice it needs, only when it needs it —
> instead of paying for the whole history up front.

Forking is therefore **O(1) in tokens regardless of how big the parent got.**
It turns "fork" from *"branch with the same context"* (what every other agent
does) into *"reclaim the context window."*

---

## TL;DR

- **Trigger:** `Ctrl+K → "Fork thread"` → pick a RAG mode → `Enter`.
- **Result:** a brand-new thread, empty of the parent's turns, that opens with
  a visible **⑃ Forked** card pointing at the parent transcript.
- **Cost:** one small `read`-on-demand pointer message. The parent's history
  is on disk, greppable, not on the wire.
- **The parent is never modified** — forking is non-destructive by
  construction.

---

## Fork vs. compaction — the one-table summary

Both manage a too-full context window; they are **orthogonal** and compose
cleanly (see §7).

| | Compaction (`/compact`) | Fork |
|---|---|---|
| Thread | **same** id | **new** id (`forked_from` = parent) |
| Old context | lossy summary, inline on the wire | verbatim on disk, read on demand |
| Up-front token cost | pays to summarize the prefix | ~zero (just a pointer) |
| Detail preserved | compressed to prose (lossy) | **full** (the `.md` is verbatim) |
| Purpose | keep *one* long thread going | *branch* and reclaim the window |
| Mechanism | `CompactionRecord` substitutes the wire prefix | fresh thread + `fork_note` pointer |

> **Historical note.** An earlier fork design *summarized* the parent onto the
> new thread (a `CompactionRecord` on the fork). That defeated the purpose — it
> sent the whole transcript to the summarizer, cost the window, and could hit
> "prompt too long." The shipped design is read-on-demand: no summary, no copy.
> `fork_test` locks that the fork carries **no** `CompactionRecord`.

---

## 1. The user flow

1. **Open** — `Ctrl+K`, choose **Fork thread** (or bind the command). The fork
   picker opens: a 3-row overlay.
2. **Choose RAG behaviour for the fork** — the *only* choice, because a fork
   always starts fresh. `↑↓` / `j` `k` to move:

   | Row | Fork's proactive-RAG behaviour |
   |-----|--------------------------------|
   | **RAG per turn** | retrieve context before every turn |
   | **First-turn RAG** | retrieve once, up front, then stay quiet |
   | **RAG off** | no proactive injection (search tools still work) |

3. **Fork** — `Enter`. The current thread is saved untouched; a fresh thread
   opens instantly with a **⑃ Forked** card. `Esc` / `q` cancels.

Guardrails: you can't fork an empty thread ("nothing to fork yet") or while the
agent is mid-turn / compacting / loading ("cannot fork while the agent is
working").

---

## 2. What a fork actually contains

Exactly one synthetic message — the **fork note** — and nothing else from the
parent:

```
## user   (fork_note = true)
This conversation is a fork of an earlier one. Its full transcript is
saved at:
  ~/.agentty/threads/<parentId>.transcript.md
Read it with the `read` tool (or grep it) ONLY if you need earlier
context — don't read it pre-emptively. The fork starts fresh precisely
to reclaim the context window; pull just the slice you need.
```

Two jobs, deliberately:

1. **VIEW** — it renders as a first-class **⑃ Forked** event card (fork glyph,
   info rail, `Forked` label, a one-line body, the transcript path in
   code-reference cyan). Without it, a fresh fork would be a **blank screen**:
   `messages.empty()` is false (this note exists), so the welcome screen is
   suppressed — but a bare System note wouldn't render, leaving nothing on
   screen.

2. **WIRE** — the model receives this text in full, so it *knows* it's a fork
   and where the transcript lives.

### Why it's a `Role::User` message (the robustness crux)

The pointer used to be a `Role::System` message. That is fragile: the
**ChatGPT / Responses transport drops mid-thread System messages** (it folds
them into the top-level `instructions`). A dropped pointer means the model
never learns the transcript exists. A **User** message survives *every*
provider path, so the pointer is provider-proof.

To keep a User message that isn't a real prompt from polluting the machinery,
`fork_note` is a **wire-inert marker flag** (mirroring `smart_routing` /
`proactive_context`): every "find the newest real user turn" scan — routing,
RAG, composer `↑`/`↓` history recall, the agent-loop breaker — skips it.

---

## 3. The transcript file

`write_thread_transcript_md(parent)` (`src/io/persistence.cpp`) exports a
clean, **bounded** Markdown snapshot of the parent at fork time.

**Location:** `~/.agentty/threads/<parentId>.transcript.md` — next to the
thread `.json` files, human-readable, greppable, and outside the workspace the
`read` tool is normally sandboxed to (the fork allowlists it — see §5).

**What's written** — `## user` / `## assistant` / `## system` headers + the
message text; tool calls collapse to a single `› tool(name) {args…}` line.

**What's *not* written** — **tool OUTPUT**. A long agentic thread's bytes are
dominated by tool results (a 50 KB file read, a giant build log); those are
dropped entirely (only the tool name + a 120-char arg hint survive). This
alone strips the heaviest bytes.

**Bounded so even a 1M-token parent yields a useful artifact:**

| Cap | Value | Behaviour |
|-----|-------|-----------|
| `kMaxMsgTextBytes` | 16 KB / message | one giant paste is clipped **head+tail** (75/25) with a `… N bytes elided …` marker; ends carry the most signal |
| `kMaxTranscriptBytes` | 512 KB total | **recency-biased**: render newest→oldest until the budget is hit, emit the kept slice oldest→newest with a `[… older turns elided …]` marker; the header still reports the true total message count |

Bounding the *output* also bounds *peak memory* — there is no unbounded
in-RAM string on a fork of a runaway thread. Every text block is UTF-8-scrubbed,
so the `.md` is always well-formed even from garbage input bytes.

**Crash-safety** — written via `write_json_atomic`: temp file → `fwrite` →
`fflush` + `fsync` → atomic `rename` → parent-dir `fsync`. A reader (or a crash
mid-fork) sees either the old file or the fully-written new one, never a
truncated transcript. If the write fails (disk/permissions), the fork still
seeds a note so the thread isn't blank — it just tells the model earlier
context isn't retrievable.

---

## 4. Read-on-demand: why the transcript can be big but the fork stays cheap

"Big transcript" ≠ "big context." The transcript lives on **disk**, not on the
wire. The fork carries a ~200-byte pointer, so:

- **Fork cost is O(1)** in tokens regardless of parent size — that *is* the
  goal.
- The transcript enters context only if the model **chooses** to `read` it, and
  `read` is paginated (offset/limit) — it never slurps the whole file. It can
  `grep` for the one fact it needs.

A 500 KB transcript on disk that the model greps a 2 KB slice out of is exactly
the win; force-feeding it into the fork's context would defeat forking.

---

## 5. What the reducer does (`src/runtime/app/update/fork.cpp`)

On `ForkThread`:

1. **Persist the parent** untouched (`save_thread`), remember its id.
2. **Export the transcript** → the `.md` path (§3).
3. **Allowlist the reads root** — `~/.agentty/threads` is outside the tool
   sandbox, so the fork calls `allow_read_root(threads_dir())` on **both** the
   agentty and mcp-cpp fs layers (tools are served through mcp-cpp). Without
   this the model gets "path outside sandbox" when it follows the pointer.
4. **Create the fresh thread** — new id, `forked_from` = parent id,
   `rag_mode_override` = the chosen RAG mode, **empty** `compactions`.
5. **Seed the fork note** (§2) as `Role::User` with `fork_note = true` and
   `fork_transcript = <path>`.
6. **Switch** to the fork and rehydrate the (near-empty) view without a full
   repaint; a 5-second toast confirms *"forked · fresh context · <RAG> · prior
   transcript readable on demand."*

The parent thread's `.json` is unchanged; its provenance link lives on the
**child** (`forked_from`).

---

## 6. Persistence & provenance

- `fork_note` + `fork_transcript` round-trip through the thread `.json`
  (`message_to_json` / `parse_message`), so a reloaded fork still renders the
  ⑃ Forked card and still points the model at the transcript.
- The fork's `forked_from` records the parent id (persisted) — the history stays
  traceable and a future thread-tree view can draw the branch.
- `fork_note` messages are **persisted** (unlike `smart_routing` cards, which
  are view-only telemetry and never saved) — the pointer is real wire content.

---

## 7. Fork + compaction compose

The two features are orthogonal by construction; `compaction_wire_test` pins it:

- **Fork a compacted thread** → the transcript export renders the parent's full
  raw messages (nothing lost); the new fork starts with empty `compactions`.
- **Compact a fork** → the fork note sits at index 0; a later compaction's
  summary cleanly *subsumes* it. The wire still starts with a `User` (the
  summary), correctly **not** flagged `fork_note`.
- **Fork before any compaction** → the fork note + first prompt both ship
  uncompacted; the note is the wire head and a `User` (provider-proof).

---

## 8. Return path (intentionally not built)

A "merge the fork's conclusion back into the parent" path was considered and
**deliberately left out**. The primary fork use-case is *escaping an exhausted
parent* — the fork **becomes** the new main line, and you never go back to the
dead parent, so there is nothing to merge. Building a return path would be
speculative surface area. If an exploratory "side-branch, then fold back into a
live main thread" workflow emerges, that's the natural place to add it.

---

## 9. Files & tests

**Implementation**

| Concern | File |
|---------|------|
| Reducer (branch, export, allowlist, switch) | `src/runtime/app/update/fork.cpp` |
| Transcript writer (bounded, recency-biased) | `src/io/persistence.cpp` → `write_thread_transcript_md` |
| Marker fields + render key | `include/agentty/domain/conversation.hpp` → `Message::fork_note` / `fork_transcript` |
| ⑃ Forked card render | `src/runtime/view/thread/turn/turn.cpp` (`turn_config`, `fork_note` branch) |
| Picker view + labels | `src/runtime/view/fork_view.cpp` |
| Picker state + RAG choices | `include/agentty/runtime/fork_picker.hpp` |
| Key dispatch | `src/runtime/app/subscribe.cpp` → `on_fork_picker` |
| Wire-scan skips (routing/RAG/history/loop) | `cmd_factory.cpp`, `composer.cpp`, `stream.cpp` |
| ACP prompt prepends leading fork notes | `src/provider/external_acp_backend.cpp` |
| Persistence round-trip | `src/io/persistence.cpp` |

**Tests**

| Test | Pins |
|------|------|
| `fork_test` | fresh thread, **no `CompactionRecord`**, `forked_from`, User `fork_note` with a non-empty pointer, distinct RAG modes, empty-thread guard |
| `transcript_bound_test` | 512 KB total cap, recency (newest kept / oldest elided), per-message head+tail clip, tool output never written, `smart_routing` skipped, valid UTF-8, small-thread verbatim |
| `compaction_wire_test` | compaction wire substitution + `estimate_wire_tokens` prices the shrunken view + **fork/compaction composition** |
| `persistence_proactive_test` | `fork_note` / `fork_transcript` survive save→reload, stay `Role::User`, don't leak onto other turns |
