#!/usr/bin/env python3
"""Fake OpenAI-compatible chat server for ACP end-to-end tests.

Streams one scripted reply per request, chosen by the last user message:
  contains "write"  -> a tool call to `write` (path out.txt)
  contains "think"  -> reasoning deltas then text
  otherwise         -> plain text "hello from fake"
After a tool result is sent back, replies "done".

usage: fake_openai_server.py <port>
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0

MODELS = {"object": "list", "data": [
    {"id": "fake-small", "object": "model", "owned_by": "fake", "context_length": 32768},
    {"id": "fake-large", "object": "model", "owned_by": "fake", "context_length": 131072},
    # Named so agentty's catalog infers it can reason (effort ladder).
    {"id": "o4-mini", "object": "model", "owned_by": "fake", "context_length": 200000},
]}


def chunk(delta, finish=None):
    return {"id": "c1", "object": "chat.completion.chunk", "model": "fake",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            return self._json(MODELS)
        return self._json({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("content-length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        msgs = body.get("messages", [])
        last = msgs[-1] if msgs else {}
        text = last.get("content") if isinstance(last.get("content"), str) else json.dumps(last.get("content"))
        if os.environ.get("FAKE_DEBUG"):
            sys.stderr.write(f"REQ role={last.get('role')} text={(text or '')[:80]!r} n={len(msgs)} stream={body.get('stream')}\n")
        if os.environ.get("FAKE_TOOLS_LOG"):
            names = [t.get("function", {}).get("name", "") for t in body.get("tools") or []]
            with open(os.environ["FAKE_TOOLS_LOG"], "a") as f:
                f.write(json.dumps(names) + "\n")
        events = []
        if last.get("role") == "tool":
            events = [chunk({"role": "assistant", "content": "done"}), chunk({}, "stop")]
        elif "fail" in (text or ""):
            return self._json({"error": {"message": "upstream exploded", "type": "server_error"}}, 400)
        elif "summar" in (text or "").lower():
            events = [chunk({"role": "assistant", "content": "<summary>We said hello "}),
                      chunk({"content": "twice.</summary>"}), chunk({}, "stop")]
        elif "ask my name" in (text or ""):
            events = [chunk({"role": "assistant", "content": ""}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_ask", "type": "function",
                                             "function": {"name": "mcp__elicit__ask_name", "arguments": "{}"}}]}),
                      chunk({}, "tool_calls")]
        elif "edit file " in (text or ""):
            parts = (text or "").split("edit file ", 1)[1].split("|")
            args = json.dumps({"path": parts[0].strip(), "old_text": parts[1], "new_text": parts[2].rstrip("\n")})
            events = [chunk({"role": "assistant", "content": ""}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_e", "type": "function",
                                             "function": {"name": "edit", "arguments": args}}]}),
                      chunk({}, "tool_calls")]
        elif "write to " in (text or ""):
            target = (text or "").split("write to ", 1)[1].split()[0]
            args = json.dumps({"path": target, "content": "extra-root\n"})
            events = [chunk({"role": "assistant", "content": "Writing. "}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_x", "type": "function",
                                             "function": {"name": "write", "arguments": args}}]}),
                      chunk({}, "tool_calls")]
        elif "run pwd" in (text or ""):
            args = json.dumps({"command": "pwd"})
            events = [chunk({"role": "assistant", "content": ""}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_pwd", "type": "function",
                                             "function": {"name": "shell", "arguments": args}}]}),
                      chunk({}, "tool_calls")]
        elif "run echo" in (text or ""):
            args = json.dumps({"command": "echo term-ok; echo second-line"})
            events = [chunk({"role": "assistant", "content": "Running. "}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_sh", "type": "function",
                                             "function": {"name": "shell", "arguments": args}}]}),
                      chunk({}, "tool_calls")]
        elif "write" in (text or ""):
            args = json.dumps({"path": "out.txt", "content": "hi\n"})
            events = [chunk({"role": "assistant", "content": "Writing. "}),
                      chunk({"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                             "function": {"name": "write", "arguments": args}}]}),
                      chunk({}, "tool_calls")]
        elif "think" in (text or ""):
            events = [chunk({"role": "assistant", "reasoning_content": "let me think"}),
                      chunk({"content": "thought done"}), chunk({}, "stop")]
        else:
            events = [chunk({"role": "assistant", "content": "hello from fake"}), chunk({}, "stop")]
        if not body.get("stream"):
            content = "".join(e["choices"][0]["delta"].get("content", "") for e in events)
            return self._json({"id": "c1", "object": "chat.completion", "model": "fake",
                               "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                                            "finish_reason": "stop"}],
                               "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}})
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("cache-control", "no-cache")
        self.send_header("connection", "close")
        self.end_headers()
        for e in events:
            self.wfile.write(b"data: " + json.dumps(e).encode() + b"\n\n")
        usage = {"id": "c1", "object": "chat.completion.chunk", "model": "fake", "choices": [],
                 "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}
        self.wfile.write(b"data: " + json.dumps(usage).encode() + b"\n\n")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True


srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
