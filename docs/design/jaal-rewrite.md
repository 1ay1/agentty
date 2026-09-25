# agentty on jaal: the design

This branch is not a port. Swapping `maya::Cmd` for `jaal::Cmd` is a rename
and a weekend; doing only that would carry every workaround the old runtime
forced into a runtime that doesn't need them.

The important discovery, on reading jaal properly: **jaal was designed
against agentty's shape.** D36 in `jaal/docs/decisions.md` cites agentty's
232 message types and 20 hand-grouped domains by name, and measures the
rebuild costs we hit. D29's `children<>` exists for "agentty's sessions".
D7's deep `Sendable` "found a real race in agentty (`LazyBytes`, 19 TSan
reports → 0) on its first run against real code".

So most of this work is not writing adapters. It is **deleting the
machinery agentty built because the old runtime had none**, and letting
jaal's version do the job it was built from.

## The three properties we are buying

1. **`update` is pure.** A reducer takes the model and a message and
   produces the next model plus a DESCRIPTION of what should happen. No
   globals, no disk, no threads started. Callable in a test with no world.
2. **Effects are values.** `Cmd` is data; the runtime performs it. A test
   asserts `expect_effect<save_settings>(1)` instead of mocking a
   filesystem, and replay (D23) folds a recorded run without re-running it.
3. **The host is swappable.** A program that names its effects in its `Cmd`
   type runs on the terminal host, on `jaal::headless` in a test, or on a
   future ACP host — and the compiler checks the host can serve every one.

## What jaal already has that agentty hand-rolled

This is the heart of the redesign. Each row is code we delete, not port.

| agentty today | jaal | why theirs is better |
|---|---|---|
| `Deps` — a mutable global of 11 `std::function`s, installed at startup, reached from 84 places in reducers | effects as data | the reducer says *what*; the host does it. No global, no erasure, testable without a world |
| by-hand staleness: `for_provider` stamped into a fetch, compared at delivery | `jaal::debounce<T>` + token | D23's exact bug. `ready(token)` is correct where comparing values is not ("ab" → "a" matches a stale timer) |
| `auth_snapshot()` + a mutex, because workers read auth the UI thread swaps | `jaal::shared<T>` (`Sendable && Frozen`) | no writer exists, so no race to guard. D8 |
| `Cmd::task_isolated` + agentty's own thread bookkeeping | `task` with `placement::isolated` | stop tokens, bounded shutdown (D20), abandonment that's safe under ASan/TSan |
| streaming turn as a task that posts back | `Sub::stream(key, body, args...)` | D21: runs while subscribed, cancelled when not, and **a cancelled stream cannot land a message in a model that no longer expects it** — which is the class of bug `m.s.active()` guards are working around |
| 23 domain reducers dispatched by a hand-written 10-arm `std::visit` | D36 tree routing + `handled_as_group` | jaal descends the variant tree itself; a missing leaf is a compile error that names the domain file to open |
| `sizeof(Msg)` pinned by the heaviest leaf; 19 s rebuilds | same tree, measured | 1.19 s per domain TU, 1.77 s for the loop TU |

The `children<>` row is deliberately left out of step 1: agentty's threads
are a list of child conversations and `jaal::children<>` is the right shape
for them, but that is a bigger change than this branch should start with.
It is noted at the end.

## What is actually wrong today

Measured on this tree:

| | count | breaks |
|---|---|---|
| `deps()` reads inside reducers | 84 | (1) |
| ...of which reach the filesystem seam | 36 | (1), (2) |
| `pair<Model, Cmd>` returns | 172 | ceremony |
| `maya::Cmd<Msg>` sites | 126 | (3) |
| effect factories reading global provider/auth | 24 | (1) |

### The root cause is a missing type

`update/appearance.cpp`, verbatim:

```cpp
void persist(const Model& m) {
    auto s = deps().load_settings();   // read through the seam
    s.ui = m.d.ui;
    deps().save_settings(s);           // write through the seam
}
```

Be fair to this code: `save_settings` is already WRITE-BEHIND, explicitly
so a reducer can't stall a frame on disk. **This is not a latency bug.**

The question is why the READ is there, and `init()` answers it:

```cpp
auto settings = deps().load_settings();
m.d.model_id           = settings.model_id;
m.d.profile            = settings.profile;
m.d.ui                 = settings.ui;
m.d.show_changes_strip = settings.show_changes_strip;
m.d.show_reasoning     = settings.show_reasoning;
// ...~8 more
```

`init()` UNPACKS `store::Settings` field-by-field into `m.d`. The Model
never holds the record. So a reducer that changes one preference has no way
to produce a `Settings` to save — it must read the last one back and patch
it. **The impurity is downstream of a missing field.**

Give `Domain` the record:

```cpp
struct Domain {
    store::Settings persisted;   // the whole thing, one field
    // ...everything NOT persisted
};
```

and persisting becomes a value with nothing to read first:

```cpp
struct SaveSettings { store::Settings s; };
using save_settings = jaal::pure_fx<SaveSettings, "save_settings">;

Cmd update(Model& m, AppearanceThemeChanged e) {
    m.d.persisted.ui.theme = e.theme;
    return Cmd::fx<save_settings>({m.d.persisted});
}
```

`provider_keys` stays sealed outside the Model as it is today; the host's
save handler merges it back. That is a host concern — it owns the keystore.

**This deletes `Deps`.** Every member is either an effect (`save_thread`,
`delete_thread`, `write_file`) or a pure function that never needed erasing
(`title_from`, `new_thread_id`). The write-behind cache goes too: it exists
to make an impure reducer safe, and the runtime already coalesces effects.

## The streaming turn is a stream

The biggest structural win, and the one I'd have missed without reading
D21/D32. Today a turn is a task that posts messages back, and the reducers
carry `m.s.active()` guards so a late message from a cancelled turn doesn't
corrupt a new one.

jaal's answer:

```cpp
static Sub subscribe(const Model& m) {
    if (!m.s.active()) return Sub::none();
    return Sub::stream("turn/" + std::to_string(m.s.turn_id),
                       run_turn, m.s.request);   // args by value, Sendable
}
```

- Cancelling is *not asking any more* — the reducer clears `m.s`, and the
  reconciler stops the stream by key (D11).
- D32 closes the stale-message hole in the runtime: a stopped stream's
  messages cannot be delivered. The guards become unnecessary rather than
  merely redundant.
- The turn id in the key means two turns can never reconcile to one
  subscription (D29's prefix bug, already solved).

## The effect row

```cpp
using Cmd = jaal::Cmd<Msg,
    // agentty's own
    save_settings, save_thread, delete_thread, write_file,
    fetch_models, run_tool, /* ... */
    // maya's terminal effects
    maya::commit_scrollback, maya::write_clipboard, maya::query_clipboard,
    maya::reset_inline, maya::force_redraw, maya::emit_host_sequence,
    maya::suspend>;
```

`jaal::require_host_for<Host, AgenttyApp>()` then proves at compile time
that the host serves every one. The ACP host becomes a host implementing
the non-terminal subset, and the compiler lists what it still owes.

## What we do NOT change

- **The `d` / `s` / `ui` split.** Persisted domain, live stream, session UI
  is the right decomposition.
- **The 23 domain variants.** D36 says this is the shape that scales;
  jaal's contribution is routing them for us, with `handled_as_group`.
- **`visual_hash`.** maya's host uses it to skip `view()`. Survives.
- **The reducer file layout.** One domain per TU is what keeps rebuilds at
  1.19 s.

## Order

Each step builds and passes tests alone.

1. **`Settings` into the Model; persistence becomes an effect.** Deletes
   `Deps`. Pure agentty work against the CURRENT maya — worth doing even if
   the migration stalls.
2. **Effect factories take their inputs.** Drops `auth_snapshot()`'s mutex
   and the by-hand staleness stamps (`jaal::debounce` replaces them in 5).
3. **Bump the maya submodule** to jaal-rewrite maya; link `maya::app`.
4. **`update` signature** — `Cmd update(Model&, Leaf)`, mechanically, 23
   files, 172 sites. Add `handled_as_group` per domain so D36 routes it.
5. **`maya::Cmd` → `jaal::Cmd`**, declare the row, `require_host_for`.
6. **The turn becomes a `Sub::stream`**; delete the staleness guards D32
   makes unnecessary.
7. **Delete `Deps`, `auth_snapshot`, the write-behind cache** and the rest
   of the old runtime's scaffolding.

Later, not this branch: threads as `jaal::children<>`, and `sim<P>`
(D28) over the turn state machine — 900k seeds/second against a state
machine we currently test by hand.

## How we will know it worked

- `grep -rn 'deps()' src/runtime/app/update/` prints nothing.
- A reducer test is: build a Model, send a Msg, assert on the new Model and
  `expect_effect<save_settings>(1)`.
- `AgenttyApp` runs on `jaal::headless` with no terminal.
- The stale-turn guards are gone and the tests that covered them still pass.

## Where it actually landed

All four, with one documented exception.

**`deps()` in `update/`: one call left**, `refresh_record`'s. It is not
scaffolding — `provider_keys` is the one field the Model does not own,
because the credential helpers (`credentials::add_key`, `vault::key_clear`,
`account_switch`) write `settings.json` directly from paths that have no
`Deps`. A reducer that touches keys has to re-read first. The seam itself is
gone from every other reducer: persistence is four effects
(`runtime/store_fx.hpp`) run by `app::Host`, and `new_thread_id` /
`title_from` turned out to be pure functions that never needed erasing.

**Effect-asserting tests exist** — `tests/reducer_effects_test.cpp`, no deps
and no disk. Tests that assert on the store use `agtest::fx::run` to play
the host's part, which is stricter than the old seam: a save only appears
if the arm actually returned it.

**The turn did NOT become a `Sub::stream`, and does not need to.** Step 6
assumed the staleness guards were the cost of a turn being a task. They
weren't: `phase::Active` holds the turn's entire state — cancel token,
retry budgets, timing — *inside* the variant, so leaving `Streaming`
destroys it and a late message from a dead turn has nothing to corrupt.
Staleness is unrepresentable rather than guarded against. `grep 'm.s.active()'`
in `update/stream.cpp` returns nothing.

The login workers DID become keyed sources, because their bug was real:
as one-shot tasks their `stop_token` only fired at kernel shutdown, so Esc
left a worker polling for the rest of its 900 s budget. That is the case
`Sub::stream` is for — work that must stop when the program stops asking.

**Two hooks failed silently before they were found**, both the same shape:
jaal detects optional hooks with a `requires` test, so a signature it
cannot call reads as "this doesn't exist" rather than as an error.
`AgenttyApp::init` kept the old `pair<Model, Cmd> init()` — every saved
setting and thread was loaded and thrown away. `terminal_host::attach` took
an exact context type — a derived host never got attached, so agentty opened
and accepted no keys. `tests/program_hooks_test.cpp` static-asserts both
layers now, because nothing else would notice.

Still open, and genuinely later: threads as `jaal::children<>`, `sim<P>`
over the turn state machine, `resume_from`/`journal` for crash recovery.
