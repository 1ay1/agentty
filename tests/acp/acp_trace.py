#!/usr/bin/env python3
"""Print every message for a few prompts, for poking at `agentty acp` by hand.

usage: acp_trace.py <agentty> <cwd> <prompt>...   (AGENTTY_HOME from env)
"""
import json
import os
import subprocess
import sys

agentty, cwd, prompts = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3:]
p = subprocess.Popen([agentty, "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, cwd=cwd)


def send(o):
    p.stdin.write((json.dumps(o) + "\n").encode())
    p.stdin.flush()


def until(i):
    while True:
        line = p.stdout.readline()
        if not line:
            sys.exit("agent exited")
        m = json.loads(line)
        if m.get("method") == "session/update":
            u = m["params"]["update"]
            print("  update", u.get("sessionUpdate"), json.dumps(u)[:160])
        elif m.get("method"):
            print("  request", m["method"])
            send({"jsonrpc": "2.0", "id": m["id"], "error": {"code": -32601, "message": "no"}})
        elif m.get("id") == i:
            return m


caps = {"session": {"compaction": {}, "notices": {}, "configOptions": {"boolean": {}}}}
send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"protocolVersion": 1, "clientCapabilities": caps}})
until(1)
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {"cwd": cwd, "mcpServers": []}})
sid = until(2)["result"]["sessionId"]
for i, t in enumerate(prompts):
    send({"jsonrpc": "2.0", "id": 10 + i, "method": "session/prompt",
          "params": {"sessionId": sid, "prompt": [{"type": "text", "text": t}]}})
    print(repr(t), "->", json.dumps(until(10 + i))[:200])
p.kill()
