// agentty::provider::dispatch_stream — the single provider-routing point.
// See dispatch.hpp for why this lives here and not inline in main.cpp, and why
// it is provider-agnostic (type-erased routes) rather than naming concrete
// transports.

#include "agentty/provider/dispatch.hpp"

#include <utility>

#include "agentty/provider/ollama/provider.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::provider {

namespace {
// Human tag for the route taken — which ADAPTER (thin ugly per-provider
// shim) owns this turn. This is the single most useful fact when a model
// misbehaves: the same model id can stream through different dialects on
// different hosts (claude-* over OpenAI-compat on an aggregator, gpt-*
// over /responses on Copilot but /chat/completions on Azure).
// Slot rows carry their tag in the registry (slot_tag); only the
// slot-less arms (ACP / per-call compat) are named here.
[[nodiscard]] std::string_view route_tag(const Selection& sel) noexcept {
    if (sel.kind == Kind::ExternalAcp)  return "acp";
    if (const auto s = long_lived_slot(sel); s != RouteSlot::None)
        return slot_tag(s);
    if (sel.openai_endpoint.native_api) return "ollama-native";
    return "openai-chat";
}
} // namespace

LongLived long_lived_slot(const Selection& sel) {
    // ONE field read. The registry's routing_consistent() static_assert
    // proves every LongLived row carries a unique slot, so there is nothing
    // to derive and nothing to drift: the row IS the routing decision.
    // Row-less selections (a custom host that matched no preset) fall to the
    // per-call transports — by construction, since they have no row to read.
    return sel.row ? sel.row->route : RouteSlot::None;
}

StreamResult dispatch_stream(const ProviderRouter& router, const Selection& sel,
                             Request req, EventSink sink) {
    // THE turn fingerprint. One Debug line naming every heterogeneity
    // decision that shaped this request BEFORE any bytes hit the wire:
    // which adapter, which model id, what effort survived the clamp,
    // whether tools are advertised (and how many), whether the weak-model
    // JSON protocol is on, whether reasoning display is requested. When a
    // user reports "model X does Y on provider Z", this line + the
    // end-of-turn result line bracket the whole story.
    AGT_LOG(Model, Debug, "dispatch.turn",
            "route={} provider={} model={} effort={} tools={} json_protocol={} "
            "show_reasoning={} ctx_window={} max_tokens={} retry={}",
            route_tag(sel), sel.provider_id(), req.model,
            req.effort.empty() ? "off" : req.effort, req.tools.size(),
            req.json_protocol ? 1 : 0, req.show_reasoning ? 1 : 0,
            req.context_window, req.max_tokens, req.retry_count);
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
        const auto& fn = router.long_lived[static_cast<std::size_t>(slot)];
        if (!fn) {
            // A routed row whose slot main() never bound. Cannot happen when
            // main() binds every enumerator (the registry proves rows↔slots
            // 1:1), but if a refactor drops one, fail as a named StreamError
            // — not a bad_function_call unwinding through the worker thread.
            AGT_LOG(Model, Error, "dispatch.unbound_slot", "route={}",
                    slot_tag(slot));
            sink(Msg{StreamError{std::string{"internal: provider slot '"}
                                 + std::string{slot_tag(slot)}
                                 + "' is not bound", std::nullopt}});
            return StreamResult::failed("unbound provider slot");
        }
        return fn(std::move(req), std::move(sink));
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
