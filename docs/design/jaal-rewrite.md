# agentty on jaal: the design

This branch is not a port. Moving `maya::Cmd` to `jaal::Cmd` is a rename
and a weekend; doing only that would carry every shortcut the old runtime
allowed into a runtime built to forbid them.

What follows is what agentty should look like when jaal's Elm model is
taken seriously, what it looks like today, and the order to close the gap.

## The three properties we are buying

jaal's Elm loop is worth having because of what it makes TRUE, not because
of how it spells things:

1. **`update` is pure.** A reducer takes the model and a message and
   produces the next model plus a DESCRIPTION of what should happen. It
   reads no globals, touches no disk, starts no work. You can call it in a
   test with no world attached and get the whole behaviour.
2. **Effects are values.** `Cmd` is data. The runtime performs it. That
   means a test asserts `expect_effect<save_settings>(1)` instead of
   mocking a filesystem, and replay can fold a recorded result without
   re-running the child.
3. **The host is swappable.** A program that only names its effects in its
   `Cmd` type can run on the terminal host, on `jaal::headless` in a test,
   or on a future ACP host — and the compiler checks the host can serve
   what the program asks for.

Every item below is here because it breaks one of those three.

## What is actually wrong today

Measured on this tree, not guessed:

| | count | what it breaks |
|---|---|---|
| `deps()` reads inside reducers | 84 | (1) purity |
| ...of which reach the filesystem seam | 36 | (1) and (2) |
| `pair<Model, Cmd>` returns | 172 | ceremony only |
| `maya::Cmd<Msg>` sites | 126 | (3) |
| effect factories reading global provider/auth state | 24 | (1) |

### The one that matters: reducers call the world

`update/appearance.cpp`, verbatim:

```cpp
void persist(const Model& m) {
    auto s = deps().load_settings();   // read through the seam
    s.ui = m.d.ui;
    deps().save_settings(s);           // write through the seam
}
```

Be precise about what is and isn't wrong here, because the existing code
already solved the obvious problem. `save_settings` is WRITE-BEHIND: it
publishes to an in-memory cache and a worker does the fsync, explicitly so
no reducer can block on settings IO "even if it tries". That was the right
fix for the old runtime and it works. `write_file` (diff-review, 4 sites)
is the one that still writes synchronously, and it is user-initiated.

So this is not a latency bug. It is a PURITY bug, and the cost is
structural:

- `update` cannot be called without a world attached. Testing a reducer
  means installing a `Deps` with seven closures in it.
- `Deps` must exist at all — a mutable global, installed at startup,
  reached through type erasure, because reducers need to call out.
- The read-modify-write says the Model is not trusted as the source of
  truth: we re-read settings from the seam to edit one field we already
  have in `m.d.ui`.
- The write-behind cache exists to make an impure reducer safe. Make the
  reducer pure and the cache has nothing left to protect.

jaal will not stop you doing any of this. Nothing stops you. But it is the
difference between "we use an Elm runtime" and "we are Elm".

## The target design

### 1. Persistence is an effect

First, the thing that forces the read-modify-write. `store::Settings` has
~10 fields; the Model does not hold one. It scatters them — `model_id`,
`profile`, `ui`, `favorite_models`, `provider`, `provider_models`,
`context_overrides`, `recent_models` all live as separate members of
`m.d`, and `provider_keys` doesn't live in the Model at all (it is sealed
to a different file). So a reducer that wants to persist ONE field has no
way to produce a whole `Settings` — it must re-read the last one from the
seam and patch it. The impurity is downstream of a missing type.

So: give the Model the settings it owns.

```cpp
struct Model {
    struct Domain {
        store::Settings persisted;   // the whole record, one field
        // ...everything else that is NOT persisted
    };
};
```

and then persisting is a value, with nothing to read first:

```cpp
struct SaveSettings { store::Settings s; };
using save_settings = jaal::pure_fx<SaveSettings, "save_settings">;

Cmd update(Model& m, AppearanceThemeChanged e) {
    m.d.persisted.ui.theme = e.theme;
    return Cmd::fx<save_settings>({m.d.persisted});
}
```

`provider_keys` stays out of the Model and out of `Settings`-at-rest, as
it is today: the host's save handler merges the sealed key map back in.
That is a host concern (it owns the keystore), not a reducer's.

The host performs it, off the UI thread, coalesced — the same write-behind
guarantee `Deps::save_settings` gives today, except now it is the runtime's
job instead of a closure the reducer calls. Note what fell out: no
`load_settings()` first. The Model already holds the settings; if it
didn't, the reducer had no business editing them.

**This deletes `Deps` entirely.** Everything in it is either an effect
(`save_*`, `write_file`, `delete_thread`) or a pure function that never
needed erasing (`title_from`, `new_thread_id`). The write-behind cache
goes with it: the runtime already coalesces effects.

### 2. Effect factories take what they need

Today `fetch_models()` reaches for `provider::active()` and
`auth_snapshot()` from inside the task body, on a worker thread, with a
mutex to make the race survivable. The mutex is a symptom: the effect is
reading state it wasn't given.

```cpp
// before: fetch_models() — reads active provider + auth from globals
// after:  the reducer, which HAS the model, says which provider
Cmd update(Model& m, ProviderSwitched e) {
    m.d.provider = e.id;
    return Cmd::fx<fetch_models>({e.id, auth_for(m, e.id)});
}
```

The payload is `Sendable` by construction, the staleness check the
factory does by hand (`for_provider` vs delivery-time provider) becomes
comparing the reply's tag against the model, and the mutex disappears
with the shared read.

### 3. `update` mutates, and says so

jaal's shape is `Cmd update(Model&, Msg)`. 172 `return {std::move(m), ...}`
become `return {}`. This is the least interesting change and the one that
touches the most lines — do it mechanically, in its own commit, after the
two above, so a rename never lands mixed with a behaviour change.

The 10-arm `std::visit` in `update.cpp` stays as-is: it is already the
right shape, it just loses the pair.

### 4. One effect row, declared once

```cpp
using Cmd = jaal::Cmd<Msg,
    // agentty's own
    save_settings, save_thread, delete_thread, write_file,
    launch_stream, run_tool, fetch_models, /* ... */
    // maya's terminal effects
    maya::commit_scrollback, maya::write_clipboard, maya::query_clipboard,
    maya::reset_inline, maya::force_redraw, maya::emit_host_sequence,
    maya::suspend>;
```

`jaal::require_host_for<Host, AgenttyApp>()` then proves, at compile
time, that the host can serve every one. That is property (3): the ACP
host we keep talking about becomes a host that implements the non-terminal
subset, and the compiler tells us exactly which effects it still owes.

## What we do NOT change

- **The Model's shape.** `d` / `s` / `ui` is already the right split:
  persisted domain, live stream state, this session's panels.
- **The Msg domain variants.** 23 domain arms with a unique-leaf proof is
  good design; jaal changes nothing about it.
- **`visual_hash`.** maya's host already uses it to skip `view()`. It
  survives verbatim.
- **The reducer file layout.** 23 files by domain, one concern each.
- **Subscriptions.** `subscribe()` is already a pure function of the model
  that returns data. It ports as a spelling change.

## Order

Each step builds and passes tests on its own. No step needs the next one
to make sense.

1. **Settings/threads become effects.** Delete `Deps`. Biggest behaviour
   win, biggest diff, entirely independent of jaal — this is worth doing
   even if the migration stalls.
2. **Effect factories take their inputs.** Drops `auth_snapshot()`'s
   mutex and the by-hand staleness checks.
3. **Bump the maya submodule** to the jaal-rewrite maya, link `maya::app`.
4. **`update` signature**, mechanically, 23 files.
5. **`maya::Cmd` → `jaal::Cmd`**, declare the row, add
   `require_host_for`.
6. **Delete what the old runtime needed** and the new one doesn't.

Steps 1 and 2 are pure agentty work against the CURRENT maya. They are
the ones that make this a redesign instead of a rename.

## How we will know it worked

- `update` compiles and runs with no `deps()`, no filesystem, no globals.
- A reducer test is: build a Model, send a Msg, assert on the new Model
  and on `expect_effect<save_settings>(1)`.
- `AgenttyApp` runs on `jaal::headless` with no terminal.
- `grep -rn 'deps()' src/runtime/app/update/` prints nothing.
