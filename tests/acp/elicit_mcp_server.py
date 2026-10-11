#!/usr/bin/env python3
"""Minimal MCP stdio server whose one tool asks the user a question.

tools/call "ask_name" sends elicitation/create (form mode) to the client and
returns "hello <name>" from the answer, or "declined"/"cancelled".
"""
import json
import sys

next_id = [1000]


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def read():
    line = sys.stdin.readline()
    return json.loads(line) if line else None


def call_client(method, params):
    next_id[0] += 1
    rid = next_id[0]
    send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    while True:
        m = read()
        if m is None:
            return None
        if m.get("id") == rid and "method" not in m:
            return m.get("result")


TOOL = {"name": "ask_name", "description": "Ask the user their name.",
        "inputSchema": {"type": "object", "properties": {}}}
LINK = {"name": "open_link", "description": "Send the user to a sign-in page.",
        "inputSchema": {"type": "object", "properties": {}}}
url_ok = [False]   # the client advertised elicitation.url

while True:
    m = read()
    if m is None:
        break
    meth, rid = m.get("method"), m.get("id")
    if meth == "initialize":
        el = ((m["params"].get("capabilities") or {}).get("elicitation") or {})
        url_ok[0] = isinstance(el.get("url"), dict)
        send({"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": m["params"].get("protocolVersion", "2025-06-18"),
            "capabilities": {"tools": {}, "prompts": {}},
            "serverInfo": {"name": "elicit-test", "version": "1"}}})
    elif meth == "prompts/list":
        send({"jsonrpc": "2.0", "id": rid, "result": {"prompts": [{
            "name": "greet", "description": "Greet someone",
            "arguments": [{"name": "who", "required": True}]}]}})
    elif meth == "prompts/get":
        who = (m["params"].get("arguments") or {}).get("who", "?")
        send({"jsonrpc": "2.0", "id": rid, "result": {"messages": [
            {"role": "user", "content": {"type": "text", "text": f"PROMPT-OK greet {who}"}}]}})
    elif meth == "tools/list":
        send({"jsonrpc": "2.0", "id": rid, "result": {"tools": [TOOL, LINK]}})
    elif meth == "tools/call" and m["params"].get("name") == "open_link":
        if not url_ok[0]:
            text = "no-url-support"
        else:
            res = call_client("elicitation/create", {
                "mode": "url", "message": "Sign in to continue",
                "elicitationId": "el-1", "url": "https://example.com/connect?e=el-1"})
            text = "url " + (res or {}).get("action", "none")
            send({"jsonrpc": "2.0", "method": "notifications/elicitation/complete",
                  "params": {"elicitationId": "el-1"}})
        send({"jsonrpc": "2.0", "id": rid, "result": {"content": [{"type": "text", "text": text}]}})
    elif meth == "tools/call":
        res = call_client("elicitation/create", {
            "mode": "form", "message": "What is your name?",
            "requestedSchema": {"type": "object",
                                "properties": {"name": {"type": "string", "title": "Name"}},
                                "required": ["name"]}})
        action = (res or {}).get("action", "none")
        text = f"hello {res['content']['name']}" if action == "accept" else action
        send({"jsonrpc": "2.0", "id": rid, "result": {"content": [{"type": "text", "text": text}]}})
    elif rid is not None:
        send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown"}})
