# agentty — the one idea

agentty is a type-theoretic agent. Every value is typed, every state machine is
a sum type, every fallible operation is `std::expected<T, E>`, every
cross-cutting concern (effects, identity, capabilities) is a type-level
fact. The **dynamism boundary** — JSON, network, terminal IO — is one thin
layer at the edge; everything past it is fully typed.

This document is the contract you accept when contributing. If a change makes
the codebase look inconsistent — if it requires a reader to ask "wait, is
this *that* style or *this* style?" — it doesn't go in. There is one style.

---

## The seven rules

### 1. State machines are `std::variant`. Every alternative carries its own data.

A state with N possible shapes is a closed sum type — never an `enum + bool +
maybe_field` constellation. Each alternative owns the data that's only valid
in that state.

```cpp
// ✗ Bool encodes a state machine. `output` is meaningless when error=false?
struct ToolExecOutput { ToolCallId id; std::string output; bool error; };

// ✓ Sum type. Each arm owns exactly the data that's valid in it.
struct ToolExecOutput { ToolCallId id; std::expected<std::string, ToolError> result; };
```

Already-correct examples to read: `ToolUse::Status` (Pending/Approved/
Running/Done/Failed/Rejected — each with its own fields), `Phase`, `Msg`,
`provider::ErrorClass`.

Indicator you're violating this: a comment that says "this field is only
valid when X is true." That's a sum type screaming to escape.

### 2. Fallible operations return `std::expected<T, FooError>`. `FooError` carries an `enum class Kind`.

No string error returns. No exceptions across layer boundaries (catch at the
boundary; convert to typed). The kind is enumerated; the detail is a free
string for human reading.

```cpp
// ✗ Errors are strings. Caller must substring-match to react.
std::expected<Response, std::string> send(...);

// ✓ Errors are typed. Caller dispatches via switch.
enum class HttpErrorKind { Tls, Timeout, Refused, Status, Cancel };
struct HttpError { HttpErrorKind kind; int http_status = 0; std::string detail; };
std::expected<Response, HttpError> send(...);
```

Already-correct: `ToolError`, `ExecResult`, `provider::ErrorClass`.

If you find yourself string-matching error messages downstream, that's a
sign the upstream layer should have returned a typed error.

### 3. Strong typedefs for IDs. Never raw `std::string` for an ID, path, URL, or name.

```cpp
// ✗
void run(std::string id, std::string path, std::string command);
// ↑ caller can swap arguments freely, compiler is silent

// ✓
void run(ToolCallId id, FilePath path, BashCommand command);
```

Already-correct: `ToolCallId`, `ThreadId`, `ModelId`, `CheckpointId`,
`ToolName`. Add new tagged types when introducing new conceptual identities;
follow `domain/id.hpp`'s `Id<Tag>` pattern.

### 4. One source of truth per concept.

A policy decision lives in **exactly one** function. A field's invariants
are checked in **exactly one** place. No tool can override the permission
policy; no caller can hand-roll its own version of `provider::classify`.

```cpp
// ✗ Per-tool lambdas — easy to forget, easy to make inconsistent.
t.needs_permission = [](Profile p){ return p != Profile::Write; };

// ✓ Tool declares its capabilities; ONE pure function decides.
t.effects = {Effect::WriteFs};
// permission(effects, profile) lives in tools/policy.hpp — only place.
```

Already-correct: `provider::classify` (one error classifier), `policy::
permission` (one permission decider), `update` (one reducer with one
`std::visit`).

### 5. Layered separation. Domain types are pure values. I/O lives behind concepts.

```
include/agentty/domain/      ← pure value types only. No I/O. No UI. No threads.
include/agentty/{io,provider,tool}/ ← satisfies a concept; injected via deps.
src/runtime/              ← Elm loop. update is pure. Cmd carries the IO.
maya/                     ← presentation. Consumes a Model, emits Msg.
```

A function in `domain/` that opens a file is a violation. A reducer that
calls `http::send` directly is a violation (use `Cmd::task` + `deps()`).
A view function that mutates the model is a violation.

The dynamism boundary — JSON parsing, raw HTTP, terminal escape sequences —
is **one layer**. Past it, code speaks in typed values.

### 6. Effects in types, not in conventions.

If a function modifies the filesystem, that's a fact about its type. It
gets `Effect::WriteFs`. The capability is the *only* thing the policy reads.

```cpp
// ✗ The policy is wherever you remembered to put it.
if (tool_name == "bash" || tool_name == "write" || ...) prompt();

// ✓ The capability is in the type. The policy is one function.
if (policy::permission(td->effects, profile) == Decision::Prompt) prompt();
```

Already-correct: `tools::Effect`, `tools::EffectSet`, `tools::policy::
permission`. Add new effects (`Clipboard`, `Browser`) by extending the
enum + the policy table — never by adding a per-call override.

### 7. No `bool` for state. No `optional<T>` for "absent because state X".

```cpp
// ✗ The bool encodes "is something happening." The optional encodes "do
//   we have a result." Two phantom state machines, no exhaustiveness.
bool active;
std::optional<Result> result;

// ✓ Make the state machine explicit. The compiler will tell you if you
//   forget an arm.
using Status = std::variant<Idle, InFlight, Done, Failed>;
Status status;
```

`bool` is fine for things that are genuinely binary AND independent of
other state — `expanded`, `collapsed`, `dim`. It is NOT fine for things
that gate the validity of other fields.

---

## Patterns by layer

### Domain (`include/agentty/domain/`)
- Pure value types. `std::variant` for state, `std::expected` for fallibility.
- `to_string(X)` for every enum/variant — used by serialization and rendering.
- Strong IDs via `Id<Tag>`.
- No I/O. No threads. No `static` mutable state.

### IO + Provider (`include/agentty/io/`, `include/agentty/provider/`)
- Satisfies a concept; injected via `deps()` so tests inject fakes.
- All public methods return `std::expected<T, FooError>` with a typed `Kind`.
- Catch all exceptions at the boundary; convert to typed.
- Raw JSON / strings stay inside; typed values come out.

### Tools (`include/agentty/tool/`, `src/tool/tools/*`)
- Each tool: typed `Args` struct + `parse_args(json) → expected<Args, ToolError>`
  + `run(Args) → ExecResult`. Glued together by `util::adapt<Args>`.
- Declares its `effects` — never its own permission lambda.
- Schema is hand-authored JSON today; future work is to derive it from
  `Args` via reflection.

### Runtime (`src/runtime/`)
- `update(Model, Msg) → (Model, Cmd<Msg>)` — pure function. Single
  `std::visit`. One arm per `Msg` variant. Heavy reducers split into
  `update/{stream,modal,tool}.cpp` but still composed via the same visitor.
- `view(Model) → Element` — pure function. No state mutation.
- `subscribe(Model) → Sub<Msg>` — pure function. Returns the subscriptions
  active in this state.
- Side effects go through `Cmd::task` / `Cmd::after`.

### View (`src/runtime/view/*`)
- Reads `Model`, returns `Element`. Never mutates.
- Width-aware widgets use `ComponentElement` with a `measure` callback so
  the parent layout can size them correctly.
- Per-speaker / per-state colors come from `palette.hpp` — not hard-coded.

---

## Reading guide for new contributors

The fastest path to reading the codebase, top to bottom:

1. `include/agentty/domain/` — the value types. Start here. `conversation.hpp`
   for the chat model, `session.hpp` for streaming state, `profile.hpp` for
   permission tier.
2. `include/agentty/runtime/msg.hpp` — every event the runtime can react to,
   as a closed variant. This is the menu.
3. `src/runtime/app/update.cpp` — the reducer. Top-level `std::visit` with
   one arm per Msg. Heavy paths jump to `update/{stream,modal,tool}.cpp`.
4. `src/runtime/app/cmd_factory.cpp` — how side effects are constructed.
   `launch_stream`, `run_tool`, `kick_pending_tools`.
5. `src/runtime/view/view.cpp` — top-level view composition. Then
   `thread.cpp`, `composer.cpp`, `statusbar.cpp` for the panels.
6. `include/agentty/tool/{effects,policy}.hpp` — the capability + policy
   layer. Read this before reading any individual tool.
7. `src/tool/tools/*` — tools, all built from the same template.

If anything you read above doesn't match this document, the document wins
— file an issue.

---

## Worked examples — "what does this look like in agentty?"

### Typed errors at the dynamism boundary

`http::Client::send` returns `HttpResult = std::expected<Response, HttpError>`.
The HttpError carries an `enum class HttpErrorKind { Cancelled, Resolve,
Connect, Tls, Protocol, SocketHangup, Timeout, PeerClosed, Status, Body,
Unknown }` — every internal failure site (poll() error, SSL_connect
failure, nghttp2 frame error, idle timeout, …) constructs the right kind
via a static factory. The error classifier dispatches on `kind`:

```cpp
constexpr ErrorClass classify(const http::HttpError& e) noexcept {
    using K = http::HttpErrorKind;
    switch (e.kind) {
        case K::Cancelled:                  return ErrorClass::Cancelled;
        case K::Resolve: case K::Connect: case K::Tls:
        case K::Protocol: case K::SocketHangup:
        case K::Timeout: case K::PeerClosed: return ErrorClass::Transient;
        case K::Status:
            if (e.http_status == 401 || e.http_status == 403) return ErrorClass::Auth;
            if (e.http_status == 429)                          return ErrorClass::RateLimit;
            if (e.http_status == 408 || e.http_status == 502
             || e.http_status == 503 || e.http_status == 504
             || e.http_status == 529)                          return ErrorClass::Transient;
            return ErrorClass::Terminal;
        case K::Body: case K::Unknown:      return ErrorClass::Terminal;
    }
}
```

No substring sniffing. Adding a new `HttpErrorKind` fails to compile here
until handled — the policy is exhaustive by construction.

### Sum-type credentials

`auth::Credentials = std::variant<cred::None, cred::ApiKey, cred::OAuth>`.
Each alternative owns the data only valid in its state — `None` has no
data, `ApiKey` has just `key`, `OAuth` has `access_token / refresh_token /
expires_at_ms`. Operations are free functions that `std::visit` the
variant, so adding a new alternative (e.g. `cred::DeviceFlow`) won't
compile until every accessor handles it.

### Typed deserialization

`persistence::load_thread_file(path) → std::expected<Thread,
DeserializeError>`. `DeserializeError::Kind = { JsonParse, MissingField,
InvalidValue, InvalidVariantTag, Io }`. `parse_message`, `parse_thread`,
`parse_tool_status` propagate typed errors via the monadic shape. The
directory-walking loader logs the kind + field path to stderr instead
of `catch (...) { continue; }` — the user sees *which* file was bad and
*why*.

### Derived state, not parallel state

`StreamState::active()` is a derived `bool` (= `!is_idle()`), not a
field. There used to be a `bool active` that callers had to keep in
lock-step with `phase`; readers had to remember the invariant ("active
iff phase != Idle"). Now there's nothing to remember — the variant is
the truth, the bool is a view of it.

### Sum-type pickers

`pick::OneAxis = variant<Closed, OpenAt{index}>` for the model picker
and thread list. `pick::TwoAxis = variant<Closed, OpenAtCell{file,
hunk}>` for diff review. `pick::Modal` for the todo overlay. The
canonical `bool open + int index` anti-pattern (where `index` is
meaningless when `open == false`) is gone everywhere; opening is
`m.ui.x = pick::OpenAt{i}`, closing is `m.ui.x = pick::Closed{}`,
mutating is `if (auto* p = pick::opened(m.ui.x)) p->index = …`.

### Effects-in-types for permission

Each tool declares a `tools::EffectSet` (subset of `{ReadFs, WriteFs,
Net, Exec}`). The single `tools::policy::permission(EffectSet, Profile)
→ Decision` constexpr function is the only thing that decides whether
a tool prompts. Adding a new effect is one enum entry + one switch arm
in the policy; no per-tool override is possible — `ToolDef::needs_
permission` is gone from the type.

### Tool result via `expected`, not parallel bool

`Msg::ToolExecOutput { id, std::expected<std::string, ToolError> result }`
— used to be `{id, output, bool error}` where `output` meant different
things based on the bool. Reducer just dispatches `if (e.result) …
else …` and the typed `ToolError::kind` flows through to the view.

These six examples — taken together — are what "type-theoretic C++"
looks like in this codebase. Match them when adding new code.

---

## Compile-time proofs ("if it builds, it's right")

C++26's `consteval` + `static_assert` lets us turn invariants into
build-time proofs. Where agentty can prove a property at compile time, it
*does* — and any future change that would break the property breaks the
build instead of producing a wrong answer at runtime.

### Trust matrix (`include/agentty/tool/policy.hpp`)

The permission policy `(EffectSet × Profile) → Decision` is a `constexpr`
function, so each cell of the trust matrix can be `static_assert`-ed.
The `proofs` namespace at the bottom of the header is the matrix:

```cpp
static_assert(permission(kPure,  Profile::Write) == Decision::Allow);
static_assert(permission(kExec,  Profile::Write) == Decision::Allow);
static_assert(permission(kRead,  Profile::Ask)   == Decision::Allow);
static_assert(permission(kWrite, Profile::Ask)   == Decision::Prompt);
static_assert(permission(kRead,  Profile::Minimal) == Decision::Prompt);
// … 17 cells total + 5 structural properties
```

A reader can verify the policy by reading these `static_assert`s. A
contributor changing the policy gets a **build error** at the cell that
broke. There is no test runner, no fixture file — the proof lives next
to the function it checks.

### HTTP error → ErrorClass mapping (`include/agentty/provider/error_class.hpp`)

Same shape, applied to the retry policy:

```cpp
static_assert(classify(HttpError{HttpErrorKind::Timeout, 0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Status, 429, ""}) == ErrorClass::RateLimit);
static_assert(classify(HttpError{HttpErrorKind::Status, 401, ""}) == ErrorClass::Auth);
// … every HttpErrorKind + every status that has a defined retry semantics
```

If someone reorders the switch arms in `classify` or adds a new
HttpErrorKind without updating it, the build breaks before any HTTP
request can take a wrong branch.

### Tool catalog (`include/agentty/tool/spec.hpp`)

The catalog of every tool — name, effects, FGTS opt-in — is a
`constexpr std::array`, and the invariants are `consteval` predicates:

```cpp
static_assert(all_names_unique());
static_assert(only_known_exec_tools());
static_assert(no_writefs_and_exec_combo());
static_assert(readonly_invariants());
static_assert(only_web_is_net());
```

The lookup is a `consteval` function and the per-tool factories use a
`FixedName` non-type template parameter:

```cpp
constexpr const auto& kSpec = spec::require<"bash">();   // ✓ compiles
constexpr const auto& kSpec = spec::require<"bsh">();    // ✗ static_assert
```

So a typo in a tool factory becomes a build error at the tool's
translation unit. The catalog is *the* source of truth for capabilities;
no factory can override its declared effects, no factory can register
itself under a name not in the catalog.

### Refinement types (`include/agentty/domain/refined.hpp`)

`Refined<T, Predicate>` carries a type-level proof that some predicate
holds on its inner value. Constructors are private; the only way to
make one is `try_make`, which validates and returns `expected`.

```cpp
auto cmd = NonEmpty<std::string>::try_make(args.command);
if (!cmd) return std::unexpected(ToolError::invalid_args("..."));
// past this point, `cmd->value()` is provably non-empty.

using Port = Bounded<std::uint16_t, 1, 65535>;   // 0 / 65536+ unrepresentable
```

The refinement primitives have their own `static_assert` self-tests
(`namespace tests` in the header). Adding a new refinement should add
a static_assert next to it.

### Why this matters

A reader skimming `policy.hpp`, `error_class.hpp`, `spec.hpp` sees the
proofs first and the implementations second. They don't have to trust
the implementation — the proofs are right there, evaluated at compile
time. A Rust contributor would write `#[test]` for these and run them
in CI; we write `static_assert` and the test runs every build. Same
discipline, smaller blast radius.

The rule for new code: **if it can be a `constexpr` predicate, write the
predicate AND the `static_assert`s that pin it.** Don't ship a runtime
table without compile-time correctness checks for its rows.
