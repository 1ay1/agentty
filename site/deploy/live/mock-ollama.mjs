// mock-ollama.mjs — a minimal Ollama-compatible server so the REAL agentty
// binary can run a full agent loop with no cloud, no key, no model download.
//
// agentty (--provider ollama) hits exactly three endpoints:
//   GET  /api/tags   → model list
//   POST /api/show   → per-model capabilities (must advertise "tools")
//   POST /api/chat   → NDJSON streaming chat (native path)
//
// We answer all three. /api/chat streams a canned-but-context-aware reply
// token by token as {"message":{"content":"..."},"done":false} lines, then a
// final {"done":true}. It never calls a real model, so it's free and fast,
// yet the agentty process, its TUI, tool loop, and streaming are 100% real.

import http from "node:http";

const MODEL = "agentty-demo:latest";

// ---- canned reply engine -------------------------------------------------
// Keeps the demo coherent and safe: recognises a few intents, otherwise gives
// a friendly generic answer. No tool calls by default (the sandbox demo is
// about the UX, not filesystem writes on a public box).
function replyFor(messages) {
  const lastUser = [...messages].reverse().find((m) => m.role === "user");
  const q = (lastUser?.content || "").toLowerCase().trim();

  if (/^(hi|hey|hello|yo|sup|hola)\b/.test(q))
    return "Hey! I'm agentty — a coding agent running right in your terminal. This is a live demo backed by a mock model, so I won't touch real files. Ask me what I can do, or try a slash command like `/help`.";

  if (/what.*(can|do) you|help|capab/.test(q))
    return "I pair-program in your terminal. In the real thing I read and edit files, run sandboxed shell commands, search your codebase with local retrieval, and drive any model — Claude, GPT, Groq, or a local Ollama model.\n\nHere I'm a scripted demo, but the terminal, streaming, and tool timeline you see are the actual agentty TUI. Try `/model`, `/help`, or just chat.";

  if (/who (are|r) you|what are you|about/.test(q))
    return "I'm agentty: a native C++26 coding agent that ships as one static binary — sub-millisecond startup, sandboxed by default, bring-your-own-model. Think of me as a drop-in claude-code alternative you can read, fork, and run offline.";

  if (/speed|fast|performance|startup/.test(q))
    return "Fast is the point. agentty is statically-linked C++26 — it cold-starts in under a millisecond, spawns subprocesses with posix_spawn, and never pauses for garbage collection mid-stream. No Node runtime to boot, no node_modules to load.";

  if (/model|provider|claude|gpt|ollama/.test(q))
    return "I'm bring-your-own-model. Point me at Anthropic (Claude, via API key or your Pro/Max OAuth), OpenAI, Groq, OpenRouter, Together, Cerebras, or a local Ollama model that needs no key at all. Switch provider with ^P and model with ^/ — live, mid-thread, no restart.";

  if (/sandbox|safe|secure|security/.test(q))
    return "Every shell and build command runs inside a sandbox by default — bwrap on Linux, sandbox-exec on macOS. An approved `bash` call sees your workspace and system libs, but your home dir and ~/.ssh stay invisible. Safety is structural, not an afterthought.";

  if (/thank/.test(q))
    return "Anytime! If you want the real thing: `curl -fsSL https://agentty.org/install.sh | sh`.";

  // generic, echoes intent so it feels responsive
  return `Good question. In a real session I'd reason about "${(lastUser?.content || "").slice(0, 80)}", then read the relevant files and act. This demo runs the genuine agentty terminal against a mock model, so you're seeing the real UX — install it to point me at a live model and your own repo:\n\ncurl -fsSL https://agentty.org/install.sh | sh`;
}

function tokenize(text) {
  // stream in small word-ish chunks so it looks like real generation
  return text.match(/\S+\s*|\s+/g) || [text];
}

export function createMockOllama() {
  return http.createServer((req, res) => {
    const url = req.url || "/";

    if (req.method === "GET" && (url === "/" || url === "")) {
      res.writeHead(200, { "content-type": "text/plain" });
      return res.end("Ollama is running");
    }

    if (req.method === "GET" && url.startsWith("/api/tags")) {
      res.writeHead(200, { "content-type": "application/json" });
      return res.end(JSON.stringify({
        models: [{
          name: MODEL, model: MODEL, size: 0,
          details: { family: "agentty", parameter_size: "demo", quantization_level: "none" },
        }],
      }));
    }

    if (req.method === "POST" && url.startsWith("/api/show")) {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", () => {
        res.writeHead(200, { "content-type": "application/json" });
        res.end(JSON.stringify({
          capabilities: ["completion", "tools"],
          model_info: { "agentty.context_length": 32768 },
          details: { family: "agentty", parameter_size: "demo" },
        }));
      });
      return;
    }

    if (req.method === "POST" && url.startsWith("/api/chat")) {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", () => {
        let messages = [];
        try { messages = JSON.parse(body).messages || []; } catch {}
        res.writeHead(200, { "content-type": "application/x-ndjson" });
        const text = replyFor(messages);
        const toks = tokenize(text);
        let i = 0;
        const tick = () => {
          if (i < toks.length) {
            res.write(JSON.stringify({
              model: MODEL, created_at: new Date().toISOString(),
              message: { role: "assistant", content: toks[i] }, done: false,
            }) + "\n");
            i++;
            setTimeout(tick, 22 + Math.random() * 26);
          } else {
            res.write(JSON.stringify({
              model: MODEL, created_at: new Date().toISOString(),
              message: { role: "assistant", content: "" },
              done: true, done_reason: "stop",
              total_duration: 100000000, eval_count: toks.length,
            }) + "\n");
            res.end();
          }
        };
        setTimeout(tick, 120);
      });
      return;
    }

    // /api/embed etc. — return empty so nothing 500s
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ embeddings: [] }));
  });
}
