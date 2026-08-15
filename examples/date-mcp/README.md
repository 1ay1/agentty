# date-mcp — a tiny date/time plugin for agentty

An MCP server in ~130 lines of C++ (built with agentty's own
[mcp-cpp](../../mcp-cpp)). It gives the model something it can't know on
its own — the current date/time — plus a small date-math helper.

Tools:

| tool | what it does |
|------|--------------|
| `current_date` | today's date & time (UTC + local + weekday) |
| `days_between` | signed whole days from date `a` to date `b` (YYYY-MM-DD) |

## Build

From this directory:

```bash
cmake -B build
cmake --build build -j8 --target date_server
# → build/date_server
```

(No external deps — mcp-cpp is header-only and picked up from the
submodule in this repo.)

## Add it to agentty

```bash
agentty plugin add date -- "$(pwd)/build/date_server"
```

That writes a `date` server into `~/.agentty/mcp.json`. With live reload
it connects immediately; otherwise restart agentty. Its tools appear as
`mcp__date__current_date` and `mcp__date__days_between`.

You can also add it straight from the TUI: **Ctrl+K → Plugins → `a`**,
then type:

```
date -- /absolute/path/to/build/date_server
```

Then ask agentty *"what day is it?"* or *"how many days until 2026-12-25?"*
and watch it call the tool.

## Verify without agentty

MCP is newline-delimited JSON-RPC on stdio, so you can drive it by hand:

```bash
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli","version":"1"}}}' \
'{"jsonrpc":"2.0","method":"notifications/initialized"}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"current_date","arguments":{}}}' \
| ./build/date_server
```

## Use it as a template

`date_server.cpp` is the whole thing. To make your own plugin: copy it,
`register_tool(spec, lambda)` for each tool (the docstring/`description`
is what the model reads — write it for the model; the `inputSchema` is
its typed arguments), and keep `transport.start(server.engine());
transport.join();` at the end to serve until agentty closes stdin.

To build **outside** this repo, replace the `add_subdirectory` in
`CMakeLists.txt` with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(mcp
    GIT_REPOSITORY https://github.com/1ay1/mcp-cpp.git
    GIT_TAG        master)
FetchContent_MakeAvailable(mcp)
```

See [`docs/PLUGINS.md`](../../docs/PLUGINS.md) for the full plugin guide
(Python, Node, and C++ examples).
