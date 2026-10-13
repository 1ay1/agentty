---
title: Plugins (MCP servers)
description: Extend agentty with external tools by adding MCP servers — a browser driver, a database client, a hosted API — from a single mcp.json or the agentty plugin CLI.
nav_section: Advanced
nav_order: 35
slug: plugins
---

A **plugin** is an external [MCP](https://modelcontextprotocol.io) server that adds tools to agentty. Point agentty at a Playwright server and the model can drive a browser; add a Postgres server and it can query your database. Plugins are how you extend the agent's capabilities beyond its native toolset without recompiling anything.

Under the hood a plugin *is* an MCP server — this page is the practical "add a tool to my agent" guide; [MCP Server](/docs/mcp) covers the full protocol, OAuth, and serving agentty's own tools the other way.

There are two ways to configure plugins and they edit the same files — pick whichever fits: the **TUI** for the common cases (add / enable / disable / remove / approve, no JSON), or **editing `mcp.json`** directly for full control (env vars, headers, per-tool filters, timeouts).

## Where plugins live

agentty reads **both** of these and unions them (a project server and a user server are both active; on a name clash the project one wins):

- `~/.agentty/mcp.json` — **user** scope: your servers, on every project.
- `<project>/.agentty/mcp.json` — **project** scope: committed with the repo, shared with your team.

`$AGENTTY_MCP_CONFIG` points at an explicit file that overrides both. A project `mcp.json` can ride in on a clone, so its command-spawning servers are gated — see [Trust](#trust).

:::note
Plugins are lazy and opt-in. With no `mcp.json` present, startup is a single `stat()` that finds nothing — zero overhead when you aren't using any.
:::

## Configure from the TUI

Open the command palette with [[Ctrl+K]] and choose **Plugins**. The picker lists every configured server (from both scopes), its live connection state, and its tools — read from the same model the agent uses, so it's never stale. No restart is needed for anything you do here.

**What each row shows**

- A health badge: **●** connected · **◌** connecting · **⚠** error / needs attention · dim = disabled.
- A scope tag on non-user rows (`project · …`); your own user servers are unbadged.
- The tool count and, expanded beneath a connected server, each tool with its own on/off state (dimmed when the parent is off).

**Keys**

| Key | Action |
|-----|--------|
| [[↑]] [[↓]] | move between rows |
| [[a]] | **add** a plugin inline (spec formats below) |
| [[Enter]] | on a server: **enable / disable** · on a tool sub-row: **toggle that tool** · on an untrusted project server: **approve** it (*trust & enable*) |
| [[d]] | **remove** the server — press once to arm (row turns ⚠ *press d again to remove*), again to confirm |
| [[esc]] | close |

Every edit writes back to the **file the server actually came from** — toggling a project server edits the project `mcp.json`, a user server edits yours; the two never cross. A fresh inline add goes to your user config unless you pass `--project`.

**Adding inline** — press [[a]], type a `name`, then one recipe:

```text
weather --uvx mcp-weather           # PyPI package, via uv
today --python scripts/today.py     # a local Python script
playwright --npx @playwright/mcp    # an npm package (npx -y …)
docs --http https://mcp.acme.dev    # a remote HTTP server (or --sse)
mydb -- /usr/local/bin/db-mcp --port 5432   # any command, verbatim after --
```

Append `--project` to write the repo config. The server connects immediately.

The same operations work headless from a shell, for scripts / CI / SSH-without-TUI:

```bash
agentty plugin add weather --uvx mcp-weather
agentty plugin add docs --http https://mcp.acme.dev
agentty plugin list [--project]          # ✓ trusted · — pending approval
agentty plugin approve <name> --project  # per-server trust
agentty plugin remove <name>
```

The inline add covers the common shapes. For a server that needs **environment variables, HTTP headers, per-tool filters, or custom timeouts**, edit the file directly — next.

## Configure by editing `mcp.json`

Everything the TUI writes is plain JSON you can hand-edit; agentty picks up changes on the next connect (start a new session, or toggle the server in the picker). Edits are **round-trip safe** — keys agentty doesn't own, other servers, and the `servers` vs `mcpServers` spelling are all preserved byte-for-byte.

The top-level key is `mcpServers` (the `servers` spelling is also read). Each entry is one server, keyed by the name you'll see in tool names (`mcp__<name>__<tool>`).

### A stdio server (a local command)

```json
{
  "mcpServers": {
    "playwright": {
      "type": "stdio",
      "command": "npx",
      "args": ["-y", "@playwright/mcp"],
      "env": { "PLAYWRIGHT_BROWSERS_PATH": "/opt/ms-playwright" },
      "disabled": false,
      "timeoutMs": 30000,
      "connectTimeoutMs": 10000,
      "maxOutputChars": 30000,
      "trustAnnotations": false,
      "tools": {
        "include": ["browser_click", "browser_snapshot", "browser_navigate"],
        "exclude": ["browser_install"],
        "pin": ["browser_snapshot"]
      }
    }
  }
}
```

### An HTTP / SSE server (a remote url, no command)

```json
{
  "mcpServers": {
    "acme": {
      "type": "http",
      "url": "https://mcp.acme.dev/mcp",
      "headers": { "X-Tenant": "acme-prod" },
      "tools": { "exclude": ["delete_everything"] }
    }
  }
}
```

Use `"type": "sse"` for a Server-Sent-Events endpoint. HTTP/SSE servers spawn no local command, so they're never trust-gated. If the server needs OAuth rather than a static header, omit `headers` and authorize once with `agentty mcp-login acme` (see [Authorizing an OAuth-gated server](/docs/mcp#authorizing-an-oauth-gated-server)).

### Every field

| Field | Applies to | Effect |
|-------|-----------|--------|
| `type` | all | `stdio` (default when `command` is set), `http`, or `sse`. |
| `command` | stdio | the executable to spawn. |
| `args` | stdio | argument vector passed to `command`. |
| `env` | stdio | extra environment variables for the spawned process (merged over the inherited env) — the place for an API key or path a local server needs. |
| `url` | http/sse | the server endpoint. |
| `headers` | http/sse | extra HTTP headers on every request — e.g. a static bearer token or tenant id. |
| `disabled` | all | `true` keeps a server configured but off, without deleting it (what the picker's disable toggle writes). |
| `trustAnnotations` | all | default `false` — a server's read-only tool *hints* can't silently weaken your [permission checks](/docs/sandboxing); set `true` only for a server you trust to self-declare effects. |
| `tools.include` | all | allow-list: advertise only these tools (omit to advertise all). |
| `tools.exclude` | all | deny-list: hide these tools (applied after `include`). The picker's per-tool toggle edits this set. |
| `tools.pin` | all | keep these tools in every turn regardless of the 16-tool ranking (see [the tool budget](#the-tool-budget)). |
| `timeoutMs` | all | per-call deadline (default 30 s; override globally with `AGENTTY_MCP_TIMEOUT_MS`). |
| `connectTimeoutMs` | all | handshake / connect deadline. |
| `maxOutputChars` | all | cap a tool's output before it enters context. |

Unknown keys are ignored (and preserved), so a config shared with Claude Desktop / Cursor / VS Code works as-is — they read the same `mcpServers` shape. Connected tools carry stable provenance names (`mcp__playwright__browser_click`) that can't collide with or impersonate a native tool, and live `tools/list_changed` updates are honoured without a restart.

## Trust

A **project** `mcp.json` can arrive inside a repo you cloned, and an stdio server is a command agentty would spawn. So project stdio servers don't auto-connect — each is untrusted until you approve it, per-server, by content hash. Approve from the picker ([[Enter]] on the *trust & enable* row) or the CLI (`agentty plugin approve <name> --project`); edit the command afterwards and it re-gates. Your own `~/.agentty` servers and remote HTTP/SSE servers are never gated. Full rationale: [Plugin Trust](/docs/plugin-trust).

`AGENTTY_MCP_ALLOW_PROJECT=1` is the blanket alternative — it trusts every server in a project config at once (fine for a repo you fully control, not for a fresh clone).

## The tool budget

A model chooses worse when it's handed hundreds of tool schemas. agentty sends **all native and pinned tools plus at most 16 MCP tools**, ranked for the current request. The always-available `mcp_search_tools` / `mcp_call` broker exposes the long tail on demand, so a big plugin (Playwright alone has dozens of tools) never floods every turn. Use `pin` (in a server's `tools.pin`) to force an important tool into every turn regardless of ranking.

## Safety

- Plugin tools run under the same [permission profile](/docs/sandboxing) as native tools — a write-capable plugin tool still asks for approval on `Ask`/`Minimal`.
- A configured plugin is **never re-exported** when agentty runs as an MCP server itself (`agentty mcp-serve`), preventing credential leaks and recursive proxy loops.
- Independent servers run concurrently; a crashed server reconnects on its next call instead of wedging the session.

## Related

- [Plugin Trust](/docs/plugin-trust) — why project servers need approval, and how content-hash trust works.
- [Build a Plugin](/docs/build-a-plugin) — write your own MCP server (Python & C++ walkthroughs + the protocol).
- [MCP Server](/docs/mcp) — the full protocol: serving agentty's tools, resources, OAuth, ACP pass-through.
- [Retrieval](/docs/retrieval) — fold a plugin's MCP **resources** into `search_docs` with `AGENTTY_RAG_MCP=1`.
- [Sandboxing & permissions](/docs/sandboxing) — how tool calls are gated.
