---
title: MCP Server
description: Serve agentty's native tools over the Model Context Protocol, and consume other MCP servers from inside agentty.
nav_section: Advanced
nav_order: 30
slug: mcp
---

agentty speaks the [Model Context Protocol](https://modelcontextprotocol.io) both ways: it can **serve** its native tools to any MCP client, and **consume** tools from other MCP servers inside a thread.

## Serving agentty's tools (mcp-serve)

`agentty mcp-serve` runs headless — no terminal UI — and exposes agentty's native toolset over MCP on stdio. An external MCP client (Claude Desktop, an IDE, another agent) drives `tools/list` and `tools/call` over stdin/stdout; diagnostics go to stderr.

```bash
agentty mcp-serve
```

The served tools are the same ones the TUI uses: file `read`/`write`/`edit`, shell `bash`, code search (`grep`/`glob`/`find_definition`), `web_fetch`/`web_search`, `diagnostics`, and the `git_*` family. Filesystem tools stay sandboxed to the [workspace boundary](/docs/workspace) and shell calls run inside the [OS sandbox](/docs/sandboxing), exactly as they do interactively.

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

The reverse also works: drop a `.agentty/mcp.json` in your project and agentty connects to those servers on startup, appending their tools to its own registry. The model can't tell an MCP tool from a native one — and `tools/list_changed` is honoured live, so a server that adds a tool mid-session becomes callable on the next turn.

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

:::note
MCP consumption is lazy and opt-in — with no `.agentty/mcp.json` present, startup is a single `stat()` that returns nothing, so there is zero overhead when you aren't using it.
:::

## Searching an MCP server's resources

Beyond tools, an MCP server can expose **resources** (`resources/*`) — documents, wiki pages, reference material. agentty can fold those into its [retrieval engine](/docs/retrieval) so `search_docs` searches them alongside your local docs, skills, and memory, all fused into one ranked, source-tagged result set. It's off by default; enable with `AGENTTY_RAG_MCP=1` (requires an MCP config to be present). From the model's view a docs folder and an MCP server are the same thing — a knowledge source behind one interface.
