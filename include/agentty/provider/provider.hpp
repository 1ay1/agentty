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
#include <string_view>
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

    // Whether the user wants the model's reasoning/thinking SHOWN (the global
    // Settings.show_reasoning, ^R in the model picker). When true the Anthropic
    // transport requests VISIBLE thinking (adds the interleaved-thinking beta,
    // WITHOUT redact-thinking) so its thinking deltas reach the wire instead of
    // being redacted. Other transports already stream their reasoning text
    // unconditionally; this flag doesn't change their request — it only gates
    // the DISPLAY, which the view handles. Off by default (dead-air-free wire).
    bool show_reasoning = false;

    // Stable host-conversation identity. Providers with native server-side
    // threads (notably Codex app-server) use this to retain the provider
    // thread instead of serialising the entire transcript into every prompt.
    // Empty means the caller has no durable conversation identity.
    std::string session_key;
};

using EventSink = std::function<void(Msg)>;

// ── Uniform request lowering ─────────────────────────────────────────────
//
// `provider::Request` is the SUPERSET request every adapter receives. Each
// HTTP transport still has its own local `Request` (anthropic::Request,
// openai::Request) carrying just the fields its wire needs. Historically each
// adapter lowered provider::Request into its local one by hand, field by
// field — and that is exactly how fields got silently dropped (openai copied
// context_window; anthropic forgot session_key).
//
// `lower_shared` copies the fields EVERY transport shares, in one place, by
// (member-of-dst = member-of-src) pairs guarded so a typo can't compile. An
// adapter becomes: `lower_shared(treq, req);` then set the 1-2 fields unique
// to that wire (endpoint, json_protocol). Add a shared field here once and
// every transport inherits it — no adapter can forget it.
//
// `effort` is shared too: it's the model-agnostic reasoning tier (already
// gated to "" by effort_wire_for when the model can't reason). Each wire
// merely ENCODES it differently (Anthropic thinking.budget_tokens, OpenAI-Chat
// reasoning_effort, Responses reasoning.effort) — the VALUE is single-source.
// (The Responses transport reads req.effort straight off the abstract Request
// and never calls lower_shared, so it isn't listed as a consumer here.)
//
// Templated on the destination so it works for any transport Request that
// exposes the same-named members; the requires-clause makes a missing member a
// crisp error at the call site instead of deep in instantiation.
template <class TReq>
    requires requires(TReq t, Request r) {
        t.model = std::move(r.model);
        t.system_prompt = std::move(r.system_prompt);
        t.messages = std::move(r.messages);
        t.tools = std::move(r.tools);
        t.max_tokens = r.max_tokens;
        t.auth = std::move(r.auth);
        t.retry_count = r.retry_count;
        t.effort = std::move(r.effort);
    }
void lower_shared(TReq& dst, Request& src) {
    dst.model         = std::move(src.model);
    dst.system_prompt = std::move(src.system_prompt);
    dst.messages      = std::move(src.messages);
    dst.tools         = std::move(src.tools);
    dst.max_tokens    = src.max_tokens;
    dst.auth          = std::move(src.auth);
    dst.retry_count   = src.retry_count;
    dst.effort        = std::move(src.effort);
    // Optional field: only the Anthropic transport needs show_reasoning (to
    // request VISIBLE thinking); other transports stream reasoning text
    // unconditionally and don't declare it. Copy iff the destination has it,
    // so this stays the ONE lowering site without forcing every wire's local
    // Request to grow a field it ignores.
    if constexpr (requires { dst.show_reasoning = src.show_reasoning; })
        dst.show_reasoning = src.show_reasoning;
}

// How a streamed turn ended, as a value (defined in stream_epilogue.hpp, which
// every transport already includes to end its turn). Forward-declared here so
// the concept can name it without this header pulling the epilogue in.
struct StreamResult;

// ── The contract every provider satisfies ────────────────────────────
// `stream` drives one turn: it pushes streamed Msgs through `sink` AND returns
// a StreamResult naming how the turn ended. The two never disagree — the
// terminal Msg is derived from the same classification as the return value.
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<StreamResult>;
};

} // namespace agentty::provider
