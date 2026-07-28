// agentty::provider::dispatch_stream — the single provider-routing point.
// See dispatch.hpp for why this lives here and not inline in main.cpp.

#include "agentty/provider/dispatch.hpp"

#include <utility>

#include "agentty/provider/acp_provider_adapter.hpp"
#include "agentty/provider/ollama/provider.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::provider {

void dispatch_stream(NativeProviders natives, Request req, EventSink sink) {
    // Dispatch on the LIVE selection so a picker switch retargets the next
    // request with no seam rebuild. active() hands back a by-value snapshot
    // taken under the selection mutex, so the stream worker can't observe a
    // torn mid-select() endpoint.
    const Selection sel = active();

    if (sel.kind == Kind::ExternalAcp) {
        // Drive an external ACP agent subprocess (the built-in
        // claude-agent-acp reference agent, codex-acp, or a config-defined
        // id). The adapter presents it as a plain Provider, so this is a
        // single branch — not a Kind fan-out across the codebase.
        stream_external_acp(sel.acp_agent_id, std::move(req), std::move(sink));
        return;
    }

    if (sel.kind == Kind::OpenAI) {
        // ChatGPT/Codex: OAuth Responses backend, long-lived (holds refreshed
        // tokens), owned by main() — route to the shared instance.
        if (sel.openai_endpoint.label == "chatgpt") {
            natives.chatgpt.stream(std::move(req), std::move(sink));
            return;
        }
        // Ollama native /api/chat (NDJSON) — cheap value transport built from
        // the active endpoint so a host/tls change takes effect immediately.
        if (sel.openai_endpoint.native_api) {
            ollama::OllamaProvider p{sel.openai_endpoint};
            p.stream(std::move(req), std::move(sink));
            return;
        }
        // Every other OpenAI-compatible endpoint (openai/groq/openrouter/
        // together/cerebras/llama.cpp/custom host).
        openai::OpenAIProvider p{sel.openai_endpoint};
        p.stream(std::move(req), std::move(sink));
        return;
    }

    // Anthropic (default) — long-lived, owned by main().
    natives.anthropic.stream(std::move(req), std::move(sink));
}

} // namespace agentty::provider
