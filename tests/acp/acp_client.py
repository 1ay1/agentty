#!/usr/bin/env python3
"""ACP client harness: drives `agentty acp` over stdio like an editor does.

Every message agentty sends is validated against the official ACP v1 JSON
schema, so a field the spec doesn't allow (or a missing required one) fails
the run. Scenarios mirror what Zed and JetBrains actually do.

usage: acp_client.py <agentty> <schema.json> [scenario...]
env:   the caller sets AGENTTY_HOME / provider vars for a sandboxed run
"""
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time

try:
    import jsonschema
except ImportError:          # schema checks are skipped, scenarios still run
    jsonschema = None


class Client:
    def __init__(self, argv, schema_path, cwd):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, cwd=cwd, bufsize=0)
        self.q = queue.Queue()
        self.next_id = 0
        self.updates = []        # (sessionId, update) in arrival order
        self.violations = []
        self.permission_answer = "allow_once"
        self.client_requests = []
        self.files = {}          # path -> text, for fs/* callbacks
        self.schema = None
        if jsonschema is None and schema_path:
            print("  note: python jsonschema not installed, schema checks skipped")
        if jsonschema and schema_path:
            s = json.load(open(schema_path))
            self.defs = s["$defs"]
            self.schema = s
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._drain_err, daemon=True).start()
        self.stderr = []

    # ── transport ──────────────────────────────────────────────────────
    def _read(self):
        for line in self.p.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self.q.put(json.loads(line))
            except json.JSONDecodeError:
                self.violations.append(f"non-JSON line on stdout: {line[:200]!r}")

    def _drain_err(self):
        for line in self.p.stderr:
            self.stderr.append(line.decode(errors="replace"))

    def send(self, obj):
        self.p.stdin.write((json.dumps(obj) + "\n").encode())
        self.p.stdin.flush()

    # ── schema ─────────────────────────────────────────────────────────
    def check(self, def_name, obj, where):
        if not self.schema:
            return
        sub = {"$ref": f"#/$defs/{def_name}", "$defs": self.defs}
        v = jsonschema.Draft202012Validator(sub)
        for e in v.iter_errors(obj):
            path = "/".join(str(x) for x in e.absolute_path)
            self.violations.append(f"{where}: {def_name} at /{path}: {e.message[:300]}")

    # ── request / response ─────────────────────────────────────────────
    def request(self, method, params, result_def=None, timeout=30):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                m = self.q.get(timeout=max(0.01, deadline - time.time()))
            except queue.Empty:
                break
            if self._handle_incoming(m):
                continue
            if m.get("id") == rid:
                if "error" in m:
                    return None, m["error"]
                if result_def:
                    self.check(result_def, m["result"], method)
                return m["result"], None
        raise TimeoutError(f"{method}: no response in {timeout}s")

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                m = self.q.get(timeout=0.05)
            except queue.Empty:
                continue
            self._handle_incoming(m)

    def _handle_incoming(self, m):
        """Notifications and agent->client requests. True if consumed."""
        meth = m.get("method")
        if not meth:
            return False
        if "id" not in m:
            if meth == "session/update":
                self.check("SessionNotification", m["params"], "session/update")
                self.updates.append((m["params"]["sessionId"], m["params"]["update"]))
            return True
        self.client_requests.append(m)
        rid, p = m["id"], m.get("params", {})
        if meth == "session/request_permission":
            self.check("RequestPermissionRequest", p, meth)
            opts = p.get("options", [])
            pick = next((o for o in opts if o.get("kind") == self.permission_answer), opts[0] if opts else None)
            res = {"outcome": {"outcome": "selected", "optionId": pick["optionId"]}} if pick \
                else {"outcome": {"outcome": "cancelled"}}
            self.send({"jsonrpc": "2.0", "id": rid, "result": res})
        elif meth == "fs/read_text_file":
            self.check("ReadTextFileRequest", p, meth)
            path = p["path"]
            try:
                text = self.files.get(path) or open(path).read()
                self.send({"jsonrpc": "2.0", "id": rid, "result": {"content": text}})
            except OSError as e:
                self.send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32002, "message": str(e)}})
        elif meth == "fs/write_text_file":
            self.check("WriteTextFileRequest", p, meth)
            self.files[p["path"]] = p["content"]
            with open(p["path"], "w") as f:
                f.write(p["content"])
            self.send({"jsonrpc": "2.0", "id": rid, "result": {}})
        else:
            self.send({"jsonrpc": "2.0", "id": rid,
                       "error": {"code": -32601, "message": f"client does not implement {meth}"}})
        return True

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()

    def kinds(self, sid=None):
        return [u.get("sessionUpdate") for s, u in self.updates if sid is None or s == sid]


# ── scenarios ──────────────────────────────────────────────────────────

FAILS = []


def expect(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILS.append(what)


ZED_CAPS = {"fs": {"readTextFile": True, "writeTextFile": True}, "terminal": False,
            "session": {"notices": {}, "compaction": {}, "configOptions": {"boolean": {}}},
            "_meta": {"terminal_output": True, "terminal-auth": True}}


def scenario_config(c, cwd):
    print("config: thinking effort + show_thinking toggle, bad values rejected")
    r, err = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    eff = config_option(r, "thought_level")
    expect(eff is not None, "thinking level offered for a reasoning model")
    show = config_option(r, "show_thinking")
    expect(show is not None and show.get("type") == "boolean", "show_thinking boolean offered")
    if eff:
        vals = [o["value"] for o in eff["options"]]
        expect("none" in vals and "auto" in vals, f"effort has auto + off ({vals})")
        pick = next((v for v in vals if v not in ("auto", "none")), None)
        if pick:
            r2, err = c.request("session/set_config_option",
                                {"sessionId": sid, "configId": "effort", "value": pick},
                                "SetSessionConfigOptionResponse")
            expect(err is None and config_option(r2, "effort")["currentValue"] == pick,
                   f"effort set to {pick}")
    _, err = c.request("session/set_config_option",
                       {"sessionId": sid, "configId": "effort", "value": "ludicrous"})
    expect(err is not None and err.get("code") == -32602, "bad effort -> InvalidParams")
    _, err = c.request("session/set_config_option",
                       {"sessionId": sid, "configId": "bogus", "value": "x"})
    expect(err is not None, "unknown configId rejected")
    if show:
        r3, err = c.request("session/set_config_option",
                            {"sessionId": sid, "configId": "show_thinking", "type": "boolean",
                             "value": False}, "SetSessionConfigOptionResponse")
        expect(err is None and config_option(r3, "show_thinking")["currentValue"] is False,
               "show_thinking toggled off")
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "please think"}]}, "PromptResponse", timeout=60)
    expect(err is None, f"reasoning turn {err or ''}")
    return sid


def scenario_tools(c, cwd):
    print("tools: a write asks permission, runs, and reports a diff")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    before = len(c.client_requests)
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "please write the file"}]},
                       "PromptResponse", timeout=90)
    expect(err is None and r.get("stopReason") == "end_turn", f"tool turn {err or r}")
    asked = [m["method"] for m in c.client_requests[before:]]
    expect("session/request_permission" in asked, f"asked permission ({asked})")
    expect(os.path.exists(os.path.join(cwd, "out.txt")), "file written")
    calls = [u for s, u in c.updates if s == sid and u.get("sessionUpdate") == "tool_call"]
    expect(any(u.get("kind") == "edit" for u in calls), "tool_call kind=edit")
    done = [u for s, u in c.updates if s == sid and u.get("sessionUpdate") == "tool_call_update"
            and u.get("status") == "completed"]
    expect(any(any(x.get("type") == "diff" for x in (u.get("content") or [])) for u in done),
           "completed update carries a diff")


def scenario_terminal(c, cwd):
    print("terminal: a shell call streams into an embedded terminal (Zed _meta)")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    c.request("session/set_mode", {"sessionId": sid, "modeId": "write"})
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "run echo"}]}, "PromptResponse", timeout=90)
    expect(err is None, f"shell turn {err or ''}")
    ups = [u for s, u in c.updates if s == sid]
    info = [u for u in ups if u.get("sessionUpdate") == "tool_call"
            and (u.get("_meta") or {}).get("terminal_info")]
    expect(bool(info), "tool_call carries _meta.terminal_info")
    out = "".join((u.get("_meta") or {}).get("terminal_output", {}).get("data", "") for u in ups)
    expect("term-ok" in out, f"output streamed as terminal_output ({out[:60]!r})")
    exits = [(u.get("_meta") or {}).get("terminal_exit") for u in ups
             if (u.get("_meta") or {}).get("terminal_exit")]
    expect(exits and exits[-1].get("exit_code") == 0, f"terminal_exit with code 0 ({exits})")


def scenario_permissions(c, cwd):
    print("permissions: four options, and 'always reject' sticks for the session")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    seen_kinds = set()
    before = len(c.client_requests)
    c.permission_answer = "reject_always"
    c.request("session/prompt", {"sessionId": sid,
              "prompt": [{"type": "text", "text": "please write the file"}]}, timeout=90)
    reqs = [m for m in c.client_requests[before:] if m["method"] == "session/request_permission"]
    for m in reqs:
        seen_kinds |= {o["kind"] for o in m["params"]["options"]}
    expect(seen_kinds == {"allow_once", "allow_always", "reject_once", "reject_always"},
           f"all four option kinds offered ({sorted(seen_kinds)})")
    before = len(c.client_requests)
    c.request("session/prompt", {"sessionId": sid,
              "prompt": [{"type": "text", "text": "please write the file again"}]}, timeout=90)
    again = [m for m in c.client_requests[before:] if m["method"] == "session/request_permission"]
    expect(not again, "a tool rejected always is not asked about again")
    c.permission_answer = "allow_once"


def scenario_user_command(c, cwd):
    print("slash: a user command shows in the menu and expands before the model sees it")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    c.pump(0.3)
    menus = [u["availableCommands"] for s, u in c.updates
             if s == sid and u.get("sessionUpdate") == "available_commands_update"]
    names = {x["name"] for x in (menus[-1] if menus else [])}
    expect("greet" in names, f"user command listed ({sorted(names)})")
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "/greet world"}]}, "PromptResponse", timeout=60)
    expect(err is None, f"/greet runs {err or ''}")
    titles = [u.get("title") for s, u in c.updates
              if s == sid and u.get("sessionUpdate") == "session_info_update" and u.get("title")]
    expect(titles and titles[0].startswith("/greet"), f"title is what was typed ({titles[:1]})")


def scenario_auth(agentty, schema, cwd):
    print("auth: no credentials -> a terminal login method the editor can run")
    env_home = os.environ["AGENTTY_HOME"]
    empty = tempfile.mkdtemp()
    os.environ["AGENTTY_HOME"] = empty
    try:
        c = Client([agentty, "acp"], schema, cwd)
        caps = dict(ZED_CAPS)
        caps["auth"] = {"terminal": True}
        r = initialize(c, caps)
        methods = r.get("authMethods") or []
        term = [m for m in methods if m.get("type") == "terminal"]
        expect(bool(term) and term[0].get("args") == ["login"], f"terminal auth method ({methods})")
        _, err = c.request("authenticate", {"methodId": "agentty-login"})
        expect(err is not None and err.get("code") == -32000, f"authenticate without login -> AuthRequired ({err})")
        c.close()
        return c
    finally:
        os.environ["AGENTTY_HOME"] = env_home


def scenario_fork(c, cwd, src):
    print("fork: a new session carries the history and keeps going on its own")
    r, err = c.request("session/fork", {"sessionId": src, "cwd": cwd, "mcpServers": []},
                       "NewSessionResponse")
    expect(err is None and r.get("sessionId") and r["sessionId"] != src, f"fork returns a new id {err or ''}")
    if err:
        return
    fid = r["sessionId"]
    expect(config_option(r, "model") is not None, "fork result carries the controls")
    r, err = c.request("session/prompt", {"sessionId": fid,
                       "prompt": [{"type": "text", "text": "say hello"}]}, "PromptResponse", timeout=60)
    expect(err is None, f"fork takes a prompt {err or ''}")
    r, _ = c.request("session/list", {"cwd": cwd}, "ListSessionsResponse")
    ids = [s["sessionId"] for s in r["sessions"]]
    expect(fid in ids and src in ids, "both sessions listed")
    _, err = c.request("session/fork", {"sessionId": "nope", "cwd": cwd, "mcpServers": []})
    expect(err is not None and err.get("code") == -32602, "fork of an unknown session -> InvalidParams")


def scenario_mcp(c, cwd, agentty):
    print("mcp: a server the editor passes in session/new reaches the model")
    if not os.environ.get("FAKE_TOOLS_LOG"):
        print("  skip (no tool log)")
        return
    srv = {"name": "edsrv", "command": agentty, "args": ["mcp-serve", "-w", cwd], "env": []}
    r, err = c.request("session/new", {"cwd": cwd, "mcpServers": [srv]}, "NewSessionResponse")
    expect(err is None, f"session/new with mcpServers {err or ''}")
    sid = r["sessionId"]
    seen = False
    for _ in range(30):          # connecting happens in the background
        open(os.environ["FAKE_TOOLS_LOG"], "w").close()
        c.request("session/prompt", {"sessionId": sid,
                  "prompt": [{"type": "text", "text": "say hello"}]}, timeout=60)
        lines = open(os.environ["FAKE_TOOLS_LOG"]).read().splitlines()
        names = json.loads(lines[-1]) if lines else []
        if any(n.startswith("mcp__edsrv__") for n in names):
            seen = True
            break
        time.sleep(0.5)
    expect(seen, "the editor's MCP server tools are offered to the model")


def scenario_commands(c, cwd):
    print("commands: /compact reports a compaction entity, /new clears context")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    for t in ("say hello", "say hello again"):
        c.request("session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": t}]},
                  "PromptResponse", timeout=60)
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "/compact"}]}, "PromptResponse", timeout=60)
    expect(err is None and r.get("stopReason") == "end_turn", f"/compact finishes {err or ''}")
    c.pump(0.2)
    statuses = [u.get("status") for s, u in c.updates
                if s == sid and u.get("sessionUpdate") == "compaction_update"]
    expect(statuses[:1] == ["in_progress"] and statuses[-1:] == ["completed"],
           f"compaction in_progress -> completed ({statuses})")
    expect("compaction_summary_chunk" in c.kinds(sid), "summary streamed")
    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "/new"}]}, "PromptResponse")
    expect(err is None and r.get("stopReason") == "end_turn", f"/new {err or ''}")


def scenario_failure(c, cwd):
    print("failure: a provider error is an error response with the reason, plus a notice")
    r, _ = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    sid = r["sessionId"]
    _, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "fail please"}]}, timeout=60)
    expect(err is not None and "upstream exploded" in err.get("message", ""),
           f"error carries the reason ({err})")
    c.pump(0.3)
    expect("notice" in c.kinds(sid), "notice sent to a client that renders them")


def initialize(c, caps=ZED_CAPS):
    r, err = c.request("initialize", {"protocolVersion": 1, "clientCapabilities": caps,
                                      "clientInfo": {"name": "acp_client.py", "version": "1"}},
                       "InitializeResponse")
    expect(err is None, f"initialize succeeds {err or ''}")
    return r


def config_option(r, cat_or_id):
    for o in (r or {}).get("configOptions") or []:
        if o.get("id") == cat_or_id or o.get("category") == cat_or_id:
            return o
    return None


def latest_config(c, sid):
    for s, u in reversed(c.updates):
        if s == sid and u.get("sessionUpdate") == "config_option_update":
            return {"configOptions": u.get("configOptions")}
    return None


def scenario_basic(c, cwd):
    print("basic: initialize, new, prompt, list, load, set model")
    init = initialize(c)
    caps = init["agentCapabilities"]
    expect(caps.get("loadSession") is True, "advertises loadSession")
    sc = caps.get("sessionCapabilities") or {}
    for k in ("list", "resume", "close", "fork"):
        expect(k in sc, f"advertises sessionCapabilities.{k}")

    r, err = c.request("session/new", {"cwd": cwd, "mcpServers": []}, "NewSessionResponse")
    expect(err is None, f"session/new {err or ''}")
    sid = r["sessionId"]
    c.pump(0.5)
    model = config_option(r, "model") or config_option(latest_config(c, sid), "model")
    expect(model is not None, "model selector offered (configOptions category model)")
    mode = (r.get("modes") or {}).get("availableModes") or config_option(r, "mode")
    expect(bool(mode), "permission modes offered")
    expect("available_commands_update" in c.kinds(sid), "slash commands advertised")

    r, err = c.request("session/prompt", {"sessionId": sid,
                       "prompt": [{"type": "text", "text": "say hello"}]}, "PromptResponse", timeout=60)
    expect(err is None, f"prompt {err or ''}")
    expect(r and r.get("stopReason") == "end_turn", f"stopReason end_turn ({r})")
    text = "".join(u["content"].get("text", "") for s, u in c.updates
                   if s == sid and u.get("sessionUpdate") == "agent_message_chunk")
    expect("hello from fake" in text, f"streamed the reply ({text!r})")

    if model and model.get("options"):
        opts = model["options"]
        flat = []
        for o in opts:
            flat += o.get("options", [o]) if "group" in o else [o]
        other = next((o["value"] for o in flat if o["value"] != model.get("currentValue")), None)
        if other:
            r2, err = c.request("session/set_config_option",
                                {"sessionId": sid, "configId": model["id"], "value": other},
                                "SetSessionConfigOptionResponse")
            expect(err is None, f"switch model to {other} {err or ''}")
            m2 = config_option(r2, "model")
            expect(m2 and m2.get("currentValue") == other, "response reflects the new model")
    return sid


def scenario_reload(agentty, schema, cwd, env, sid):
    print("reload: a NEW process loads the session (what IntelliJ does on reopen)")
    c = Client([agentty, "acp"], schema, cwd)
    try:
        initialize(c)
        r, err = c.request("session/list", {"cwd": cwd}, "ListSessionsResponse")
        expect(err is None, f"session/list {err or ''}")
        ids = [s["sessionId"] for s in (r or {}).get("sessions", [])]
        expect(sid in ids, "session/list includes the earlier session")
        r, err = c.request("session/load", {"sessionId": sid, "cwd": cwd, "mcpServers": []},
                           "LoadSessionResponse")
        expect(err is None, f"session/load from a fresh process {err or ''}")
        c.pump(0.5)
        k = c.kinds(sid)
        expect("user_message_chunk" in k and "agent_message_chunk" in k, "history replayed")
        # And from a different cwd, as an IDE launched elsewhere would.
        other = tempfile.mkdtemp()
        r, err = c.request("session/load", {"sessionId": sid, "cwd": other, "mcpServers": []},
                           "LoadSessionResponse")
        expect(err is None, f"session/load with another cwd {err or ''}")
        r, err = c.request("session/prompt", {"sessionId": sid,
                           "prompt": [{"type": "text", "text": "again"}]}, "PromptResponse", timeout=60)
        expect(err is None and r.get("stopReason") == "end_turn", f"prompt after load {err or r}")
    finally:
        c.close()
    return c


def main():
    agentty, schema = os.path.abspath(sys.argv[1]), sys.argv[2]
    cwd = tempfile.mkdtemp(prefix="agentty_acpc_")
    os.makedirs(os.path.join(cwd, ".git"), exist_ok=True)
    os.makedirs(os.path.join(cwd, ".agentty", "commands"), exist_ok=True)
    with open(os.path.join(cwd, ".agentty", "commands", "greet.md"), "w") as f:
        f.write("---\ndescription: Say hi\nargument-hint: who\n---\nSay hello to $ARGUMENTS.\n")
    c = Client([agentty, "acp"], schema, cwd)
    try:
        sid = scenario_basic(c, cwd)
        scenario_config(c, cwd)
        scenario_tools(c, cwd)
        scenario_terminal(c, cwd)
        scenario_permissions(c, cwd)
        scenario_user_command(c, cwd)
        scenario_fork(c, cwd, sid)
        scenario_mcp(c, cwd, agentty)
        scenario_commands(c, cwd)
        scenario_failure(c, cwd)
    finally:
        c.close()
    c2 = scenario_reload(agentty, schema, cwd, os.environ, sid)
    c3 = scenario_auth(agentty, schema, cwd)
    for cl in (c, c2, c3):
        for v in cl.violations:
            print("  SCHEMA " + v)
            FAILS.append(v)
    if FAILS:
        sys.stderr.write("".join(c.stderr[-30:]) + "".join(c2.stderr[-30:]))
    print(f"\n{len(FAILS)} failure(s)" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
