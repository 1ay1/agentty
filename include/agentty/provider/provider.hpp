#pragma once
// agentty::provider — the abstraction over "something that streams a chat
// completion".  A Provider is domain, not infrastructure: the conversation
// speaks to a Provider, and a Provider happens to speak HTTP+SSE to an
// Anthropic endpoint (or, in tests, to a deterministic in-memory script).
//
// The runtime never names a concrete type; anything satisfying the concept
// is accepted.

#include <concepts>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/io/http.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::provider {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // Anthropic's fine-grained tool streaming flag — see ToolDef in
    // tool/registry.hpp for the full story. Mirrored on the wire as
    // `eager_input_streaming: true` per tool when set.
    bool eager_input_streaming = false;
};

// Per-request output cap that matches Claude Code v2.1.113's main-loop
// config. The binary's docs explicitly warn that `max_tokens > ~16000`
// puts traffic on a slower path that risks SDK HTTP timeouts — an earlier
// 64000 default was correlated with 20-30 s mid-stream pauses on long
// write/edit calls.
//
// Trade-off: a single tool_use whose `content` field exceeds ~12-13k
// tokens of file body will hit the cap and arrive truncated (model burns
// its budget streaming input_json_delta, then stop_reason=max_tokens).
// For most edits/writes this is fine; bump per-request for known-huge
// generations.
inline constexpr int kSafeMaxTokens = 16384;

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    int max_tokens = kSafeMaxTokens;

    // Model's real context window (input+output token budget). Used by the
    // Ollama transport to set options.num_ctx so long agent conversations
    // aren't silently truncated to Ollama's tiny default window. 0 = unknown
    // (the transport falls back to a safe agent-sized default). Hosted
    // providers ignore it. Set by launch_stream from the selected ModelInfo.
    int context_window = 0;

    // Typed credential — the variant arm names the wire header. See
    // agentty/auth/auth.hpp for the AuthHeader definition; transports
    // never see a loose (header, style) pair.
    auth::AuthHeader auth;

    // Optional cancellation handle. Tripping the token from the UI thread
    // tears down the in-flight stream within ~200 ms. Null means uncancellable.
    http::CancelTokenPtr cancel;

    // 0-based count of prior failed attempts for THIS turn (the source
    // ctx's transient_retries). Historically surfaced on the wire as
    // x-stainless-retry-count — that header (part of the Anthropic JS SDK
    // fingerprint) is NO LONGER sent, since agentty identifies as itself
    // rather than impersonating the SDK/CLI. Retained for internal retry
    // bookkeeping and available to any provider that wants it.
    int retry_count = 0;

    // Weak-model JSON-protocol mode (agent-zero style). Set by launch_stream
    // for tiny local models on the Ollama native endpoint: the transport
    // drops the native `tools` array and instead inlines the tool catalog
    // into the prompt, expecting ONE {tool_name,tool_args} JSON object back.
    // Ignored by providers that don't implement it (Anthropic, OpenAI-compat).
    bool json_protocol = false;

    // Reasoning effort (output_config.effort wire value: "low" | "medium" |
    // "high" | "xhigh" | "max"). Empty = omit — no thinking, the default.
    // Already clamped to the model's capability by launch_stream (see
    // effort_wire_for). The Anthropic transport, when this is non-empty,
    // additionally enables adaptive thinking. Other transports ignore it.
    std::string effort;
};

using EventSink = std::function<void(Msg)>;

// ── The contract every provider satisfies ──────────────────────────────────
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};

} // namespace agentty::provider
