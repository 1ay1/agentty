#pragma once
// agentty::provider::openai — the wire layer that talks to any
// OpenAI-compatible Chat Completions endpoint (OpenAI, Groq, OpenRouter,
// Together, Cerebras, a local Ollama / llama.cpp server, …).
//
// Why "OpenAI-compatible" is the right abstraction boundary: every one of
// those backends speaks POST /v1/chat/completions with `stream: true` and
// emits the SAME SSE `data: {choices:[{delta:{...}}]}` frame shape. So a
// single transport, parameterised on a base URL + bearer key + model id,
// lights up the whole family. The differences collapse to configuration:
//
//   • base URL   — api.openai.com / api.groq.com / openrouter.ai / localhost
//   • auth       — Authorization: Bearer <key>  (Ollama: none)
//   • model id   — provider-specific string
//
// The transport translates the abstract provider::Request into the OpenAI
// JSON body, reads the SSE stream, and dispatches the SAME agentty Msgs the
// Anthropic transport does (StreamTextDelta / StreamToolUseStart|Delta|End /
// StreamUsage / StreamFinished / StreamError / StreamHeartbeat). The runtime,
// tool loop, retry/stall watchdog and persistence are all provider-agnostic —
// they only ever see those Msgs.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::provider::openai {

using auth::AuthHeader;
using auth::BearerHeader;
using auth::ApiKeyHeader;
using auth::is_empty;

// Endpoint description for an OpenAI-compatible backend. Constructed once at
// startup (from settings / env / --provider) and threaded into every request.
//
//   host        — TLS SNI + Host header + cert pin (e.g. "api.openai.com").
//   port        — usually 443; Ollama default is 11434 over plain HTTP.
//   path        — chat completions path, default "/v1/chat/completions".
//   models_path — model listing path, default "/v1/models".
//   use_tls     — false for a local http:// Ollama / llama.cpp server.
//   label       — provider tag stamped onto ModelInfo (for the picker).
struct Endpoint {
    std::string host        = "api.openai.com";
    std::uint16_t port      = 443;
    std::string path        = "/v1/chat/completions";
    std::string models_path = "/v1/models";
    bool        use_tls     = true;
    std::string label       = "openai";
    // When true, use Ollama's NATIVE /api/chat protocol (NDJSON stream,
    // structured message.tool_calls) instead of the OpenAI-compat
    // /v1/chat/completions shim. The shim makes weak local models leak
    // tool calls as raw JSON in `content` (even on a bare "hi"); the native
    // endpoint applies the model's chat template and answers cleanly. Set
    // for the "ollama" preset only.
    bool        native_api  = false;

    // Built-in presets for the common free / hosted backends. Pass a bare
    // name ("openai", "groq", "openrouter", "together", "cerebras",
    // "ollama") or a full "host[:port]" string.
    [[nodiscard]] static Endpoint from_spec(std::string_view spec);
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<provider::ToolSpec> tools;
    int max_tokens;
    AuthHeader auth;
    int retry_count = 0;
    Endpoint endpoint;
    // First-class weak-model support (agent-zero style). When true the Ollama
    // transport does NOT send a native `tools` array; instead it inlines the
    // tool catalog into the system prompt and instructs the model to answer
    // with ONE JSON object `{"thoughts":[...], "tool_name":"...",
    // "tool_args":{...}}`. Tiny models (<=8B) follow "emit one JSON object"
    // far more reliably than the native function-call channel, which they
    // tend to ignore or fill with malformed JSON. The salvage/extraction
    // path (forgiving first-`{`..last-`}` parse with tool_name/tool_args
    // aliases) then turns that object into a real tool call. Set per-model
    // by launch_stream for weak Ollama models.
    bool json_protocol = false;
};

using EventSink = std::function<void(Msg)>;

// Runs a streaming chat-completions request synchronously on the calling
// thread. Each SSE delta is translated into an agentty Msg via `sink`.
// Returns when the stream closes. `cancel` is polled at frame boundaries.
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel = {});

// Build the OpenAI-shaped `messages` array from our Thread. Exposed for tests.
[[nodiscard]] nlohmann::json build_messages(const Thread& t);

// Translate our provider::ToolSpec list into OpenAI `tools: [{type:function,
// function:{name, description, parameters}}]`. Exposed for tests.
[[nodiscard]] nlohmann::json build_tools(const std::vector<provider::ToolSpec>& tools);

// Fetch available models from the endpoint's /v1/models.
[[nodiscard]] std::vector<ModelInfo> list_models(const AuthHeader& auth,
                                                 const Endpoint& endpoint);

// Extra system-prompt guidance appended ONLY for OpenAI-compatible backends.
// Weak local models (Ollama qwen2.5-coder, llama.cpp templates) over-call
// tools (e.g. firing `remember` at a bare "hi") and leak calls as content
// text instead of the structured channel. This addendum nudges them to chat
// in plain text when no tool is needed and to emit one well-formed call when
// one is. Hosted OpenAI/Groq models ignore it harmlessly.
[[nodiscard]] std::string_view local_model_prompt_addendum();

// Full slim system prompt for local / OpenAI-compat models. Replaces (does
// NOT append to) the hosted Claude prompt: a short decision-first instruction
// set + environment block + the user's CLAUDE.md memory tiers + skills
// catalog. The verbose Claude agentic prose is omitted because it primes small
// models to over-call tools and leak them as content text.
[[nodiscard]] std::string local_model_system_prompt();

// Test-only: feed a complete OpenAI SSE byte buffer through the same parser
// the live stream uses and collect every dispatched Msg. Lets a unit test
// verify the delta→Msg translation (text, tool-call assembly, finish_reason,
// usage, [DONE]) without a network round-trip.
[[nodiscard]] std::vector<Msg> parse_sse_for_test(
    std::string_view sse_bytes,
    std::vector<std::string> known_tools = {});

// Same, for the native Ollama /api/chat NDJSON path (feed_ndjson).
[[nodiscard]] std::vector<Msg> parse_ndjson_for_test(
    std::string_view ndjson_bytes,
    std::vector<std::string> known_tools = {});

} // namespace agentty::provider::openai
