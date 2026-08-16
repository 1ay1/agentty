# Plugin / MCP tool model — design

## The problem this fixes

Plugin/tool state was scattered across three sources that could disagree,
and every consumer (the picker especially) reconciled them at read time:

1. **mcp.json** — which servers exist, and each server's `tools.exclude`.
2. **ConnectionPool** — the live connected servers + the tools they
   advertise.
3. **wire-cache Snapshot** — the projected catalog the model actually sees.

The picker built its tool list by *unioning* (1)+(2)+(3), which produced
the vanishing-row / duplicate / stale bugs: a toggled tool could be
filtered from (3) but not yet in the picker's view of (1), so it fell
through. Toggling re-spawned the server (hang), and there was no single
place that knew "the truth" about a tool's state.

## The model

One authoritative in-memory model, owned by the bridge
(`agentty::mcp`), rebuilt only when the config changes or a server
re-lists. Everything else is a projection of it.

```
PluginModel                       (the single source of truth)
  servers: [ ServerState ]
    name        (config key)
    command     (config)
    connected   (live: did the handshake succeed?)
    error       (live: why not, if !connected)
    tools: [ ToolState ]
      bare      (advertised name, from the live server)
      enabled   (derived: NOT in config tools.exclude)
      title/desc(advertised metadata, for the UI)
```

- **The name list is authoritative from the live server** (what it
  advertises), never from the wire catalog. A tool that exists but is
  disabled is still in the model — it can never vanish.
- **enabled is derived once** from config `tools.exclude`, in the model —
  not recomputed by each consumer.
- The wire catalog (what the model sees) is a *pure projection*: every
  tool whose `enabled` is true, minus the budget trim (below).

## Operations (all robust, all in the bridge)

| op | effect | live? | re-spawn? |
|----|--------|-------|-----------|
| `plugin_add(name, cmd, args)` | write config, connect the new server | yes | new server only |
| `plugin_remove(name)` | write config, drop the server | yes | teardown only |
| `tool_set_enabled(server, bare, on)` | write config exclude, re-derive `enabled`, re-project | yes | **no** |
| `reload()` | re-read config, connect added / drop removed / keep unchanged | yes | only changed servers |

Key robustness rules:

- **Reloads are serialized + coalesced** (one at a time; a request during
  a reload sets a pending flag so no edit is lost, no stacked spawns).
- **Toggling never re-spawns** — it's a config write + re-project. The
  server stays connected; `enabled` flips; the catalog rebuilds.
- **Unchanged servers are not re-spawned on reload** — reload diffs the
  new config against connected servers and only spawns/tears down the
  delta. (Today reload rebuilds the whole pool.)
- **Every accessor returns a value snapshot** (holds the shared_ptr across
  the copy) — no reference into swappable cache memory escapes, so a
  concurrent reload can't dangle a reader (the UAF crash).

## The tool budget

Providers cap how many tools a request may carry (and OAuth/first-party
paths reject some client-defined tools entirely — the mcp_search_tools
400 already handled). Beyond that, a very large tool set bloats every
request. So the model exposes a **budget**:

- `kToolBudget` — soft cap on total wire tools (native + enabled MCP).
- The projection trims *MCP* tools past the budget (native tools always
  ship; they're the core toolset), lowest-priority first, and records how
  many were trimmed.
- `PluginModel::over_budget()` / `trimmed_count()` drive a **warning in
  the picker** ("N tools over the budget of M — K MCP tools were dropped
  from this session; disable some to make room"), so the user sees exactly
  what happened and can act.

## Layering

```
agentty::mcp   PluginModel + operations + projection + budget   (truth)
      │  value snapshots only
      ▼
tools::registry   merges native + projected MCP into the wire catalog
      │  value snapshot
      ▼
settings_items    turns a PluginModel snapshot into picker rows
      │
      ▼
settings_list_view / reducer   pure UI: render rows, dispatch ops
```

The picker becomes a *pure* view of `PluginModel` + a dispatcher of the
four operations. It holds no reconciliation logic and no state of its
own — which is what makes the vanishing/duplicate/stale class impossible
by construction.
