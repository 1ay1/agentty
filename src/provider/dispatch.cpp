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

LongLived long_lived_slot(const Selection& sel) {
    // Purely registry-driven. oauth_native (a row flag) picks the ChatGPT/Codex
    // long-lived transport; the Anthropic dialect picks the Anthropic one.
    // Everything else (per-call OpenAI-compat / Ollama, or the ACP arm) has no
    // long-lived slot. No label compares, no is_chatgpt idiom.
    if (sel.is_oauth_native())        return LongLived::ChatGpt;
    if (sel.kind == Kind::Anthropic)  return LongLived::Anthropic;
    return LongLived::None;
}

StreamResult dispatch_stream(const ProviderRouter& router, const Selection& sel,
                             Request req, EventSink sink) {
    // 1) External ACP agent subprocess. Routed through an erased fn (bound in
    //    main() to stream_external_acp) so dispatch has no dependency on the
    //    acp TU. The agent id travels on the Selection.
    if (sel.kind == Kind::ExternalAcp) {
        return router.external_acp(sel.acp_agent_id, std::move(req), std::move(sink));
    }

    // 2) Long-lived native provider (Anthropic, ChatGPT/Codex): holds
    //    cross-turn OAuth/connection state, constructed once in main(). The
    //    slot is derived from registry data, NOT from a label ladder.
    if (const LongLived slot = long_lived_slot(sel); slot != LongLived::None) {
        return router.long_lived[static_cast<std::size_t>(slot)](
            std::move(req), std::move(sink));
    }

    // 3) Per-call transport, built fresh from the active Endpoint so a
    //    host/path/tls change takes effect on the very next turn. Ollama speaks
    //    its native /api/chat NDJSON dialect; every other OpenAI-compatible
    //    endpoint (openai/groq/openrouter/together/cerebras/llama.cpp/custom)
    //    goes through the shared OpenAI transport.
    if (sel.openai_endpoint.native_api) {
        ollama::OllamaProvider p{sel.openai_endpoint};
        return p.stream(std::move(req), std::move(sink));
    }
    openai::OpenAIProvider p{sel.openai_endpoint};
    return p.stream(std::move(req), std::move(sink));
}

StreamResult dispatch_stream(const ProviderRouter& router, Request req, EventSink sink) {
    // Dispatch on the LIVE selection so a picker switch retargets the next
    // request with no seam rebuild. active() hands back a by-value snapshot
    // taken under the selection mutex, so the stream worker can't observe a
    // torn mid-select() endpoint.
    const Selection sel = active();
    return dispatch_stream(router, sel, std::move(req), std::move(sink));
}

} // namespace agentty::provider
