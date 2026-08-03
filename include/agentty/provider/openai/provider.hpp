#pragma once
// agentty::provider::openai::OpenAIProvider — concrete adapter satisfying the
// `provider::Provider` concept by translating the abstract request into an
// openai::transport call. Holds an Endpoint (base URL / port / tls) so one
// process can target OpenAI, Groq, OpenRouter, a local Ollama, etc.

#include <utility>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/openai/transport.hpp"

namespace agentty::provider::openai {

class OpenAIProvider {
public:
    OpenAIProvider() = default;
    explicit OpenAIProvider(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    provider::StreamResult stream(provider::Request req, provider::EventSink sink) {
        Request oreq;
        provider::lower_shared(oreq, req);          // shared core
        oreq.context_window = req.context_window;   // OpenAI-family: Ollama num_ctx
        oreq.session_key    = req.session_key;       // prompt_cache_key routing
        oreq.endpoint       = endpoint_;
        return openai::run_stream_sync(std::move(oreq), std::move(sink), std::move(req.cancel));
    }

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    Endpoint endpoint_;
};

static_assert(provider::Provider<OpenAIProvider>);

} // namespace agentty::provider::openai
