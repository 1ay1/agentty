#pragma once
// agentty::provider::anthropic::AnthropicProvider — the concrete adapter that
// satisfies the `provider::Provider` concept by translating the abstract
// request into an anthropic::transport call.

#include <utility>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/anthropic/transport.hpp"

namespace agentty::provider::anthropic {

class AnthropicProvider {
public:
    // run_stream_sync takes no capability today; the cap is the
    // Provider-concept contract (thread it through if the transport locks).
    provider::StreamResult stream(const maya::unlocked&, provider::Request req,
                                  provider::EventSink sink) {
        Request areq;
        provider::lower_shared(areq, req);   // model/system/messages/tools/max_tokens/auth/retry/effort
        return run_stream_sync(std::move(areq), std::move(sink), std::move(req.cancel));
    }
};

static_assert(provider::Provider<AnthropicProvider>);

} // namespace agentty::provider::anthropic
