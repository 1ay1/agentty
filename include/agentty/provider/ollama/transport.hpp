#pragma once
// agentty::provider::ollama — dedicated transport for a local Ollama server.
//
// Ollama is NOT just an OpenAI-compatible endpoint. Its native /api/chat
// protocol (the one Ollama itself documents and Zed's provider speaks) is a
// better fit: it applies the model's own chat template, returns STRUCTURED
// `message.tool_calls` (no leaked-JSON-in-content guessing), accepts
// per-request `options.num_ctx` / `num_predict`, keeps the model resident via
// `keep_alive`, and streams NDJSON (one JSON object per line) rather than SSE.
//
// This module is modelled on zed-industries/zed crates/ollama/src/ollama.rs:
//   POST {base}/api/chat   {model, messages, stream, keep_alive, options, tools, think}
//   NDJSON: {"message":{role,content,tool_calls,images,thinking}, "done":bool,
//            "done_reason", "prompt_eval_count", "eval_count"}
//
// Tool calls arrive on TWO channels, by model strength:
//   1. Capable models  — native structured `message.tool_calls` (trusted as-is).
//   2. Weak models      — JSON-protocol mode (agent-zero style): no native
//      `tools` array is sent; the tool catalog is inlined in the system prompt
//      and the model answers with ONE {thoughts, tool_name, tool_args} object,
//      which the forgiving extractor turns into a real call. Plus a salvage
//      net for models that leak a tool-call JSON into `content` despite the
//      native channel. launch_stream picks the channel per model id
//      (is_weak_model) so tiny local models (3B/7B) work first-class.
//
// The transport reuses openai::Endpoint / openai::Request (so provider
// selection plumbing is unchanged — Ollama is still a Kind::OpenAI preset with
// native_api=true) and dispatches the SAME agentty Msgs every other provider
// does.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/io/http.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::provider::ollama {

using openai::Endpoint;
using openai::Request;
using EventSink = std::function<void(Msg)>;

// Run a streaming /api/chat request synchronously on the calling thread.
// Each NDJSON frame is translated into an agentty Msg via `sink`. Returns when
// the stream closes. `cancel` is polled at frame boundaries.
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel = {});

// System prompt tuned for local models served by Ollama. Plain, firm, with an
// explicit "use earlier messages" recall reminder and an environment block;
// includes the user's CLAUDE.md tiers but omits the verbose Claude agentic
// prose and the big learned-memory / skills dumps that bloat the prompt and
// confuse small models.
[[nodiscard]] std::string system_prompt();

// Build the native messages array from our Thread. Exposed for tests.
[[nodiscard]] nlohmann::json build_messages(const std::vector<Message>& msgs);

// Test-only: feed an NDJSON byte buffer through the live parser and collect
// every dispatched Msg (no network round-trip). Pass json_protocol=true to
// exercise the weak-model single-object protocol (no native tools array; the
// whole reply is one {tool_name,tool_args} object, possibly noise-wrapped).
[[nodiscard]] std::vector<Msg> parse_ndjson_for_test(
    std::string_view ndjson_bytes,
    std::vector<std::string> known_tools = {},
    bool json_protocol = false);

} // namespace agentty::provider::ollama
