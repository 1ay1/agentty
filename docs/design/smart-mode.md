# Smart Mode — role-based execution routing

**Status:** design proposal (v2 refinements below; core design unchanged)
**Author:** agentty
**Scope:** cost reduction without quality regression, via a three-model role hierarchy

---

## 0. What makes it a selling point (v2 refinements)

The core idea (roles, not model names; resolve `role → model` at dispatch) is
right and unchanged. Four refinements turn it from "cheaper routing" into a
feature users *feel*:

1. **Per-role reasoning effort, not just per-role model.** The biggest quality
   × cost lever is often *how hard the model thinks*, orthogonal to *which*
   model. Smart Mode pins an effort per role: **Strategic thinks hard** (high
   reasoning effort), **Implementation medium**, **Utility not at all** (effort
   off — grep/read/commit-msg don't need a reasoning budget). This alone can
   beat the model swap: a flagship at zero effort on a mechanical task is pure
   waste; a mid model at high effort on a hard bug can match the flagship.
   `RoleProfile { ModelId model; Effort effort; }` — the resolver returns both.

2. **Zero-config by default.** Turning Smart Mode *on* with no slots set
   auto-fills from the signed-in catalog: Strategic = the flagship you're
   already on, Implementation = the strongest mid-tier, Utility = the cheapest
   capable. It works instantly; power users override any slot. No "configure 3
   models before it does anything" wall.

3. **Legible, live cost accounting.** The pitch isn't "trust us, it's cheaper"
   — it's *visible*. A per-turn footer readout: `Strategic 12k · Impl 4k ·
   Utility 1k · saved ~63% vs all-flagship`. Users watch the routing work.
   Backed by a `RoleSpend` accumulator keyed on `ModelRole`, rendered in the
   status bar and the turn meta.

4. **Utility owns retrieval — tie it to the RAG stack we shipped.** The
   Utility role is the natural home for the internal retrieval calls (thread
   fork carry-context, proactive `search_docs`, HyDE query expansion). Those
   already run on the cheapest capable model; Smart Mode makes that *explicit
   and user-visible* as "Utility work," so "recall" and "gather" read as one
   coherent tier instead of scattered internal cheapness.

**Rollout stays the same, foundation-first:** Step 1 (resolver + settings +
RoleProfile) and Step 2 (internal utility calls — compaction summary, commit
messages, HyDE, fork/thread retrieval) are pure wins with zero user-visible
behaviour change and ship first. The visible orchestrator-workers routing
(Step 3+) lands behind the `smart_mode` flag, off by default, once the
foundation is proven.

---

## 1. The idea, restated

Smart Mode lets the user pin **three** models to three **roles** and then routes
every unit of LLM work to the model that fits the work's *intent*:

| Role | Occupied by (user's choice) | Answers the question |
|------|-----------------------------|----------------------|
| **Strategic** | e.g. Claude Opus 5 | *What should we build?* — architecture, API design, debugging hard bugs, review, decomposition |
| **Implementation** | e.g. Claude Sonnet 5 | *Build what Strategic decided* — write/edit code, fix compiles, write tests |
| **Utility** | e.g. Claude Haiku 5 | *Gather info / mechanical work* — grep, read, summarise, docs, commit messages |

The planner thinks in **roles, never model names**. The runtime resolves
`role → model` at dispatch time. Nothing inside the engine ever writes
`if (model == Opus)`. This is the correct abstraction and we adopt it wholesale.

```cpp
enum class ModelRole { Strategic, Implementation, Utility };
```

---

## 2. The one architectural decision that makes or breaks this

The submitted spec describes a **planner → static `ExecutionPlan` → runtime
loops over typed steps** pipeline. That is the *right mental model* but the
*wrong execution model* for agentty — and, as it turns out, for every
production coding agent that has actually shipped this.

### 2.1 What agentty actually is

agentty is a **single reactive turn-loop** (Elm-style: `Msg → update → Cmd`).
One user prompt triggers exactly one `launch_stream` (`src/runtime/app/
cmd_factory.cpp`), which fixes `req.model` for the **whole turn**. The *model
itself* emits tool calls mid-stream; `kick_pending_tools` runs them; results
feed back; the **same** model continues until it stops. There is no separate
planning phase and no pre-computed graph of steps. Tools are chosen by the
model reactively, not enumerated up front.

### 2.2 What the research says (don't build a planner)

Source-level analyses of Claude Code (arXiv 2604.14228, *"Dive into Claude
Code"*) and Anthropic's own engineering writeups converge on one finding:

> The architecture invests in **deterministic infrastructure (context
> management, tool routing, recovery) rather than decision scaffolding
> (explicit planners or state graphs)** … it does **not impose explicit
> planning graphs** on the model's reasoning.

Claude Code implements exactly the role-routing this spec wants — but via the
**orchestrator-workers** pattern (one of Anthropic's five workflow patterns),
**keeping the core loop reactive**. The strong model runs the loop and
*delegates* bounded sub-tasks to isolated subagents; it does not emit a static
typed plan that a scheduler replays. The rejected alternative — "encode
decision logic as explicit state graphs with typed edges" — is named in the
paper as the **LangGraph family**, chosen against deliberately.

A literal `vector<ExecutionStep>` planner in agentty would be a *second agent
framework* bolted next to the existing one: it would duplicate context
assembly, tool dispatch, permission preflight, the doom-loop breaker,
compaction, and cancellation — every one of which is already load-bearing in
the reactive loop. That is a multi-month rewrite with a large regression
surface and **worse** results (a static plan can't adapt when step 3's grep
comes back empty).

### 2.3 The decision

> **Smart Mode is realised as role-tagged delegation on top of the existing
> reactive loop — NOT as a standalone planner emitting a static step graph.**

The user's role taxonomy is preserved *exactly*. What changes is *where the
role classification happens*: the **Strategic model, running the main loop, is
the planner** — it decides moment-to-moment which work to keep and which to
delegate, using a tool. This is the orchestrator-workers pattern, and it is the
only version of this feature that (a) matches agentty's grain, (b) matches
every shipped competitor, and (c) reuses the subagent + model-tier machinery
that **already exists** in the codebase (commits `8f9f503`, `2ede4b4`,
`4dc3c4a`).

The three success invariants from the spec still hold verbatim:

- Every LLM request is tagged with exactly one role.
- All model selection is a role lookup, never a name check.
- Strategic = decisions, Implementation = code, Utility = mechanical.
- Context between roles is minimised and structured.
- Behaviour is identical across Anthropic / OpenAI / Google / local.

---

## 3. Architecture

```
                 Smart Mode OFF                    Smart Mode ON
                 ─────────────                     ─────────────
   User prompt                            User prompt
       │                                      │
       ▼                                      ▼
   launch_stream (1 model, all work)     launch_stream  ─── role=Strategic
       │                                      │  (main loop = the planner)
       ▼                                      │
   model emits tools, loop runs           reactively decides per action:
       │                                      ├─ mechanical? → task(role=Utility)
       ▼                                      ├─ code write?  → task(role=Implementation)
    result                                    └─ needs a decision? → do it itself (Strategic)
                                              │
                                              ▼
                                           result
```

Three request *origins*, each carrying a **role**:

1. **The main turn** — role is `Strategic` (Smart Mode on) or the single active
   model (off). This is the orchestrator.
2. **Delegated subagents** — the `task` tool already spawns isolated agents.
   Each agent *type* maps to a role (§5). This is where Utility/Implementation
   work runs.
3. **Internal utility calls** — a few engine-internal LLM calls (thread title,
   commit-message synthesis, compaction summary) are inherently Utility and
   route there directly (§6).

Every one of these funnels through `role_model()` — the **single** point where
a role becomes a concrete model id.

---

## 4. The role → model resolver (the core of §"never check model names")

A tiny, pure, provider-agnostic module. This is the *only* code that knows the
mapping; everything else asks it.

```cpp
// include/agentty/domain/smart_mode.hpp
namespace agentty {

enum class ModelRole : std::uint8_t { Strategic, Implementation, Utility };

struct SmartMode {
    bool    enabled = false;
    ModelId strategic;        // e.g. "claude-opus-5"
    ModelId implementation;   // e.g. "claude-sonnet-5"
    ModelId utility;          // e.g. "claude-haiku-5"

    // The whole contract in one function. When disabled, EVERY role resolves
    // to `fallback` (the user's single active model) — so callers can request
    // roles unconditionally and Smart Mode off is a pure no-op.
    [[nodiscard]] ModelId model(ModelRole r, const ModelId& fallback) const {
        if (!enabled) return fallback;
        switch (r) {
            case ModelRole::Strategic:      return strategic.empty()      ? fallback : strategic;
            case ModelRole::Implementation: return implementation.empty() ? fallback : implementation;
            case ModelRole::Utility:        return utility.empty()        ? fallback : utility;
        }
        return fallback;
    }
};
} // namespace agentty
```

Design notes:

- **Disabled ⇒ fallback for all roles.** Callers never branch on `enabled`;
  they always `smart.model(role, active_model)`. Off = every role collapses to
  today's single-model behaviour, byte-for-byte. Zero risk when off.
- **Empty slot ⇒ fallback.** A half-configured Smart Mode degrades gracefully.
- **Provider-agnostic.** `ModelId` is just a string; the resolver never asks
  which vendor. Anthropic/OpenAI/Gemini/local all work identically (spec
  success criterion #7). Note: all three slots **must be on the same provider**
  — cross-provider auth/caching don't transfer (§9).

### 4.1 Relationship to the existing capability-tier router

We already shipped `ModelCapabilities::Tier {Weak,Cheap,Mid,Flagship}` +
`cheapest_capable_model()` (commit `4dc3c4a`) — an *automatic* per-provider
strength ordering used to route read-only subagents to the cheapest capable
model. Smart Mode is the **explicit** counterpart: the user pins the three
models instead of the engine guessing.

They compose cleanly:

- **Smart Mode ON** → role slots win. `role_model(Utility)` returns the user's
  Utility pick outright.
- **Smart Mode OFF** → the existing tier auto-router still applies to read-only
  subagents (unchanged). So users get sensible cheap-routing for free, and
  power users get exact control by turning Smart Mode on.

`tier()` also *validates* a Smart Mode config: warn (non-blocking) in the UI if
`tier(strategic) < tier(implementation) < tier(utility)` is violated, e.g. the
user put Haiku in Strategic and Opus in Utility (§7).

---

## 5. Wiring roles into the delegation path (subagents)

The `task` tool already spawns isolated subagents with typed roles
(`src/tool/mcp_tools_backends.cpp`, `AgentType`). Today those types are
`explorer / reviewer / tester / coder / general`. Map each to a `ModelRole`:

| Agent type | read_only | ModelRole | Rationale |
|------------|-----------|-----------|-----------|
| `explorer` | yes | **Utility** | search / read / map — mechanical retrieval |
| `reviewer` | yes | **Strategic** | reviewing *is* an engineering judgment |
| `tester`   | no  | **Implementation** | writes and runs tests |
| `coder`    | no  | **Implementation** | writes/edits code |
| `general`  | no  | **Implementation** | mixed execution work |

> Note this *refines* the current shipped behaviour: today `reviewer` (read-only)
> is auto-routed to the **cheapest** model by the tier router. Under the role
> taxonomy, review is a Strategic act and should get the strong model. Smart
> Mode makes that explicit and correct; Smart-Mode-off keeps today's cheap
> routing (a deliberate, documented difference).

Implementation: `AgentType` gains a `ModelRole role;` field. `run_one_completion`
already computes `req.model`; replace the tier-router call with:

```cpp
// src/tool/mcp_tools_backends.cpp — run_one_completion
req.model = cfg.smart.enabled
          ? cfg.smart.model(type.role, ModelId{cfg.model}).value   // Smart Mode: role lookup
          : (type.read_only                                        // legacy auto-router
               ? cheapest_capable_model(cfg.model, cfg.candidates)
               : cfg.model);
```

`subagent::Config` gains a `SmartMode smart;` field, refreshed from settings
next to `set_candidates` (the `ModelsLoaded` reducer, `picker.cpp`).

**The main-loop role.** When Smart Mode is on, the *main* turn runs on
`Strategic`. That's a one-line change in `launch_stream`:

```cpp
// src/runtime/app/cmd_factory.cpp — launch_stream
const ModelId active{m.d.model_id};
req.model = m.d.smart.model(ModelRole::Strategic, active).value;
```

So the orchestrator is always the strategic model; delegation moves cheap work
off it. This is the entire "planner runs on Strategic" mechanism — no new loop.

### 5.1 How the strategic model actually delegates (the "planner")

The orchestrator delegates through the **`task` tool it already has**, plus a
system-prompt policy (Smart-Mode-only paragraph) that teaches it the taxonomy:

> *You are the Strategic model. Do the thinking — architecture, decisions,
> review — yourself. DELEGATE mechanical retrieval (searching, reading,
> summarising) to `task(agent_type:"explorer")` and code execution to
> `task(agent_type:"coder")`. Prefer delegation for any bounded, well-specified
> sub-task; keep only the decisions.*

This is the orchestrator-workers pattern verbatim, and it's how Claude Code's
own subagent delegation works. The model — not a hard-coded scheduler — decides
what to delegate, because (per the research) capable models route better than
static graphs. The taxonomy in the spec (§Responsibilities) becomes the
delegation *guidance*, not a compile-time state machine.

---

## 6. Internal utility calls (free wins, no model reasoning needed)

A few engine-internal LLM calls are *unconditionally* Utility. Route them to
the Utility model whenever Smart Mode is on — pure savings, zero behaviour
change:

| Call site | Today | Smart Mode |
|-----------|-------|------------|
| Thread title synthesis | active model | Utility |
| `git_commit` message drafting (if/when LLM-assisted) | active model | Utility |
| Compaction summary (`wire_messages_for_compaction`) | active model | Utility |
| Proactive-RAG query rewriting (if added) | — | Utility |

Each is a `req.model = smart.model(ModelRole::Utility, active)` at its call
site. These are the highest-ROI, lowest-risk edits and can ship **first**, before
any delegation work, as a standalone increment.

---

## 7. UX

### 7.1 Configuration surface

A dedicated **Smart Mode** picker (new command-palette entry + a modal),
mirroring the existing model picker. Three slots, each opening the standard
model list scoped to the active provider:

```
┌─ Smart Mode ──────────────────────────────┐
│  ● Enabled                                 │
│                                            │
│  🧠 Strategic       Claude Opus 5      ▸   │
│  ⚙  Implementation  Claude Sonnet 5    ▸   │
│  ⚡ Utility         Claude Haiku 5     ▸   │
│                                            │
│  ⚠ Utility is stronger than Strategic —    │  ← soft tier-order warning
│     roles may cost more than expected.     │
└────────────────────────────────────────────┘
```

- Each slot ▸ opens the existing model picker filtered to the active provider.
- The tier-order warning (§4.1) is advisory, never blocking — the user is in
  control (matches Claude Code's "human decision authority" value).
- Defaults on first enable: auto-fill from tiers — Strategic = highest-tier
  available, Implementation = a Mid model, Utility = the cheapest capable. One
  keypress to a sane config.

### 7.2 In-flight transparency (the spec's key UX insight)

The user must *see why each model is being used*. The tool-call card for a
delegated `task` already renders in the timeline; add a **role badge + model
name**:

```
  🧠 Strategic · Claude Opus 5     designing auth architecture
  ⚡ Utility   · Claude Haiku 5     ⤷ searching repository (explorer)
  ⚙ Impl      · Claude Sonnet 5    ⤷ implementing OAuth (coder)
```

Icons: 🧠 Strategic, ⚙ Implementation, ⚡ Utility (from the spec). The badge is
derived from the subagent's `ModelRole` (already on `AgentType`) — no new
plumbing beyond passing the role to the task card view. This turns cost control
into a *legible* feature instead of hidden magic.

### 7.3 Status line

When Smart Mode is on, the status/footer shows a compact `SMART` chip; hovering
/ expanding lists the three bindings. Off = chip hidden, current behaviour.

---

## 8. Context transfer between roles (§"structured artifact, not transcript")

The spec's insight — pass a **structured artifact** downstream, not the whole
reasoning transcript — is already how agentty subagents work and why they're
cheap: a subagent returns **one condensed report**, not its transcript
(`subagent_report_test` locks this). The parent sees only that report.

We formalise it slightly for Smart Mode:

- **Strategic → Implementation.** When the orchestrator delegates a coding
  sub-task, it passes a *brief* (objective + constraints + relevant file:line
  refs) as the `task` prompt — not its own chain-of-thought. The subagent's
  lean prompt (commit `2ede4b4`) already omits parent-only machinery.
- **Utility → Strategic.** An explorer returns a tight map (paths, symbols,
  gotchas), already byte-capped (commit `2ede4b4` caps tool output at 8 KiB).

No new artifact schema is strictly required for v1 — the existing report
contract *is* the artifact. A future increment could enforce a JSON shape
(`{objective, constraints[], plan[]}`) for Strategic outputs; deferred as
optional polish (§11).

---

## 9. Cross-provider constraint

All three role slots **must belong to the same provider** in v1. Reasons:

- Auth: each provider has its own credential; a single turn's delegation chain
  shares `req.auth`.
- Caching: the prompt-cache/session-key work (`1785c44`) is per-provider; a
  cross-provider hop is a guaranteed cache miss and defeats the savings.
- Simplicity: the resolver stays a pure `role → ModelId` map with no
  provider-switch side effects mid-turn.

The picker enforces this by scoping all three slots to the active provider.
Switching provider clears/reseeds Smart Mode from that provider's tiers.
Cross-provider Smart Mode (Opus strategic + a local Utility model) is a
compelling **future** direction but requires per-role auth threading through the
delegation path — explicitly out of scope for v1 (§11).

---

## 10. Failure escalation

The spec's escalation (Implementation stuck → back to Strategic) maps onto
existing machinery:

- A delegated `coder`/`tester` subagent that can't proceed **returns a report
  saying so** (it already does — the report contract). The orchestrator
  (Strategic) reads that report and decides: revise the brief and re-delegate,
  or handle it itself. No new control flow — the reactive loop *is* the
  escalation path.
- The **doom-loop breaker** (`agent_loop_should_break`) already stops a
  subagent spinning on the same failing call, returning control to the parent
  with a clear message. Utility never "escalates to Strategic" implicitly; it
  just returns, and the Strategic orchestrator decides.

This is strictly better than a static escalation edge: the strong model
adjudicates every stuck sub-task with full context.

---

## 11. Rollout plan (incremental, each step shippable)

1. **Resolver + settings + persistence** (`SmartMode`, `smart_mode:` YAML block,
   load/save). Inert until wired. *No behaviour change.*
2. **Internal utility calls** (§6) — route title/compaction/commit-msg to
   Utility. Immediate savings, tiny surface, no UX. *Ship.*
3. **Subagent role mapping** (§5) — `AgentType.role`, resolver in
   `run_one_completion`, `Config.smart`. Delegation now honours roles.
4. **Main-loop Strategic role** (§5) + delegation system-prompt policy (§5.1).
   The orchestrator pattern goes live.
5. **UX** — Smart Mode picker (§7.1), role badges (§7.2), status chip (§7.3).
6. **(Future)** structured Strategic artifact schema (§8); cross-provider roles
   (§9); per-role effort tiers.

Steps 1–2 are pure wins with near-zero risk and can land immediately. Steps 3–5
deliver the full vision. Step 6 is optional polish.

### Test plan

- `smart_mode_test` (new): resolver truth table — disabled ⇒ fallback for all
  roles; empty slot ⇒ fallback; each role ⇒ its model; provider-scoping.
- Extend `subagent_report_test`: with Smart Mode on, an `explorer` task runs on
  the Utility model, a `coder` task on Implementation, `reviewer` on Strategic;
  with it off, legacy tier routing is unchanged.
- `persistence` round-trip for the `smart_mode:` block.
- Reuse `model_caps_test` for the tier-order validation warning.

---

## 12. Configuration format

```yaml
smart_mode:
  enabled: true
  strategic:      { provider: anthropic, model: claude-opus-5 }
  implementation: { provider: anthropic, model: claude-sonnet-5 }
  utility:        { provider: anthropic, model: claude-haiku-5 }
```

Stored under the existing `Settings` (`include/agentty/store/store.hpp`) as a
nested object; `provider` on each slot is validated to equal the active
provider in v1 (§9). The planner and runtime depend only on the role→model map,
never on the provider/model names — satisfying the spec's final invariant.

---

## 13. Why this is the best version of the feature

- **Preserves the user's whole vision** — three user-owned models, role-based
  routing, "engine controls *when*, user controls *which*", full transparency.
- **Matches agentty's architecture** — extends the reactive loop + existing
  subagent delegation instead of forking a second framework.
- **Matches what actually shipped elsewhere** — orchestrator-workers is the
  pattern Claude Code uses; static planning graphs are the path the research
  explicitly identifies as the road *not* taken.
- **Reuses shipped foundations** — the tier router, lean subagent prompt,
  tool-output caps, session-key caching, and condensed-report contract from the
  last four commits are exactly the substrate this needs.
- **Incremental and low-risk** — Smart-Mode-off is a byte-for-byte no-op;
  early steps ship pure savings before any UX; every step is independently
  testable.
```
