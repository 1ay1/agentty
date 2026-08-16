---
title: Build a Plugin
description: Write your own MCP server to give agentty new tools — a browser driver, a database client, an internal API. A complete walkthrough in Python (no build) and C++ (with mcp-cpp), plus the protocol under the hood.
nav_section: Advanced
nav_order: 36
slug: build-a-plugin
---

A [plugin](/docs/plugins) is an [MCP](https://modelcontextprotocol.io) server —
a small program that advertises **tools** the model can call. agentty spawns it,
lists its tools, and calls them on the model's behalf. This page shows you how
to write one from scratch: first the fast path in Python, then a native C++
server, then what's actually happening on the wire.

Two runnable examples ship in the mcp-cpp repo and are referenced throughout:

- **[`wordcount-mcp`](https://github.com/1ay1/mcp-cpp/tree/master/examples/wordcount-mcp)** — ~120 lines of pure-stdlib **Python**, no build.
- **[`date-mcp`](https://github.com/1ay1/mcp-cpp/tree/master/examples/date-mcp)** — ~130 lines of **C++** built with mcp-cpp.

:::tip
You do **not** need to write a server for most things — the [MCP ecosystem](https://github.com/modelcontextprotocol/servers) has hundreds ready to add (Playwright, Postgres, GitHub, filesystem, …). Write your own when you have an internal API or a bespoke tool the model should reach.
:::

## What a plugin is, minimally

An MCP server does exactly three things over its stdin/stdout:

1. **Handshake** — respond to `initialize` announcing your name and that you
   have tools.
2. **List** — respond to `tools/list` with each tool's name, description, and a
   JSON Schema for its arguments.
3. **Call** — respond to `tools/call` by running the named tool and returning a
   text result.

That's it. It's newline-delimited JSON-RPC 2.0 on stdio — one JSON object per
line in, one per line out. `stderr` is yours for logging (agentty routes it to
`~/.agentty/stderr.log`, never the screen).

## The fast path: Python, no dependencies

You can write a complete server with the standard library — no `pip install`, no
framework. Here is the shape (the full file is `wordcount-mcp/wordcount_server.py`):

```python
import json, sys

TOOLS = [{
    "name": "word_count",
    "description": "Count words, lines, and characters in a piece of text.",
    "inputSchema": {
        "type": "object",
        "properties": {"text": {"type": "string", "description": "text to measure"}},
        "required": ["text"],
    },
}]

def call_tool(name, args):
    if name == "word_count":
        t = args.get("text", "")
        return f"words: {len(t.split())}\nlines: {len(t.splitlines())}\nchars: {len(t)}"
    raise ValueError(f"unknown tool: {name}")

def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n"); sys.stdout.flush()

for line in sys.stdin:                          # one JSON-RPC message per line
    req = json.loads(line)
    m, rid = req.get("method"), req.get("id")
    if m == "initialize":
        send({"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": "2025-06-18",
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "wordcount", "version": "1.0.0"}}})
    elif m == "tools/list":
        send({"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}})
    elif m == "tools/call":
        p = req["params"]
        text = call_tool(p["name"], p.get("arguments", {}))
        send({"jsonrpc": "2.0", "id": rid,
              "result": {"content": [{"type": "text", "text": text}]}})
```

Adding a tool is three steps: append to `TOOLS`, handle it in `call_tool`, done.

**Add it:**

```bash
agentty plugin add wordcount --python "/abs/path/to/wordcount_server.py"
```

`--python` is a recipe — it writes `command: python3, args: [<abs-path>]` into
`~/.agentty/mcp.json`. The server connects immediately; in `Ctrl+K → Plugins` it
shows **● wordcount · N tools active**, and the tools reach the model as
`mcp__wordcount__word_count`.

For a richer Python server (typed outputs, resources, prompts, HTTP transport)
use the official [Python SDK](https://github.com/modelcontextprotocol/python-sdk)
— but for a tool or two, stdlib is plenty.

## Native C++ with mcp-cpp

agentty's own tools are built on [mcp-cpp](https://github.com/1ay1/mcp-cpp), a
header-only C++ MCP library — so a C++ server is a natural fit when you want
speed or to reuse existing C++ code. The whole server is a few `register_tool`
calls (see `date-mcp/date_server.cpp`):

```cpp
#include <mcp/mcp.hpp>
using namespace mcp;

int main() {
    Server server{{.name = "date", .version = "1.0.0"}};

    Tool t;
    t.name = "current_date";
    t.description = "Today's date and time. Call for anything date-sensitive.";
    t.inputSchema = { {"type", "object"}, {"properties", Json::object()} };
    server.register_tool(std::move(t), [](const Json&) -> CallToolResult {
        std::time_t now = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC",
                      std::gmtime(&now));
        CallToolResult r;
        r.content.push_back(TextContent{buf});
        return r;
    });

    StdioTransport transport;
    transport.start(server.engine());   // serve over stdin/stdout
    transport.join();                   // until the client closes stdin
}
```

**Build** — it builds with the mcp-cpp examples, or standalone via
`FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(mcp
    GIT_REPOSITORY https://github.com/1ay1/mcp-cpp.git
    GIT_TAG        master)
FetchContent_MakeAvailable(mcp)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

**Add the built binary** (it's a plain executable, so the no-recipe form):

```bash
agentty plugin add date -- "/abs/path/to/date_server"
```

## What the recipe writes

Both flows land in `~/.agentty/mcp.json` — the standard [`mcpServers`](https://modelcontextprotocol.io) format every MCP client understands:

```json
{
  "mcpServers": {
    "wordcount": { "type": "stdio", "command": "python3",
                   "args": ["/abs/path/wordcount_server.py"] },
    "date":      { "type": "stdio", "command": "/abs/path/date_server" }
  }
}
```

You can hand-edit this file directly — agentty preserves every key it doesn't
own, writes atomically (a crash never truncates it), and refuses to touch a file
it can't parse (so a typo won't destroy your config). See
[Plugins](/docs/plugins) for per-server policy (`disabled`, `tools.include`/
`exclude`, timeouts, `trustAnnotations`) and HTTP/OAuth servers.

## The protocol, in one exchange

If you want to *see* it, drive any server by hand — it's just lines of JSON:

```bash
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli","version":"1"}}}' \
'{"jsonrpc":"2.0","method":"notifications/initialized"}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"word_count","arguments":{"text":"one two three"}}}' \
| python3 wordcount_server.py
```

The four exchanges, in order:

| # | client → server | server → client |
|---|-----------------|-----------------|
| 1 | `initialize` | name, version, `capabilities.tools` |
| — | `notifications/initialized` (no id) | *(nothing — it's a notification)* |
| 2 | `tools/list` | the `tools` array (name + schema) |
| 3 | `tools/call` {name, arguments} | `content: [{type:"text", text:"…"}]` |

agentty does exactly this on connect (1 + 2), caches the tool list, and sends
one `tools/call` (3) each time the model invokes your tool. A tool that fails
should return `isError: true` in its result (not crash) — agentty surfaces that
to the model as a tool error it can react to.

## Design tips

- **Write the `description` for the model, not for humans.** It's the only thing
  the model reads to decide *when* to call your tool. "Count words in text" is
  better than "wc wrapper".
- **Keep tools focused.** One clear job per tool beats a mega-tool with a `mode`
  argument — the model routes better.
- **Return text the model can use.** Structured, labelled output ("words: 5")
  reads better than a bare number.
- **Don't flood the tool list.** agentty ships native tools + at most 16 MCP
  tools per turn (ranked), with a broker for the long tail — but a lean server
  is still easier for the model to use. See the [tool budget](/docs/plugins#the-tool-budget).
- **Log to stderr, never stdout.** stdout is the JSON-RPC channel; a stray
  `print` there corrupts the protocol. (agentty captures stderr to
  `~/.agentty/stderr.log`.)

## Related

- [Plugins](/docs/plugins) — adding, toggling, and configuring servers.
- [MCP Server](/docs/mcp) — agentty as an MCP server (the other direction), OAuth, resources.
- [Tools](/docs/tools) — the native toolset your plugin sits alongside.
