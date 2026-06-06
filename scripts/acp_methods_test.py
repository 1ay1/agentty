#!/usr/bin/env python3
# Protocol-surface test for agentty's ACP agent. Exercises every method that
# does NOT require the model (so it runs offline/in CI): initialize, the full
# session lifecycle (new/list/load/resume/close/delete), set_mode,
# set_config_option, and logout. Asserts the responses match the ACP v1 shapes.
#
# session/prompt is covered separately by acp_smoke.py (needs a live model).
import json, subprocess, sys, threading, queue, os, tempfile

# Run in a throwaway HOME so we don't touch the user's real thread store and
# logout doesn't nuke real credentials.
env = dict(os.environ)
env["HOME"] = tempfile.mkdtemp(prefix="acp_test_home_")

proc = subprocess.Popen(
    ["./build/agentty", "acp"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
    text=True, bufsize=1, env=env,
)

resp = {}            # id -> result/error
cv = threading.Condition()
notifications = []

def send(obj):
    proc.stdin.write(json.dumps(obj) + "\n"); proc.stdin.flush()

def reader():
    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        msg = json.loads(line)
        if "method" in msg and "id" not in msg:
            notifications.append(msg)
            continue
        if "id" in msg and ("result" in msg or "error" in msg):
            with cv:
                resp[msg["id"]] = msg
                cv.notify_all()

threading.Thread(target=reader, daemon=True).start()

_nid = [0]
def call(method, params=None):
    _nid[0] += 1
    i = _nid[0]
    send({"jsonrpc":"2.0","id":i,"method":method,"params":params or {}})
    with cv:
        cv.wait_for(lambda: i in resp, timeout=10)
    if i not in resp:
        raise SystemExit(f"FAIL: no response to {method}")
    return resp[i]

fails = []
def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails.append(msg)

# ── initialize ───────────────────────────────────────────────────────
r = call("initialize", {"protocolVersion":1,"clientCapabilities":{}})["result"]
check(r.get("protocolVersion") == 1, "initialize → protocolVersion 1")
caps = r.get("agentCapabilities", {})
check(caps.get("loadSession") is True, "caps.loadSession")
sc = caps.get("sessionCapabilities", {})
check(all(k in sc for k in ("list","resume","close","delete")),
      "caps.sessionCapabilities {list,resume,close,delete}")
check("logout" in caps.get("auth", {}), "caps.auth.logout")

# ── session/new → modes ──────────────────────────────────────────────
cwd = env["HOME"]
r = call("session/new", {"cwd":cwd,"mcpServers":[]})["result"]
sid = r["sessionId"]
check(bool(sid), "session/new → sessionId")
modes = r.get("modes", {})
ids = {m["id"] for m in modes.get("availableModes", [])}
check(ids == {"ask","write","minimal"}, "session/new → modes {ask,write,minimal}")
check(modes.get("currentModeId") in ids, "session/new → valid currentModeId")

# ── session/set_mode ─────────────────────────────────────────────────
r = call("session/set_mode", {"sessionId":sid,"modeId":"write"})
check("result" in r, "session/set_mode → ok")
got_mode_update = any(
    n.get("method")=="session/update" and
    n["params"].get("update",{}).get("sessionUpdate")=="current_mode_update" and
    n["params"]["update"].get("currentModeId")=="write"
    for n in notifications)
check(got_mode_update, "session/set_mode → current_mode_update notification")

# ── session/set_config_option ────────────────────────────────────────
r = call("session/set_config_option",
         {"sessionId":sid,"configId":"model","value":"claude-test"})
check("result" in r and "configOptions" in r["result"],
      "session/set_config_option → {configOptions}")

# ── session/list ─────────────────────────────────────────────────────
r = call("session/list", {})["result"]
listed = {s["sessionId"] for s in r.get("sessions", [])}
check(sid in listed, "session/list → includes new session")
entry = next((s for s in r["sessions"] if s["sessionId"]==sid), {})
check(entry.get("cwd")==cwd, "session/list → reports cwd")

# cwd filter
r = call("session/list", {"cwd":"/nonexistent/path"})["result"]
check(sid not in {s["sessionId"] for s in r.get("sessions",[])},
      "session/list → cwd filter excludes mismatch")

# ── session/load ─────────────────────────────────────────────────────
# (no on-disk thread yet — session has no messages — but the live session
#  is in memory, so load should succeed and return modes)
r = call("session/load", {"sessionId":sid,"cwd":cwd,"mcpServers":[]})["result"]
check("modes" in r, "session/load → returns modes")

# ── session/resume ───────────────────────────────────────────────────
r = call("session/resume", {"sessionId":sid,"cwd":cwd,"mcpServers":[]})["result"]
check("modes" in r and "configOptions" in r, "session/resume → {modes,configOptions}")

# ── session/close ────────────────────────────────────────────────────
r = call("session/close", {"sessionId":sid})
check("result" in r, "session/close → ok")

# ── session/delete ───────────────────────────────────────────────────
r = call("session/delete", {"sessionId":sid})
check("result" in r, "session/delete → ok")
r = call("session/list", {})["result"]
check(sid not in {s["sessionId"] for s in r.get("sessions",[])},
      "session/delete → removed from session/list")

# ── logout ───────────────────────────────────────────────────────────
r = call("logout", {})
check("result" in r, "logout → ok")

# ── unknown method → error ───────────────────────────────────────────
r = call("does/not/exist", {})
check("error" in r and r["error"]["code"] == -32601,
      "unknown method → -32601 MethodNotFound")

proc.stdin.close(); proc.terminate()
print()
if fails:
    print(f"{len(fails)} FAILURES"); sys.exit(1)
print("ALL PASS")
