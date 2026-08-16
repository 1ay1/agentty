---
title: MCP Server
description: Serve agentty's native tools over the Model Context Protocol, and consume other MCP servers from inside agentty.
nav_section: Advanced
nav_order: 30
slug: mcp
---

agentty speaks the [Model Context Protocol](https://modelcontextprotocol.io) both ways: it can **serve** its native tools to any MCP client, and **consume** tools from other MCP servers inside a thread.

:::note
Just want to add a tool (a browser driver, a database client)? See **[Plugins](/docs/plugins)** for the practical `mcp.json` / `agentty plugin add` guide. This page is the full protocol reference.
:::

## Serving agentty's tools (mcp-serve)

`agentty mcp-serve` runs headless — no terminal UI — and exposes agentty's native toolset over MCP on stdio. An external MCP client (Claude Desktop, an IDE, another agent) drives `tools/list` and `tools/call` over stdin/stdout; diagnostics go to stderr.

```bash
agentty mcp-serve
```

The served tools are the same native tools the TUI uses: file `read`/`write`/`edit`/`move`/`remove`, shell `bash`, long-running `process_*` sessions, focused `test`, code search (`grep`/`glob`/`find_definition`/`find_references`), web fetch/search, diagnostics, and the `git_*` family including `git_show` and `git_blame`. Filesystem tools stay sandboxed to the workspace boundary and shell/process calls run inside the OS sandbox, exactly as they do interactively.

`mcp-serve` is deliberately **native-only**. A configured external MCP server is never re-exported implicitly, preventing credential leaks and recursive MCP proxy loops.

## Point a client at it

Any MCP client can launch agentty as a stdio server. For a client that reads a JSON config (Claude Desktop shown here):

```json
{
  "mcpServers": {
    "agentty": {
      "command": "agentty",
      "args": ["mcp-serve"]
    }
  }
}
```

## Consuming other MCP servers

The reverse also works: drop a `.agentty/mcp.json` in your project and agentty connects to those servers on startup. External tools receive stable provenance names such as `mcp__playwright__browser_click`; they can never collide with or impersonate native tools. `tools/list_changed` is honoured live, including removals and schema replacements.

To keep provider requests fast and tool choice accurate, agentty sends all native and pinned tools plus at most 16 MCP tools ranked for the current user request. The always-available `mcp_search_tools` and `mcp_call` broker exposes the long tail without injecting hundreds of schemas into every turn.

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

Per-server policy is explicit:

```json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp"],
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

- `disabled:true` keeps a configured server off without deleting it.
- `trustAnnotations` defaults to `false`: remote read-only hints cannot silently weaken permission checks. Enable it only for a server you trust.
- `include`/`exclude` filter the advertised surface; `pin` keeps important tools in every turn.
- Stdio and HTTP servers receive the workspace through MCP roots.
- Calls stream MCP progress into the live tool card, propagate Esc cancellation, and reconnect a poisoned/crashed server on its next call.
- Independent servers execute concurrently; each individual transport retains its own serialization.

:::note
MCP consumption is lazy and opt-in — with no `.agentty/mcp.json` present, startup is a single `stat()` that returns nothing, so there is zero overhead when you aren't using it.
:::

## Authorizing an OAuth-gated server

Some hosted MCP servers require OAuth (MCP spec **2026-07-28** authorization). When you run agentty against one, an unauthorized call returns an actionable error instead of hanging. Authorize once with:

```bash
agentty mcp-login <server>
```

This runs the full OAuth 2.1 + PKCE flow: agentty discovers the authorization server from the endpoint's protected-resource metadata (RFC 9728), dynamically registers itself (`application_type=native`, so the loopback redirect is accepted), opens your browser to authorize, catches the redirect on a local loopback port, and validates the response — including the **RFC 9207 `iss` check** that blocks authorization-server mix-up attacks *before* the code is exchanged.

### When the server doesn't support Dynamic Client Registration

Some authorization servers don't offer a registration endpoint. Give agentty a `client_id` instead — an `https://` value is used as a **CIMD** (Client ID Metadata Document) URL, the 2026-07-28 preferred path where the URL *is* the client identity; anything else is treated as a pre-registered public client. In precedence order:

```bash
agentty mcp-login acme --client-id https://agentty.example/client.json   # CIMD URL
agentty mcp-login acme --client-id my-registered-client-id               # pre-registered id
```

…or set it per-server in `mcp.json` (`"client_id"`) or globally via `AGENTTY_MCP_CLIENT_ID`:

```json
{ "mcpServers": { "acme": { "url": "https://mcp.acme.dev/mcp",
    "client_id": "https://agentty.example/client.json" } } }
```

If the discovery probe can't find the metadata URL from the 401 challenge, pass it explicitly with `--metadata <url>`.

The resulting token is **issuer-bound** (it will never be replayed to a different authorization server), stored encrypted at rest (`~/.agentty/mcp_tokens/<server>.json`, `chmod 600`), attached automatically to every request to that server, and **refreshed transparently** when it expires. Manage tokens with:

```bash
agentty mcp-status          # list servers and which are authorized
agentty mcp-logout <server> # forget a stored token
```

A statically-configured `Authorization` header in `mcp.json` still wins, so you can also just paste a bearer token if you have one:

```json
{ "mcpServers": { "acme": { "url": "https://mcp.acme.dev/mcp",
    "headers": { "Authorization": "Bearer sk-..." } } } }
```

## External ACP agents

When an external ACP agent is selected, trusted servers from the same MCP configuration are passed through `session/new.mcpServers`. The delegated agent owns those calls and agentty renders their ACP tool updates as observed activity; it does not execute them a second time. Workspace-local MCP configuration still requires `AGENTTY_MCP_ALLOW_PROJECT=1`.

Agentty does not silently inject a nested unrestricted `agentty mcp-serve` into delegated agents. That would duplicate built-ins, bypass clear execution ownership, and make recursive `task` flows possible.

## Searching an MCP server's resources

Beyond tools, an MCP server can expose **resources** (`resources/*`) — documents, wiki pages, reference material. agentty can fold those into its [retrieval engine](/docs/retrieval) so `search_docs` searches them alongside your local docs, skills, and memory, all fused into one ranked, source-tagged result set. It's off by default; enable with `AGENTTY_RAG_MCP=1` (requires an MCP config to be present). From the model's view a docs folder and an MCP server are the same thing — a knowledge source behind one interface.
