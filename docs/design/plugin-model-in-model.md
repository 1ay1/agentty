# Plugin model — owned by the TEA Model

**Status:** SHIPPED. Follows `docs/design/plugin-model.md` (which unified the
three-source truth into one `PluginModel`). This is the next step: putting
that snapshot *inside* the Elm/TEA Model.

## The bug class this makes impossible

agentty is a strict Elm/TEA app: `view(Model)` and `visual_hash(Model)` are
**pure functions of the Model**, and the render loop skips `view()+render()`
whenever `visual_hash` is unchanged (that's how a 30 fps tick stays free).

The Plugins panel violated this. Its truth — the MCP connection state — lived
in a **process-global connection pool**, reached by a side-effecting
`plugin_model()` call *at render time*. Every recurring bug was a direct
consequence:

| Symptom | Root |
|---|---|
| Panel stuck on "connecting…" after the server connected | pool changed, but `visual_hash(Model)` can't see the pool → render gated away. "Fixed" once with a `reload_nonce` — a hack that manually mirrors external state into the Model. |
| Panel stuck on "connecting…" forever (until a turn was sent) | connection was a **lazy side effect** of the first `registry()` access, not driven by the update loop → never fired if you just opened the panel. |
| Selector lag, paste mis-routing (same family) | view/input depended on state the pure loop couldn't see. |

The tell was the `reload_nonce`: **when you must poke a nonce to mirror
external state into the Model so the loop notices, that state should have been
in the Model.**

## The design

Bring the snapshot into the Model; drive connection through the update loop —
exactly the pattern agentty already uses for threads (`load_threads_async` →
`ThreadsLoaded{data}` → stored in `m.d.threads`).

```
m.ui.plugins : mcp::PluginModel      // the snapshot the view renders
m.ui.plugins_loading : bool          // a connect Cmd is in flight

OpenSettingsList{Plugins}            // opening the panel …
  → plugins_loading = true
  → load_plugins_async(reconnect)    //   … dispatches a Cmd (loop-driven)

load_plugins_async  (task_isolated worker)
  → registry() [cold connect] + reload_mcp_plugins() [resync]
  → dispatch PluginsUpdated{ plugin_model() }

PluginsUpdated{snapshot}             // reducer stores the result …
  → m.ui.plugins = snapshot
  → plugins_loading = false          //   … the ONLY source the view reads
```

- **`view` is pure again.** `items_for(Plugins)` renders `m.ui.plugins`; it
  never calls `plugin_model()`. So `visual_hash` covers it automatically (it's
  a Model field) — the `reload_nonce` hack is **deleted**, and lag is
  impossible by construction.
- **Connection is a Cmd, not a lazy side effect.** Opening the panel *is* the
  trigger; startup fires it too (`init`). "Never connected because no turn was
  sent" cannot happen.
- **The global pool stays** — it owns the sockets/child processes. But its
  UI-facing *value* is the Model snapshot the reducer holds. The pool is an
  effect target; the Model is the truth.

## Why this kills the whole class

The invariant is now structural, not vigilance-based:

1. Anything the Plugins view shows is a field of `m.ui.plugins`.
2. `visual_hash` hashes `m.ui.plugins` (server count + per-server
   connected/error/tool-count + loading flag), so any change repaints — no
   nonce, no "forgot to hash this axis."
3. `visual_hash_coverage_test` asserts every view axis (including the plugins
   snapshot and loading flag) advances the hash, so a future field that the
   view reads but the hash forgets **fails the build**.
4. `plugins_in_model_test` asserts the reducer contract: open arms loading +
   returns a Cmd; `PluginsUpdated` stores the snapshot; `items_for` is a pure
   projection.

To reintroduce the old bug you'd have to (a) move `PluginModel` back out of the
Model, and (b) delete two tests. The architecture no longer *allows* the
render gate and the connection truth to disagree.

## Include-cycle note

`PluginModel`/`ServerState`/`ToolState` are plain value types (`<string>` /
`<vector>` only). They were split out of `mcp/client.hpp` into
`mcp/plugin_model.hpp` so the Model can own a snapshot **without** dragging in
the whole MCP client API (which depends on `tools::ToolDef` and would form an
include cycle through `model.hpp`). `client.hpp` and `model.hpp` both include
the light header; only TUs that actually connect include `client.hpp`.

## Files

| Concern | File |
|---|---|
| Snapshot value type (cycle-free) | `include/agentty/mcp/plugin_model.hpp` |
| Owned in the Model | `include/agentty/runtime/model.hpp` (`ui.plugins`, `ui.plugins_loading`) |
| Connect Cmd | `src/runtime/app/cmd_factory.cpp` (`load_plugins_async`) |
| Message | `include/agentty/runtime/msg.hpp` (`PluginsUpdated`) |
| Reducer (open → Cmd, update → store) | `src/runtime/app/update/settings_list.cpp` |
| Pure projection | `src/runtime/settings_items.cpp` (`plugins(model, loading)`) |
| Render gate | `include/agentty/runtime/app/program.hpp` (`visual_hash` hashes `ui.plugins`) |
| Startup trigger | `src/runtime/app/init.cpp` |
| Tests | `tests/plugins_in_model_test.cpp`, `tests/visual_hash_coverage_test.cpp` |
