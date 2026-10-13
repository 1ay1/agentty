# Why agentty stays modern C++ (and doesn't move to Rust)

This is not a language flame-war. It's a claim with receipts: **agentty
already gets the guarantees people move to Rust for — and it gets them at
*compile time*, in the header, next to the code they constrain.** The
codebase *is* the argument. Read the files this doc points at and judge
for yourself.

fish moved C++ → Rust (4.0). Good decision *for fish*. This doc walks
through fish's stated reasons and shows, file by file, why they don't
apply here — and where agentty's approach is strictly sharper.

---

## The receipts, up front

- **151 `static_assert` / `consteval` proofs across 18 headers** in
  `include/agentty/` (300+ counting the vendored `maya`/`mcp-cpp` trees),
  all evaluated every build. Count them:
  `grep -rc static_assert include/agentty/ | awk -F: '{s+=$2} END{print s}'`.
- **The build is the test runner.** `cmake --build build` green ⇒ every
  invariant below holds. There is no separate CI step that can be skipped,
  no fixture file that can drift, no `#[test]` someone forgot to run.
- The proofs live **next to the code they constrain**, so a reader sees the
  contract before the implementation and a contributor who breaks an
  invariant gets a compile error *at the line that broke it*.

---

## fish's reasons, mapped onto agentty

### "The C++ toolchain was painful on old / LTS systems"

Real for a shell that ships in every distro's base image and must build on
a decade of toolchains. agentty is an application you build from a pinned
toolchain — `mcp-cpp` and `maya` are vendored and synced, the build is one
`cmake --build`. The distro-toolchain-matrix pain fish felt is not a pain
agentty has.

### "Memory safety on untrusted input"

A shell parses arbitrary user command lines — a huge, adversarial surface.
agentty's untrusted input is your prompt, the model's output, and tool
results, all funnelled through **one thin dynamism boundary** (`docs/DESIGN.md`
rule 5). Past that boundary every value is typed:

- `http::Client::send → std::expected<Response, HttpError>` — errors are a
  closed `enum class HttpErrorKind`, never a stringly-typed guess.
- `persistence::load_thread_file → std::expected<Thread, DeserializeError>`
  — no `catch(...) { continue; }`; the user sees *which* file and *why*.
- `auth::Credentials = variant<None, ApiKey, OAuth>` — each arm owns only
  the data valid in its state.

The bugs that actually bit agentty in the field (see the project memory:
scrollback rewrite races, UTF-8 splicing in the Anthropic transport,
control bytes in tool cards, streaming reducer ordering) are **protocol and
rendering-model bugs**. Rust's borrow checker catches none of them. They're
logic invariants — and agentty pins logic invariants at compile time, which
is the thing this doc is about.

### "Cargo / crates.io dependency management"

A genuine convenience. Not a reason to rewrite a working, disciplined
codebase — it's a build-system preference, addressable with vendoring and a
pinned toolchain (which agentty already does).

### "Rust's `#[test]` discipline"

This is the crux, and it's where agentty is **sharper, not weaker**. A Rust
contributor proving a table correct writes `#[test]` functions and runs
them in CI. agentty writes `consteval` predicates and `static_assert`s them
— so the proof runs **every build**, can't be skipped, and lives in the
same header as the table. Same discipline, smaller blast radius.

---

## The exhibit — proofs you can read in five minutes

### `include/agentty/tool/policy.hpp` — the trust matrix
The permission policy `(EffectSet × Profile) → Decision` is a pure
`constexpr` function. The `proofs` namespace `static_assert`s every cell:
`permission(kWrite, Profile::Ask) == Decision::Prompt`, and 16 more. Change
the policy and the build breaks *at the cell that changed*.

### `include/agentty/provider/error_class.hpp` — the retry policy
`classify(HttpError) → ErrorClass` is exhaustive on the enum (add a
`HttpErrorKind` without handling it ⇒ compile error) and every retry-
relevant status is pinned as a `static_assert`:
`classify({Status, 429}) == RateLimit`, `classify({Status, 401}) == Auth`,
`classify({Status, 503}) == Transient`. No HTTP request can
take a wrong retry branch without the build failing first.

### `include/agentty/tool/spec.hpp` — the capability catalog
A `constexpr std::array` of every tool, with `consteval` invariants proven
over it:
- `kinds_bijective()` — name ↔ Kind is a bijection; `kind_of`/`name_of`
  round-trip. A typo in a tool factory (`spec::require<"bsh">()`) is a
  `static_assert` failure, not a runtime `nullptr`.
- `only_known_exec_tools()` — only `bash`, `diagnostics`, `task` may carry
  `Effect::Exec`. Adding a fourth is a deliberate, reviewed build break.
- `no_writefs_and_exec_combo()`, `readonly_invariants()`,
  `only_web_is_net()` — capability shape proven, not documented-and-hoped.
- `truncation_matches_effect_shape()` — a tool's output-truncation strategy
  must follow from its effects (Exec ⇒ tail-clip logs, etc.).
- `sched_effects_match_except_task()` + `task_divergence_is_exact()` — the
  one place scheduling-effects diverge from permission-effects (so subagents
  fan out concurrently instead of serialising behind a coarse Exec lock) is
  pinned *exactly*. Rename `task`, change its effects, or edit
  `sched_effects`, and the build fails.

### `include/agentty/domain/refined.hpp` — refinement types
`Refined<T, Predicate>` carries a type-level proof that a predicate holds.
Constructors are private; the only way in is `try_make`, which validates
and returns `expected`. `NonEmpty<std::string>`, `Bounded<uint16_t,1,65535>`
(a port that literally *cannot* hold 0 or 65536). The primitives carry their
own `static_assert` self-tests.

### `include/agentty/util/env.hpp` — the env catalog
`every_var_has_row()` + `all_names_unique()` + `names_well_formed()`, all
`consteval`. The `Var` enum and the string catalog are proven to be in
bijection at build time — the Rust-in-CI check, run every compile.

---

## When moving *would* be right

Honesty matters or the argument is worthless. Move to Rust if:

- You want a large external plugin/contributor ecosystem and C++ is
  demonstrably scaring contributors away.
- You start hitting genuine **data races** the borrow checker would prevent.
  (agentty's concurrency bugs to date have been protocol/ordering logic, not
  data races — a different failure class.)
- You're doing a clean-slate rewrite for unrelated reasons anyway.

None of those is true today. Until one is, the move throws away a large body
of subtle, hard-won, *compile-time-enforced* correctness to re-earn it in a
new language. The receipts above are that body of work.

---

## The one-line version

> A Rust contributor writes `#[test]` and hopes CI runs it. agentty writes
> `static_assert` and the compiler runs it — every build, in the header,
> next to the code. 151 proofs, zero that can be skipped. Same discipline,
> smaller blast radius.

Read `docs/DESIGN.md` for the full contract. Then read the five headers
above. The code is the argument.
