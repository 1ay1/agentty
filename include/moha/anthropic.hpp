#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/auth.hpp"
#include "moha/model.hpp"
#include "moha/msg.hpp"

namespace moha::anthropic {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    int max_tokens = 8192;

    // Auth — caller fills this from auth::resolve().
    std::string auth_header;                 // "Bearer <t>" or raw API key
    auth::Style auth_style = auth::Style::ApiKey;
};

using EventSink = std::function<void(Msg)>;

// Runs a streaming request synchronously on the calling thread. Each SSE event
// is dispatched through `sink` as a Msg (StreamStarted / StreamTextDelta /
// StreamToolUse* / StreamUsage / StreamFinished / StreamError). Returns when
// the stream closes. Designed to be driven by maya's Cmd::task worker so that
// every `sink()` call routes straight through maya's BackgroundQueue.
void run_stream_sync(Request req, EventSink sink);

// Build the Anthropic-shaped messages array from our Thread.
nlohmann::json build_messages(const Thread& t);

// Standard system prompt with env info.
std::string default_system_prompt();

// Tool specs corresponding to our local tool implementations.
std::vector<ToolSpec> default_tools();

} // namespace moha::anthropic
