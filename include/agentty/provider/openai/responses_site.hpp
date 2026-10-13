#pragma once
// agentty::provider::openai — Responses-dialect access for the OpenAI-compat
// family (api.openai.com, OpenRouter, and any host whose registry row carries
// a `responses_path`).
//
// The chat transport in openai/transport.cpp keeps owning /chat/completions.
// This header is the OTHER destination, for turns that provider::dialect_for()
// routes to Responses — which for GPT-5.4+ and GPT-6-class models is not an
// optimisation but the only dialect that accepts tool calls at all.
//
// See responses_site.cpp for why this file is thin: the dialect's real work
// (input[] construction, encrypted reasoning replay, the SSE state machine)
// is shared with ChatGPT and Copilot in provider/responses/codec.cpp.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"

namespace agentty::provider::openai {

// Where a Responses turn should be dialled, resolved from the registry row.
struct ResponsesEndpoint {
    std::string   host;
    std::uint16_t port = 443;
    std::string   path;              // the row's `responses_path`
    bool          use_tls = true;
    std::string   provider_id;       // for routing feedback on failure
    // Static headers this host requires on every call (Copilot's editor
    // identification block; a gateway's required extras). Mirrors
    // Endpoint::extra_headers on the chat path.
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

// Fill `out` from the provider row's `responses_path`. False when the row
// advertises no Responses endpoint — the caller must then stay on chat.
[[nodiscard]] bool responses_endpoint_for(std::string_view provider_id,
                                          ResponsesEndpoint& out) noexcept;

// Stream one turn over the Responses dialect.
//
// On a failure that identifies the ENDPOINT as unavailable (404, or a 400
// naming the api/model pairing) this records the fact via
// note_dialect_rejected(), so the next turn for this (provider, model) is
// routed back to chat automatically. Credential and rate-limit failures are
// deliberately NOT treated as dialect evidence.
[[nodiscard]] provider::StreamResult stream_responses(const ResponsesEndpoint& ep,
                                                      provider::Request req,
                                                      EventSink sink);

} // namespace agentty::provider::openai
