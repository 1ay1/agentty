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
agentty plugin list      # show configured plugins (--project marks ✓/— trust)
agentty plugin approve <name> --project   # trust a project server (per-server)
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

Open the command palette with [[Ctrl+K]] and choose **Plugins** to see every configured server, its connection state, and its tools. From there you can:

- **Add** a plugin inline — press [[a]] and type a `name command args…` spec (e.g. `playwright npx -y @playwright/mcp`); it's written to `mcp.json` and the server connects immediately, no restart.
- **Enable/disable individual tools** per server.
- **Approve** an untrusted project server ([[Enter]] on a *trust & enable* row) to let its config connect.
- **Remove** a plugin.

A warning appears if you've left an unusually large number of tools active — too many tool schemas dilute the model's tool choice (see the tool-budget note below). The `agentty plugin add` CLI does the same thing from a shell.

## Config scope

Plugins can be declared in two places, and agentty reads **both at once**:

- `~/.agentty/mcp.json` — **user** scope: your servers, on every project.
- `<project>/.agentty/mcp.json` — **project** scope: committed with the repo, shared with your team.

The **Plugins** picker shows servers from both, badged with the scope they came from (a project server reads `project · …`; user servers are unbadged, the common case). On a name collision, the project entry wins. `$AGENTTY_MCP_CONFIG` points at an explicit file that overrides both.

Every edit in the picker — enable/disable a server, toggle a tool, remove — writes back to the **file that server actually came from**, so toggling a project server edits the project `mcp.json` and a user server edits yours; the two never cross. Adding a new plugin inline writes to your user config by default.

:::note
Project-scoped **stdio** servers spawn a command from a file that rode in with the repo, so they don't auto-connect until you vouch for them. Trust is **per-server**: in the **Plugins** picker each untrusted project server is labelled *untrusted project config — approve to enable*; press [[Enter]] on it (*trust & enable*) to approve **that one server** and connect it. Approving one server doesn't trust a different one, so a repo can't smuggle a new server in behind one you already approved. (Headless? `agentty plugin approve <name> --project` does the same, and `agentty plugin list --project` shows each server's ✓/— trust state.) Approvals are recorded under `~/.agentty` (a clone can't approve itself), keyed by the server's exact command + args — edit them and that server re-gates, so a swapped command can never ride in on an old approval. `AGENTTY_MCP_ALLOW_PROJECT=1` still blanket-trusts a whole project config. HTTP/SSE servers spawn no local command and aren't gated this way.
:::

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

- [Plugin Trust](/docs/plugin-trust) — why project servers need approval, and how content-hash trust works.

- [Build a Plugin](/docs/build-a-plugin) — write your own MCP server (Python & C++ walkthroughs + the protocol).
- [MCP Server](/docs/mcp) — the full protocol: serving agentty's tools, resources, OAuth, ACP pass-through.
- [Retrieval](/docs/retrieval) — fold a plugin's MCP **resources** into `search_docs` with `AGENTTY_RAG_MCP=1`.
- [Sandboxing & permissions](/docs/sandboxing) — how tool calls are gated.
