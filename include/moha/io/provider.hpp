#pragma once
// moha::io::Provider — concept for an LLM streaming backend.
//
// A Provider knows how to issue one streaming completion request and emit
// Msgs as the stream advances.  Implementations:
//   - moha::io::AnthropicProvider     (production — claude.ai / API key)
//   - tests/MockProvider              (deterministic, in-memory)
//
// The runtime never names a concrete provider type; it accepts anything
// satisfying the Provider concept.

#include <concepts>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/io/auth.hpp"
#include "moha/model.hpp"
#include "moha/msg.hpp"

namespace moha::io {

struct ProviderToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

struct ProviderRequest {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ProviderToolSpec> tools;
    int max_tokens = 32000;

    std::string auth_header;
    auth::Style auth_style = auth::Style::ApiKey;
};

using EventSink = std::function<void(Msg)>;

// ── The contract every provider satisfies ──────────────────────────────────
template <class P>
concept Provider = requires(P& p, ProviderRequest req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};

} // namespace moha::io
