#include "agentty/provider/prompt_policy.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/prompt.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::provider {

std::string system_prompt_for(Io io, const Selection& selection) {
    // An ACP backend is a delegated agent, not a model wire transport. Its
    // session owns instructions; provider::Request.system_prompt is ignored.
    if (selection.kind == Kind::ExternalAcp) return {};

    if (selection.kind == Kind::OpenAI) {
        // ChatGPT/Codex carries a localhost:0 sentinel Endpoint because its
        // long-lived transport does not use the generic endpoint at all. Its
        // registry capability must win before localhost detection.
        if (selection.is_oauth_native())
            return system_prompt_with_overlay(io, "chatgpt");

        // Compact prompting is a model/endpoint capability decision, not a
        // provider-brand decision. Keep it for self-hosted servers (Ollama's
        // native API, or an OpenAI-compatible one), where small models
        // benefit from short imperative instructions. "Self-hosted" means a
        // server YOU run -- loopback, a LAN box (192.168.x.x, gpu-box,
        // *.local), a tailnet -- the same rule the window probe uses. It used
        // to be loopback-only, so a llama.cpp/vLLM on another machine fed
        // gpt-oss the long hosted prompt and none of the local action rules
        // (#91).
        if (uses_local_prompt(selection))
            return local_system_prompt_for(selection);

        // Hosted OpenAI-compatible model on the shared base + any openai delta.
        return system_prompt_with_overlay(io, "openai");
    }

    // Anthropic (and any other wire that reaches here) receives the shared
    // agent/tool/RAG policy plus its provider overlay. Provider adapters only
    // decide how this string is encoded on their wire.
    return system_prompt_with_overlay(io, "anthropic");
}

bool uses_local_prompt(const Selection& selection) {
    if (selection.kind != Kind::OpenAI || selection.is_oauth_native()) return false;
    const auto& ep = selection.openai_endpoint;
    // Ollama's native API is always a server you run.
    if (ep.native_api) return true;
    return !ep.use_tls && openai::detail::is_local_endpoint(ep);
}

std::string local_system_prompt_for(const Selection& selection) {
    return selection.openai_endpoint.native_api ? ollama::system_prompt()
                                                : openai::local_model_system_prompt();
}

} // namespace agentty::provider
