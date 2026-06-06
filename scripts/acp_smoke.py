#!/usr/bin/env python3
# Minimal ACP client harness to exercise agentty's `acp` subcommand:
# drives initialize/new/prompt and auto-answers session/request_permission.
import json, subprocess, sys, threading

proc = subprocess.Popen(
    ["./build/agentty", "acp"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr, text=True, bufsize=1,
)

lock = threading.Lock()
def send(obj):
    with lock:
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()

done = threading.Event()

def reader():
    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        msg = json.loads(line)
        print("<<", json.dumps(msg)[:300])
        # Auto-answer permission requests.
        if msg.get("method") == "session/request_permission":
            send({"jsonrpc":"2.0","id":msg["id"],
                  "result":{"outcome":{"outcome":"selected","optionId":"allow_once"}}})
        # Final prompt response → done.
        if msg.get("id") == 3 and "result" in msg:
            done.set()

t = threading.Thread(target=reader, daemon=True); t.start()

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{}}})
send({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":".","mcpServers":[]}})
prompt = sys.argv[1] if len(sys.argv) > 1 else "Use the list_dir tool on '.' and tell me how many entries there are."
send({"jsonrpc":"2.0","id":3,"method":"session/prompt",
      "params":{"sessionId":"sess-1","prompt":[{"type":"text","text":prompt}]}})

done.wait(timeout=90)
proc.stdin.close()
proc.terminate()
