#pragma once
// agentty::provider::openai::OpenAIProvider — concrete adapter satisfying the
// `provider::Provider` concept by translating the abstract request into an
// openai::transport call. Holds an Endpoint (base URL / port / tls) so one
// process can target OpenAI, Groq, OpenRouter, a local Ollama, etc.

#include <utility>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/openai/transport.hpp"

namespace agentty::provider::openai {

class OpenAIProvider {
public:
    OpenAIProvider() = default;
    explicit OpenAIProvider(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    void stream(provider::Request req, provider::EventSink sink) {
        Request oreq;
        oreq.model         = std::move(req.model);
        oreq.system_prompt = std::move(req.system_prompt);
        oreq.messages      = std::move(req.messages);
        oreq.tools         = std::move(req.tools);
        oreq.max_tokens    = req.max_tokens;
        oreq.context_window = req.context_window;
        oreq.auth          = std::move(req.auth);
        oreq.retry_count   = req.retry_count;
        oreq.endpoint      = endpoint_;
        openai::run_stream_sync(std::move(oreq), std::move(sink), std::move(req.cancel));
    }

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    Endpoint endpoint_;
};

static_assert(provider::Provider<OpenAIProvider>);

} // namespace agentty::provider::openai
