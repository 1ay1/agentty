// agentty::provider::dispatch_stream — the single provider-routing point.
// See dispatch.hpp for why this lives here and not inline in main.cpp, and why
// it is provider-agnostic (type-erased routes) rather than naming concrete
// transports.

#include "agentty/provider/dispatch.hpp"

#include <utility>

#include "agentty/provider/ollama/provider.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::provider {

void dispatch_stream(const Routes& routes, const Selection& sel,
                     Request req, EventSink sink) {
    if (sel.kind == Kind::ExternalAcp) {
        // Drive an external ACP agent subprocess (the built-in
        // claude-agent-acp reference agent, codex-acp, or a config-defined
        // id). Routed through an erased Routes fn (bound in main() to
        // stream_external_acp) so dispatch has no dependency on the acp TU.
        routes.external_acp(sel.acp_agent_id, std::move(req), std::move(sink));
        return;
    }

    if (sel.kind == Kind::OpenAI) {
        // ChatGPT/Codex: OAuth Responses backend, long-lived (holds refreshed
        // tokens), owned by main() — route to its erased StreamFn.
        if (sel.is_chatgpt()) {
            routes.chatgpt(std::move(req), std::move(sink));
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
    routes.anthropic(std::move(req), std::move(sink));
}

void dispatch_stream(const Routes& routes, Request req, EventSink sink) {
    // Dispatch on the LIVE selection so a picker switch retargets the next
    // request with no seam rebuild. active() hands back a by-value snapshot
    // taken under the selection mutex, so the stream worker can't observe a
    // torn mid-select() endpoint.
    const Selection sel = active();
    dispatch_stream(routes, sel, std::move(req), std::move(sink));
}

} // namespace agentty::provider
