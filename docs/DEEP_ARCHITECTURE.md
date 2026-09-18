# Deep Architecture — How agentty Actually Works

A comprehensive technical deep-dive into agentty's internals: the Elm loop, provider heterogeneity, persistence, RAG pipeline, concurrency model, and every architectural decision that makes this a zero-compromise native terminal agent.

This is the map for someone who wants to **understand the whole system** — not just change one file. For changing code, start with [ARCHITECTURE.md](ARCHITECTURE.md). For specific subsystems, see the docs index at the end.

---

## Core Design Philosophy

**agentty** is a **zero-compromise native terminal agent** that rejects every industry norm:

| Industry Norm | agentty's Position |
|--------------|-------------------|
| "Electron is fine" | 16.7 MB **static binary**, 3 ms startup, no runtime |
| "JSON is enough" | **Algebraic types everywhere** — sum types, strong IDs, compile-time proofs |
| "The user should configure" | **Working defaults** — BM25 without config, OAuth in 2 clicks |
| "Thread safety via locks" | **Pure functional core** — Elm loop, explicit Cmd seam |
| "Providers are snowflakes" | **Heterogeneity as data** — registry rows, catalog facts, wire scaffolds |
| "Plugins need an SDK" | **MCP is the interface** — any stdio server is a plugin |
| "SQLite for everything" | **Append-only JSONL** — 1.4 ms turn save, debuggable, no WAL |

---

## 1. The Elm Loop — Predictable Concurrency

The entire runtime is one pure function applied in a loop:

```cpp
(Model, Msg) → (Model, Cmd<Msg>)
```

- **Model** — the whole application state tree (one aggregate struct)
- **Msg** — a closed sum type of every event that can happen
- **Cmd** — descriptions of side effects to run (network, disk, timers)

The runtime executes `Cmd`s on background threads and feeds their results back as new `Msg`s.

### What This Eliminates

| Problem | How Elm Eliminates It |
|---------|----------------------|
| Race conditions | Update is single-threaded |
| Deadlocks | No locks |
| Callback hell | Effects are values |
| Debugging nightmares | Time-travel possible — every state is reproducible from the Msg sequence |
| Shared mutable state | Every update returns a new Model |

### The Four Maya Hooks

Bound in `include/agentty/runtime/app/program.hpp`:

| Hook | Meaning |
|------|---------|
| `init` | Load settings + recent threads via Store seam |
| `update` | The reducer — `src/runtime/app/update.cpp` |
| `view` | `Model → Element` |
| `subscribe` | Timers and the live stream subscription |
| `visual_hash` | Render-skip gate; identical hash → skip frame |
| `needs_warmup` | One-shot fast scrollback rehydration on resume |

`main.cpp` is wiring only: parse argv, resolve credentials, construct the concrete `AnthropicProvider` + `FsStore`, install them behind the `Deps` seam, then hand `AgenttyApp` to `maya::run`.

### Msg Is Split By Domain

A naive design inlines every leaf event in one giant variant. That pins `sizeof(Msg)` to the heaviest leaf, instantiates an N-wide `std::visit` dispatch table, and forces the whole reducer TU to rebuild on any leaf change.

agentty instead groups leaves into ~15 **domain sub-variants** in `msg.hpp`:

```
variant<ComposerMsg, StreamMsg, ToolMsg, LoginMsg, 
        ThreadListMsg, CommandPaletteMsg, MentionPaletteMsg,
        SymbolPaletteMsg, TodoMsg, DiffReviewMsg, 
        CheckpointMsg, MetaMsg, ...>
```

The top-level reducer in `update.cpp` is then a small `std::visit` that forwards each domain to its own TU:

```cpp
auto step = std::visit(overload{
    [&](msg::ComposerMsg cm) { return detail::composer_update(std::move(m), std::move(cm)); },
    [&](msg::StreamMsg   sm) { return detail::stream_update  (std::move(m), std::move(sm)); },
    [&](msg::ToolMsg     tm) { return detail::tool_update    (std::move(m), std::move(tm)); },
    // … nine more domain arms …
}, msg);
```

Each `update/<domain>.cpp` recompiles only when its own leaves change. Call sites still build a `Msg` directly via `std::variant`'s converting constructor — only the owning domain accepts a given leaf, so the wrap is unambiguous.

**Compile-time win**: Adding a `ToolMsg` variant doesn't touch stream/composer TUs.

---

## 2. Provider Heterogeneity — Worse Is Better

> "There is no clean abstraction over LLM providers. The industry has at least three response-endpoint dialects (OpenAI Responses, Anthropic Messages, Mistral Conversations), the same model id can stream through different dialects on different hosts (`gpt-*` over `/responses` on Copilot but `/chat/completions` on Azure), effort enums differ per host, reasoning visibility is account-tier-gated, and hosts occasionally ship a model with its tool grammar disabled."

Instead of fighting this with a "clean" abstraction, agentty **embraces heterogeneity as first-class data**.

### Three Tables of Truth

| Table | What It Holds | Example | Where It Lives |
|-------|---------------|---------|----------------|
| **Registry Row** | Wire dialect, auth mechanism, streaming shape | `Wire::AnthropicMessages`, `auth::AnthropicKey` | `provider_registry.hpp` |
| **Catalog Fact** | Per-model quirks, effort support, tool grammar | `supports_effort()`, `is_weak_tool_user()` | `domain/catalog.hpp` |
| **Wire Scaffold** | Shared framing, salvage, retry logic | `SseFramer`, `finish_stream()`, `parse_retry_after()` | `provider/stream_scaffold.hpp` |

**Key insight**: Model differences are **declarative**, not behavioral. A new provider is adding a row to `provider_registry.hpp`, not forking the streaming loop.

### The Provider Concept

The `Provider` concept is deliberately tiny:

```cpp
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};
```

Anything that streams a chat completion satisfies it — the real Anthropic and OpenAI-compatible transports in production, a deterministic in-memory script in tests.

### The StreamResult End-State

Every provider returns (via the `EventSink` epilogue):

```cpp
provider::StreamResult {
    StreamEnd end;           // Normal | Cancelled | Errored
    std::optional<error>;
    std::optional<retry_after>;
    int http_status;
    
    bool ok() const;
    bool cancelled() const;
    bool already_terminated() const;
}
```

**Uniform epilogue** — whether Anthropic SSE or OpenAI chat/completions, the terminal state is the same shape. Error handling, retry backoff, and salvage logic live **once**.

### Shared Wire Scaffolding

Every ingress concern that is genuinely SHARED lives in exactly one place so a fix can't drift into three copies of itself:

- `provider::finish_stream` — terminal event epilogue
- `wire::SseFramer` / `LineFramer` — byte-level framing
- `provider::parse_retry_after` — Retry-After backoff parsing
- `wire::scrub_utf8` — strict UTF-8 validation
- `wire::could_be_tool_json` — leaked-tool-call prefix sniffer (weak local models need this)
- `provider::usage::{from_openai, from_responses, from_ollama}` — the three token-usage wire shapes
- `auth::bearer_token` — OpenAI-family auth header emission

Anthropic keeps its own arm-typed visit since an API key must route to `x-api-key`, never `Bearer`.

### Four Wire Dialects

| Dialect | Providers | File |
|---------|-----------|------|
| `AnthropicMessages` | Claude API, Pro, Max (OAuth) | `src/provider/anthropic/transport.cpp` |
| `OpenAIChat` | OpenAI, Groq, OpenRouter, Together, Cerebras, DeepSeek, xAI, Gemini, Mistral, Fireworks | `src/provider/openai/transport.cpp` |
| `OpenAIResponses` | ChatGPT Web (OAuth), GitHub Copilot (OAuth) | `src/provider/chatgpt/responses.cpp` |
| `OllamaNative` | Ollama (local) | `src/provider/ollama/transport.cpp` |

See [PROVIDER_HETEROGENEITY.md](PROVIDER_HETEROGENEITY.md) for the full design.

---

## 3. Thread Persistence — Append-Only Truth

**Problem**: SQLite adds 250 KB, JSONL is fast enough.

**Solution**: Per-thread structure optimized for the access pattern:

```
~/.agentty/threads/
  <id>.jsonl         # one message per line, append-only
  <id>.ofs           # 8-byte LE offsets (O(1) random access)
  <id>.meta.json     # title, timestamps, compactions (mutable metadata)
  blobs/<hash>       # images, tool output (content-addressed)
  index.json         # picker cache: which threads exist
```

### Measured Facts

From a 597 MB / 549-thread store, RelWithDebInfo build:

| Operation | Latency | Notes |
|-----------|---------|-------|
| Save one turn | **1.4 ms** | Append + 8-byte offset write |
| Load thread picker | **12 ms** | Scanning 549 `.meta.json` files |
| Rebuild `.ofs` from scratch | **12 ms** | When lost/corrupt |
| Parse overhead | JSON (simdjson) 1.4 ms ≈ SQLite 1.3 ms | Not worth 250 KB dep |

### Four Properties

| Property | Mechanism |
|----------|-----------|
| A turn costs O(1) to save | Append one line + 8 bytes |
| A torn write costs ≤1 turn | `.jsonl` is line-atomic; worst case = last message lost |
| Random access stays fast | `.ofs` index; loading message N doesn't scan N-1 lines |
| Blobs are shared | Content-addressed; one hash, many referrers |

### Why No SQLite

1. Parse cost of JSON (simdjson, 1.4 ms) ≈ SQLite query (1.3 ms) on this corpus
2. 250 KB dependency cost > performance win
3. Append-only JSONL is **debuggable** — `tail -f <id>.jsonl` during a session works
4. Content-addressed blobs enable **structural GC** — no refcounting bugs

### Blob GC — Mark-and-Sweep

**9 of 190 blobs** in a real store had multiple referrers. This means **per-thread deletion is a data-loss bug**.

The GC is structural:

```cpp
// ANY JSON key == "blob" or ending in "_blob"
// is a reference (because put_or_inline() generates <field>_blob)
agentty::blobs::collect_in(threads_dir, dry_run=true);
```

Mark phase scans `.jsonl` + `.meta.json`, sweep phase unlinks unreferenced blobs. **No per-file bookkeeping**, so no state to corrupt.

Shipped in commit `0de214b1`, design in [THREAD_STORE.md](THREAD_STORE.md).

---

## 4. RAG Pipeline — Production-Grade Retrieval

The default profile is **conservative** — no PhD required, no config file:

```
Query
  ↓
Structural Chunking (code-aware, preserves context)
  ↓
BM25 (always) + Dense Embeddings (if Ollama reachable)
  ↓
Reciprocal Rank Fusion (RRF, k=60)
  ↓
Feature Reranker (deterministic, no model)
  ↓
MMR Diversification (λ=0.7, avoids redundant passages)
  ↓
Adjacent-Hit Stitching (merge consecutive passages)
  ↓
Query-Focused Compression (strict aggregate budget)
```

### Two Indexes

| Tool | What It Searches | Index |
|------|-----------------|-------|
| `search_docs` | Documentation, skills, learned memory, MCP resources | `~/.agentty/rag_docs.ragdb` |
| `search_code` | Semantic code search (when you don't know the identifier) | `./.agentty/rag_code.ragdb` |

### Working Defaults

- **BM25 is always enabled** — zero config, instant results
- **Dense embeddings are opportunistic** — if Ollama is reachable on localhost, agentty adds semantic retrieval
- **Expensive stages are opt-in** via environment variables:
  - `AGENTTY_RAG_PRF=1` — Pseudo-Relevance Feedback (query expansion)
  - `AGENTTY_RAG_GRAPH=1` — GraphRAG (PageRank + community detection)
  - `AGENTTY_RAG_CORRECTIVE=1` — LLM-based grading (slow, needs model)

### Provenance

Every passage retains source and path:

```
docs:guide/auth.md:20-44
skill:release-checklist
memory:fact-id
mcp:file://resource-uri
code:src/auth/token.cpp:80-126
```

Use the provenance to cite, open, or verify retrieved information rather than treating it as ungrounded model knowledge.

**Key design choice**: If you don't have an embedding server, you still get BM25 immediately. If you do, you get hybrid. No config required, no waiting for setup.

See [website/retrieval.md](website/retrieval.md) for the full pipeline.

---

## 5. Smart Mode — Complexity-Scaled Orchestration

**One master switch + three model slots**:

| Role | When It's Used | Example |
|------|----------------|---------|
| **Strategic** | Main turns, orchestration | Sonnet-4 (flagship) |
| **Implementation** | Delegated coding tasks | Sonnet-3.5 (mid-tier) |
| **Utility** | Compaction, summaries | Haiku (cheap) |

### Four Behaviors When Enabled

| Behavior | What It Does | Where |
|----------|--------------|-------|
| Internal routing | Utility engine calls (compaction) → cheapest model | `smart::utility_model` |
| Orchestration | Main turn on Strategic + `<smart-mode>` delegation directive | `cmd_factory`, `launch_stream` |
| Subagent routing | Each `task` worker's model by its role | `mcp_tools_backends`, `run_one_completion` |
| Complexity-scaled effort | Classify turn → scale Strategic effort | `smart::classify_turn` + `effort_for_score` |

### The 🧠 Routing Card

Renders as first-class UI — you see which model served the turn, with a per-role accent color.

### Cascade Feedback

In-session bias self-corrects from delegation + build outcomes:

- Successful delegation → slight boost to Strategic's confidence in delegation
- Build failures → slight bias toward Implementation (more implementation detail next time)

### Learned Workspace State

Persists:

- Effort level distributions per complexity bucket
- Decomposition patterns that worked

### Off Is a Strict No-Op

Zero tokens, zero latency, zero wire changes when disabled.

Shipped design in [design/smart-mode.md](design/smart-mode.md).

---

## 6. Sandboxing — Topology-Based Confinement

| Backend | Platform | Mechanism |
|---------|----------|-----------|
| `bwrap` (bubblewrap) | Linux | Mount namespaces |
| `sandbox-exec` | macOS | Sandbox profiles |

### What's Wrapped

`shell`, `diagnostics`, `git`, `process_start`, lifecycle hooks, external ACP agents.

### What's NOT Wrapped

Provider traffic (in-process, never crosses this boundary).

### Key Properties

- **The network is shared** — `--share-net` is passed so tools can hit APIs. This is intentional — a headless CI agent or SSH-airgap deployment needs outbound HTTP for providers.
- **Paths outside the workspace** are not bound at all → `No such file or directory` (not "permission denied").
- **Availability is a capability probe** — agentty runs a real confinement attempt at startup. Reporting `sandbox: active` means it actually works, not just that the binary exists.

### Invocation

```bash
--sandbox auto   # wrap when a backend is available (default)
--sandbox on     # wrap, and refuse to start if no backend is available
--sandbox off    # never wrap
```

The startup banner names the backend you actually got:

```
agentty: sandbox: active (bwrap)
```

See [SANDBOX.md](SANDBOX.md) for the full design.

---

## 7. Tool System — Typed Bundles, JSON Edge

Every tool is a `concept`:

```cpp
template <class T>
concept Tool = requires {
    typename T::Args;
    typename T::Result;
    { T::name() }         -> std::convertible_to<std::string_view>;
    { T::description() }  -> std::convertible_to<std::string_view>;
    { T::input_schema() } -> std::convertible_to<nlohmann::json>;
    { T::effects() }      -> std::convertible_to<EffectSet>;
} && requires(const nlohmann::json& args) {
    { T::execute(args) }  -> std::convertible_to<ExecResult>;
};
```

**Typed internally, JSON at the boundary.** The model sees JSON schemas; agentty executes typed C++ functions.

### Per-Tool Output Budget

A runaway `bash`/`grep`/`read` can't blow the context window. Truncation is UTF-8-safe with three strategies:

| Strategy | When | Examples |
|----------|------|----------|
| **Head** | Ordered chunks | `read`, `write`, `edit` |
| **Tail** | Logs, build output | `bash`, `diagnostics` |
| **HeadTail** | Both ends carry signal | `grep`, `web_*`, `git diff` |

### Permission Policy — Constexpr Matrix

Every tool declares an `EffectSet` over four bits: `ReadFs`, `WriteFs`, `Net`, `Exec`.

The active **Profile** plus that effect set feed the pure `constexpr` function `policy::permission(effects, profile)` in `tool/policy.hpp`:

| Profile | Pure | ReadFs | WriteFs | Net | Exec |
|---------|------|--------|---------|-----|------|
| **Write** | Allow | Allow | Allow | Allow | Allow |
| **Ask** | Allow | Allow | **Prompt** | **Prompt** | **Prompt** |
| **Minimal** | Allow | **Prompt** | **Prompt** | **Prompt** | **Prompt** |

The entire table is **proved at compile time** — 48 cells (16 effect sets × 3 profiles). A second function, `expected_decision`, re-states the policy independently, and an exhaustive `constexpr` loop `static_assert`s `permission(e, p) == expected_decision(e, p)` over every cell — so a one-handed change to either side breaks the **build**, not a test nobody runs.

### Shipped Tools

`read`, `write`, `edit`, `move`, `remove`, `bash`, `process_start`/`process_poll`/`process_stop`, `grep`, `glob`, `list_dir`, `outline`, `repo_map`, `find_definition`, `search_structural`, `rewrite_structural`, `replace`, `extract`, `aggregate`, `read_filter`, `web_fetch`, `web_search`, `json_query`, `todo`, `diagnostics`, `test`, `git_status`, `git_diff`, `git_log`, `git_show`, `git_blame`, `git_commit`, `git_branch`, `git_stash`, `git_rebase`, `git_cherry_pick`, `remember`, `forget`, `wipe_memory`, `search_docs`, `search_code`, `task` (subagent dispatch), `skill` (load a skill body on demand).

The tree-walking tools (`grep`, `glob`, `list_dir`, `repo_map`, `find_definition`, the @-file picker, the symbol index) share one directory skip-list (`should_skip_dir`: `.git`, `node_modules`, `build*`, `cmake-build*`, `_deps`, `target`, `vendor`, `.venv`, …) so generated and fetched trees never flood results.

---

## 8. MCP Integration — Plugins Are Just MCP Servers

**There is no separate plugin API.**

Any MCP server (Python, Node, Go, shell wrapper, another agentty) is a plugin:

### Integration Flow

1. **Config**: `~/.agentty/mcp.json` (user) or `./.agentty/mcp.json` (project, trust-gated)
2. **Startup**: spawn command, `initialize` + `tools/list` handshake
3. **Namespace**: `mcp__<server>__<tool>` (collision-free)
4. **First-class**: same permission prompts, tool cards, cancellation as native tools

### Config Shape

Claude-Desktop-compatible:

```json
{
  "mcpServers": {
    "today":  { "command": "python3", "args": ["/abs/path/today.py"] },
    "github": { "command": "mcp-server-github",
                "env": { "GITHUB_TOKEN": "…" } }
  }
}
```

### Passthrough Tools (Proxy-Advertised)

For LiteLLM/enterprise gateways that inject schemas but can't execute:

```json
{
  "mcpServers": {
    "headroom": {
      "type": "passthrough",
      "url": "http://localhost:8787/v1/retrieve",
      "passthrough": ["headroom_retrieve"]
    }
  }
}
```

agentty POSTs args to the URL and returns the response body as the tool result.

### Why Out-of-Process

| Benefit | Why It Matters |
|---------|----------------|
| Plugin can't segfault the TUI | Isolation |
| Can't read agentty's credentials or heap | Security |
| Killed cleanly on cancel | Resource cleanup |
| Brings its own runtime | No Python-version or ABI fights |
| stdio IPC (~1 ms) is noise | Model round-trip takes seconds |

This is a deliberate architecture decision, not a limitation.

### Managing Plugins

```bash
agentty plugin add today --python tools/today.py     # local Python script
agentty plugin add fetch --uvx mcp-server-fetch      # PyPI package via uv
agentty plugin add fs    --npx @modelcontextprotocol/server-filesystem /tmp
agentty plugin add gomcp -- /usr/local/bin/my-go-server --flag x
agentty plugin list
agentty plugin remove fetch
```

See [PLUGINS.md](PLUGINS.md) for the full design + examples.

---

## 9. Rendering — Zero-Copy, Diff-Based, SIMD

**maya** (the TUI framework) does:

- **Compile-time UI DSL** — `t<"Hello"> | Bold | border_<Round>` is type-state safe
- **SIMD frame diff** — AVX2/SSE4.2/NEON, 64-bit packed cells, O(1) compare
- **Real flexbox** — Yoga layout (`grow()`, `gap()`, `align()`, `justify()`)
- **Responsive by measurement** — `row({a,b,c,d})` wraps/stacks automatically

### agentty's Rendering Loop

```cpp
Model → view(m) → Element → maya::render() → terminal escape codes
```

**Rendering is pure** — `view : Model → Element`. maya owns every pixel, border, animation.

### Optimization Strategies

| Strategy | What It Does | Impact |
|----------|--------------|--------|
| **Frozen content cache** | Finalized messages cache their `Element` and return the same pointer every frame | Never re-render finished messages |
| **Streaming content is stateful** | `StreamingMarkdown` keeps a block-boundary cache | Each delta is `O(new_chars)`, not `O(full_text)` |
| **Width-adaptive widgets** | `StatusBar` drops breadcrumb < 130 cols, token stream < 110, context bar < 55 | Graceful degradation |
| **Visual hash gate** | Identical hash → skip frame | Don't render when nothing changed |

### Measured Costs

From a 29 MB / 2519-message thread:

- **Rendering**: 4 ms (not the bottleneck)
- **Image decode at load**: the real cost (fixed with lazy `ImageContent`)

### The Adapter Pattern

Every adapter file under `src/runtime/view/` has the same shape: one function `Model → SomeWidget::Config`.

```
src/runtime/view/
├── view.cpp                              # AppLayout
├── thread/
│   ├── thread.cpp                        # Thread
│   ├── conversation.cpp                  # Conversation
│   └── turn/
│       ├── turn.cpp                      # Turn
│       └── agent_timeline/
│           ├── agent_timeline.cpp        # AgentTimeline
│           └── tool_body_preview.cpp     # ToolBodyPreview
└── status_bar/
    ├── status_bar.cpp                    # StatusBar
    ├── phase_chip.cpp                    # PhaseChip
    └── token_stream_sparkline.cpp        # TokenStreamSparkline
```

**One widget, one adapter file.** Filenames mirror the widget they adapt; the directory tree mirrors the widget hierarchy.

See [RENDERING.md](RENDERING.md) for the full design.

---

## 10. Memory System — Learned Facts

Two scopes:

| Scope | Path | When |
|-------|------|------|
| `project` | `./.agentty/memory.jsonl` | This codebase only |
| `user` | `~/.agentty/memory.jsonl` | All projects |

### Smart Scope Routing

If you `remember` a first-person fact ("I prefer fish shell") with `scope=project`, it auto-routes to `user` and notes `"scope→user"` in the reply.

### Dedup Is Automatic

Near-identical facts refresh timestamp + hit count instead of writing duplicates.

### Pinned Facts

Pass `pin=true` for facts the user has explicitly emphasized ("always do X", "never do Y"). Pinned facts survive cap rollover and render with ★.

### Reflective Write-Back

After finishing a non-trivial task (root-caused a bug, discovered build invocation), distill **at most one** durable, verified fact and store it with `remember` (scope=project, tag by topic).

**The bar**: Would a fresh session waste ≥10 minutes rediscovering this? If yes, store; if routine, don't.

Never store secrets.

### Tools

| Tool | Purpose |
|------|---------|
| `remember` | Persist a fact |
| `forget` | Remove by id or substring |
| `wipe_memory` | Clean slate (requires confirmation) |

Facts are searchable via `search_docs` (they go into the RAG index).

---

## 11. ACP Server Mode — Zed Integration

agentty speaks the **Agent Client Protocol**:

```
┌─────────┐         ACP          ┌──────────┐
│   Zed   │ ◄── JSON-RPC/stdio ──►│ agentty  │
└─────────┘                       └──────────┘
```

### acp-cpp — First C++ Implementation

Header-only, algebraic types, no `nlohmann::json` blobs:

```cpp
StdioTransport tx(zed_out, zed_in);
ClientConnection client(tx.sink(), handlers);
client.on_agent_message_chunk = [](AgentMessageChunk c) {
    match(c.content,
        [](TextContent t) { std::cout << t.text; },
        [](auto&) {});
};
```

### Four Layers

| Layer | Headers | Role |
|-------|---------|------|
| **1. Algebra** | `core.hpp`, `codec.hpp` | `Unit`, `Maybe<A>`, `Pair<A,B>` (closed, typed) |
| **2. Protocol** | `protocol.hpp` | Typed requests/responses (not JSON blobs) |
| **3. Connections** | `client.hpp`, `server.hpp` | `AgentConnection`, `ClientConnection` (symmetric) |
| **4. Transports** | `stdio.hpp` | stdio, network (pluggable) |

### Same Tool Catalog

TUI, `agentty run` (headless), ACP mode, and subagents all see the same merged tools (native + MCP).

See [acp-cpp/README.md](../acp-cpp/README.md) for the full protocol design.

---

## 12. Testing — 200+ Tests, Golden Streams

171 test cases across 18 files (CTest infrastructure):

### Notable Test Categories

| Test | What It Validates |
|------|------------------|
| `anthropic_sse_golden_test` | Replays real SSE streams |
| `scrollback_oracle_test` | Terminal state correctness |
| `provider_conformance_test` | Shared protocol compliance |
| `wire_fragmentation_test` | Partial JSON, mid-string cutoffs |
| `sandbox_escape_test` | Confinement boundary |
| `native_visibility_test` | Theme invariants (after #45 "invisible reasoning blocks") |

### Provider Conformance Suite

Every wire dialect goes through the same validation:

- Truncation retry logic
- Retry-After backoff
- Terminal event discipline (stop/error emit exactly once)
- UTF-8 scrubbing

### Theme Invariants

Learned from bugs, guarded by tests:

1. **Channel bug** — Never read `r()/g()/b()` off a non-Rgb `LitColor` without `has_channels()` guard
2. **Palette bug** — Never paint a filled element when `theme_owns_canvas() == false` (under `theme::native`)

Both guarded by tests that keep **one StylePool across frames** (the only way to catch cross-frame render state bugs).

See `tests/theme_alternation_test.cpp` for the pattern.

---

## 13. What Makes This Fast

| Optimization | Impact | Where |
|--------------|--------|-------|
| **Static binary** | 3 ms startup (no Python/Node interpreter) | Build system |
| **Elm loop** | No race conditions, no locks, pure update | Architecture |
| **SIMD frame diff** | Only changed cells write to terminal | maya |
| **Frozen content cache** | Finalized messages render once, return pointer | `cache.cpp` |
| **Append-only .jsonl** | Save turn = 1.4 ms (no journal, no WAL) | Persistence |
| **Structural chunking** | RAG chunks preserve code boundaries | RAG |
| **BM25-first** | Zero config, instant results | RAG |
| **Per-tool output budget** | Truncate before context window, not after | Tool system |
| **ccache + split update TUs** | Incremental rebuilds ~1.7s (debug preset) | Build system |
| **Content-addressed blobs** | One hash, many referrers | Persistence |
| **Provider scaffolds** | Shared logic lives once | Provider system |

### Build System — One Shared Tree

**One build tree** (`./build`), shared by every preset:

```bash
cmake --build build --target agentty -j12  # ~1.7s incremental
```

**No LTO in Debug** — `MAYA_HEAVY_OPT` defaults OFF when `CMAKE_BUILD_TYPE=Debug`, so a one-file edit rebuilds in ~10s, not minutes.

### Test Discipline

From learned memory:

- Build debug binary **first** (`--target agentty`)
- Report it as ready **before** building/running tests
- Never run full suite routinely (slow)
- Prefer focused tests: `ctest -R <pattern>`

### Measured Facts

| Operation | Latency | Notes |
|-----------|---------|-------|
| Debug binary | 1.7s incremental | No LTO, ccache enabled |
| Full suite | Slow | Never run unless warranted |
| Release binary | Multi-minute | LTO enabled |

---

## 14. Concurrency Model — Explicit Cmd Seam

### The Rule

> Anything reachable from a REDUCER goes through maya's Cmd seam (`task`/`task_isolated`); infrastructure BELOW the runtime (`io/http`, `io/persistence`, `acp/server`, `mcp/*`, `rag/adapter`, `tool/mcp_tools_backends`) keeps its own threads because `agentty mcp-serve|run|acp` never call `maya::run()` — there is no BackgroundQueue on those paths.

### Three Concurrency Domains

| Domain | Where | Threading Model |
|--------|-------|-----------------|
| **Update loop** | `src/runtime/app/update.cpp` | Single-threaded, pure |
| **Cmd effects** | maya's `BackgroundQueue` | Thread pool, results → Msg |
| **Infrastructure** | `io/*`, `mcp/*`, `rag/*` | Own threads (headless modes) |

### HTTP Streaming

- Request built on UI thread
- `http::Client` dispatches to worker thread
- `StreamHandler` callbacks fire on worker thread
- Each callback posts a `Msg` to the UI thread
- Update loop processes `Msg`, returns new `Model`

### Cancel Tokens

`cancel` is a `shared_ptr<http::CancelToken>` for the in-flight HTTP/2 stream — set when `launch_stream` dispatches the worker, nulled when the terminal `Msg` lands.

Tripping it from the UI thread (`Msg::CancelStream`) tears the stream down within a few hundred ms.

Shipped design in commit `7e6eb320` + maya `fd2bfc1`.

---

## 15. Streaming State Machine

`StreamState` (see `include/agentty/domain/session.hpp`) owns the per-turn lifetime of a single in-flight LLM request.

### Phase

`Phase` is a `std::variant` of four empty structs:

```cpp
variant<Idle, Streaming, AwaitingPermission, ExecutingTool>
```

Modeling it as a variant rather than an `enum class` lets us express "exactly one of these is true" in the type system and get exhaustiveness via `std::visit`.

### Truncation Retries

Two conditions trigger a transparent retry:

1. **Missing required field** — `guard_truncated_tool_args` notices the parsed args object lacks a schema-required key (e.g. `write` without `content`). Implies the wire died before the field arrived.
2. **Mid-string cutoff** — `ended_inside_string` detects the wire ended inside a JSON string value. The tool is marked `stream_mid_string_truncated` and left `Pending`; `finalize_turn` treats the flag exactly like (1) for retry purposes.

Only on retry-budget exhaustion does the tool surface as `Failed` with the actionable "re-emit the tool with the full payload—prefer `edit` over `write`" message.

### Live tok/s Speedometer

Anthropic only emits `message_delta.usage.output_tokens` rarely — often just once, right before `message_stop`.

Instead we accumulate the byte length of every text/json delta as it arrives (`live_delta_bytes`) and divide by ~4 (the Claude tokenizer averages ~3.5–4 bytes per token) to get the live rate.

`first_delta_at` is stamped on the first non-empty delta so the divisor excludes time-to-first-token (TTFT). Both reset on every `StreamStarted` so each sub-turn after a tool exec measures cleanly.

### Sparkline Ring Buffer

`rate_history` is a fixed-size ring buffer (`kRateSamples = 16`, sized to match the status-bar sparkline width) sampled every ~500 ms.

The status bar renders those samples as a row of ▁▂▃▄▅▆▇█ glyphs next to the numeric rate, giving the user a visual "wire is alive" cue and a glance-readable trend (rising / steady / falling).

See [design/streaming.md](design/streaming.md) for the full state machine.

---

## 16. Seams — How Concrete Types Stay Hidden

`AgenttyApp` must not be templated on the Provider and Store types — that would force every translation unit to know the concrete types and rebuild when they change.

Instead, `include/agentty/runtime/app/deps.hpp` defines a small `Deps` struct of `std::function`s:

### Three Seams

| Seam | Functions | Purpose |
|------|-----------|---------|
| **Provider** | `stream(Request, EventSink)` | Abstract over Anthropic/OpenAI/Ollama/etc. |
| **Store** | `save_thread`, `load_threads`, `load_thread`, `load_settings`, `save_settings`, `new_thread_id`, `title_from` | Abstract over filesystem/cloud/test stores |
| **Auth context** | Typed `AuthHeader` | Current session credentials |

`main.cpp` calls `app::install(provider, store, auth_header)` once at startup; the reducer reaches the seams through `app::deps()`.

`update_auth(...)` live-swaps credentials after an in-app login without restarting the process — in-flight streams cached the header at request-build time, so they are unaffected.

---

## 17. Key Architectural Invariants

These are the load-bearing rules. Break one and something fundamental breaks.

1. **The Elm loop is sacred** — Every state change is `(Model, Msg) → (Model, Cmd)`. No shared mutable state, no callbacks.

2. **Providers are heterogeneous** — Differences live in tables (registry, catalog, scaffolds), not branches.

3. **Persistence is append-only** — `.jsonl` for messages, content-addressed blobs, structural GC.

4. **Tools are typed internally, JSON at the boundary** — The model sees schemas, agentty executes functions.

5. **Plugins are MCP servers** — No separate API, no SDK, no ABI. stdio is the interface.

6. **Rendering is pure** — `Model → Element`. maya owns every pixel, border, animation.

7. **Concurrency is explicit** — `Cmd` seam, background threads, results fed back as `Msg`.

8. **Tests guard invariants** — Theme bugs, provider conformance, sandbox escapes all have tests.

9. **Fast is a feature** — 3 ms startup, 1.4 ms turn save, SIMD diff, frozen content cache are non-negotiable.

10. **Worse is better** — JSON + ripgrep + bubblewrap + append-only files beat "clean" abstractions.

---

## 18. Directory Layout

`include/agentty/` and `src/` mirror each other by domain:

```
include/agentty/          src/
├── domain/               ├── domain/
│   ├── conversation.hpp     │   └── …
│   ├── catalog.hpp          │
│   ├── session.hpp          │
│   └── id.hpp               │
├── runtime/              ├── runtime/
│   ├── model.hpp            │   ├── app/
│   ├── msg.hpp              │   │   ├── update.cpp
│   ├── panel/               │   │   └── update/
│   └── view/                │   ├── panel/
│                            │   └── view/
├── provider/             ├── provider/
│   ├── stream_scaffold.hpp  │   ├── anthropic/
│   └── registry.hpp         │   ├── openai/
│                            │   ├── ollama/
│                            │   └── chatgpt/
├── tool/                 ├── tool/
│   ├── registry.hpp         │   └── tools/
│   └── policy.hpp           │
├── io/                   ├── io/
│   ├── http.hpp             │   ├── http.cpp
│   ├── persistence.hpp      │   └── auth.cpp
│   └── auth.hpp             │
└── scope/                └── scope/
```

Headers carry types and inline logic; `src/` carries heavier implementations.

### Domain Breakdown

| Directory | What It Holds | I/O? |
|-----------|---------------|------|
| `domain/` | Pure data — `session`, `conversation`, `catalog`, `todo`, `profile`, strong-id newtypes | **No** |
| `runtime/` | App logic — `Model`, `Msg`, panels, view | **No** (pure) |
| `provider/` | Provider concept + implementations (Anthropic, OpenAI, Ollama, ChatGPT/Codex) | **Yes** (HTTP) |
| `tool/` | Tool concept, registry, permission policy, native tools | **Yes** (exec, fs, net) |
| `scope/` | Config-resolution algebra (Locus × Dialect precedence, provenance, Trust) | **No** |
| `io/` | HTTP, TLS, OAuth, persistence, clipboard | **Yes** |
| `acp/` | ACP server mode | **Yes** (stdio) |
| `mcp/` | MCP bridge | **Yes** (stdio, child processes) |
| `rag/` | RAG adapter | **Yes** (embed backend) |

---

## 19. Loop Mode — Repeat Until You Stop It

**Shipped in v0.7.0.** Press `^B` to arm a message — agentty re-sends it after **every completed turn** until you press `^B` again.

### The Shape

| Concern | Decision |
|---------|----------|
| What repeats | A **snapshot** taken at arm time (`loop_text` / `loop_attachments`) |
| When it repeats | After a turn that **ended normally**, from `finalize_turn` |
| On failure | **Back off**, don't stop and don't spin |
| On `Esc` | Stop outright — Esc means stop |
| Composer while armed | Shows the armed prompt, **read-only** |
| Wire visibility | **None** — an auto-send is a plain user turn |

All state lives on `ComposerState` (`m.ui.composer.loop_*`) and never leaves the UI layer: not serialized into the thread, not sent as a header, no provider code branches on it.

### Why a Snapshot, Not the Live Composer

Arming copies the payload rather than re-reading `composer.text` each iteration:

1. **You can keep typing** — the thing that repeats is the thing you armed
2. **The display can't lie** — paired with read-only lock, the box and `loop_text` are the same bytes by construction

Arming on an **empty** composer is refused.

### Backoff — The Part That Matters

The first version gated only on `is_idle()`. It never asked *why* the turn ended, so a rate-limited turn re-sent instantly and agentty deepened the user's own rate limit. **This was observed in the field.**

The rule now:

- **Success** re-sends immediately (the completed turn already took wall-clock time)
- **Failure** arms `loop_wait_until_ms`:
  - Provider's `Retry-After` is obeyed **verbatim** when present
  - Otherwise `loop_failures` escalates per consecutive failure:

| Class | Base | Cap |
|-------|------|-----|
| RateLimit / Auth | 30 s | 10 min |
| Transient / other | 5 s | 2 min |

- **Cancelled** disarms (Esc is unambiguous)

### Waking a Sleeping Loop

`finalize_turn` is the normal re-send site, but a sleeping loop has no turn to end. The **Tick** resumes it: `animation_demand` keeps the frame clock armed while `loop_wait_until_ms > 0`, and the Tick fires the moment the deadline passes.

### UX

- `⟳ LOOP` while armed, `⟳ LOOP ×N` once it has re-fired
- `⟳ RETRY 24s` counting down while backing off
- Brand-tinted border while idle-armed
- Highest keep-priority chip (app acting on its own must never render as idle)

See [design/loop-mode.md](design/loop-mode.md) for the full design.

---

## 20. Scope Model — Config Resolution Algebra

**Problem**: Five config concerns (memory, skills, agents, commands, MCP) each answered "where does this live, who placed it, may I execute it" with incompatible shapes.

**Solution**: One pure algebra they all fold through.

### Three Axes (Kept Separate)

```cpp
Locus     // WHOSE config: Explicit ▷ Local ▷ Project ▷ User
Dialect   // DIRECTORY tribe: .agentty ▷ .agents ▷ .claude
Trust     // May I EXECUTE: Trusted | Pending | Blocked
```

- **`Locus`** is a precedence lattice — declaration order **is** resolution order
- **`Dialect`** is a separate axis so the six-root ladder is the **product** `Locus × Dialect`
- **`Trust`** is bound to **content**, never inferred from `Locus`

### The Model

```cpp
Source {                    // A resolved origin — carries provenance
  locus, dialect, base, writable
}

Tagged<T> {                 // Every resolved item knows its origin
  value: T, source: Source
}

Layout { leaf, explicit_env }
Env { home, project_root, … }
```

### Two Monoids Over One Source List

`plan(Layout, Env)` emits the ordered `Source` list (Locus-major, Dialect-minor). Two resolvers fold it:

| Resolver | Monoid | Used By |
|----------|--------|--------|
| `resolve_first` | **override** — first present source wins | memory, hooks, MCP-as-one-file |
| `resolve_union` | **union** — merge all, first-key-wins shadow | skills, agents, commands |

`resolve_union`'s first-key-wins rule *is* "project ▷ user, native ▷ interop" for free.

### Trust (The MCPoison Fix)

Cursor's CVE-2025-54136 pinned trust to a server's **name**. agentty binds it to **content**:

- `Explicit` / `User` config is **implicitly trusted**
- `Project` / `Local` executable config starts **`Pending`** and becomes `Trusted` only when *that exact content hash* is approved
- Change the bytes → approval is void → re-gate

Approvals persist **outside** any committed file, so a cloned repo can never approve its own servers.

See [design/scope-model.md](design/scope-model.md) for the full algebra.

---

## 21. Clipboard & Image Paste

Paste an image with `Ctrl+V`. Locally this just works. **Over SSH it needs one setting**.

### The 30-Second Fix (kitty over SSH)

Add to **your laptop's** `~/.config/kitty/kitty.conf`:

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
clipboard_max_size 0
```

Then **fully restart kitty**. Inside tmux:

```bash
tmux set -g allow-passthrough on
```

### Why SSH Is Different

Your clipboard lives on the machine your **terminal** runs on. When agentty runs on a remote host, it asks the *terminal* over the pty. Two dialects:

| Dialect | Carries | Supported By |
|---------|---------|-------------|
| **OSC 5522** | images **and** text | **kitty only** |
| **OSC 52** | text only | iTerm2, WezTerm, Ghostty, foot, Terminal.app, xterm |

A terminal that doesn't know OSC 5522 ignores it and answers the text request.

### Diagnose in 10 Seconds

```bash
probe() {
  exec < /dev/tty
  old=$(stty -g); stty raw -echo min 0 time 0
  printf "$2" > /dev/tty
  got=0; i=0
  while [ $i -lt 3 ]; do
    n=$(dd bs=4096 count=1 2>/dev/null | wc -c)
    got=$((got + n)); i=$((i + 1)); sleep 1
  done
  stty "$old"; printf '%-22s %s bytes\n' "$1" "$got"
}
probe "control (DA1)"      '\033[c'
probe "clipboard (OSC 52)" '\033]52;c;?\033\\'
```

Read the result:

| control | clipboard | Meaning |
|---------|-----------|--------|
| **0** | 0 | Replies never reach you — suspect mosh or a pty issue |
| **>0** | **0** | Terminal is **refusing clipboard reads** (kitty `clipboard_control`) |
| >0 | >0 | Reads permitted; if images fail, use ferry or `@path` |

### Clipboard Ferry (Non-kitty Terminals)

Terminals without OSC 5522 can't send image bytes. Ferry over SSH:

```bash
# macOS laptop (brew install pngpaste)
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop pngpaste -'

# Wayland laptop
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop wl-paste -t image/png'

# X11 laptop
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop xclip -selection clipboard -t image/png -o'
```

See [website/clipboard.md](website/clipboard.md) for the full guide.

---

## 22. Security — Audit Findings & Hardening

**Status: All findings remediated.** 55/55 tests green. Scope: workspace boundary, sandbox, subprocess spawner, tool permissions, credential storage, TLS, OAuth.

### Critical Findings (Remediated)

| # | Severity | Finding | Fix |
|---|----------|---------|-----|
| **F1** | **HIGH** | PKCE `code_verifier` + OAuth `state` from `std::mt19937_64` (non-CSPRNG) | Rewrote `random_urlsafe` to draw from OpenSSL **`RAND_bytes`** |
| **F2** | **MED** | Anti-CSRF `state` generated but **never verified** | `exchange_code` now does **constant-time compare** against expected value |
| F3 | LOW | Credentials file written in-place with `O_TRUNC` (crash → corruption) | `write_private` now writes temp + `fsync` + `rename` atomically |
| F4 | LOW | Config dir created with process umask (potentially world-listable) | `config_dir()` best-effort **`chmod 0700`** |
| F5 | LOW | No TLS certificate pinning | **Opt-in SPKI pinning** via `AGENTTY_TLS_PINS` |

### Credential-At-Rest Hardening (Two Opt-In Layers)

Baseline: `credentials.json` sealed with **AES-256-GCM**, key = `HKDF-SHA256(machine_seed, salt)`.

#### Layer 1: Passphrase Factor

When configured, key becomes `HKDF( machine_seed ‖ MHF(passphrase, salt) )` — decryption requires **both** machine **and** secret.

- **KDF**: Prefers **Argon2id** (t=3, m=64 MiB, p=1) when OpenSSL ≥ 3.2, falls back to **scrypt** (N=2¹⁵, r=8, p=1)
- **Envelope versioning**: v2 files stamp `kdf` + exact params, v1 files stay machine-only
- **Fail-closed**: v2 file with no/wrong passphrase → `nullopt`

**Enable**: `AGENTTY_PASSPHRASE=<secret>` or `AGENTTY_ENCRYPT_PASSPHRASE=1` (prompt)

#### Layer 2: OS Keystore Backing

When enabled, OS vault holds the sealed envelope; encrypted file remains as fallback.

- **Backends**: libsecret (Linux), `security` CLI (macOS), Credential Manager (Windows)
- **Secret hygiene**: stdin-fed, never in argv/`ps`
- **Enable**: `AGENTTY_USE_KEYSTORE=1`

### Subsystems Found Sound (No Change Needed)

| Subsystem | Verdict |
|-----------|--------|
| **Workspace boundary** | Component-wise containment via `weakly_canonical` — symlink-aware |
| **Subprocess spawner** | `posix_spawn`, stdin ← `/dev/null`, argv form bypasses shell |
| **Permission model** | 4-bit `EffectSet` → constexpr matrix, **exhaustively `static_assert`-proven** |
| **Credential crypto** | AES-256-GCM, HKDF-SHA256, GCM tag set before `DecryptFinal` (fails closed) |
| **TLS verification** | Full chain + hostname, TLS 1.2 floor, system roots |

See [SECURITY_AUDIT.md](SECURITY_AUDIT.md) for the full report.

---

## 23. Stats System — One Pass, One Vocabulary

**Status: Proposed** (supersedes single-tab viewer shipped in `ea4591e1`).

Answers "what did this session actually do?" across ten groups in a tabbed panel.

### Three Rules

1. **The stream path pays nothing per token** — not one branch, not one add
2. **The transcript is the SSOT** — a statistic about a turn is stored on it
3. **A new statistic is data, not code** — touches a struct field, fold line, table row

### Layer 1: Per-Turn Telemetry

`finalize_turn` seals a record onto the assistant `Message`:

```cpp
struct Telemetry {
    std::uint32_t ttft_ms, stream_ms;
    std::uint32_t input_tokens, output_tokens, reasoning_tokens;
    std::uint32_t cache_read, cache_creation;
    std::uint16_t transient_retries, mid_stream_failures, no_progress_failures;
    StopReason stop;
};
std::optional<Telemetry> telemetry;  // absent on old/user messages
```

**Cost: one struct write per turn.** Nothing per token, nothing per frame. 48 bytes on a message already carrying kilobytes.

**Three properties**:
- Stats survive reload (persisted with message)
- Stats survive fork/rewind (travels with the turn)
- Cache ratios stop being lies (recoverable, not discarded)

### Layer 2: Facts (Incremental Fold)

```cpp
Facts::refresh(const Thread&, const Session&);  // amortised O(1) per frame
```

One struct holds every group's counters:

```cpp
struct Facts {
    struct Session   { ... } session;
    struct Models    { Tally by_model, by_role; } models;
    struct Smart     { ... } smart;
    struct Tokens    { ... } tokens;
    struct Cache     { ... } cache;
    struct Tools     { Tally by_name; Hist latency; } tools;
    // … 4 more groups
};
```

**Incremental cursor**: `Facts` carries `consumed` — folds only new messages:
- Messages `[0, size-1)` are **sealed** (folded once, ever)
- Message `size-1` is **live tail** (recomputed each refresh)

On a settled 800-turn thread, a frame costs **zero folds**. During a stream: **one**.

**Invalidation** (fork/rewind/compaction): full rebuild on epoch mismatch.

**Data structures**:
- `Tally` — `vector<pair<string, uint32>>`, linear scan (< 20 models, < 40 tools)
- `Hist` — 24 log2 buckets (1ms … 4h), 96 bytes, O(1) add, O(24) quantile

### Layer 3: Metric (One Number Vocabulary)

```cpp
enum class Unit { Count, Tokens, Bytes, Millis, Ratio, Usd, Rate };

struct Metric {
    std::string label;  // SSO: no allocation
    Unit unit;
    double value;
    double of;          // denominator; 0 = no share bar
};
```

`format(Unit, double)` is the **single** place a number becomes text. Tokens are `12.4k` everywhere, durations `3.2s` everywhere.

### The Widget: maya::StatSheet

**Shipped** (maya `38d6dbb`). Scans entries, derives one geometry, paints all rows aligned:

```cpp
StatSheet s;
s.hero("62%", "of routed turns ran below the Strategic model");
s.heading("By role");
s.entry({.label = "Strategic", .value = "12", .detail = "38%", .share = 0.38});
s.entry({.label = "Output", .value = "1.2k/s", .spark = rate_history});
```

**One entry type, not four.** `label` / `value` / `detail` / `share` / `spark` / `wide` — what's absent doesn't draw.

**Two graph forms** (maya `1ec853d`):
- **`band`** — full-width track split into coloured segments ("what is this made of")
- **`plot`** — braille line chart (2×4 dot matrix, 8× a block chart's resolution)

See [STATS.md](STATS.md) for the full design.

---

## 24. Model Catalog & Capabilities — Static Analysis from ID Strings

The `ModelCapabilities` system answers "what can this model do?" from **just the model ID string** — no network calls, no provider-specific logic, pure static analysis.

### The Problem

Providers expose models with:
- Varying effort support (low/medium/high/xhigh/max)
- Different context windows (32k/128k/200k/1M)
- Tool grammar quirks (weak tool users, disabled schemas)
- Reasoning modes (adaptive thinking, extended thinking)
- Family-specific features (1M suffix support, thinking budgets)

The UI needs this at **model-picker build time** to show effort toggles, context gauges, and feature warnings before a request is made.

### The Solution: Parse the ID

`ModelCapabilities::from_id(std::string_view id)` is a **giant `constexpr` function** that tokenizes the model ID and infers:

```cpp
struct ModelCapabilities {
    enum class Family { Unknown, Haiku, Sonnet, Opus, Fable, Mythos, Gpt };
    
    Family family;
    int generation;           // 3, 4, 5, ...
    int revision;             // 0, 1, 2, ...
    bool reasoning_compat;    // o1/o3/deepseek-reasoner
    bool visual;
    int context_window;       // in tokens
    
    constexpr bool supports_effort() const;
    constexpr bool supports_effort_max() const;
    constexpr bool uses_adaptive_thinking() const;
    constexpr bool supports_1m_suffix() const;
    constexpr Tier tier() const;  // Weak | Cheap | Mid | Flagship
};
```

### Parsing Strategy

**Tokenize on `-` and `_`**, then classify each token:

| ID Pattern | Family | Generation | Notes |
|------------|--------|------------|-------|
| `claude-3.5-sonnet-*` | Sonnet | 3 | Fractional gen → revision=5 |
| `claude-4-opus-*` | Opus | 4 | — |
| `gpt-4o-*`, `chatgpt-4o-*` | Gpt | 4 | 'o' after gen → Gpt family |
| `o1-*`, `o3-*` | Gpt | 5, 7 | o-series → reasoning_compat=true |
| `deepseek-reasoner` | (inferred) | — | reasoning_compat=true |
| `deepseek-r1-*` | (inferred) | — | reasoning_compat=true, r-series |

**Context window defaults** by family + generation:
- Haiku-3: 200k, Haiku-4: 200k
- Sonnet-3: 200k, Sonnet-4: 200k
- Opus-3: 200k, Opus-4: 1M (inferred)
- GPT-4: 128k, GPT-5: 128k, o-series: 200k

### Runtime Overrides (Three Scoped Caches)

Static analysis can be wrong (new model, changed capabilities). Three **global caches** override the static inference:

```cpp
namespace catalog_reasoning_detail {
    std::map<std::string, bool>& map_();  // model_id → supports reasoning
}
namespace catalog_effort_detail {
    std::map<std::string, std::uint8_t>& map_();  // model_id → effort bitset
}
namespace catalog_context_detail {
    std::map<std::string, int>& map_();  // model_id → context window
}
```

**Populated from**:
1. **Provider `/models` endpoint** — Anthropic, OpenAI list responses
2. **Wire evidence** — actual effort levels accepted, context window observed
3. **Manual override** — `AGENTTY_MODEL_CAPS` env var (testing)

**Scoped by provider** — `set_caps_provider_scope("openai")` before merging catalog facts, so the same `gpt-4` can have different caps on OpenAI vs Azure.

**Poisoned set** — if a model returns an error on effort, it's marked "poisoned" and effort is disabled for that model going forward.

### Capability Queries (Constexpr When Possible)

```cpp
constexpr bool supports_effort() const {
    if (family == Family::Fable || family == Family::Mythos)
        return generation >= 1;
    if (family == Family::Opus)
        return generation >= 4;
    if (family == Family::Sonnet)
        return generation >= 4 || (generation == 3 && revision >= 7);
    if (family == Family::Gpt)
        return generation >= 5;
    if (reasoning_compat)  // o1/o3/deepseek-reasoner
        return true;
    return false;
}

constexpr bool uses_adaptive_thinking() const {
    if (family == Family::Fable || family == Family::Mythos)
        return generation >= 1;
    if (family == Family::Opus || family == Family::Sonnet)
        return generation >= 4;
    return false;
}
```

**Everything is `constexpr`** so the compiler can fold it at build time for known IDs.

### The Epoch System (Cache Invalidation)

```cpp
namespace caps_epoch_detail {
    std::atomic<std::uint64_t>& counter();
    std::uint64_t caps_epoch() noexcept;  // current epoch
    void bump_caps_epoch() noexcept;       // invalidate all caches
}
```

When the provider catalog refreshes (models added/removed), `bump_caps_epoch()` increments the counter. Every cached capability derivation checks `caps_epoch()` and recomputes if stale.

This is **cheaper than full-table invalidation** — only the UI components that query caps see the bump.

### Tier Classification (For Smart Mode)

```cpp
enum class Tier { Weak = 0, Cheap = 1, Mid = 2, Flagship = 3 };

constexpr Tier tier() const {
    if (is_flagship()) return Tier::Flagship;  // Opus-4, Sonnet-4, GPT-5
    if (family == Family::Haiku) return Tier::Cheap;
    if (family == Family::Sonnet && generation == 3) return Tier::Mid;
    if (family == Family::Gpt && generation == 4) return Tier::Mid;
    // ...
}
```

Smart Mode uses `tier()` to classify models into Strategic/Implementation/Utility slots.

### Why This Matters

**No network round-trip** — the model picker can show "⚡ Supports effort" / "🧠 Adaptive thinking" / "📊 1M context" **instantly**, from a pure string parse.

**Provider-agnostic** — the same capability logic works for Anthropic, OpenAI, Ollama, custom hosts.

**Testable** — `static_assert(ModelCapabilities::from_id("claude-4-opus-20250514").supports_effort())` proves correctness at compile time.

**Evolvable** — new models get static inference; wire evidence corrects mistakes.

See `include/agentty/domain/catalog.hpp` (2028 lines, 132 definitions) for the full system.

---

## 25. Identity, Capability, Entitlement — Three Layers That Look Like One

**Problem**: Three concepts routinely conflated because in the common case (one account per provider) all three have the same cardinality.

### The Three Layers

| Layer | Key | Answers | Lifetime |
|-------|-----|---------|----------|
| **Identity** | `(provider, model)` | *What is this thing?* | As long as the model exists |
| **Capability** | `(provider, model)` | *What can it do?* | Changes when provider changes dispatch table |
| **Entitlement** | `(provider, account, model)` | *What may **I** do with it?* | Changes when subscription changes |

### Identity: `(provider, model)`

What the wire needs to dispatch: endpoint, dialect, slug. `claude-opus-4-5` is the same model — same tokenizer, context window, tool grammar — on your work account and personal one.

**Reductio**: If identity were `(provider, account, model)`, switching accounts would invalidate:
- Current model selection
- MRU / recents list
- Smart Mode role pins
- Per-provider model recall
- Every favourite

**Structural argument**: Account is **1:1 with provider** at any instant (one active credential). A key component with cardinality 1 adds no discriminating power.

### Capability: `(provider, model)`

What the model can do — reasoning support, effort ladder, tool grammar, context window, tier. Account-blind because it describes the model **as the provider serves it**, not as your subscription permits.

**Provider-scoped** (not globally model-scoped) because the same model ID can behave differently on different hosts (one gateway enables a tool grammar another disabled).

### Entitlement: `(provider, account, model)`

What **this subscription** may use. Not a property of the model — a property of your relationship with the provider.

**Canonical case**: Anthropic's 1M-context beta. The model supports it, the provider serves it, whether *you* may use it depends on your plan. OAuth tokens carry no entitlement field — **the only way to learn it is to try and be rejected** (HTTP 400).

### The Failure Mode (Before This Was Fixed)

`Settings::context_1m_blocked` — a **single global bool**.

```
Max account   →  1M works
switch to Pro →  400, learn "blocked"      (bool = true)
switch to Max →  reducer clears the bool   (fact destroyed)
switch to Pro →  400 again. Forever.
```

Every hop re-discovers the same rejection because the answer is **thrown away** rather than **filed under whose answer it was**.

**General shape**: When a fact's true scope is finer than its storage key, the only correction is deletion — and deletion is lossy. You'll recognize it by a **manual reset hook** (the smell; missing key axis is the disease).

### The Fix: Key It, Don't Reset It

`include/agentty/domain/entitlement.hpp` stores facts under:

```
"<fact>\x1f<provider>\x1f<account>[\x1f<folded-model>]"
```

Reset hooks **deleted**. Outgoing account's facts never in incoming account's way.

**Four key design decisions**:
1. **Separator is US (0x1f), not `/`** — account labels are user-typed; `/` would allow forging keys
2. **Model component is `capkey::norm_model`-folded** — `mistral-medium-3.5` = `-3-5` = case variants
3. **Storage is negative-only** — absent ⇒ not blocked (permissive by default)
4. **`""` is legitimate account** — single-account users key under `""`, no migration

**Behavioural rule**: Forget on removal, never on switch. Switching must remember (that's the point).

### How to Tell Which Layer

1. Would this change if the provider swapped models? → identity/capability
2. Would two users on same provider/model get different answers? → entitlement
3. Does it survive re-login? → if no, session state (not stored)
4. Is there a manual reset hook? → it's misfiled

| Fact | Layer | Why |
|------|-------|-----|
| Context window, tokenizer | Capability | Provider-served property |
| Tool grammar | Capability | Host advertises or doesn't |
| Reasoning, effort ladder | Capability | Dispatch-table fact |
| 1M-context beta | **Entitlement** | Same model, different answer per subscription |
| Copilot premium tier | **Entitlement** | Billing tier decides model families |

See [IDENTITY_CAPABILITY_ENTITLEMENT.md](IDENTITY_CAPABILITY_ENTITLEMENT.md) for the full theory.

---

## 26. Context Window Resolution — Five Rungs, Strongest Evidence First

**You should never have to set this.** agentty resolves it automatically.

### The Ladder

| # | Rung | Source |
|---|------|--------|
| 1 | Per-model override | What you set for exact provider+model |
| 2 | **Live advertised** | Gateway's `/v1/models` row or probe |
| 3 | **models.dev** | Public catalog, 7,824 models |
| 4 | ID inference | Claude/GPT families, `[1m]` suffix |
| 5 | Env / default | `AGENTTY_MAX_CONTEXT_TOKENS`, else 200k |

Rungs 2–4 are automatic.

### Rung 2: What the Gateway Says

Read from `/v1/models` row in whichever spelling the host uses:
- `context_length`
- `top_provider.context_length` (OpenRouter's per-deployment)
- `max_model_len` (vLLM)
- `max_input_tokens` (LiteLLM)
- `n_ctx` / `n_ctx_train` (llama.cpp)

When a row carries **nothing**, agentty asks directly:
- `GET /v1/model/info` — LiteLLM management route
- `GET /props` — llama.cpp's `n_ctx`
- `POST /api/show` — Ollama's `context_length`

**This rung outranks every static source** — same model name behind two gateways can be served at different sizes, only the gateway knows.

### Rung 3: models.dev

Public catalog of 7,824 models. Makes non-Claude models honest — ID inference knows Claude/GPT families, answers 0 for everything else.

**Scoped by provider first** — same bare ID served at different sizes by different hosts.

**Measured coverage**:
```
openrouter  368/368      google      39/39      anthropic  14/14
openai       43/48       mistral     32/34      xai        12/12
groq         14/16       deepseek     4/4       cerebras    2/2
```

### When It Can't Be Automatic

**Private gateway** serving unknown model ID on host that advertises nothing. Escape hatch:

```bash
AGENTTY_MAX_CONTEXT_TOKENS=1000000 agentty
```

Last rung deliberately — it's global, would stamp one number across all models. Anything that knows a given model wins.

**Check resolution**: Model picker's `ctx` column shows actual window. `auto` means nothing declared, set the env var.

See [CONTEXT_WINDOW.md](CONTEXT_WINDOW.md) for the full ladder.

---

## 27. Airgap Mode — SSH Remote Without Provider Credentials

Run agentty on a **remote host** (server, airgap box) while keeping provider credentials **only on your laptop**.

### Architecture

```
┌─────────────────┐              SSH               ┌──────────────────┐
│  Laptop         │◄──────────────────────────────►│  Remote Host     │
│  (has creds)    │                                 │  (no creds)      │
│                 │                                 │                  │
│  agentty serve  │    ← stdio over SSH reverse ←  │  agentty --acp   │
│  (ACP agent)    │                                 │  (ACP client)    │
└─────────────────┘                                 └──────────────────┘
```

**Remote host**: Runs the TUI, has workspace files, **zero provider credentials**

**Laptop**: Runs headless ACP agent (`agentty serve`), holds OAuth tokens, makes provider requests

**Wire**: JSON-RPC over SSH reverse tunnel (stdio transport)

### Setup (One Command)

**On laptop** (the machine with credentials):

```bash
agentty serve --provider anthropic
```

**On remote** (over SSH):

```bash
ssh -R 9988:localhost:9988 remote-host
agentty --acp tcp://localhost:9988
```

That's it. The remote TUI talks to your laptop's agent; provider traffic never touches the remote.

### Security Properties

- **Credentials never cross SSH** — OAuth tokens stay on laptop
- **Workspace never crosses SSH** — files stay on remote
- **Mutual consent** — both sides must be running, can't be passive backdoor
- **Auditable** — every request visible in `agentty serve` logs

### Use Cases

1. **Corporate locked-down box** — can't store OAuth tokens, can SSH out
2. **Airgap compliance** — workspace under policy, agent outside
3. **Shared server** — everyone's own laptop credentials, shared code
4. **Trust boundary** — credentials on hardware you control

### How It Works

Remote agentty boots in **ACP client mode** (`--acp <url>`). Instead of constructing an Anthropic/OpenAI provider, it constructs an `AcpProviderAdapter` that:

1. Translates `Provider::stream(Request)` → `agent.stream_message(params)`
2. Receives `agent_message_chunk` events over JSON-RPC
3. Translates back to agentty's internal stream events
4. Fires the same `EventSink` callbacks as native providers

**Same tool catalog** — MCP servers, native tools, sandbox all run **on the remote** (where the workspace is). Only LLM requests cross the wire.

**Same rendering** — TUI, shortcuts, panels all local. No latency except model streaming.

See [website/airgap.md](website/airgap.md) for the full guide.

---

## 28. Related Documents

### Core Architecture

- [ARCHITECTURE.md](ARCHITECTURE.md) — The original architecture guide (this doc is the deep-dive companion)
- [PROVIDER_HETEROGENEITY.md](PROVIDER_HETEROGENEITY.md) — Provider design in detail
- [THREAD_STORE.md](THREAD_STORE.md) — Persistence design + measurement narrative
- [RENDERING.md](RENDERING.md) — Rendering pipeline, widgets, adapters

### Subsystems

- [SANDBOX.md](SANDBOX.md) — Sandboxing design
- [PLUGINS.md](PLUGINS.md) — MCP integration + plugin examples
- [website/retrieval.md](website/retrieval.md) — RAG pipeline
- [design/smart-mode.md](design/smart-mode.md) — Smart Mode design
- [design/streaming.md](design/streaming.md) — Streaming state machine
- [design/scope-model.md](design/scope-model.md) — Config-resolution algebra
- [IDENTITY_CAPABILITY_ENTITLEMENT.md](IDENTITY_CAPABILITY_ENTITLEMENT.md) — Layering question for provider/account facts

### Maya (TUI Framework)

- [maya/README.md](../maya/README.md) — Maya framework overview
- [docs/agent_panel/](agent_panel/) — Panel subsystem deep-dive

### Protocols

- [acp-cpp/README.md](../acp-cpp/README.md) — ACP protocol implementation
- [mcp-cpp/README.md](../mcp-cpp/README.md) — MCP protocol implementation

---

## 29. Principles That Guide This Design

### Type Safety Over Convenience

Strong IDs (`ThreadId`, `ToolCallId`, `MessageId`) are distinct types. Swapping two is a compile error, not a runtime bug. `ModelCapabilities` queries are `constexpr` when possible.

### Proof Over Testing

The permission policy matrix is proved at compile time. 48 cells, exhaustive `static_assert`, breaks the build on mismatch. Loop mode backoff is unit-tested for every failure class.

### Data Over Behavior

Provider differences are registry rows, catalog facts, wire scaffolds — not `if (provider == X)` branches. Scope resolution is a fold over `Source` values. Stats are projections, not accumulators.

### Simplicity Over Cleverness

BM25 + optional embeddings beats a mandatory config file. Loop mode snapshots the payload instead of re-reading the composer. Append-only JSONL over SQLite.

### Debuggability Over Opacity

Append-only `.jsonl` lets you `tail -f` a live session. Every resolved config item carries its `Source` provenance. Stats survive reload because they're on the message.

### Explicitness Over Magic

`Cmd` seam makes effects visible. The scope model makes "project ▷ user" a declared lattice, not an implicit rule. Backoff schedules are in a table, not buried in code.

### Worse Is Better Over Perfect

JSON + ripgrep + bubblewrap + JSONL are good enough and have zero new dependencies. Clipboard ferry over SSH instead of inventing a protocol.

### Fast Is a Feature

3 ms startup, 1.4 ms turn save, SIMD diff, frozen content cache, incremental stats fold are architectural choices, not optimizations. The stream path pays nothing per token.

### Security Is a Property

OAuth state is CSPRNG-derived and constant-time compared. Trust is bound to content hashes. Sandboxing is capability-probed. Credentials are AES-256-GCM sealed with optional passphrase + keystore.

### Evolvability Through Algebra

Scope is `Locus × Dialect × Trust`. Stats are `Facts` folded into `Metrics`. Capabilities are parsed from IDs with runtime overrides. Adding a new concern is filling a table, not rewriting a system.

### The Code Is the Truth

Where this doc and the code disagree, the code wins. This doc is a map; the code is the territory. Invariants are `static_assert`-proven or unit-tested, never assumed.

---

This is a **production-grade, type-theoretically grounded, zero-dependency terminal agent** that treats milliseconds, compile-time proofs, and structural types as non-negotiable. Every architectural decision — from the Elm loop to the capability parser to the scope algebra — follows from those principles.
