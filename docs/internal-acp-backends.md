# Internal ACP — one turn vocabulary for every backend

> Status: design. Supersedes the ad-hoc per-provider stream epilogue
> (`include/agentty/provider/stream_epilogue.hpp`). Motivated by two live
> bugs that are really the *same* bug — provider transports each open-code
> "how a turn streams and ends", and they drift.

## 1. The problem

agentty talks to four model backends through four hand-written transports:

- `src/provider/anthropic/transport.cpp` — Messages SSE.
- `src/provider/openai/transport.cpp` — Chat Completions SSE.
- `src/provider/ollama/transport.cpp` — NDJSON.
- `src/provider/chatgpt/responses.cpp` — the Codex **Responses** API.

Each one independently decides how to frame text, thinking, tool calls,
usage, cancellation, and the terminal event. They have drifted, and the
drift shows up as user-visible bugs:

- **"cancelled" after a clean Codex turn.** ChatGPT aborts the read early
  after `response.completed`; the HTTP layer reports the intentional abort
  as *cancelled*; the open-coded post-loop forwarded it as a spurious
  `StreamError`. (Patched in `stream_epilogue.hpp`, but that patch is a
  band-aid on the symptom, not the shape.)
- **Codex tool loop / duplicate turns.** The Responses API under
  `store:false` needs the model's own `reasoning` items replayed between
  tool rounds (`include:["reasoning.encrypted_content"]`). The Codex
  transport requests `include:[]` and drops reasoning, so gpt-5-codex loses
  its plan between rounds and re-issues the same call. Only the Codex
  adapter has this quirk; nothing structural stops the next provider from
  growing its own.

Every one of these is a *per-provider difference leaking into the core*.
The runtime should never see them.

## 2. The realization: we already own the vocabulary

agentty is already an ACP **agent**: `agentty acp` (`src/acp/server.cpp`)
maps agentty's engine onto the ACP `session/update` stream for Zed. That
stream — `acp::SessionUpdate` in `acp-cpp/include/acp/updates.hpp` — is a
complete, normalized description of a streaming agent turn:

```
SessionUpdate ≅
    AgentMessageChunk  { content; messageId? }   // model prose
  + AgentThoughtChunk  { content; messageId? }   // reasoning / thinking
  + ToolCall           ToolCall                  // a tool the model wants
  + ToolCallUpdate     ToolCallUpdate            // progress / result / status
  + Plan               { entries }               // todo/plan
  + UsageUpdate         { used; size; cost? }
  + AvailableCommands / CurrentMode / ConfigOptions / SessionInfo
```

We emit this **outbound** today. The fix for "every provider is different"
is to make the same coproduct the **inbound** boundary: every backend
produces a `SessionUpdate` stream, and the core consumes only that. Then
agentty speaks ACP on *both* sides and a provider quirk lives in exactly one
adapter.

## 3. What the reference adapters taught us

`agentclientprotocol/codex-acp` and `agentclientprotocol/claude-agent-acp`
are the canonical way to put a backend behind ACP. Both are **thin protocol
translators**, and neither touches the model API directly:

- **codex-acp** spawns the **Codex App Server** (codex-rs's own JSON-RPC
  engine — which owns the Responses API, reasoning replay, the tool loop,
  the sandbox, MCP) and maps its `ServerNotification`s → `session/update`:
  `item/agentMessage/delta` → `agent_message_chunk`,
  `item/reasoning/*Delta` → `agent_thought_chunk`,
  `item/started` → `tool_call`, `item/completed` → `tool_call_update`,
  `thread/tokenUsage/updated` → `usage_update`, `turn/completed` → the turn
  settles with a stop reason, `error` → a classified failure.
- **claude-agent-acp** drives the **Claude Agent SDK** `query()` stream and
  forwards each `SDKMessage` the same way.

Three patterns worth stealing verbatim:

1. **One long-lived consumer** per session translates the upstream event
   stream; it is a `switch` over event kinds producing `SessionUpdate`s.
2. **One send chokepoint** — every update goes through a single function, so
   cross-cutting concerns (answer tracking, dedup, terminal latch) are a
   property of *sending*, not of every call site. This is exactly the
   "emit the terminal event once" rule `stream_epilogue.hpp` tries to
   enforce — but generalized to the whole stream.
3. **Per-turn stop reason** accumulated during the turn, read when the turn
   settles.

The deeper lesson: the upstream owns the hard parts, the adapter owns the
*translation*. agentty has two flavors of "upstream" (see §6).

## 4. Architecture

One boundary. Two backend flavors feeding it. The reducer downstream is
unchanged for now (a bridge maps `SessionUpdate` → the existing `Msg`s).

```
              ┌──────────────── agentty runtime / reducer ────────────────┐
              │      consumes ONE normalized stream of acp::SessionUpdate   │
              │      (+ a per-round StopReason to drive the agent loop)     │
              └───────────────────────────▲────────────────────────────────┘
                                          │  session_update → Msg  (bridge)
        ┌─────────────────────────────────┼─────────────────────────────────┐
        │                                 │                                   │
 ┌──────┴────────┐               ┌────────┴────────┐                 ┌────────┴─────────┐
 │ NativeAdapter  │  in-process   │ NativeAdapter   │                 │ ExternalAcp       │
 │  Anthropic     │  HTTP+SSE     │  Codex/Responses│                 │  Backend          │
 │  wire → SU     │               │  wire → SU      │                 │  drive a real ACP │
 │  quirk: think  │               │  quirk: reason  │                 │  agent subprocess │
 │  signature     │               │  replay         │                 │  (codex-acp,      │
 │  replay        │               │                 │                 │   claude-agent-acp)│
 └───────────────┘                └─────────────────┘                 │  SU passes through│
                                                                       └───────────────────┘
```

## 5. The interface

The provider contract stops being "emit `Msg`". It becomes "drive one round,
emit `SessionUpdate`, settle with a `StopReason`." Quirks stay inside.

```cpp
// include/agentty/provider/acp_backend.hpp
namespace agentty::provider {

// The single streaming sink. One normalized event in ACP's vocabulary.
struct TurnSink {
    std::function<void(acp::SessionUpdate)> update;
};

struct TurnError {
    std::string  message;
    std::optional<std::chrono::seconds> retry_after;
    bool         auth_expired = false;   // 401/403 → OAuth refresh path
    bool         user_cancel  = false;   // Esc, not a fault
};

struct TurnResult {
    // agentty's StopReason keeps `ToolUse` — a ROUND-level signal the agent
    // loop reads to decide continue-vs-finish. (ACP's own StopReason is a
    // FULL-TURN concept and is applied only at the outbound server seam.)
    StopReason               stop  = StopReason::EndTurn;
    std::optional<TurnError> error;      // set → the round failed
};

// Every backend — native in-process adapter OR external ACP subprocess.
class AcpBackend {
public:
    virtual ~AcpBackend() = default;

    // Drive ONE provider round-trip (one model completion). Streams
    // SessionUpdates through `sink`; returns the round's stop reason.
    // agentty's existing agent loop (kick_pending_tools) turns N rounds
    // into one user-visible turn and executes tools itself.
    virtual TurnResult prompt(const Request&               req,
                              const TurnSink&              sink,
                              const http::CancelTokenPtr&  cancel) = 0;
};

} // namespace agentty::provider
```

Key seam: **tool execution stays agentty's job.** A `SU_ToolCall` is only the
model *requesting* a tool; agentty's sandbox/permission/RAG tool stack runs
it and feeds the result back on the next round. This is why normalizing to
ACP does not surrender agentty's identity — providers never executed tools,
they only asked.

## 6. Two backend flavors, one boundary

- **Native, in-process (default).** The current HTTP+SSE transports,
  refactored to emit `SessionUpdate` + a `TurnResult` instead of `Msg`s.
  agentty keeps its agent loop, tools, sandbox, RAG, permissions. Round
  granularity = one model completion; `StopReason::ToolUse` drives the loop.
- **External ACP subprocess (opt-in).** For a backend that already ships an
  ACP adapter, spawn it (`codex-acp`, `claude-agent-acp`, any ACP agent) and
  drive it with acp-cpp's client side (`acp/agent.hpp`). Its `session/update`
  stream *is already* `SessionUpdate` — near-zero translation. Here the
  subprocess owns the tool loop; agentty is the ACP **client**, servicing
  `fs/read_text_file`, `fs/write_text_file`, `terminal/create` callbacks
  through its own sandbox + permission policy. This is the escape hatch that
  gets Codex's exact behavior (reasoning replay included) for free — but it
  trades away agentty's own tools, so it is a backend *choice*, not the
  unification's core.

Both converge on the identical inbound type. The runtime cannot tell which
flavor produced a given `SessionUpdate`, which is the whole point.

## 7. Mapping tables

### agentty `Msg` ⇄ `acp::SessionUpdate`

| agentty `Msg`                    | `acp::SessionUpdate`                         |
|----------------------------------|----------------------------------------------|
| `StreamTextDelta{text}`          | `SU_AgentMessageChunk{TextContent{text}}`    |
| `StreamThinkingDelta{text,sig}`  | `SU_AgentThoughtChunk{TextContent{text}}` (\*) |
| `StreamToolUseStart{id,name}`    | `SU_ToolCall{ id, title=name, Pending }`     |
| `StreamToolUseDelta{partial}`    | `SU_ToolCallUpdate{ id, rawInput+=partial }` |
| `StreamToolUseEnd{}`             | `SU_ToolCallUpdate{ id, InProgress }`        |
| `StreamUsage{...}`               | `SU_Usage{ used, size, cost? }`              |
| `StreamStarted / StreamHeartbeat`| (no arm — liveness only; bridge → NoOp)      |
| `StreamFinished{stop} / Error`   | not an arm — carried by `TurnResult`         |

(\*) the opaque `signature` / Codex `encrypted_content` is **backend-private
replay state**, not a wire-visible field. It rides in the adapter's own
request-builder (see §8), not in the `SessionUpdate`. That is exactly why
this design fixes the reasoning-loop: replay state is *owned by the one
adapter that needs it*.

### StopReason (round-level, agentty) vs ACP (turn-level)

| agentty `StopReason` | meaning                | ACP `StopReason` at server seam |
|----------------------|------------------------|---------------------------------|
| `ToolUse`            | run tools, loop again  | (internal — never surfaced)     |
| `EndTurn`            | model done             | `end_turn`                      |
| `MaxTokens`          | output cap hit         | `max_tokens`                    |
| `StopSequence`       | stop seq matched       | `end_turn`                      |
| `TurnError.user_cancel` | Esc                 | `cancelled`                     |

## 8. How this kills both live bugs

- **"cancelled" after a clean turn.** There is no per-provider post-loop to
  get wrong. A round ends by returning a `TurnResult`. A clean close returns
  `{stop}`; a real Esc returns `{error.user_cancel=true}`. The "emit exactly
  one terminal" rule is structural (one return value), not a convention four
  files must remember. `stream_epilogue.hpp` dissolves into the interface.
- **Codex tool loop.** The Codex native adapter requests
  `include:["reasoning.encrypted_content"]`, captures each `reasoning`
  item's id + encrypted payload as it streams, stores it as backend-private
  replay state keyed on the assistant turn, and re-emits it — immediately
  before the `function_call` it preceded — when building the next round's
  `input[]`. This is the identical *shape* as the Anthropic adapter replaying
  a thinking block's `signature`. Both are "provider requires its opaque
  reasoning token echoed back with tool calls"; both now live in one place
  per provider and never touch the core.

## 9. Migration plan (incremental, always green)

1. **Land the boundary type** — `acp_backend.hpp` (this doc's §5) +
   `provider/acp_bridge.{hpp,cpp}` with `to_msgs(const SessionUpdate&,
   BridgeState&) -> std::vector<Msg>`. The reducer is untouched.
2. **Wrap, don't rewrite.** Adapt the existing `stream(Request, EventSink)`
   providers behind `AcpBackend` with a shim that maps their current `Msg`s
   → `SessionUpdate` and back, proving the bridge round-trips. No behavior
   change; delete nothing yet.
3. **Migrate transports one at a time**, native emission of `SessionUpdate`,
   quirks confined:
   - Anthropic (reference — thinking-signature replay already correct).
   - Codex/Responses — **fold in reasoning replay here** (fixes the loop).
   - OpenAI, Ollama.
   Each migration is a self-contained PR; the bridge keeps the reducer stable.
4. **Delete `stream_epilogue.hpp`** once all four return `TurnResult`.
5. **Add `ExternalAcpBackend`** (§6) driving codex-acp / claude-agent-acp
   over acp-cpp's client side. Selectable per provider.
6. **(Optional) collapse the bridge** — teach the reducer to consume
   `SessionUpdate` directly and retire the `Msg` stream variants, making the
   inbound and outbound ACP seams literally the same code.

## 10. Open decisions

- **Concept vs. vtable.** The current `provider::Provider` is a *concept*
  (`p.stream(...)`). Pluggable backends (native + subprocess) want type
  erasure; `AcpBackend` is a small virtual base. One virtual call per round
  is free next to a network round-trip.
- **How far to push external ACP.** Ship it as an opt-in backend, or make it
  the *primary* path for Codex/Claude (delegating tools too)? Recommend
  opt-in first; revisit once native parity is proven.
- **Plan / AvailableCommands / Mode arms.** Inbound today only needs message
  / thought / tool_call / tool_call_update / usage. The rest are outbound-only
  until a backend emits them.

## 11. Shipped: selecting an external ACP agent

External ACP agents are now first-class SELECTABLE providers, wired through
the same one dispatch seam (`main.cpp` `stream_fn`) as the native backends —
no `Kind` fan-out. The pieces:

- **Registry** (`provider/registry.hpp`): `Kind::ExternalAcp` — but NO
  hardcoded per-agent rows. ACP is generic, like Zed's `agent_servers`: the
  provider picker lists agents from `enumerate_acp_agents()` (one built-in
  reference agent + every `acp-agents.json` entry) as dynamic virtual rows.
  `AuthStyle::None` (the agent handles its own auth), so selecting one never
  prompts for a key. There is deliberately no `codex-acp` row — Codex is a
  first-class NATIVE provider here, so a second "Codex (ACP)" row would be
  redundant; drive codex-acp by naming it in `acp-agents.json`.
- **Adapter** (`provider/acp_provider_adapter.cpp`): presents
  `ExternalAcpBackend` as a plain `stream(Request, EventSink)` Provider —
  spawns + caches the subprocess per agent id, translates each round's
  `session/update` into the same `Stream*` Msgs the native providers emit,
  settles from the `TurnResult`. Cached processes are torn down at exit via
  `release_acp_agents()` (bounded even for a wedged agent).
- **Launch config** (`provider/acp_agents.{hpp,cpp}`): resolves a spec id to
  its argv. Built-in defaults mean a user with the binary on `$PATH` selects
  it with ZERO config. To override the argv / pin a path / add another agent,
  drop an `acp-agents.json` (resolution mirrors `mcp.json`):

  1. `$AGENTTY_ACP_AGENTS` — explicit path (trusted).
  2. `~/.agentty/acp-agents.json` — user-global (trusted).
  3. `./.agentty/acp-agents.json` — workspace-local (gated behind
     `AGENTTY_ACP_ALLOW_PROJECT=1`, since it spawns arbitrary commands).

  ```json
  {
    "acpAgents": {
      "claude-agent-acp": {
        "command": "claude-agent-acp",
        "args": ["--stdio"],
        "env": { "FOO": "bar" },
        "cwd": "/optional/working/dir"
      },
      "my-agent": { "command": "my-acp", "args": ["serve"] },
      "codex-acp":  { "command": "codex-acp", "args": ["acp"] }
    }
  }
  ```

  Every config id (and the one built-in reference agent) is selectable
  straight from the provider picker, or via a raw `--provider <id>` spec.
  Tests: `acp_agents_test` (config + enumerate + selection routing) and
  `external_acp_backend_test` (the backend itself + hardened lifecycle).
```
