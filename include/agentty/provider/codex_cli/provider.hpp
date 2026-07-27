#pragma once

// ChatGPT-authenticated Codex provider — a native, in-process bridge to the
// OpenAI Responses backend (the same endpoint that powers Codex in the desktop
// app and chatgpt.com/codex). There is NO `codex` binary at runtime.
//
// Design goals (parity with the Anthropic / OpenAI / Ollama transports):
//
//   • Reverse-engineered OAuth, works like Claude. Authentication is the
//     ChatGPT Auth-Code + PKCE flow (`agentty login` → ChatGPT); tokens are
//     stored encrypted and auto-refreshed in-process. No delegation to an
//     external CLI, no ~/.codex secrets.
//   • Direct Responses-API streaming. One turn maps to one SSE request against
//     chatgpt.com/backend-api/codex/responses; tool calls, reasoning, and text
//     are surfaced as first-class agentty stream events.
//   • Cross-platform + native speed. Pure HTTPS transport, no subprocess on any
//     platform.

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

// The Codex model line-up exposed when signed in with ChatGPT OAuth.
[[nodiscard]] std::vector<ModelInfo> list_models();

static_assert(provider::Provider<CodexCliProvider>);

} // namespace agentty::provider::codex_cli
