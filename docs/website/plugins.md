---
title: Plugins (MCP servers)
description: Extend agentty with external tools by adding MCP servers — a browser driver, a database client, a hosted API — from a single mcp.json or the agentty plugin CLI.
nav_section: Advanced
nav_order: 35
slug: plugins
---

A **plugin** is an external [MCP](https://modelcontextprotocol.io) server that adds tools to agentty. Point agentty at a Playwright server and the model can drive a browser; add a Postgres server and it can query your database. Plugins are how you extend the agent's capabilities beyond its native toolset without recompiling anything.

Under the hood a plugin *is* an MCP server — this page is the practical "add a tool to my agent" guide; [MCP Server](/docs/mcp) covers the full protocol, OAuth, and serving agentty's own tools the other way.

## Adding a plugin

Two ways, same result — an entry in `mcp.json`:

**From the CLI:**

```bash
agentty plugin add       # interactive: name + command/url
agentty plugin list      # show configured plugins
agentty plugin remove <name>
```

**By hand** — drop a `.agentty/mcp.json` in your project (or `~/.agentty/mcp.json` for all projects):

```json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp"]
    }
  }
}
```

agentty connects on startup, and the server's tools become available in the thread with stable provenance names like `mcp__playwright__browser_click` — they can never collide with or impersonate a native tool. Live `tools/list_changed` updates (including removals) are honoured without a restart.

:::note
Plugins are lazy and opt-in. With no `mcp.json` present, startup is a single `stat()` that finds nothing — zero overhead when you aren't using any.
:::

## Managing plugins in the app

Open the command palette with [[Ctrl+K]] and choose **Plugins** to see every configured server, its connection state, and its tools. From there you can **enable/disable individual tools** per server and **remove** a plugin. A warning appears if you've left an unusually large number of tools active — too many tool schemas dilute the model's tool choice (see the tool-budget note below).

To add a new one, use `agentty plugin add` from a shell (authoring is deliberately a terminal action).

## The tool budget

A model chooses worse when it's handed hundreds of tool schemas. agentty sends **all native and pinned tools plus at most 16 MCP tools**, ranked for the current request. The always-available `mcp_search_tools` / `mcp_call` broker exposes the long tail on demand, so a big plugin (Playwright alone has dozens of tools) never floods every turn. Use `pin` to force an important tool into every turn regardless of ranking.

## Per-plugin policy

Every server entry accepts explicit policy:

```json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp"],
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

| Field | Effect |
|-------|--------|
| `disabled` | keep a server configured but off, without deleting it |
| `trustAnnotations` | default `false` — a remote server's read-only hints can't silently weaken your [permission checks](/docs/sandboxing); enable only for a server you trust |
| `tools.include` / `exclude` | filter which of the server's tools are advertised |
| `tools.pin` | keep a tool in every turn regardless of the 16-tool ranking |
| `timeoutMs` / `connectTimeoutMs` | per-call and connect deadlines |
| `maxOutputChars` | cap a tool's output before it enters context |

## HTTP & OAuth plugins

A plugin can be a hosted HTTP server, not just a local command:

```json
{ "mcpServers": { "acme": { "url": "https://mcp.acme.dev/mcp" } } }
```

If the server requires OAuth, authorize once with `agentty mcp-login acme` — agentty runs the full OAuth 2.1 + PKCE flow, stores the token encrypted, and refreshes it transparently. See [MCP Server → Authorizing an OAuth-gated server](/docs/mcp#authorizing-an-oauth-gated-server) for the details, including servers without dynamic registration.

## Safety

- Plugin tools run under the same [permission profile](/docs/sandboxing) as native tools — a write-capable plugin tool still asks for approval on `Ask`/`Minimal`.
- A configured plugin is **never re-exported** when agentty runs as an MCP server itself (`agentty mcp-serve`), preventing credential leaks and recursive proxy loops.
- Independent servers run concurrently; a crashed server reconnects on its next call instead of wedging the session.

## Related

- [MCP Server](/docs/mcp) — the full protocol: serving agentty's tools, resources, OAuth, ACP pass-through.
- [Retrieval](/docs/retrieval) — fold a plugin's MCP **resources** into `search_docs` with `AGENTTY_RAG_MCP=1`.
- [Sandboxing & permissions](/docs/sandboxing) — how tool calls are gated.
