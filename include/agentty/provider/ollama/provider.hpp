#pragma once
// agentty::provider::ollama::OllamaProvider — concrete adapter satisfying the
// `provider::Provider` concept by translating the abstract request into an
// ollama::transport /api/chat call. Holds the Endpoint (host/port) of the
// local Ollama server.

#include <utility>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/ollama/transport.hpp"

namespace agentty::provider::ollama {

class OllamaProvider {
public:
    OllamaProvider() = default;
    explicit OllamaProvider(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    void stream(provider::Request req, provider::EventSink sink) {
        Request oreq;
        oreq.model         = std::move(req.model);
        oreq.system_prompt = std::move(req.system_prompt);
        oreq.messages      = std::move(req.messages);
        oreq.tools         = std::move(req.tools);
        oreq.max_tokens    = req.max_tokens;
        oreq.auth          = std::move(req.auth);
        oreq.retry_count   = req.retry_count;
        oreq.json_protocol = req.json_protocol;
        oreq.endpoint      = endpoint_;
        ollama::run_stream_sync(std::move(oreq), std::move(sink), std::move(req.cancel));
    }

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    Endpoint endpoint_;
};

static_assert(provider::Provider<OllamaProvider>);

} // namespace agentty::provider::ollama
