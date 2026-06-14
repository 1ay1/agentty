// agentty::provider::openai — OpenAI-compatible Chat Completions transport.
//
// Mirrors the Anthropic transport's structure (SSE parser → StreamCtx →
// dispatch → the SAME agentty Msgs) but speaks the OpenAI wire format:
//
//   POST {base}/v1/chat/completions   {model, messages, tools, stream:true}
//   SSE: data: {"choices":[{"delta":{...},"finish_reason":...}], "usage":...}
//   data: [DONE]   terminates the stream.
//
// The hard part vs. Anthropic is the tool-call streaming shape. OpenAI streams
// `delta.tool_calls: [{index, id, function:{name, arguments}}]` where:
//   • the first delta for a given `index` carries id + function.name,
//   • subsequent deltas for the same index carry `function.arguments`
//     fragments (a partial JSON string) and omit id/name,
//   • there is no explicit "tool call done" event — a call closes when a
//     NEW index appears, or when the stream finishes / finish_reason arrives.
//
// We translate that into agentty's block model: StreamToolUseStart on first
// sight of an index, StreamToolUseDelta per arguments fragment, and
// StreamToolUseEnd when the index is superseded or the stream ends.

#include "agentty/provider/openai/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/util/base64.hpp"

namespace agentty::provider::openai {

using json = nlohmann::json;

namespace {

// ── UTF-8 scrub (same defence as the Anthropic transport) ───────────────────
// A tool output or pasted blob can carry invalid UTF-8; nlohmann's dump()
// throws on it. Replace every malformed byte with U+FFFD so the request
// builds instead of the turn dying with a json type_error.
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    auto push_replacement = [&] { out.append("\xEF\xBF\xBD"); };
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++p; continue; }
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
        if (extra < 0 || p + extra >= end) { push_replacement(); ++p; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k)
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { push_replacement(); ++p; continue; }
        out.append(reinterpret_cast<const char*>(p), extra + 1);
        p += extra + 1;
    }
    return out;
}

// ── Per-tool-call streaming accumulator ─────────────────────────────────────
// One slot per OpenAI tool_calls[].index. We need to remember the id+name we
// saw on the opening delta so StreamToolUseEnd / salvage paths have them.
struct ToolCallSlot {
    std::string id;
    std::string name;
    bool started = false;   // StreamToolUseStart already emitted
};

struct StreamCtx {
    EventSink sink;

    // SSE line buffer + parse cursor (same amortised-drain scheme as anthropic).
    std::string buf;
    std::size_t read_pos = 0;
    std::string data_accum;
    bool        skip_event = false;

    // Tool-call streaming state, indexed by OpenAI's tool_calls[].index.
    std::vector<ToolCallSlot> tool_slots;
    int  active_tool_index = -1;     // the index currently open, -1 = none
    bool in_tool_use = false;
    bool any_structured_tool = false; // a real tool_calls[] delta arrived

    // ── Leaked-tool-call salvage (weak local models) ────────────────────
    // Some Ollama/llama.cpp models (e.g. qwen2.5-coder:7b) emit a tool call
    // as a bare {"name":..,"arguments":..} JSON in `content` instead of the
    // structured `tool_calls[]` channel. We HOLD leading text that could
    // still be such a JSON (rather than streaming it then retracting), and
    // at finish either convert it to a real tool call or flush it as text.
    std::string text_hold;          // buffered leading text under suspicion
    bool        holding   = true;   // still might be a leaked tool-call JSON
    bool        any_text_flushed = false;
    int         salvage_seq = 0;    // uniquifies synthesised salvage call ids
    std::vector<std::string> known_tools;  // tool names we may salvage to

    StopReason stop_reason = StopReason::Unspecified;
    bool terminated = false;

    StreamCtx() { buf.reserve(64 * 1024); data_accum.reserve(8 * 1024); }
};

// Could `s` (so far) be the START of a bare tool-call JSON object? We only
// keep holding while the text looks like it's heading toward one; the moment
// it can't be, we stop holding and flush as ordinary prose. Cheap structural
// check: ignore leading whitespace, require a leading '{', and require that
// every non-whitespace char seen is plausible JSON-object material.
[[nodiscard]] bool could_be_tool_json(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'
                            || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size()) return true;          // only whitespace so far
    return s[i] == '{';                       // a JSON object must open with {
}

constexpr std::size_t kSseCompactThreshold = 64 * 1024;
constexpr std::size_t kSseDataAccumMax     = 4 * 1024 * 1024;

[[nodiscard]] StopReason parse_openai_finish(std::string_view fr) noexcept {
    // OpenAI finish_reason: "stop" | "length" | "tool_calls" |
    // "content_filter" | "function_call" (legacy). Map onto agentty's enum.
    if (fr == "stop")           return StopReason::EndTurn;
    if (fr == "length")         return StopReason::MaxTokens;
    if (fr == "tool_calls")     return StopReason::ToolUse;
    if (fr == "function_call")  return StopReason::ToolUse;
    return StopReason::Unspecified;
}

// Close the currently-open tool call, if any, with a StreamToolUseEnd.
void close_active_tool(StreamCtx& ctx) {
    if (ctx.in_tool_use) {
        ctx.sink(StreamToolUseEnd{});
        ctx.in_tool_use = false;
        ctx.active_tool_index = -1;
    }
}

// True iff the held buffer opens like a bare tool-call JSON object but is
// INCOMPLETE — it begins with `{` (after optional whitespace / a ```json
// fence) yet does not parse as a complete JSON value. That is the signature of
// a tool call the model leaked into `content` whose wire cut off mid-body
// ("upstream cut off"). A COMPLETE object that simply named an unadvertised
// tool is NOT incomplete — it still surfaces as text so the user sees what the
// model meant.
[[nodiscard]] bool hold_is_truncated_tool_json(std::string_view sv) noexcept {
    auto ltrim = [](std::string_view& s) {
        while (!s.empty() && (s.front()==' '||s.front()=='\t'
                              ||s.front()=='\n'||s.front()=='\r')) s.remove_prefix(1);
    };
    ltrim(sv);
    if (sv.starts_with("```")) {
        sv.remove_prefix(3);
        if (sv.starts_with("json")) sv.remove_prefix(4);
        ltrim(sv);
    }
    if (sv.empty() || sv.front() != '{') return false;
    try { (void)json::parse(sv); return false; }   // complete — keep as text
    catch (...) { return true; }                    // truncated — drop
}

// Emit any held text as ordinary prose and stop holding. Called when the
// held buffer can no longer be a leaked tool-call JSON, or at finish when
// salvage did not apply.
//
// IMPORTANT: if the hold opens like a tool-call object but is INCOMPLETE (the
// wire cut off mid-`content`, so it can't parse), we DROP it instead of
// flushing it as visible prose. Dumping a half-written
// `{"name":"remember","arguments":{...` into the assistant body both shows JSON
// garbage AND round-trips back to a weak local model (qwen/llama.cpp), which
// then re-leaks the same call next turn — the stuck "upstream cut off"
// re-invocation. A COMPLETE object (even one naming an unadvertised tool) is
// kept and surfaced so the user sees the model's intent.
void flush_text_hold(StreamCtx& ctx) {
    ctx.holding = false;
    if (ctx.text_hold.empty()) return;
    if (hold_is_truncated_tool_json(ctx.text_hold)) {
        ctx.text_hold.clear();   // truncated leaked tool call — drop
        return;
    }
    ctx.sink(StreamTextDelta{ctx.text_hold});
    ctx.any_text_flushed = true;
    ctx.text_hold.clear();
}

// At finish: if the model leaked a tool call into `content` (no structured
// tool_calls arrived, and the held text is exactly one {"name","arguments"}
// object naming a known tool), synthesise a real tool call. Returns true if
// the held text was consumed as a tool call (caller must NOT also flush it).
[[nodiscard]] bool try_salvage_tool_call(StreamCtx& ctx) {
    if (ctx.any_structured_tool) return false;
    if (ctx.text_hold.empty())   return false;

    // Trim surrounding whitespace; some models wrap the JSON in a fenced
    // ```json block — strip a single leading/trailing fence if present.
    std::string_view sv{ctx.text_hold};
    auto ltrim = [](std::string_view& s) {
        while (!s.empty() && (s.front()==' '||s.front()=='\t'
                              ||s.front()=='\n'||s.front()=='\r')) s.remove_prefix(1);
    };
    auto rtrim = [](std::string_view& s) {
        while (!s.empty() && (s.back()==' '||s.back()=='\t'
                              ||s.back()=='\n'||s.back()=='\r')) s.remove_suffix(1);
    };
    ltrim(sv); rtrim(sv);
    if (sv.starts_with("```")) {
        sv.remove_prefix(3);
        if (sv.starts_with("json")) sv.remove_prefix(4);
        ltrim(sv);
        if (auto p = sv.rfind("```"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        rtrim(sv);
    }
    if (sv.empty() || sv.front() != '{') return false;

    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
        return false;

    std::string name = j["name"].get<std::string>();
    // Only salvage to a tool we actually advertised — never invent a call.
    bool known = false;
    for (const auto& t : ctx.known_tools) if (t == name) { known = true; break; }
    if (!known) return false;

    // arguments may be an object (qwen) or a JSON string (some templates).
    std::string args = "{}";
    if (j.contains("arguments")) {
        const auto& a = j["arguments"];
        if (a.is_string())      args = a.get<std::string>();
        else if (a.is_object()) args = a.dump();
    } else if (j.contains("parameters") && j["parameters"].is_object()) {
        args = j["parameters"].dump();   // some templates use "parameters"
    }

    // Uniquify the id — two leaked calls in one turn (or a leaked call plus a
    // structured one) must not collide on a single ToolCallId, or the reducer
    // tool-use state machine keys both onto the same card and the second
    // appears as a duplicate stuck Pending.
    std::string call_id = "call_salvaged_" + std::to_string(ctx.salvage_seq++);
    ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
    ctx.sink(StreamToolUseDelta{args});
    ctx.sink(StreamToolUseEnd{});
    ctx.stop_reason = StopReason::ToolUse;
    ctx.text_hold.clear();
    ctx.holding = false;
    return true;
}

// Handle one choices[0].delta object.
void handle_delta(StreamCtx& ctx, const json& delta) {
    // Plain assistant text.
    if (delta.contains("content") && delta["content"].is_string()) {
        const auto& s = delta["content"].get_ref<const std::string&>();
        if (!s.empty()) {
            if (ctx.holding) {
                ctx.text_hold += s;
                // Stop holding the moment it can't be a leading tool-call
                // JSON — then this delta (and all after) stream as prose.
                if (!could_be_tool_json(ctx.text_hold))
                    flush_text_hold(ctx);
            } else {
                ctx.sink(StreamTextDelta{s});
                ctx.any_text_flushed = true;
            }
        }
    }

    // Tool-call fragments.
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        // A real structured tool call wins over any leaked-JSON suspicion:
        // drop the held text (it was never prose) and disable salvage.
        if (!delta["tool_calls"].empty()) {
            ctx.any_structured_tool = true;
            ctx.holding = false;
            ctx.text_hold.clear();
        }
        for (const auto& tc : delta["tool_calls"]) {
            const int index = tc.value("index", 0);
            if (index < 0) continue;
            if (static_cast<std::size_t>(index) >= ctx.tool_slots.size())
                ctx.tool_slots.resize(index + 1);
            auto& slot = ctx.tool_slots[index];

            // Opening fragment for this index carries id + function.name.
            if (tc.contains("id") && tc["id"].is_string())
                slot.id = tc["id"].get<std::string>();
            std::string fn_name;
            std::string fn_args;
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    fn_name = fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string())
                    fn_args = fn["arguments"].get<std::string>();
            }
            if (!fn_name.empty()) slot.name = fn_name;

            // A new index means the previous tool call is finished.
            if (index != ctx.active_tool_index) {
                close_active_tool(ctx);
            }

            // Emit Start the first time we have enough to open this index.
            // OpenAI guarantees id+name on the FIRST fragment of each call,
            // so by the time we see arguments we already have them. Some
            // OpenAI-compatible servers (Ollama, llama.cpp) synthesise an
            // id only if asked; fall back to a positional id so the tool
            // loop still has a stable ToolCallId to key on.
            if (!slot.started) {
                if (slot.id.empty())
                    slot.id = "call_" + std::to_string(index);
                ctx.sink(StreamToolUseStart{ToolCallId{slot.id},
                                            ToolName{slot.name}});
                slot.started = true;
                ctx.in_tool_use = true;
                ctx.active_tool_index = index;
            }

            if (!fn_args.empty()) ctx.sink(StreamToolUseDelta{fn_args});
        }
    }
}

// Parse + dispatch one SSE `data:` payload.
void dispatch_data(StreamCtx& ctx, const std::string& data) {
    if (data.empty()) return;
    if (data == "[DONE]") {
        close_active_tool(ctx);
        if (!ctx.terminated) {
            // Salvage a leaked tool call (or flush held text as prose)
            // before the terminal event.
            if (!try_salvage_tool_call(ctx)) flush_text_hold(ctx);
            ctx.sink(StreamFinished{ctx.stop_reason});
            ctx.terminated = true;
        }
        return;
    }

    json j;
    try { j = json::parse(data); } catch (...) { return; }

    // Top-level error object (some servers stream an error frame mid-body).
    if (j.contains("error")) {
        std::string msg = "unknown error";
        if (j["error"].is_object())
            msg = j["error"].value("message", msg);
        else if (j["error"].is_string())
            msg = j["error"].get<std::string>();
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }

    // Usage can arrive on a final frame (when stream_options.include_usage
    // is set) OR be attached to the last choices frame.
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        StreamUsage su;
        su.input_tokens  = u.value("prompt_tokens", 0);
        su.output_tokens = u.value("completion_tokens", 0);
        // OpenAI's prompt_tokens_details.cached_tokens ≈ Anthropic's
        // cache_read. Surface it so the context gauge reflects cache hits.
        if (u.contains("prompt_tokens_details")
            && u["prompt_tokens_details"].is_object()) {
            su.cache_read_input_tokens =
                u["prompt_tokens_details"].value("cached_tokens", 0);
        }
        ctx.sink(su);
    }

    if (!j.contains("choices") || !j["choices"].is_array()
        || j["choices"].empty()) {
        return;
    }
    const auto& choice = j["choices"][0];

    if (choice.contains("delta") && choice["delta"].is_object())
        handle_delta(ctx, choice["delta"]);

    // finish_reason terminates the choice. Stash it for StreamFinished and
    // close any open tool call. We do NOT emit StreamFinished here — the
    // `[DONE]` sentinel (or emit_terminal at stream close) does, so a usage
    // frame after finish_reason still lands.
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        ctx.stop_reason = parse_openai_finish(
            choice["finish_reason"].get<std::string_view>());
        close_active_tool(ctx);
    }
}

// SSE line feeder. OpenAI streams `data: {json}\n\n` frames (no `event:`
// lines), so the parser is simpler than Anthropic's: accumulate `data:`
// lines, dispatch on the blank-line terminator.
void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
    ctx.buf.append(data, len);
    auto& read_pos = ctx.read_pos;
    std::string_view buf{ctx.buf};
    while (true) {
        const auto nl = buf.find('\n', read_pos);
        if (nl == std::string_view::npos) break;
        std::string_view line = buf.substr(read_pos, nl - read_pos);
        read_pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.empty()) {
            if (!ctx.skip_event && !ctx.data_accum.empty())
                dispatch_data(ctx, ctx.data_accum);
            ctx.data_accum.clear();
            ctx.skip_event = false;
        } else if (ctx.skip_event) {
            continue;
        } else if (line.starts_with("data:")) {
            std::size_t s = 5;
            while (s < line.size() && line[s] == ' ') ++s;
            const std::size_t add = (line.size() - s)
                + (ctx.data_accum.empty() ? 0 : 1);
            if (ctx.data_accum.size() + add > kSseDataAccumMax) {
                ctx.data_accum.clear();
                ctx.skip_event = true;
                continue;
            }
            if (!ctx.data_accum.empty()) ctx.data_accum.push_back('\n');
            ctx.data_accum.append(line.data() + s, line.size() - s);
        }
        // `:` comments and unknown fields silently dropped (SSE spec).
    }
    if (read_pos >= kSseCompactThreshold) {
        ctx.buf.erase(0, read_pos);
        read_pos = 0;
    }
}

// True iff an assistant message carries any tool_calls (whose results must
// follow as `role:"tool"` messages in OpenAI's format).
[[nodiscard]] bool is_assistant_with_results(const Message& m) noexcept {
    return m.role == Role::Assistant && !m.tool_calls.empty();
}

} // namespace

// ── Endpoint presets ────────────────────────────────────────────────────────
Endpoint Endpoint::from_spec(std::string_view spec) {
    auto eq = [](std::string_view a, const char* b) {
        return a == std::string_view{b};
    };
    if (spec.empty() || eq(spec, "openai")) {
        return Endpoint{"api.openai.com", 443, "/v1/chat/completions",
                        "/v1/models", true, "openai"};
    }
    if (eq(spec, "groq")) {
        return Endpoint{"api.groq.com", 443, "/openai/v1/chat/completions",
                        "/openai/v1/models", true, "groq"};
    }
    if (eq(spec, "openrouter")) {
        return Endpoint{"openrouter.ai", 443, "/api/v1/chat/completions",
                        "/api/v1/models", true, "openrouter"};
    }
    if (eq(spec, "together")) {
        return Endpoint{"api.together.xyz", 443, "/v1/chat/completions",
                        "/v1/models", true, "together"};
    }
    if (eq(spec, "cerebras")) {
        return Endpoint{"api.cerebras.ai", 443, "/v1/chat/completions",
                        "/v1/models", true, "cerebras"};
    }
    if (eq(spec, "ollama")) {
        return Endpoint{"localhost", 11434, "/v1/chat/completions",
                        "/v1/models", false, "ollama"};
    }
    // Treat anything else as a raw "host[:port]" — defaults to https on 443,
    // plain http if a non-443 port is given (a local server convention).
    Endpoint ep;
    ep.label = "openai-compatible";
    std::string s{spec};
    if (auto colon = s.rfind(':'); colon != std::string::npos) {
        ep.host = s.substr(0, colon);
        try { ep.port = static_cast<std::uint16_t>(std::stoi(s.substr(colon + 1))); }
        catch (...) { ep.port = 443; }
        ep.use_tls = (ep.port == 443);
    } else {
        ep.host = std::move(s);
        ep.port = 443;
        ep.use_tls = true;
    }
    return ep;
}

// ── Messages array (OpenAI shape) ───────────────────────────────────────────
//
// Differences from Anthropic:
//   • System prompt is a `role:"system"` message at the head, not a top-level
//     `system` field — handled in run_stream_sync, not here.
//   • Assistant tool calls live on the assistant message as
//     `tool_calls:[{id, type:"function", function:{name, arguments}}]`.
//   • Tool results are SEPARATE `role:"tool"` messages with a
//     `tool_call_id` — one per call, emitted right after the assistant
//     message that requested them.
//   • Images: OpenAI uses `content:[{type:"image_url", image_url:{url:
//     "data:<mime>;base64,<...>"}}]`.
json build_messages(const Thread& t) {
    json arr = json::array();
    for (const auto& m : t.messages) {
        const bool has_text   = !m.text.empty();
        const bool has_images = (m.role == Role::User && !m.images.empty());
        const bool has_tools  = is_assistant_with_results(m);

        if (has_text || has_images || has_tools) {
            json msg;
            msg["role"] = (m.role == Role::User) ? "user" : "assistant";

            // Expand chip placeholders into their bodies for the wire.
            std::string wire_text = m.attachments.empty()
                ? m.text
                : attachment::expand(m.text, m.attachments);
            wire_text = scrub_utf8(wire_text);

            if (has_images) {
                // Multimodal content array (text + image_url parts).
                json content = json::array();
                if (!wire_text.empty())
                    content.push_back({{"type", "text"}, {"text", wire_text}});
                for (const auto& img : m.images) {
                    std::string url = "data:" + img.media_type + ";base64,"
                                    + agentty::util::base64_encode(img.bytes);
                    content.push_back({{"type", "image_url"},
                                       {"image_url", {{"url", url}}}});
                }
                msg["content"] = std::move(content);
            } else {
                // Plain string content. OpenAI requires `content` present even
                // when an assistant message is pure tool_calls — use empty
                // string in that case (null is also accepted but "" is safer
                // across compatible servers).
                msg["content"] = wire_text;
            }

            if (has_tools) {
                json calls = json::array();
                for (const auto& tc : m.tool_calls) {
                    json fn;
                    fn["name"] = tc.name.value;
                    // OpenAI wants arguments as a STRING (serialized JSON).
                    fn["arguments"] = tc.args.is_null()
                        ? std::string{"{}"}
                        : tc.args.dump();
                    calls.push_back({
                        {"id", tc.id.value},
                        {"type", "function"},
                        {"function", std::move(fn)},
                    });
                }
                msg["tool_calls"] = std::move(calls);
            }
            arr.push_back(std::move(msg));
        }

        // Tool results as separate role:"tool" messages.
        if (has_tools) {
            for (const auto& tc : m.tool_calls) {
                // Send whatever output we have; non-terminal calls (rare on
                // the OpenAI path) still need a paired tool message or the
                // next request 400s on an unanswered tool_call_id.
                std::string out = tc.output();
                if (out.empty()) {
                    if (tc.is_rejected())      out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                }
                arr.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tc.id.value},
                    {"content", scrub_utf8(out)},
                });
            }
        }
    }
    return arr;
}

// ── Tools array (OpenAI function shape) ──────────────────────────────────────
json build_tools(const std::vector<provider::ToolSpec>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", t.name},
                {"description", t.description},
                {"parameters", t.input_schema},
            }},
        });
    }
    return arr;
}

// ── Header builder ───────────────────────────────────────────────────────────
namespace {
http::Headers build_request_headers(const AuthHeader& auth) {
    http::Headers h;
    h.push_back({"accept", "application/json"});
    h.push_back({"content-type", "application/json"});
    h.push_back({"user-agent", "agentty/" AGENTTY_VERSION});
    // Both header arms emit `Authorization: Bearer <token>` for the OpenAI
    // family — OpenAI/Groq/OpenRouter all use bearer keys. (ApiKeyHeader's
    // raw `sk-...` value goes out the same way; there's no `x-api-key` here.)
    std::visit([&](const auto& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, ApiKeyHeader>) {
            if (!a.value.empty())
                h.push_back({"authorization", "Bearer " + a.value});
        } else if constexpr (std::is_same_v<T, BearerHeader>) {
            if (!a.token.empty())
                h.push_back({"authorization", "Bearer " + a.token});
        }
    }, auth);
    return h;
}
} // namespace

// ── Streaming entry point ────────────────────────────────────────────────────
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    // Ollama and other local servers accept an empty key. Only error out when
    // the endpoint is a TLS/hosted one that needs auth.
    if (req.endpoint.use_tls && is_empty(req.auth)) {
        sink(StreamError{"not authenticated — set the provider's API key "
                         "(e.g. OPENAI_API_KEY) or run 'agentty login'"});
        return;
    }

    StreamCtx ctx;
    ctx.sink = std::move(sink);
    // Tools we advertised this turn — the salvage path only converts a
    // leaked-JSON "tool call" into a real one when it names one of these.
    ctx.known_tools.reserve(req.tools.size());
    for (const auto& t : req.tools) ctx.known_tools.push_back(t.name);
    auto emit_terminal = [](StreamCtx& c, std::optional<std::string> err,
                            std::optional<std::chrono::seconds> retry_after = {}) {
        if (c.terminated) return;
        if (c.in_tool_use) {
            c.sink(StreamToolUseEnd{});
            c.in_tool_use = false;
        }
        if (err) {
            c.sink(StreamError{*err, retry_after});
        } else {
            // Successful close without a [DONE] sentinel: still salvage a
            // leaked tool call (or flush held text) before finishing.
            if (!try_salvage_tool_call(c)) flush_text_hold(c);
            c.sink(StreamFinished{c.stop_reason});
        }
        c.terminated = true;
    };

    // ── Build the request body ──────────────────────────────────────────────
    json body;
    body["model"]  = req.model;
    body["stream"] = true;
    // max_tokens is `max_tokens` on the OpenAI chat endpoint (newer models
    // also accept max_completion_tokens; max_tokens stays accepted for the
    // whole compatible family, so use it for portability).
    body["max_tokens"] = req.max_tokens;
    // Ask for a usage frame on the final SSE event so the context gauge can
    // update even on streaming requests.
    body["stream_options"] = {{"include_usage", true}};

    // messages: system prompt first, then the conversation.
    json messages = json::array();
    if (!req.system_prompt.empty()) {
        messages.push_back({{"role", "system"},
                            {"content", scrub_utf8(req.system_prompt)}});
    }
    {
        json conv = build_messages(Thread{ThreadId{""}, "", req.messages, {}, {}});
        for (auto& m : conv) messages.push_back(std::move(m));
    }
    body["messages"] = std::move(messages);

    if (!req.tools.empty())
        body["tools"] = build_tools(req.tools);

    std::string body_str;
    try {
        body_str = body.dump();
    } catch (const nlohmann::json::exception& e) {
        ctx.sink(StreamError{std::string{"request build failed (invalid UTF-8): "}
                             + e.what()});
        ctx.sink(StreamFinished{StopReason::Unspecified});
        return;
    }

    // ── HTTP request ────────────────────────────────────────────────────────
    http::Request hreq;
    hreq.method  = http::HttpMethod::Post;
    hreq.host    = req.endpoint.host;
    hreq.port    = req.endpoint.port;
    hreq.path    = req.endpoint.path;
    hreq.plaintext = !req.endpoint.use_tls;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.headers = build_request_headers(req.auth);
    hreq.body    = std::move(body_str);

    int  http_status = 0;
    bool is_success  = false;
    std::string error_body;
    std::optional<std::chrono::seconds> retry_after_hint;

    http::StreamHandler handler;
    handler.on_headers = [&](int status, const http::Headers& hh) {
        http_status = status;
        is_success  = (status >= 200 && status < 300);
        if (is_success) return;
        auto eq_ci = [](std::string_view a, std::string_view b) noexcept {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
                if (x != y) return false;
            }
            return true;
        };
        for (const auto& h : hh) {
            if (!eq_ci(h.name, "retry-after")) continue;
            try {
                size_t consumed = 0;
                auto v = std::stoul(h.value, &consumed);
                if (consumed == h.value.size() && v > 0)
                    retry_after_hint = std::chrono::seconds(v);
            } catch (...) {}
            break;
        }
    };
    handler.on_chunk = [&](std::string_view chunk) -> bool {
        if (is_success) {
            feed_sse(ctx, chunk.data(), chunk.size());
        } else {
            if (error_body.size() < 64 * 1024)
                error_body.append(chunk.data(),
                    std::min(chunk.size(), 64 * 1024 - error_body.size()));
        }
        return true;
    };

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(0);   // streaming unbounded
    tos.ping    = std::chrono::milliseconds(15'000);
    tos.idle    = std::chrono::milliseconds(90'000);

    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    if (!result) {
        std::string msg = std::string{"http: "} + result.error().render();
        // Local backend unreachable — the daemon almost certainly isn't
        // running. Name the concrete fix; a bare connection-refused is
        // opaque to someone who just expected agentty to "work with ollama".
        if (!req.endpoint.use_tls)
            msg += "  (is the server running? start it with 'ollama serve', "
                   "or check the --provider host:port)";
        emit_terminal(ctx, std::move(msg));
        return;
    }

    if (!is_success) {
        std::string msg = "HTTP " + std::to_string(http_status);
        try {
            auto j = json::parse(error_body);
            if (j.contains("error") && j["error"].is_object()
                && j["error"].contains("message"))
                msg += ": " + j["error"]["message"].get<std::string>();
            else if (j.contains("message"))
                msg += ": " + j["message"].get<std::string>();
            else if (!error_body.empty())
                msg += ": " + error_body.substr(0, 300);
        } catch (...) {
            if (!error_body.empty()) msg += ": " + error_body.substr(0, 300);
        }
        if (http_status == 401 || http_status == 403)
            msg += "  (check the provider API key)";
        // A 404 on a local OpenAI-compatible server (Ollama/llama.cpp)
        // almost always means the model id isn't loaded — the daemon is
        // up, it just never pulled this model. Point the user at the
        // fix instead of a bare "HTTP 404: model: <id>".
        if (http_status == 404 && !req.endpoint.use_tls)
            msg += "  (model not loaded — run 'ollama pull " + req.model
                 + "', or pick an available one with Ctrl-P)";
        emit_terminal(ctx, std::move(msg), retry_after_hint);
        return;
    }

    // 2xx — guarantee a terminal event even if [DONE] never arrived.
    emit_terminal(ctx, std::nullopt);
}

// ── Model listing ────────────────────────────────────────────────────────────
std::vector<ModelInfo> list_models(const AuthHeader& auth, const Endpoint& endpoint) {
    std::vector<ModelInfo> result;
    if (endpoint.use_tls && is_empty(auth)) return result;

    http::Request hreq;
    hreq.method = http::HttpMethod::Get;
    hreq.host   = endpoint.host;
    hreq.port   = endpoint.port;
    hreq.path   = endpoint.models_path;
    hreq.plaintext = !endpoint.use_tls;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.headers        = build_request_headers(auth);
    hreq.max_body_bytes = 2ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5'000);
    tos.total   = std::chrono::milliseconds(10'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp || resp->status != 200) return result;

    try {
        auto j = json::parse(resp->body);
        for (const auto& m : j.value("data", json::array())) {
            auto id = m.value("id", "");
            if (id.empty()) continue;
            result.push_back(ModelInfo{
                .id           = ModelId{id},
                .display_name = id,
                .provider     = endpoint.label,
            });
        }
    } catch (...) {}

    return result;
}

// ── Test harness ─────────────────────────────────────────────────────────────
std::vector<Msg> parse_sse_for_test(std::string_view sse_bytes,
                                   std::vector<std::string> known_tools) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.known_tools = std::move(known_tools);
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    feed_sse(ctx, sse_bytes.data(), sse_bytes.size());
    // Mirror run_stream_sync's terminal guarantee: if [DONE] never arrived,
    // synthesise the close so a test sees a StreamFinished.
    if (!ctx.terminated) {
        if (ctx.in_tool_use) { ctx.sink(StreamToolUseEnd{}); ctx.in_tool_use = false; }
        if (!try_salvage_tool_call(ctx)) flush_text_hold(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

} // namespace agentty::provider::openai
