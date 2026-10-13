# agentty over ACP — editor integration & config options

agentty ships a full **Agent Client Protocol (ACP)** agent:

```sh
agentty acp -w <project-root> [--profile ask|write|minimal] [-m <model>] [--provider <p>]
```

It speaks JSON-RPC 2.0 over stdio, so any ACP-compatible editor (Zed,
JetBrains, Neovim via CodeCompanion / avante / agentic.nvim, Emacs via
agent-shell, …) can drive it as a subprocess. This doc covers the part most
relevant to making agentty feel *native*: its **session config options** — the
live, cross-editor surface for agentty's own configuration (permission mode,
model, …).

## Why config options

ACP's config-option mechanism lets the agent advertise a typed list of
selectors; the editor renders them as native dropdowns and the user changes
them **live, mid-session, with no restart** via `session/set_config_option`.
This is how agentty's configuration surface reaches every editor uniformly —
you do not re-implement agentty's settings in each editor; you render the
options it emits.

## What agentty advertises

On `session/new` (and `session/resume`), agentty sends a `session/update` of
type `config_options` carrying the **complete** option set:

| configId | category | type | values | maps to |
|----------|----------|------|--------|---------|
| `mode`   | `mode`   | select | `ask` · `write` · `minimal` | agentty's permission profile *(v2+ only)* |
| `model`  | `model`  | select | the model catalog | the session's active model |

- **`mode`** is surfaced **exactly one way per connection**, gated on the ACP
  protocol version negotiated at `initialize` (clean cut, no dual-surface):
  - a **v1** client gets the permission mode via `SessionModeState` (the
    `modes` field on the session result + `current_mode` updates),
  - a **v2+** client gets it as the `mode` **config option** and NOT via
    `modes`.
  It is never emitted on both surfaces, so a client never has to reconcile two
  representations of the same setting. (agentty advertises v2 only when built
  with `-DACP_ENABLE_V2_DRAFT`; the stable build negotiates v1.)
- **`model`** is version-agnostic and best-effort: if the catalog can't be
  built, the option is simply omitted (the session still works on the server
  default).

Each `config_options` notification is the **full state**, per the v2 contract —
never a delta. Clients should replace their view of the options wholesale.

## Changing an option

The client calls `session/set_config_option`:

```json
{ "sessionId": "sess_…", "configId": "mode", "value": "write" }
```

agentty:

1. validates the value (unknown `configId` or bad value → a JSON-RPC error, not
   a silent no-op — so a typo surfaces),
2. applies it to the live session (thread-safe against an in-flight turn),
3. for `mode`, also emits the legacy `current_mode` update,
4. echoes the **complete** new config state as a `config_options`
   notification so dependent options reconcile atomically.

## Ambient configuration (files + flags)

Everything else in agentty's config surface is file/flag driven and is picked
up identically whether you launch the TUI or `agentty acp` — the ACP agent
reads the **same** config as the TUI:

| Config | Delivery |
|--------|----------|
| MCP plugins | `.agentty/mcp.json` |
| Hooks | `.agentty/hooks.json` |
| Skills / slash commands / subagents | `.agentty/{skills,commands,agents}/` |
| Model / provider default | `-m` / `--provider` on the spawn command |
| Permission profile default | `--profile` on the spawn command |
| Workspace scope | `-w <project-root>` |
| Sandbox | `--sandbox` on the spawn command |

To change an ambient setting, edit the file (or flag) and restart the
`agentty acp` subprocess. Slash commands and skills are advertised live via the
`available_commands` update, so the editor's `/` menu stays in sync per
session without a restart.

## Observability

`AGENTTY_LOG=acp=trace agentty acp` dumps every JSON-RPC frame to the structured log
(stderr is free-form under the ACP stdio framing) — the ground-truth wire log
while building or debugging a client.

## Compatibility

- agentty negotiates ACP protocol version 1 and advertises `fs` +
  `terminal` client-capability use, `loadSession`, and the config-option
  surface above.
- The `mode`/`model` options degrade gracefully: a client that ignores
  `config_options` still gets modes (via `SessionModeState`) and the server
  default model.
