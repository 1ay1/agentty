#pragma once
// agentty::provider::anthropic — the wire layer that talks to api.anthropic.com.
// Impersonates the Claude Code CLI on the wire so OAuth tokens are accepted;
// see memory/project_claude_code_wire.md for the full header/body contract.

#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/io/http.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::provider::anthropic {

// Re-export the typed credential from the auth domain. Keeping it in
// `auth::` (not duplicated here) lets the abstract `provider::Request`
// hold it without dragging an anthropic-specific header in.
using auth::AuthHeader;
using auth::ApiKeyHeader;
using auth::BearerHeader;
using auth::make_auth_header;
using auth::is_empty;

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    bool eager_input_streaming = false;
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    // Always set by AnthropicProvider from provider::Request (which owns the
    // default). No initializer here — keeping one risks the same silent-
    // override bug that masked the cap on long write/edit calls.
    int max_tokens;

    // Typed credential. The variant arm IS the header name; there's no
    // way to send (Bearer-token, "x-api-key") or vice versa because
    // build_request_headers does std::visit and the two arms emit
    // different header names.
    AuthHeader auth;

    // Per-turn retry attempt count, copied from provider::Request.
    // Stamped on the wire as x-stainless-retry-count. Default 0.
    int retry_count = 0;

    // Reasoning effort wire value ("low".."max"), copied from
    // provider::Request and already clamped to the model's capability.
    // Empty = no thinking (default). Non-empty makes run_stream_sync send
    // adaptive thinking + output_config.effort and replay thinking blocks.
    std::string effort;
};

using EventSink = std::function<void(Msg)>;

// Runs a streaming request synchronously on the calling thread. Each SSE event
// is dispatched through `sink` as a Msg. Returns when the stream closes.
// `cancel` is polled at frame boundaries; tripping it sends RST_STREAM and
// returns within ~200 ms with a StreamError("cancelled") if no cleaner
// terminal event has arrived first.
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel = {});

// Build the Anthropic-shaped messages array from our Thread.
[[nodiscard]] nlohmann::json build_messages(const Thread& t);

// Same as build_messages but emits the raw JSON string directly (hot
// path; avoids the json tree round-trip). Applies the per-result wire
// byte-budget cap on tool outputs. Exposed for tests.
[[nodiscard]] std::string messages_json_string(const Thread& t,
                                               bool include_thinking = false);

// Standard system prompt with env info.
[[nodiscard]] std::string default_system_prompt();

// Tool specs corresponding to our local tool implementations.
[[nodiscard]] std::vector<ToolSpec> default_tools();

// Fetch available models from Anthropic API. Takes the typed AuthHeader
// so the model-list endpoint shares the same header-vs-token discipline
// as the streaming path.
[[nodiscard]] std::vector<ModelInfo> list_models(const AuthHeader& auth);

} // namespace agentty::provider::anthropic
