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
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>  // only json signatures below; full type lives in transport.cpp

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
    // Custom auth header NAME (e.g. "X-API-Key") for gateways that don't
    // accept `Authorization: Bearer`. Empty (the default) keeps the standard
    // bearer header. When set, the key goes out raw as `<name>: <key>` —
    // no "Bearer " prefix. Populated from --auth-header via
    // provider::parse_selection.
    std::string auth_header_name;

    // Extra static request headers injected verbatim on EVERY request to this
    // endpoint (after the auth header). GitHub Copilot needs its editor-
    // identification block (Copilot-Integration-Id / Editor-Version / …) on
    // every call or the proxy returns 400/403. Empty for the plain OpenAI
    // family. Names should be lowercase (header names are case-insensitive and
    // the rest of agentty sends lowercase).
    std::vector<std::pair<std::string, std::string>> extra_headers;

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
    // Model's real context window (input+output token budget), e.g. 32768 for
    // qwen2.5-coder:7b. Probed from Ollama's /api/show model_info and carried
    // on ModelInfo.context_window; launch_stream copies it here. The Ollama
    // transport uses it to set `options.num_ctx` so a long agent conversation
    // isn't silently truncated to Ollama's tiny default window (~2k/4k). 0 =
    // unknown → transport falls back to a safe agent-sized default.
    int context_window = 0;
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
    // Stable per-conversation identity (the thread id). Hosted OpenAI-family
    // endpoints get it echoed as the `prompt_cache_key` body field so repeated
    // turns of the same conversation route to the same automatic-prompt-cache
    // node (higher cache-hit rate on the shared system+tools+history prefix).
    // Empty / local (Ollama) endpoints omit it.
    std::string session_key;
    // Reasoning effort wire value ("low"|"medium"|"high"|"xhigh"|"max"), or
    // empty for no reasoning. Copied by provider::lower_shared. The OpenAI-Chat
    // transport encodes it as top-level `reasoning_effort` when non-empty;
    // already gated upstream (effort_wire_for returns "" for models without
    // effort support), so the transport needs no capability re-check. Ollama
    // ignores it (local models have no server-side effort knob).
    std::string effort;
    // Whether the user wants the model's reasoning SHOWN (global ^R toggle).
    // Mirrors provider::Request.show_reasoning and anthropic::Request.
    // show_reasoning so every transport reads the same intent. On the OpenAI-
    // Chat wire reasoning text streams unconditionally (reasoning_content), so
    // this only gates the DISPLAY (handled by the view). The Ollama transport
    // uses it to decide whether to send `think:true` (enable the native
    // reasoning field). Off by default keeps the dead-air-free wire unchanged.
    bool show_reasoning = false;
};

using EventSink = std::function<void(Msg)>;

// Runs a streaming chat-completions request synchronously on the calling
// thread. Each SSE delta is translated into an agentty Msg via `sink`.
// Returns a StreamResult naming how the turn ended. `cancel` is polled at
// frame boundaries.
provider::StreamResult run_stream_sync(Request req, EventSink sink,
                                       http::CancelTokenPtr cancel = {});

// Build the OpenAI-shaped `messages` array from our Thread. Exposed for tests.
[[nodiscard]] nlohmann::json build_messages(const Thread& t);

// Translate our provider::ToolSpec list into OpenAI `tools: [{type:function,
// function:{name, description, parameters}}]`. Exposed for tests.
[[nodiscard]] nlohmann::json build_tools(const std::vector<provider::ToolSpec>& tools);

// Fetch available models from the endpoint's /v1/models.
//
// `force_probe` runs the runtime-window routes (/props, /api/ps,
// /api/v1/models) even when the address does not look self-hosted. Used at
// ADD-HOST time, where the user is waiting on an answer about this one
// host and a few extra milliseconds buy the difference between a real
// window and a guess. Routine refreshes leave it false so a hosted API
// never pays for routes it does not serve.
[[nodiscard]] std::vector<ModelInfo> list_models(const AuthHeader& auth,
                                                 const Endpoint& endpoint,
                                                 bool force_probe = false);

// The context window a /v1/models row DECLARES, or 0 when it declares none.
//
// Exposed for tests because the 0 is a contract, not an implementation
// detail: it is what distinguishes "the gateway told us" from "nobody
// knows", and every downstream behaviour hangs off that distinction — the
// picker column only says "auto" on 0, and resolve_context_window() only
// falls through to models.dev / id inference / the 200k default on 0. A
// shape that silently resolves to a number nobody declared is the bug this
// returns 0 to prevent.
//
// What a row declares is not always what the server will honour, so a
// non-zero answer here is not the end of the story. For a LOCAL endpoint
// list_models still probes and takes the smaller of the two: LM Studio's
// /v1 rows advertise `max_context_length` (what the model architecture
// supports) while the instance may be loaded at a fraction of it, and
// llama.cpp's rows carry both the served `meta.n_ctx` and the much larger
// `meta.n_ctx_train`. Declared is a ceiling; probed is the measurement.
[[nodiscard]] int advertised_context_window(const nlohmann::json& model_row);

// Should this endpoint get the full runtime-window probe?
//
// "Local" means A SERVER YOU RUN, not a server on this machine: loopback,
// RFC1918, link-local, CGNAT, IPv6 ULA, *.local, a bare single-label host,
// or anything listed in AGENTTY_PROBE_HOSTS. Everything else is assumed to
// be somebody else's API, where the probe's routes are guaranteed 404s.
//
// Exposed because it decides NETWORK BEHAVIOUR and the UI needs to explain
// it: when a host answers but reports no window, whether we ASKED is the
// difference between "this server doesn't say" and "we didn't check".
namespace detail {
[[nodiscard]] bool is_local_endpoint(const Endpoint& endpoint);
}

// Tell the transport which hosts the user opted into probing.
//
// Pushed IN from the runtime (Settings::probe_hosts) rather than read out,
// because this layer takes no dependency on the settings store — that is
// what lets a transport be constructed from just an Endpoint and an auth
// header, and tested without a filesystem. Call on startup and whenever the
// set changes; replaces the previous set wholesale.
void install_probe_hosts(std::set<std::string> hosts);

// ── Custom-host dialect probe ───────────────────────────────────
// One call answers "what is actually running at this endpoint?" before a
// custom host is committed. Tries, in order:
//   1. the endpoint's configured models_path      (explicit prefix honoured)
//   2. /v1/models                                 (the OpenAI-dialect default)
//   3. /api/tags                                  (Ollama's native protocol)
// and reports which one answered, how many models it listed, and the round
// trip time. The connect flow uses it for instant modal feedback ("✓ 12
// models · openai · 45ms" vs "✗ nothing listening") and to auto-correct the
// endpoint (adopt the answering prefix; flip native_api for a bare Ollama
// daemon) — the SOTA onboarding move: detect, don't interrogate the user.
struct HostProbe {
    enum class Dialect : std::uint8_t {
        None,          // nothing answered — host down / wrong port
        OpenAiCompat,  // an OpenAI-shape /models answered
        OllamaNative,  // /api/tags answered (bare Ollama daemon)
    };

    // WHY the probe failed, which is not derivable from http_status alone.
    //
    // Three very different problems used to arrive as the same message:
    //
    //   • 200 + HTML   a web app is serving this path. Pasting the dashboard
    //                  URL instead of the API base is the single most common
    //                  custom-host mistake, and `https://host/models` on a
    //                  real provider genuinely returns 200 text/html —
    //                  verified on yolo-auto.com. The old code saw 200, then
    //                  failed to parse, and reported "HTTP 200 — no model
    //                  list at any known path", which tells the user nothing
    //                  about the actual mistake.
    //   • 200 + JSON   reachable and speaking JSON, but not a model list.
    //                  Usually a gateway that needs a different prefix.
    //   • 401/403      the endpoint is CORRECT and wants a key. This is a
    //                  success for "did I type the host right?" and must
    //                  never read as "nothing there".
    //
    // Each one has a different fix, so each one needs a different sentence.
    enum class Failure : std::uint8_t {
        None,          // it worked
        Unreachable,   // connect failed: wrong host, wrong port, server down
        NeedsKey,      // 401/403 — right endpoint, missing credentials
        NotAnApi,      // 200 but HTML: this is a web page, not an API base
        NoModelList,   // 200 + JSON, but no recognisable model list
        HttpError,     // some other non-200
    };

    Dialect     dialect      = Dialect::None;
    Failure     failure      = Failure::None;
    std::string models_path;     // the path that answered ("" when None)
    int         model_count  = 0;
    int         http_status  = 0;   // last status seen (0 = connect failure)
    long        latency_ms   = 0;   // round trip of the answering request

    [[nodiscard]] bool ok() const noexcept { return dialect != Dialect::None; }

    // One sentence the user can act on. Kept HERE, next to the taxonomy, so
    // the message and the classification cannot drift apart — and so the TUI
    // and any future CLI surface read identically.
    [[nodiscard]] std::string explain() const;
};
[[nodiscard]] HostProbe probe_host(const AuthHeader& auth,
                                   const Endpoint& endpoint);

// ── The request body, as a pure function ─────────────────────────────────
//
// The exact JSON agentty puts on the wire, derived from a Request and
// nothing else — no sink, no socket, no globals.
//
// WHY IT IS OUT HERE: it used to be built inline inside the streaming
// function, so the only way to see what we send was a wire=trace log on a
// live call. That made "which fields go to which endpoints" a question you
// answered by reading a hot path, and left conformance.hpp's tiers as
// documentation with nothing enforcing them. As a pure function the body is
// assertable in a unit test, which is what turns those tiers from a comment
// into a contract.
//
// The tier rules it implements (evidence for each is in conformance.hpp):
//   Universal  model, stream, max_tokens, stream_options, tools
//   Hosted     prompt_cache_key    — TLS only; local servers reject it
//   Probed     reasoning_effort    — arrives pre-gated by the catalog
[[nodiscard]] nlohmann::json build_request_body(const Request& req);

// Common request headers (accept/content-type/user-agent + auth). The auth
// header is `authorization: Bearer <key>` unless `endpoint.auth_header_name`
// is set, in which case the key goes out raw under that name. Exposed for
// tests.
[[nodiscard]] http::Headers build_request_headers(const AuthHeader& auth,
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
    std::vector<std::string> known_tools = {},
    bool allow_memory_salvage = false,
    bool reason_by_default = false,
    // Mirrors Request::show_reasoning. Defaulted true so existing tests keep
    // capture semantics; the conformance suite drives both values.
    bool show_reasoning = true);

// Same, for the native Ollama /api/chat NDJSON path (feed_ndjson).
[[nodiscard]] std::vector<Msg> parse_ndjson_for_test(
    std::string_view ndjson_bytes,
    std::vector<std::string> known_tools = {},
    bool allow_memory_salvage = false);

} // namespace agentty::provider::openai
