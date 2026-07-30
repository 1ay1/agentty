#include "agentty/provider/prompt_policy.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::provider {

std::string system_prompt_for(const Selection& selection) {
    // An ACP backend is a delegated agent, not a model wire transport. Its
    // session owns instructions; provider::Request.system_prompt is ignored.
    if (selection.kind == Kind::ExternalAcp) return {};

    if (selection.kind == Kind::OpenAI) {
        // ChatGPT/Codex carries a localhost:0 sentinel Endpoint because its
        // long-lived transport does not use the generic endpoint at all. Its
        // registry capability must win before localhost detection.
        if (selection.is_oauth_native())
            return anthropic::default_system_prompt();

        const auto& ep = selection.openai_endpoint;
        if (ep.native_api) return ollama::system_prompt();

        // Compact prompting is a model/endpoint capability decision, not a
        // provider-brand decision. Keep it for local OpenAI-compatible servers
        // where small models benefit from short imperative instructions.
        const bool local_endpoint = !ep.use_tls
            && (ep.host == "localhost" || ep.host == "127.0.0.1");
        if (local_endpoint) return openai::local_model_system_prompt();
    }

    // Anthropic, native ChatGPT/Codex, and hosted OpenAI-compatible models all
    // receive the same complete agent/tool/RAG policy. Provider adapters only
    // decide how this string is encoded on their wire.
    return anthropic::default_system_prompt();
}

} // namespace agentty::provider
