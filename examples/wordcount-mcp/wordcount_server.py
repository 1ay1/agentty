#!/usr/bin/env python3
"""wordcount-mcp — a tiny Python MCP server for agentty (stdlib only).

An MCP server in ~120 lines of pure-stdlib Python. No pip install, no
framework — it speaks MCP's newline-delimited JSON-RPC over stdio directly,
so `python3 wordcount_server.py` is all it takes to run.

Tools:
  • word_count   — count words / lines / characters in a piece of text
  • reverse_text — reverse a string (handy "did the tool actually run?" check)

This is the Python counterpart to the C++ examples/date-mcp. See README.md
for how to add it to agentty.
"""

import json
import sys

# ── The tools this server exposes ────────────────────────────────────────────
# Each tool: a JSON Schema for its arguments + a Python function that runs it.
# Write the description for the MODEL — it's what it reads to decide when to
# call the tool.

TOOLS = [
    {
        "name": "word_count",
        "title": "Word count",
        "description": (
            "Count the words, lines, and characters in a piece of text. "
            "Call this when the user asks how long something is."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "the text to measure"}
            },
            "required": ["text"],
        },
    },
    {
        "name": "reverse_text",
        "title": "Reverse text",
        "description": "Reverse a string, character by character.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "the text to reverse"}
            },
            "required": ["text"],
        },
    },
]


def call_tool(name, args):
    """Run a tool and return its text result."""
    if name == "word_count":
        text = args.get("text", "")
        return (
            f"words: {len(text.split())}\n"
            f"lines: {len(text.splitlines()) or (1 if text else 0)}\n"
            f"characters: {len(text)}"
        )
    if name == "reverse_text":
        return args.get("text", "")[::-1]
    raise ValueError(f"unknown tool: {name}")


# ── MCP protocol plumbing (JSON-RPC 2.0 over stdio) ──────────────────────────
# You rarely need to touch this. It reads one JSON object per line from stdin
# and writes one JSON object per line to stdout. stderr is free for logging.

def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def reply(req_id, result):
    send({"jsonrpc": "2.0", "id": req_id, "result": result})


def error(req_id, code, message):
    send({"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}})


def handle(req):
    method = req.get("method")
    req_id = req.get("id")

    if method == "initialize":
        reply(req_id, {
            "protocolVersion": "2025-06-18",
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "wordcount", "title": "Word Count",
                           "version": "1.0.0"},
            "instructions": "Text measurement helpers (stdlib-only demo).",
        })
    elif method == "notifications/initialized":
        pass  # a notification — no id, no reply
    elif method == "tools/list":
        reply(req_id, {"tools": TOOLS})
    elif method == "tools/call":
        params = req.get("params", {})
        name = params.get("name", "")
        args = params.get("arguments", {})
        try:
            text = call_tool(name, args)
            reply(req_id, {"content": [{"type": "text", "text": text}]})
        except Exception as e:  # surface tool errors as an MCP error result
            reply(req_id, {
                "content": [{"type": "text", "text": f"error: {e}"}],
                "isError": True,
            })
    elif req_id is not None:
        error(req_id, -32601, f"method not found: {method}")
    # else: an unknown notification — ignore silently


def main():
    for line in sys.stdin:                 # one JSON-RPC message per line
        line = line.strip()
        if not line:
            continue
        try:
            handle(json.loads(line))
        except json.JSONDecodeError:
            continue                        # skip a malformed line, keep serving


if __name__ == "__main__":
    main()
