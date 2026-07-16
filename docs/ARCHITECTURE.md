# agentty — Architecture

A field guide to how the binary is put together. This is a map for someone
about to change the code, not marketing. The single source of truth is always
the code itself; where this doc and the code disagree, the code wins.

---

## 1. The shape of the program

agentty is an Elm-style application. The entire runtime is one pure function
applied in a loop:

```
(Model, Msg) -> (Model, Cmd<Msg>)
```

- **Model** is the whole application state — one aggregate struct.
- **Msg** is a closed sum type of every event that can happen.
- **Cmd** is a description of side effects to run (network, disk, timers); the
  runtime executes them and feeds their results back as new `Msg`s.

Rendering is a second pure function, `view : Model -> Element`, delegated to
**maya** — a sister TUI engine pulled in as a git submodule. The host never
constructs chrome glyphs or makes layout decisions; it builds widget *Config*
values from `Model` state and maya owns every pixel, border, and animation.

The four maya `Program` hooks are bound in
`include/agentty/runtime/app/program.hpp`:

| Hook           | Meaning                                         |
|----------------|-------------------------------------------------|
| `init`         | Load settings + recent threads via Store seam.  |
| `update`       | The reducer — `src/runtime/app/update.cpp`.     |
| `view`         | `Model -> Element`.                             |
| `subscribe`    | Timers and the live stream subscription.        |
| `visual_hash`  | Render-skip gate; identical hash → skip frame.  |
| `needs_warmup` | One-shot fast scrollback rehydration on resume. |

`main.cpp` is wiring only: parse argv, resolve credentials, construct the
concrete `AnthropicProvider` + `FsStore`, install them behind the `Deps` seam,
then hand `AgenttyApp` to `maya::run`.

---

## 2. Directory layout

`include/agentty/` and `src/` mirror each other by domain. Headers carry the
types and inline logic; `src/` carries the heavier implementations.

- **`domain/`** — pure data, no I/O. `session`, `conversation`, `catalog`,
  `todo`, `profile`, and the strong-id newtypes in `id.hpp` (`ToolCallId`,
  `ThreadId`, `OAuthCode`, `PkceVerifier`). Swapping two ids of different
  newtype is a compile error, not a debugging session.
- **`runtime/`** — the application proper.
  - `model.hpp` — the composed `Model` plus UI-only sub-states (composer,
    pickers, palette, modals) that belong to no domain.
  - `msg.hpp` — the `Msg` sum, split into domain sub-variants (see §4).
  - `app/update/<domain>.cpp` — per-domain reducers.
  - `view/` — the `Model -> Element` pipeline, one file per widget family.
- **`provider/`** — the `Provider` concept and its implementations:
  `anthropic/transport.cpp` (HTTP/2 + SSE, the OAuth/Pro/Max default) and
  `openai/transport.cpp` (any OpenAI-compatible endpoint — openai, groq,
  openrouter, together, cerebras, ollama, or a raw host). `selection.cpp`
  resolves which one a `--provider` flag / persisted setting picks.
- **`tool/`** — the `Tool` concept, the registry, the permission policy, and
  one file per tool under `tool/tools/`. `memory_store.cpp` backs
  `remember`/`forget`.
- **`io/`** — `http`, `tls` (certificate pinning), `auth` (OAuth + PKCE),
  `persistence` (atomic writes), `clipboard`.
- **`airgap/`** — SOCKS5-over-SSH so the agent can run on a host with no direct
  internet while the laptop relays the bytes.

---

## 3. Seams: how concrete types stay hidden

`AgenttyApp` must not be templated on the Provider and Store types — that would
force every translation unit to know the concrete types and rebuild when they
change. Instead, `include/agentty/runtime/app/deps.hpp` defines a small
`Deps` struct of `std::function`s:

- **Provider seam** — `stream(Request, EventSink)`.
- **Store seam** — `save_thread`, `load_threads`, `load_thread`,
  `load_settings`, `save_settings`, `new_thread_id`, `title_from`.
- **Auth context** — the typed `AuthHeader` for the session.

`main.cpp` calls `app::install(provider, store, auth_header)` once at startup;
the reducer reaches the seams through `app::deps()`. `update_auth(...)`
live-swaps credentials after an in-app login without restarting the process —
in-flight streams cached the header at request-build time, so they are
unaffected.

The `Provider` concept is deliberately tiny:

```cpp
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};
```

Anything that streams a chat completion satisfies it — the real Anthropic
and OpenAI-compatible transports in production, a deterministic in-memory
script in tests.

---

## 4. Msg: a closed sum, split for compile speed

A naive design inlines every leaf event in one giant variant. That pins
`sizeof(Msg)` to the heaviest leaf, instantiates an N-wide `std::visit`
dispatch table, and forces the whole reducer TU to rebuild on any leaf change.

agentty instead groups leaves into ~15 **domain sub-variants** in `msg.hpp`
(`ComposerMsg`, `StreamMsg`, `ToolMsg`, `ModelPickerMsg`, `ThreadListMsg`,
`CommandPaletteMsg`, `MentionPaletteMsg`, `SymbolPaletteMsg`, `TodoMsg`,
`LoginMsg`, `DiffReviewMsg`, `CheckpointMsg`, `MetaMsg`). The top-level reducer in
`update.cpp` is then a small `std::visit` that forwards each domain to its own
TU:

```cpp
auto step = std::visit(overload{
    [&](msg::ComposerMsg cm) { return detail::composer_update(std::move(m), std::move(cm)); },
    [&](msg::StreamMsg   sm) { return detail::stream_update  (std::move(m), std::move(sm)); },
    [&](msg::ToolMsg     tm) { return detail::tool_update    (std::move(m), std::move(tm)); },
    // … nine more domain arms …
}, msg);
```

Each `update/<domain>.cpp` recompiles only when its own leaves change.
Call sites still build a `Msg` directly via `std::variant`'s converting
constructor — only the owning domain accepts a given leaf, so the wrap is
unambiguous.

---

## 5. Tools: typed bundles behind a JSON edge

The `Tool` concept (`include/agentty/tool/tool.hpp`) requires a static bundle
of identity + schema + effects + behavior:

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

Tools are fully typed internally; only the dispatcher boundary speaks JSON.
`DynamicDispatch` looks a tool up in the registry, executes it inside a
`try/catch` (a crashing tool becomes a typed `ToolError`, not a process abort),
and applies a **per-tool output budget** so a runaway `read`/`bash`/`grep`
can't blow the context window. Truncation is UTF-8-safe and comes in three
strategies:

- **Head** — keep the front; right for ordered chunks (read, edit, write).
- **Tail** — keep the end; right for log streams (bash, diagnostics).
- **HeadTail** — keep both ends with a middle elision marker; right for tools
  where both ends carry signal (grep, web_*, git diff/log/status).

The shipped tools: `read`, `write`, `edit`, `bash`, `grep`, `glob`,
`list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`,
`diagnostics`, `git_status`, `git_diff`, `git_log`, `git_commit`, `remember`,
`forget`, `wipe_memory`, `task` (subagent dispatch), `skill` (load a skill
body on demand).

---

## 6. Permission policy: a constexpr matrix

Every tool declares an `EffectSet` over four bits: `ReadFs`, `WriteFs`, `Net`,
`Exec`. The active **Profile** plus that effect set feed the pure `constexpr`
function `policy::permission(effects, profile)` in `tool/policy.hpp`, which
returns `Allow` or `Prompt`. The rule:

| Profile     | Pure  | ReadFs | WriteFs | Net    | Exec   |
|-------------|-------|--------|---------|--------|--------|
| **Write**   | Allow | Allow  | Allow   | Allow  | Allow  |
| **Ask**     | Allow | Allow  | Prompt  | Prompt | Prompt |
| **Minimal** | Allow | Prompt | Prompt  | Prompt | Prompt |

`Write` is fully autonomous. `Ask` trusts read-only inspection so an agent
loop's read/grep/glob doesn't prompt on every step but gates anything that
mutates state, runs code, or hits the network. `Minimal` prompts for every tool
that touches the outside world and auto-allows only pure ones. `Exec` is the
maximal capability — a tool carrying it prompts regardless of what else it has,
on the type-theoretic claim that `bash` lets the model *author* the side
effect, so it dominates any individual filesystem mutation already gated.

The whole table is proved at compile time. `EffectSet` is a 4-bit bitset (16
sets) × 3 profiles = exactly **48 cells**. A second function,
`expected_decision`, re-states the policy independently, and an exhaustive
`constexpr` loop `static_assert`s `permission(e, p) == expected_decision(e, p)`
over every cell — so a one-handed change to either side breaks the *build*, not
a test nobody runs. A further `static_assert` pins the bitset width, firing if
a fifth `Effect` is added without extending both sides.

`DynamicDispatch::needs_permission` is the single place the runtime asks "does
this gate on the user?", and unknown tools fail closed (default to requiring
permission). The companion `policy::reason` supplies the one-line explanation
rendered in the permission card ("wants to run an arbitrary subprocess", "will
modify files on disk", …).

---

## 7. Tool scheduling: parallel-safety from the effect set

The same `EffectSet` that drives permissions also decides whether two tools may
run concurrently. `effects::is_parallel_safe(active, want)` answers "may a tool
with `want` effects start while `active` effects are in flight?":

- **`WriteFs` and `Exec` demand exclusive access.** A write can mutate state a
  sibling is reading, writing, or shelling against — two edits to "different"
  files look independent until the model picks overlapping paths. `Exec` is
  worse still because the model chose the command, so the runtime serialises.
- **`Pure`, `ReadFs`, and `Net` compose freely.** Read-read never races, `Net`
  touches neither FS nor process state, and in-memory `Pure` tools (`todo`)
  operate on data the model can't observe concurrently.

The rule is, again, proved at compile time — `effects.hpp` carries a block of
`static_assert`s pinning the exclusive/compose decisions, and the tool spec
carries `parallel_rule_is_well_founded`. Effects are chosen by *what the tool
does to the world, not how it's implemented*: `git_status` is `ReadFs` even
though it shells out to `git`, because the runtime knows what that subprocess
does; `bash` is `Exec` because the model picks the command.

---

## 8. The streaming turn: a phase FSM with a retry watchdog

A turn is not a single request — it cycles `Streaming → AwaitingPermission →
ExecutingTool → Streaming → … → Idle`. `domain/session.hpp` models this as a
phase variant where the per-turn `Active` context (cancel token, start stamp,
retry counters) lives *inside* every non-`Idle` alternative — so reading those
fields from `Idle` is a type error, not a logic bug masked by zero defaults.
Legal transitions take the source by `&&` and re-wrap its context in the
destination, so the FSM itself carries the turn state across phases.

Reliability rides on two independent pieces:

- **A retry state machine** (`retry::Fresh / StallFired / Scheduled`) replaces
  what used to be two hand-synchronised bools. A 120-s stall watchdog trips the
  cancel token (`Fresh → StallFired`); the synthetic `StreamError` schedules a
  retry via `Cmd::after` (`→ Scheduled`); a second error during the wait can't
  schedule a duplicate; `RetryStream` firing returns to `Fresh`.
- **Two independent retry budgets.** `truncation_retries` covers a stream that
  EOFs mid-tool-args; `transient_retries` covers 5xx / network / overloaded /
  429. `transient_retries` is *not* monotonic per turn — it resets to 0
  whenever the wire proves healthy (first content delta, or an SSE ping /
  thinking delta), so a connect-ping-stall sequence gets a fresh budget each
  attempt instead of latching the session terminal.

---

## 9. Safety boundaries

- **Workspace boundary.** Filesystem tools refuse any path outside the launch
  directory (or `--workspace DIR`). `--workspace /` opts out.
- **Sandbox.** `bash` and `diagnostics` run inside `bwrap` (Linux) or
  `sandbox-exec` (macOS) by default. Workspace + system libs + network are
  reachable; `~/.ssh`, `/etc`, and other projects are read-only. An approved
  `bash` call still can't `cat ~/.ssh/id_rsa`. `--sandbox auto|on|off`.
- **TLS pinning.** Certificates are pinned on the real upstreams, end-to-end,
  including through the airgap SOCKS tunnel.
- **Atomic writes.** Every persisted file is `write` + `fsync` + `rename` (or
  the Windows `MoveFileExW` equivalent), so a crash mid-write never corrupts a
  thread or the credential store.

---

## 10. Rendering performance

Idle agentty costs zero CPU: `fps = 0` means maya only renders on a `Msg`,
input, or timer tick. Two host-side optimizations keep it cheap under load:

- **`visual_hash`** mixes only the axes that change what's on screen. When the
  hash matches the previous frame, `view` + render are skipped entirely. The
  hash hashes only the *live* message tail (the frozen scrollback prefix is
  immutable archaeology), samples long strings instead of hashing every byte,
  and buckets time-driven animations so each visible step — and only each
  visible step — advances the hash. The animation bucket is phase-locked to
  whatever is actually on screen so the render gate and the animation never
  beat against each other.
- **`needs_warmup`** fires a one-shot off-wire render when a thread is resumed,
  converting the first visible frame of a tool-heavy thread from O(content) to
  O(blit).

---

## 11. Build notes

- Requires GCC 14+ / Clang 18+ / MSVC 14.40+ and CMake 3.28+ (C++26).
- `-DAGENTTY_STANDALONE=ON` statically links OpenSSL + nghttp2 + libstdc++ +
  libgcc when their `.a` archives are present; libc stays dynamic. A musl
  toolchain with `-DAGENTTY_FULLY_STATIC=ON` yields a 100% static binary.
- `-DAGENTTY_USE_MIMALLOC=ON` (default) routes global `new`/`delete` through
  mimalloc; the override lives in exactly one TU (`main.cpp`).
- **Gotcha:** `AGENTTY_AUTO_PULL_MAYA=ON` is the default and runs
  `git reset --hard origin/master` on the `maya/` submodule during build. Its
  only guard checks for *uncommitted* changes, so committed local maya work
  still gets wiped. Build with `-DAGENTTY_AUTO_PULL_MAYA=OFF` when iterating on
  maya.

---

## 12. One-paragraph mental model

`main.cpp` resolves credentials and installs a Provider + Store behind the
`Deps` seam, then hands control to maya. maya calls `view(model)` to paint and
`update(model, msg)` for every event. User input and SSE chunks become `Msg`s;
the reducer dispatches each to a per-domain handler that returns the next
`Model` plus a `Cmd` describing any side effects. Tools run behind a JSON
dispatch edge with a `constexpr` permission gate and OS-level sandboxing, and
their results loop back in as more `Msg`s. Nothing in the loop mutates global
state; the only escape hatches are the explicit `Cmd`s the runtime executes on
your behalf.
