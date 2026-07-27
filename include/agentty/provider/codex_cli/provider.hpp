#pragma once

// ChatGPT-authenticated Codex provider — a drop-in bridge to your locally
// installed `codex` CLI's **app-server** (the same JSON-RPC process that
// powers Codex in VS Code, the desktop app, and chatgpt.com/codex).
//
// Design goals (parity with the Anthropic / OpenAI / Ollama transports):
//
//   • Full conversation fidelity. A Codex thread is a durable, server-side
//     conversation; we start it once per agentty conversation (keyed on
//     Request::session_key) and replay the whole agentty transcript into it
//     the first time so the model has real context — not just the last line.
//   • Rich, legible turn rendering. Codex runs its OWN tools server-side
//     (shell, file edits, MCP, web search) behind the app-server sandbox.
//     agentty's tool cards are host-executor-bound, so we surface Codex's
//     item stream as structured markdown blocks — reasoning, commands + their
//     output, unified diffs for file changes, MCP tool calls, web searches —
//     so the transcript reads like a first-class Codex session, not a status
//     log. Agent-message text streams as normal assistant text.
//   • Cross-platform + native speed. The bidirectional child is spawned via
//     posix_spawn on POSIX and CreateProcess on Windows; the reader is a
//     bounded, cancellable, newline-delimited JSON-RPC loop. No fork/exec is
//     hand-rolled inline, no platform is degraded to a shim.
//
// It never reads ~/.codex secrets: authentication is entirely delegated to
// the CLI's own ChatGPT login (`codex login`).

#include <memory>
#include <vector>

#include "agentty/provider/provider.hpp"

namespace agentty::provider::codex_cli {

class CodexCliProvider {
public:
    CodexCliProvider();
    ~CodexCliProvider();
    CodexCliProvider(const CodexCliProvider&) = delete;
    CodexCliProvider& operator=(const CodexCliProvider&) = delete;
    void stream(provider::Request req, provider::EventSink sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Enumerates the models the installed Codex CLI exposes via the app-server's
// `model/list`. Falls back to a single sane default when the probe fails or an
// older CLI predates the method, so selection always has at least one entry.
[[nodiscard]] std::vector<ModelInfo> list_models();

static_assert(provider::Provider<CodexCliProvider>);

} // namespace agentty::provider::codex_cli
