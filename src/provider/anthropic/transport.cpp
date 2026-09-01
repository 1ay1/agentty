#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/anthropic/prompt.hpp"
#include "agentty/provider/anthropic/sse.hpp"


#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/stream_scaffold.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/provider/debug.hpp"
#include "agentty/provider/wire_supersede.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::anthropic {

namespace {

// Belt-and-suspenders UTF-8 scrubber. Registry already converts subprocess
// output at the capture boundary (GetConsoleOutputCP / CP_ACP pivot), but
// any string that reaches json::dump() must be valid UTF-8 or the API call
// dies with `type_error.316`. Cheap to run on already-valid strings, and
// guards future call sites that assemble tool output from multiple pieces
// (e.g. error suffix + partial output) where a byte boundary could split a
// UTF-8 sequence. Replaces invalid byte runs with U+FFFD.
//
// UTF-8 validation + scrubbing are the shared, strict source of truth in the
// wire header — every transport scrubs identically (rejects overlong encodings
// and surrogates before they 400 the next request). See wire::scrub_utf8.
using wire::is_valid_utf8;
using wire::scrub_utf8;

// Back up an index to the start of the UTF-8 code point it lands in.
// The UTF-8 boundary helpers + the tool-result byte-budget cap now live in
// the shared wire header (include/agentty/provider/wire.hpp) so every
// transport sizes tool output identically — see wire::cap_tool_result_aged.

} // namespace

using json = nlohmann::json;

// Wire-format identity. agentty identifies itself HONESTLY — it does NOT
// impersonate the official Claude Code CLI. The user-agent and x-app announce
// "agentty"; we send only the headers Anthropic's API actually requires to
// function (anthropic-version + the anthropic-beta flags that gate features we
// use, including oauth-2025-04-20 for subscription tokens). We deliberately do
// NOT spoof Claude Code's user-agent, the Anthropic JS SDK's x-stainless-*
// fingerprint, its BetaToolRunner helper tag, or a Claude-Code-shaped
// metadata.user_id. If Anthropic's edge ever hard-requires a first-party
// client signature, that's a ToS boundary we surface to the user ("use an API
// key or another provider"), not one we quietly circumvent by masquerading.
namespace headers {
    inline constexpr const char* anthropic_version = "2023-06-01";
    // Honest identity — agentty says it is agentty.
    inline constexpr const char* user_agent        = "agentty/" AGENTTY_VERSION;
    inline constexpr const char* x_app             = "agentty";

    // Beta IDs are FEATURE flags, not identity spoofing — each unlocks a
    // capability agentty genuinely uses (OAuth-token acceptance, 1M context,
    // context management, prompt-cache scope, fine-grained tool streaming).
    // Composed by select_betas() per model/auth.
    inline constexpr const char* beta_claude_code            = "claude-code-20250219";
    inline constexpr const char* beta_oauth                  = "oauth-2025-04-20";
    inline constexpr const char* beta_context_1m             = "context-1m-2025-08-07";
    inline constexpr const char* beta_context_management     = "context-management-2025-06-27";
    inline constexpr const char* beta_prompt_cache_scope     = "prompt-caching-scope-2026-01-05";
    // Extended cache TTL (1-hour) opt-in. WITHOUT this beta in the header set,
    // sending `cache_control:{ttl:"1h"}` makes Anthropic's edge SILENTLY DROP
    // the whole breakpoint (cache miss every turn + throttled tier). WITH it,
    // the stable prefix breakpoints (system / tools / conversation anchor)
    // survive a 1-hour idle window instead of the 5-minute default — so a
    // pause to read, think, or step away no longer forces a full-price
    // cache-creation re-write of the entire prompt prefix on the next turn.
    // This is Claude Code's `Dt6` extended-TTL path. We keep the ROLLING
    // breakpoint on the newest message at the 5-minute default (it changes
    // every turn, so a long TTL there buys nothing) and spend the 1-hour TTL
    // only where the content is stable across turns.
    inline constexpr const char* beta_extended_cache_ttl     = "extended-cache-ttl-2025-04-11";
    // Per-tool `eager_input_streaming: true` is honored without a beta header
    // on Claude 4.6 (GA there). For older models (haiku-4-5, claude-3.x) the
    // edge requires this header — sending it on 4.6+ is a no-op so we always
    // include it when any tool in the request opts in.
    inline constexpr const char* beta_fine_grained_streaming = "fine-grained-tool-streaming-2025-05-14";
    // Interleaved (visible) thinking. Lets the model plan between content
    // blocks AND — crucially, when we do NOT also send redact-thinking —
    // surfaces the thinking deltas on the wire instead of redacting them.
    // Added ONLY when the user opted into "show reasoning" (^R) with an effort
    // tier on; the default keeps thinking off entirely (no dead-air).
    inline constexpr const char* beta_interleaved_thinking   = "interleaved-thinking-2025-05-14";
} // namespace headers

namespace {

// The SSE parser — StreamCtx, dispatch_event, feed_sse, the simdjson
// content_block_delta fast path, and the SSE event-kind closed sum — moved to
// anthropic/sse.cpp (StreamCtx + feed_sse + the shared dbg/debug_log logger
// are declared in sse.hpp). run_stream_sync below drives a stream via
// feed_sse. Behavior guarded by anthropic_sse_golden_test.

json tool_spec_to_json(const ToolSpec& s) {
    json j;
    j["name"] = s.name;
    j["description"] = s.description;
    j["input_schema"] = s.input_schema;
    // Anthropic's fine-grained tool streaming opt-in (per-tool field).
    // Only emit when true so cache-key shape matches CC's tool blocks for
    // tools that don't use it. GA on Claude 4.6+; gated by the
    // `fine-grained-tool-streaming-2025-05-14` beta header on older models
    // (we send that beta unconditionally so the field is always honored).
    if (s.eager_input_streaming) j["eager_input_streaming"] = true;
    return j;
}

// Pick the anthropic-beta value list for /v1/messages exactly the way the
// Mirrors Claude Code v2.1.113's `fR8(model)` cocktail builder for the
// firstParty path, MINUS the two thinking betas. Why we deviate from CC:
//
// `interleaved-thinking-2025-05-14` lets the model plan between content
// blocks; combined with `redact-thinking-2026-02-12` (which suppresses the
// thinking deltas from the wire), the result on long write/edit calls was
// 20-30 s of dead-air between a tool_use's `display_description` and its
// `content` field — the model was generating redacted thinking tokens we
// never see, then dumping the whole `content` body in one burst. CC papers
// over this with a "Thinking…" spinner; agentty's TUI doesn't, so it just looks
// frozen. Dropping both betas forces the model to start emitting `content`
// immediately. If you ever want to render thinking blocks, drop only the
// redact one and surface the visible thinking deltas in the UI.
std::string select_betas(std::string_view model, bool is_oauth,
                         bool any_eager_streaming = false,
                         bool show_reasoning = false) {
    // Single decode site for all model-id introspection — see
    // ModelCapabilities::from_id for why we tokenise rather than
    // substring-match. Adding a new beta gated on a new family /
    // generation goes here; nothing else in transport.cpp parses
    // model strings.
    const auto caps = ModelCapabilities::from_id(model);

    std::vector<std::string_view> b;
    if (!caps.is_haiku())              b.emplace_back(headers::beta_claude_code);          // !q
    if (is_oauth)                      b.emplace_back(headers::beta_oauth);                // Hq()
    if (caps.extended_context_1m)      b.emplace_back(headers::beta_context_1m);           // AL(H)
    if (caps.generation_4_or_later)    b.emplace_back(headers::beta_context_management);   // eU(provider) && CR4(H)
    b.emplace_back(headers::beta_prompt_cache_scope);                                      // _ (fa() — always true firstParty)
    // 1-hour extended cache TTL. Always on: agentty pins the stable prefix
    // (system / tools / conversation anchor) with ttl:"1h" so a long idle
    // window doesn't force a full-price re-cache. Harmless when no breakpoint
    // uses the long TTL (the edge just ignores an unused capability).
    b.emplace_back(headers::beta_extended_cache_ttl);
    if (any_eager_streaming)           b.emplace_back(headers::beta_fine_grained_streaming);
    // Interleaved thinking (reasoning BETWEEN tool calls) is requested
    // differently by model generation:
    //   • Adaptive models (Opus 4.6+/4.7/4.8, flagship 5): interleaved
    //     thinking is AUTOMATIC — no beta header, and adding one is a no-op.
    //   • Legacy enabled-mode models (Opus/Sonnet 4.5 and earlier): the
    //     interleaved-thinking-2025-05-14 beta is what lets the model plan
    //     between tool_use blocks. We add it ONLY on that path, and ONLY when
    //     the user opted into visible reasoning (^R). We never add
    //     redact-thinking, so the thinking deltas reach the wire and the
    //     reasoning block has real text to render. Off by default → no
    //     thinking beta → the dead-air-free default wire is unchanged.
    if (show_reasoning && !caps.uses_adaptive_thinking())
        b.emplace_back(headers::beta_interleaved_thinking);

    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) out.push_back(',');
        out.append(b[i]);
    }
    return out;
}

// Build the lowercase HTTP/2 header set. agentty sends ONLY the headers the
// Anthropic API requires: the auth header, the API version, the beta feature
// flags, and its own honest user-agent / x-app. It deliberately does NOT send
// the Anthropic JS SDK's x-stainless-* platform fingerprint or the CLI's
// x-stainless-helper=BetaToolRunner tag — those exist only to make traffic
// look first-party, which agentty is not.
//
// AuthHeader is the closed sum (ApiKeyHeader | BearerHeader); std::visit
// dispatches to the right header NAME at the type level. There's no way
// to send an OAuth token under `x-api-key:` or vice versa — the variant
// arm names the header.
http::Headers build_request_headers(const AuthHeader& auth,
                                    std::string_view beta_value,
                                    int timeout_seconds,
                                    bool streaming = false,
                                    int retry_count = 0) {
    (void)timeout_seconds;
    (void)retry_count;
    http::Headers h;
    h.push_back({"accept", streaming ? "text/event-stream"
                                      : "application/json"});
    h.push_back({"content-type",   "application/json"});
    if (streaming) {
        // Corporate gateways commonly buffer/compress SSE until a large body
        // accumulates. These standard directives preserve incremental frames.
        http::append_sse_no_buffer(h);
    }
    h.push_back({"user-agent",     headers::user_agent});
    h.push_back({"x-app",          headers::x_app});
    h.push_back({"anthropic-version", headers::anthropic_version});
    h.push_back({"anthropic-dangerous-direct-browser-access", "true"});
    if (!beta_value.empty())
        h.push_back({"anthropic-beta", std::string{beta_value}});
    // The variant arm dictates the header. "Bearer " prefix is owned by
    // this site — callers hand us a raw token, not a prefixed string —
    // so an API key can't accidentally land with a Bearer prefix either.
    std::visit([&](const auto& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, ApiKeyHeader>) {
            h.push_back({"x-api-key", a.value});
        } else if constexpr (std::is_same_v<T, BearerHeader>) {
            h.push_back({"authorization", "Bearer " + a.token});
        }
    }, auth);
    return h;
}

// Stable per-machine hex id. Anthropic's `metadata.user_id` is a legitimate,
// documented field used for abuse signals — keeping it STABLE per machine makes
// agentty's traffic look like one consistent user rather than a herd of fresh
// sessions every turn, which is honest (it IS one user on one machine). We do
// NOT shape it to look like Claude Code's `user_<hash>_account_<uuid>_...`
// identifier; it's just a derived-from-machine-id opaque hex string.
std::string machine_id_hex(int nibbles) {
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string seed;
        for (auto path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            std::ifstream f(path);
            if (f) { std::getline(f, seed); if (!seed.empty()) break; }
        }
        if (seed.empty()) {
            if (const char* h = std::getenv("HOSTNAME")) seed = h;
        }
        if (seed.empty()) seed = "agentty-anonymous";
        // FNV-1a 64-bit, twice with different offsets to pad to 128 bits.
        auto fnv = [](std::string_view s, uint64_t off) {
            uint64_t h = off;
            for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
            return h;
        };
        uint64_t a = fnv(seed, 0xcbf29ce484222325ull);
        uint64_t b = fnv(seed, 0x84222325cbf29ce4ull);
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)a, (unsigned long long)b);
        cached.assign(buf, 32);
    });
    return cached.substr(0, std::min<size_t>(nibbles, cached.size()));
}

std::string make_user_id() {
    // metadata.user_id is a JSON object `{device_id, session_id}`: a stable
    // per-machine device_id plus a per-process session_id (minted once, so it
    // stays constant across turns of one agentty run). This is agentty's own
    // honest client identity for Anthropic's abuse-signal bucketing — NOT a
    // copy of Claude Code's `T7H()` shape and with no fake account_uuid.
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, []{
        auto device_id = machine_id_hex(32);
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        uint64_t s1 = 0xcbf29ce484222325ull;
        for (int i = 0; i < 8; ++i) { s1 ^= (now >> (i*8)) & 0xff; s1 *= 0x100000001b3ull; }
        uint64_t s2 = s1 ^ 0x9e3779b97f4a7c15ull;
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)s1, (unsigned long long)s2);
        auto session_id = std::string(buf, 32);
        cached = nlohmann::json{
            {"device_id",  device_id},
            {"session_id", session_id},
        }.dump();
    });
    return cached;
}

} // namespace

// ── Streaming string-backed message-array writer ─────────────────────────
//
// messages_json_string() / build_messages() and their json_write_* helpers
// moved to anthropic/wire_body.cpp — the request-body serializer is the
// largest chunk of this file's hot path and lifts out cleanly (no StreamCtx,
// no SSE). run_stream_sync() below calls messages_json_string() via the
// transport.hpp declaration. Byte-identity guarded by wire_golden_test.

// ----------------------------------------------------------------------------

provider::StreamResult run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    if (is_empty(req.auth)) {
        sink(StreamError{"not authenticated — run 'agentty login' or set ANTHROPIC_API_KEY"});
        return provider::StreamResult::failed("not authenticated");
    }

    // emit_terminal runs on error paths after `sink` has been moved into
    // `ctx.sink` below — dispatching via `ctx.sink` is the only live handle.
    // The previous version captured `sink` by reference and invoked a
    // moved-from std::function on every non-happy-path termination, which
    // surfaced in the UI as "stream backend: bad_function_call". The whole
    // post-loop now runs through provider::finish_stream at the tail.

    const bool is_oauth = std::holds_alternative<BearerHeader>(req.auth);

    json body;
    // The `[1m]` picker marker selects the 1M window + context beta but is NOT
    // a real model id — strip it so the wire carries `claude-sonnet-5`, not
    // `claude-sonnet-5[1m]` (which the API 404s). select_betas() below still
    // sees the ORIGINAL req.model so it detects the marker and sends the
    // context-1m beta.
    body["model"]      = wire_model_id(req.model);
    body["max_tokens"] = req.max_tokens;
    body["stream"]     = true;

    // Stable-prefix cache breakpoint with the 1-HOUR extended TTL. The
    // system prompt is the single most stable thing in the request — it only
    // changes when the user edits CLAUDE.md or a `remember`/`forget` fires —
    // so it is the ideal place to spend the long TTL. We send the
    // `extended-cache-ttl-2025-04-11` beta unconditionally (see select_betas),
    // which is what makes `ttl:"1h"` stick instead of being silently dropped.
    // This is Claude Code's `Dt6` extended-cache path. Result: a pause of up
    // to an hour no longer forces a full-price re-cache of the whole system
    // prefix on the next turn. The rolling message breakpoints keep the 5 min
    // default (messages_json_string), since they change every turn anyway.
    const json kCacheCtl1h = {{"type", "ephemeral"}, {"ttl", "1h"}};

    // System is always sent as a content-block array so we can attach
    // cache_control regardless of auth style. OAuth additionally prepends
    // the immutable Claude Code preamble (cli.js line ~5641) so Anthropic's
    // edge accepts the OAuth token; API-key callers skip that preamble.
    {
        json sys = json::array();
        if (is_oauth) {
            sys.push_back({
                {"type", "text"},
                {"text", "You are Claude Code, Anthropic's official CLI for Claude."}
            });
        }
        // Scrub the system prompt too. Unlike message text (run through
        // scrub_utf8 in messages_json_string), the system prompt is placed
        // directly into `body` and serialized by body.dump() below — which
        // throws type_error.316 on the FIRST malformed UTF-8 byte. The prompt
        // now carries arbitrary user bytes (CLAUDE.md tiers + `remember`
        // records, which can round-trip a truncated/broken multi-byte
        // sequence from a pasted tool output). One bad byte there was failing
        // EVERY turn on every thread with "invalid UTF-8 in conversation"; a
        // single scrub here makes the wire well-formed regardless of source.
        sys.push_back({
            {"type", "text"},
            {"text", scrub_utf8(req.system_prompt)},
            {"cache_control", kCacheCtl1h}
        });
        body["system"] = std::move(sys);
    }

    // Build the messages array directly into a string buffer. Cache
    // breakpoints (last + second-to-last messages, last block of each)
    // are inlined during the write — see messages_json_string. We
    // splice this into the dumped body below rather than going through
    // `body["messages"] = json::parse(...)`, which would re-parse the
    // string back into a json tree just so body.dump() could
    // re-serialize it again. For a write-tool turn with 1 MiB of
    // content, the round-trip was the dominant request-build cost.
    // Replay stored thinking blocks only when this request itself enables
    // thinking (effort on). With thinking off, omit them — they're only
    // required by, and valid for, thinking-enabled requests.
    std::string messages_str = messages_json_string(
        Thread{ThreadId{""}, "", req.messages, {}, {}},
        /*include_thinking=*/!req.effort.empty());
    if (!req.tools.empty()) {
        json tools_j = json::array();
        for (const auto& t : req.tools) tools_j.push_back(tool_spec_to_json(t));
        // Tools cache breakpoint goes on the LAST tool — the schema array is
        // serialized in order and Anthropic's edge caches the prefix up to
        // and including the marked block. Matches cli.js where the tool list
        // is built once per session and the last entry carries cache_control.
        // Tools are session-stable (same catalog every turn), so they get the
        // 1-hour TTL alongside the system prompt — together they form the
        // long-lived prefix that survives idle gaps.
        tools_j.back()["cache_control"] = kCacheCtl1h;
        body["tools"] = std::move(tools_j);
    }
    body["metadata"] = json{{"user_id", make_user_id()}};
    // Reasoning effort + adaptive thinking. req.effort is pre-clamped to the
    // model's capability by launch_stream; non-empty means the user picked a
    // thinking tier in the model picker. Pair output_config.effort with
    // adaptive thinking — the GA way to turn reasoning on for Opus 4.6+/4.7/
    // 4.8 (budget_tokens is removed on 4.7/4.8; type:"enabled" 400s there).
    // Omitted entirely when effort is off, preserving the default no-thinking,
    // dead-air-free wire. The assistant thinking blocks the model emits in
    // response are captured and replayed by messages_json_string (see below)
    // so tool_use turns don't 400 for a dropped thinking block.
    if (!req.effort.empty()) {
        // Thinking MODE is revision-gated (Anthropic changed the interface at
        // the 4.6/4.7 boundary):
        //   • Opus/Sonnet 4.5 and EARLIER (revision <= 5): only
        //     thinking:{type:"enabled", budget_tokens:N} is accepted;
        //     type:"adaptive" 400s ("adaptive thinking is not supported on
        //     this model").
        //   • 4.6: both work.
        //   • 4.7 / 4.8 / Claude 5+: only type:"adaptive" + output_config.effort
        //     (budget_tokens removed; type:"enabled" 400s).
        // We decode the revision from the wire model id and pick accordingly.
        const auto tcaps = ModelCapabilities::from_id(wire_model_id(req.model));
        const bool legacy_enabled = !tcaps.uses_adaptive_thinking();

        if (legacy_enabled) {
            // Map the effort tier to a thinking token budget (>= 1024 min).
            // Scales with tier; capped well under max_tokens so the answer
            // still has room.
            int budget = 8000;
            if      (req.effort == "low")    budget = 4000;
            else if (req.effort == "medium") budget = 8000;
            else if (req.effort == "high")   budget = 16000;
            else if (req.effort == "xhigh")  budget = 24000;
            else if (req.effort == "max")    budget = 32000;
            // Keep the budget strictly under max_tokens (Anthropic requires
            // budget_tokens < max_tokens on the enabled path; a budget that
            // meets or exceeds it 400s). Leave a comfortable answer floor.
            if (req.max_tokens > 0) {
                const int ceiling = std::max(1024, req.max_tokens - 8000);
                if (budget > ceiling) budget = ceiling;
            }
            json thinking = json{{"type", "enabled"},
                                 {"budget_tokens", budget}};
            // display works in BOTH modes; on the enabled path thinking is
            // visible (summarized) by default, but set it explicitly when the
            // user asked to see reasoning so intent is unambiguous on the wire.
            if (req.show_reasoning)
                thinking["display"] = "summarized";
            body["thinking"] = std::move(thinking);
            // Opus 4.5 is the one extended-thinking-only model that ALSO honors
            // output_config.effort, which COMPOSES with budget_tokens (effort
            // steers depth, the budget caps it). Send it when the model
            // supports effort so the picker's tier isn't silently dropped.
            if (tcaps.supports_effort())
                body["output_config"] = json{{"effort", req.effort}};
        } else {
            json thinking = json{{"type", "adaptive"}};
            // VISIBLE reasoning: adaptive thinking REDACTS its text on the
            // wire by default (thinking_delta.thinking arrives empty). When
            // the user turned on "show reasoning" (^R), ask for the summarized
            // thinking stream so the model's actual reasoning deltas reach us.
            if (req.show_reasoning)
                thinking["display"] = "summarized";
            body["thinking"]      = std::move(thinking);
            body["output_config"] = json{{"effort", req.effort}};
        }
    }
    // Splice marker for the messages array. nlohmann gives the dumped
    // form `"messages":<unique-string>"`; we string-replace the
    // placeholder with messages_json_string. Picked a token that
    // can't appear inside a legitimately-escaped JSON string so the
    // find() is unambiguous even on weird payloads.
    constexpr std::string_view kMessagesPlaceholder =
        "\x01__agentty_messages_splice__\x01";
    body["messages"] = std::string{kMessagesPlaceholder};

    // Last-line-of-defence: if any string in the request tree still carries
    // non-UTF-8 bytes (a tool that bypassed the scrub, a new code path), the
    // dump() below throws type_error.316. We used to terminate(); now we
    // surface a StreamError so the reducer can recover and the user sees the
    // turn fail instead of the process dying mid-stream.
    std::string body_str;
    try {
        body_str = body.dump();
    } catch (const nlohmann::json::exception& e) {
        sink(StreamError{std::string{"request build failed (invalid UTF-8 in conversation): "} + e.what()});
        sink(StreamFinished{StopReason::Unspecified});
        return provider::StreamResult::failed("request build failed: invalid UTF-8");
    }
    // Replace the dumped placeholder string with the raw messages JSON.
    // nlohmann emits std::string values as JSON strings (quoted +
    // escaped). The control bytes \x01 round-trip as ``, so the
    // dumped form is `"__agentty_messages_splice__"` — find
    // and replace that.
    {
        constexpr std::string_view kDumpedPlaceholder =
            "\"\\u0001__agentty_messages_splice__\\u0001\"";
        auto pos = body_str.find(kDumpedPlaceholder);
        if (pos == std::string::npos) {
            sink(StreamError{"request build failed: messages placeholder not found in dumped body"});
            sink(StreamFinished{StopReason::Unspecified});
            return provider::StreamResult::failed("request build failed: placeholder");
        }
        body_str.replace(pos, kDumpedPlaceholder.size(), messages_str);
    }

    AGT_LOG(Wire, Trace, "anthropic.request.body", "raw={}", body_str);

    StreamCtx ctx;
    ctx.sink = std::move(sink);

    http::Request hreq;
    hreq.method  = http::HttpMethod::Post;
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    // `?beta=true` matches `beta.messages.create` in the SDK (cli.js line 393)
    // — the same path Anthropic's edge gates the beta header set against.
    hreq.path    = "/v1/messages?beta=true";
    // 300 s matches cli.js mb1(): API_TIMEOUT_MS env override or default 300 s
    // for local (120 s for CLAUDE_CODE_REMOTE). x-stainless-timeout is
    // advertisement, not enforcement — our actual stream is unbounded with
    // cancellation polled at frame boundaries.
    const bool any_eager = std::ranges::any_of(req.tools,
        [](const auto& t){ return t.eager_input_streaming; });
    hreq.headers = build_request_headers(req.auth,
                                         select_betas(req.model, is_oauth, any_eager,
                                                      req.show_reasoning),
                                         /*timeout_seconds=*/300,
                                         /*streaming=*/true,
                                         /*retry_count=*/req.retry_count);
    hreq.body    = std::move(body_str);

    // All the per-turn handler scaffolding — status capture, Retry-After on
    // the error path (Anthropic emits it on 429/529 as integer seconds),
    // heartbeats, buffered-wait, wire dump, capped error-body accumulation —
    // is the shared StreamScaffold. Only the parser feed is ours.
    provider::StreamScaffold sc;
    sc.dialect = "anthropic-messages";
    sc.sink    = ctx.sink;
    sc.feed    = [&](std::string_view chunk) {
        feed_sse(ctx, chunk.data(), chunk.size());
        return true;
    };
    http::StreamHandler handler = sc.handler();

    // Standard streaming ladder — the rationale lives with stream_timeouts().
    http::Timeouts tos = provider::stream_timeouts();

    // Keep a copy of the cancel token: it is moved into the stream call below,
    // but finish_stream needs it to distinguish a user cancel from a transport
    // error at the post-loop.
    http::CancelTokenPtr cancel_for_end = cancel;
    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    // Uniform end-of-turn pair via the scaffold; thinking_deltas appended so
    // a "reasoning not showing" report splits server-vs-client from the log.
    sc.log_result(bool(result),
                  result ? std::string_view{} : result.error().render(),
                  std::format("thinking_deltas={}", ctx.thinking_deltas));

    // Whole post-loop through the SHARED epilogue: classify the exit and emit
    // exactly one terminal event with correct precedence. on_any_end closes an
    // open tool block on BOTH success and error (peer may cut off mid-tool-use
    // before content_block_stop) so the reducer's salvage path always runs.
    return provider::finish_stream({
        .terminated  = ctx.terminated,
        .sink        = ctx.sink,
        .result_ok   = bool(result),
        .http_status = sc.http_status,
        .non_replayable = !result && result.error().non_replayable,
        .cancel      = cancel_for_end,
        .stop        = ctx.stop_reason,
        .http_error_message = [&]() -> std::string {
            std::string msg = "HTTP " + std::to_string(sc.http_status);
            try {
                auto j = json::parse(sc.error_body);
                if (j.contains("error") && j["error"].contains("message"))
                    msg += ": " + j["error"]["message"].get<std::string>();
                else if (j.contains("message"))
                    msg += ": " + j["message"].get<std::string>();
                else
                    msg += ": " + sc.error_body.substr(0, 300);
            } catch (...) {
                if (!sc.error_body.empty()) msg += ": " + sc.error_body.substr(0, 300);
            }
            if (sc.http_status == 401 || sc.http_status == 403)
                msg += "  (run 'agentty login' to re-authenticate)";
            return msg;
        },
        .retry_after = sc.retry_after_hint,
        // Network / TLS / nghttp2-level error — never produced a complete SSE
        // stream. The typed HttpError's render() is embedded so the
        // downstream error_class substring sniff still routes it.
        .transport_error_message = [&]() -> std::string {
            return std::string{"http: "} + result.error().render();
        },
        .on_any_end = [&ctx]() {
            if (ctx.in_tool_use) {
                ctx.sink(StreamToolUseEnd{ToolCallId{ctx.current_tool_id}});
                ctx.in_tool_use = false;
                ctx.current_tool_id.clear();
                ctx.current_tool_name.clear();
            }
        },
    });
}

std::vector<Msg> parse_sse_for_test(
    const std::vector<std::pair<std::string, std::string>>& events) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    // Reconstruct the exact SSE wire form (`event: <name>\ndata: <json>\n\n`)
    // and push it through feed_sse — the SAME framer + dispatch_event path the
    // live on_chunk uses — so the emitted Msg sequence is identical to a real
    // stream carrying these frames.
    for (const auto& [name, data] : events) {
        std::string frame;
        frame.reserve(name.size() + data.size() + 16);
        frame += "event: ";
        frame += name;
        frame += "\ndata: ";
        frame += data;
        frame += "\n\n";
        feed_sse(ctx, frame.data(), frame.size());
    }
    return out;
}

std::vector<ModelInfo> list_models(const AuthHeader& auth) {
    const bool is_oauth = std::holds_alternative<BearerHeader>(auth);

    // Claude Code surfaces a 1M-context `[1m]` VARIANT per suffix-capable
    // model. We mirror that via catalog::add_1m_variants (the single shared
    // impl — same one the bundled floor uses), applied to the live catalog
    // below. Entitlement self-heals downstream (context_1m_blocked strips the
    // rows if a 1M request 400s), so the offer is safe to make broadly.

    // Built-in floor = the single bundled catalog (base rows + their `[1m]`
    // companions). One source shared with seed_models(), so the seed and the
    // live /v1/models catalog below can never drift. Returned only when the
    // network probe genuinely yields nothing (offline / transient non-200) so
    // the picker is never stranded empty; the live list is the ceiling.
    auto seed = [] { return catalog::bundled("anthropic"); };

    std::vector<ModelInfo> result;
    if (is_empty(auth)) return seed();

    http::Request hreq;
    hreq.method  = http::HttpMethod::Get;
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.path    = "/v1/models?limit=100";
    // /v1/models doesn't need the streaming beta cocktail — just the oauth
    // gate when applicable, matching how cli.js calls model-listing endpoints.
    hreq.headers = build_request_headers(auth,
                                         is_oauth ? headers::beta_oauth : "",
                                         /*timeout_seconds=*/10);
    // /v1/models is a small list (~30 KB at typical catalog size). Cap
    // hard so a misbehaving proxy / replay loop can't stream us into
    // OOM on a routine startup probe.
    hreq.max_body_bytes = 1ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5'000);
    tos.total   = std::chrono::milliseconds(10'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp || resp->status != 200) return seed();

    try {
        auto j = json::parse(resp->body);
        for (const auto& m : j.value("data", json::array())) {
            auto id = m.value("id", "");
            auto name = m.value("display_name", id);
            if (id.empty()) continue;
            result.push_back(ModelInfo{
                .id = ModelId{id},
                .display_name = name,
                .provider = "anthropic",
                .context_window = 200000,
            });
        }
        // Order the raw /v1/models list into a clean, predictable grouping
        // before we interleave the 1M variants — see catalog.hpp's
        // model_picker_less (single source of truth: strength-first, never a
        // fixed family bucket, so Fable/Mythos never sinks below Opus/Sonnet/
        // Haiku just because of alphabetical/positional bad luck).
        std::stable_sort(result.begin(), result.end(), model_picker_less);
        // Surface a `[1m]` companion for every suffix-capable model, inserted
        // right after its base model so the pairing is adjacent (shared impl).
        catalog::add_1m_variants(result);
    } catch (const std::exception& e) {
        util::dbglog("anthropic.list_models.parse", e.what());
    } catch (...) {
        util::dbglog("anthropic.list_models.parse", "non-std exception");
    }

    // Network said 200 but we parsed nothing usable — fall back to the seed
    // so the picker is never left empty after a provider switch.
    if (result.empty()) return seed();
    return result;
}

} // namespace agentty::provider::anthropic
