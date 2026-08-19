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
#include <string>
#include <vector>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"  // StreamResult (complete type for the concept check)

namespace agentty::provider::chatgpt {

class ChatGptProvider {
public:
    ChatGptProvider();
    ~ChatGptProvider();
    ChatGptProvider(const ChatGptProvider&) = delete;
    ChatGptProvider& operator=(const ChatGptProvider&) = delete;
    provider::StreamResult stream(provider::Request req, provider::EventSink sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The Codex model line-up exposed when signed in with ChatGPT OAuth. Fetched
// live from the account's `/models` catalog (mirrors codex-rs); cached for the
// process. Falls back to a small bundled list when offline / not signed in.
[[nodiscard]] std::vector<ModelInfo> list_models();

// Snapshot of the already-fetched catalog — NEVER touches the network.
// Empty until the first live list_models() succeeds. This is what UI-thread
// call sites (provider switch, recall validation) must use: list_models()
// blocks on a catalog HTTP round-trip when the cache is cold, which froze
// the reducer for the full timeout when switching to ChatGPT offline.
[[nodiscard]] std::vector<ModelInfo> list_models_cached();

// The account's default model slug (first catalog entry). Callers that need a
// concrete model id — e.g. provider-switch defaulting — should use this instead
// of hardcoding a slug the account may not offer.
[[nodiscard]] std::string default_model();

static_assert(provider::Provider<ChatGptProvider>);

} // namespace agentty::provider::chatgpt
