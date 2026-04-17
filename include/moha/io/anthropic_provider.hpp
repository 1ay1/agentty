#pragma once
// moha::io::AnthropicProvider — Provider satisfying the streaming contract
// against api.anthropic.com.  Wraps the existing C-style implementation in
// moha::anthropic so the rest of moha never sees raw HTTP.

#include "moha/io/anthropic.hpp"
#include "moha/io/provider.hpp"

namespace moha::io {

class AnthropicProvider {
public:
    void stream(ProviderRequest req, EventSink sink) {
        anthropic::Request areq;
        areq.model         = std::move(req.model);
        areq.system_prompt = std::move(req.system_prompt);
        areq.messages      = std::move(req.messages);
        areq.max_tokens    = req.max_tokens;
        areq.auth_header   = std::move(req.auth_header);
        areq.auth_style    = req.auth_style;
        areq.tools.reserve(req.tools.size());
        for (auto& t : req.tools)
            areq.tools.push_back({std::move(t.name),
                                  std::move(t.description),
                                  std::move(t.input_schema)});
        anthropic::run_stream_sync(std::move(areq), std::move(sink));
    }
};

static_assert(Provider<AnthropicProvider>);

} // namespace moha::io
