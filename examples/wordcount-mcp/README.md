# wordcount-mcp — a tiny Python MCP plugin for agentty

An MCP server in ~120 lines of **pure-stdlib Python** — no `pip install`, no
framework. It speaks MCP's newline-delimited JSON-RPC over stdio directly, so
running it is just `python3 wordcount_server.py`.

Tools:

| tool | what it does |
|------|--------------|
| `word_count` | words / lines / characters in a piece of text |
| `reverse_text` | reverse a string (a quick "did the tool run?" check) |

This is the Python counterpart to the C++ [`examples/date-mcp`](../date-mcp).
There's nothing to build.

## Requirements

Just Python 3 (`python3 --version` should print 3.8+). Nothing else.

## Add it to agentty

The server's absolute path is `<this-dir>/wordcount_server.py`. From this
directory:

```bash
agentty plugin add wordcount --python "$(pwd)/wordcount_server.py"
```

`--python` is a recipe: it writes `command: python3`, `args: [<abs-path>]` into
`~/.agentty/mcp.json` and resolves the path to absolute so it works no matter
where agentty is launched from.

You can also add it **straight from the TUI**: `Ctrl+K → Plugins → a`, then
type (use an absolute path):

```
wordcount --python /absolute/path/to/wordcount_server.py
```

Either way it connects immediately (no restart). In the Plugins panel it shows
as **● wordcount · 2 of 2 tools active**. Its tools appear to the model as
`mcp__wordcount__word_count` and `mcp__wordcount__reverse_text`.

Then ask agentty *"how many words are in this paragraph?"* or *"reverse the
word agentty"* and watch it call the tool.

### What the recipe wrote

`~/.agentty/mcp.json` now contains:

```json
{
  "mcpServers": {
    "wordcount": {
      "type": "stdio",
      "command": "python3",
      "args": ["/absolute/path/to/wordcount_server.py"]
    }
  }
}
```

## Managing it

- **Toggle on/off** — highlight the row in `Ctrl+K → Plugins` and press
  **Enter** (writes `"disabled": true`, reversible).
- **Remove** — press **d** on the row (deletes it from `mcp.json`).
- **Disable one tool** — expand the plugin and toggle a single tool.
- **From a shell** — `agentty plugin list`, `agentty plugin remove wordcount`.

## Verify without agentty

MCP is newline-delimited JSON-RPC on stdio, so you can drive it by hand:

```bash
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli","version":"1"}}}' \
'{"jsonrpc":"2.0","method":"notifications/initialized"}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"word_count","arguments":{"text":"one two three"}}}' \
| python3 wordcount_server.py
```

You should see the `initialize` reply, the two tools listed, and
`words: 3 · lines: 1 · characters: 13`.

## Make it your own

Add a tool in three steps:

1. Append an entry to `TOOLS` (name, description **written for the model**, and
   an `inputSchema` describing its arguments).
2. Handle it in `call_tool(name, args)` — return the text the model should see.
3. Save. Re-toggle the plugin in the Plugins panel (or restart) to reconnect.

The MCP plumbing below `call_tool` rarely needs changing. For a richer server
(typed outputs, resources, prompts, HTTP transport), see the official
[Python SDK](https://github.com/modelcontextprotocol/python-sdk) — but for a
tool or two, stdlib like this is plenty. See [`docs/PLUGINS.md`](../../docs/PLUGINS.md)
for the full plugin guide.
