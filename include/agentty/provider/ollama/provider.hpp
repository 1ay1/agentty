#pragma once
// agentty::provider::ollama::OllamaProvider — concrete adapter satisfying the
// `provider::Provider` concept by translating the abstract request into an
// ollama::transport /api/chat call. Holds the Endpoint (host/port) of the
// local Ollama server.

#include <utility>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/ollama/transport.hpp"

namespace agentty::provider::ollama {

class OllamaProvider {
public:
    OllamaProvider() = default;
    explicit OllamaProvider(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    provider::StreamResult stream(provider::Request req, provider::EventSink sink) {
        Request oreq;
        provider::lower_shared(oreq, req);          // shared core
        oreq.context_window = req.context_window;   // num_ctx sizing
        oreq.json_protocol  = req.json_protocol;    // weak-model JSON tool channel
        oreq.endpoint       = endpoint_;
        return ollama::run_stream_sync(std::move(oreq), std::move(sink), std::move(req.cancel));
    }

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    Endpoint endpoint_;
};

static_assert(provider::Provider<OllamaProvider>);

} // namespace agentty::provider::ollama
