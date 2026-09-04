// server.mjs — the live agentty backend.
//
// 1. Boots a mock Ollama server on a random localhost port.
// 2. Exposes a WebSocket. On connect it spawns the REAL agentty binary in a
//    PTY, in a throwaway sandbox workdir, pointed at the mock model via
//    AGENTTY_OLLAMA_HOST. PTY output → browser; browser keystrokes → PTY.
//
// The browser renders the stream with xterm.js, so visitors drive the actual
// agentty TUI — real composer, real streaming, real tool timeline — against a
// free mock model. No cloud, no key, no repo access.

import { WebSocketServer } from "ws";
import { spawn as ptySpawn } from "node-pty";
import { createMockOllama } from "./mock-ollama.mjs";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import http from "node:http";

const PORT = Number(process.env.LIVE_PORT || 8790);
const AGENTTY_BIN = process.env.AGENTTY_BIN || "agentty";
const MAX_SESSIONS = Number(process.env.LIVE_MAX_SESSIONS || 12);
const SESSION_MS = Number(process.env.LIVE_SESSION_MS || 10 * 60 * 1000); // 10 min
const IDLE_MS = Number(process.env.LIVE_IDLE_MS || 3 * 60 * 1000);        // 3 min

// ---- 1. mock Ollama on an ephemeral localhost port -----------------------
const mock = createMockOllama();
await new Promise((r) => mock.listen(0, "127.0.0.1", r));
const mockPort = mock.address().port;
const OLLAMA_HOST = `localhost:${mockPort}`;
console.log(`[live] mock ollama on ${OLLAMA_HOST}`);

// ---- 2. WebSocket + tiny health HTTP -------------------------------------
const httpServer = http.createServer((req, res) => {
  if (req.url === "/live/health" || req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    return res.end(JSON.stringify({ ok: true, sessions: sessions.size, mockPort }));
  }
  res.writeHead(426, { "content-type": "text/plain" });
  res.end("WebSocket only");
});

const wss = new WebSocketServer({ server: httpServer, path: "/live/ws" });
const sessions = new Set();

wss.on("connection", (ws, req) => {
  if (sessions.size >= MAX_SESSIONS) {
    try { ws.send(JSON.stringify({ t: "err", m: "Live demo is at capacity — try again in a minute." })); } catch {}
    ws.close();
    return;
  }

  // throwaway sandbox workdir so agentty's filesystem tools have somewhere
  // harmless to point at.
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "agentty-live-"));
  seedWorkspace(work);

  const env = {
    ...process.env,
    HOME: work,                       // isolate creds/threads to the temp dir
    AGENTTY_OLLAMA_HOST: OLLAMA_HOST, // point the real binary at the mock
    TERM: "xterm-256color",
    COLORTERM: "truecolor",
    NO_COLOR: "",
    AGENTTY_PROFILE: "ask",
  };

  let pty;
  try {
    pty = ptySpawn(AGENTTY_BIN, ["--provider", "ollama", "-m", "agentty-demo:latest"], {
      name: "xterm-256color",
      cols: 100,
      rows: 30,
      cwd: work,
      env,
    });
  } catch (e) {
    try { ws.send(JSON.stringify({ t: "err", m: "Could not start agentty: " + e.message })); } catch {}
    ws.close();
    cleanupDir(work);
    return;
  }

  const sess = { ws, pty, work, lastActivity: Date.now() };
  sessions.add(sess);
  console.log(`[live] session up (${sessions.size} active) → ${work}`);

  pty.onData((d) => {
    sess.lastActivity = Date.now();
    if (ws.readyState === ws.OPEN) ws.send(d);
  });

  pty.onExit(() => end("agentty exited"));

  ws.on("message", (raw) => {
    sess.lastActivity = Date.now();
    // control frames are JSON ({resize}); everything else is raw keystrokes
    const s = raw.toString();
    if (s.startsWith("\x00json:")) {
      try {
        const msg = JSON.parse(s.slice(6));
        if (msg.t === "resize" && msg.cols && msg.rows) {
          pty.resize(Math.max(20, msg.cols | 0), Math.max(6, msg.rows | 0));
        }
      } catch {}
      return;
    }
    pty.write(s);
  });

  const hardTimer = setTimeout(() => end("session time limit reached"), SESSION_MS);
  const idleTimer = setInterval(() => {
    if (Date.now() - sess.lastActivity > IDLE_MS) end("idle timeout");
  }, 15000);

  function end(reason) {
    if (!sessions.has(sess)) return;
    sessions.delete(sess);
    clearTimeout(hardTimer);
    clearInterval(idleTimer);
    try { ws.send(JSON.stringify({ t: "end", m: reason })); } catch {}
    try { pty.kill(); } catch {}
    try { ws.close(); } catch {}
    cleanupDir(work);
    console.log(`[live] session down (${sessions.size} active): ${reason}`);
  }

  ws.on("close", () => end("client disconnected"));
  ws.on("error", () => end("socket error"));
});

httpServer.listen(PORT, "127.0.0.1", () => {
  console.log(`[live] ws server on 127.0.0.1:${PORT} (path /live/ws)`);
});

// ---- helpers -------------------------------------------------------------
function seedWorkspace(dir) {
  try {
    fs.writeFileSync(path.join(dir, "README.md"),
      "# demo workspace\n\nThis is a throwaway sandbox for the agentty live demo.\n" +
      "Nothing here is real — the model is a mock and the directory is deleted when you leave.\n");
    fs.writeFileSync(path.join(dir, "hello.py"),
      "def greet(name):\n    return f\"hello, {name}!\"\n\n\nprint(greet(\"agentty\"))\n");
  } catch {}
}

function cleanupDir(dir) {
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}

// graceful shutdown
for (const sig of ["SIGINT", "SIGTERM"]) {
  process.on(sig, () => {
    for (const s of sessions) { try { s.pty.kill(); } catch {} cleanupDir(s.work); }
    process.exit(0);
  });
}
