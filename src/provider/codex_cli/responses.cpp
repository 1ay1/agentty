#include "agentty/provider/codex_cli/responses.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/codex_cli/codex_oauth.hpp"
#include "agentty/provider/codex_cli/oauth.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/runtime/composer_attachment.hpp"

namespace agentty::provider::codex_cli {
namespace {
using json = nlohmann::json;

// UTF-8 scrub — a pasted blob / tool output can carry invalid UTF-8 that
// nlohmann::dump() would throw on. Replace malformed bytes with U+FFFD.
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* p   = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++p; continue; }
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
        if (extra < 0 || p + extra >= end) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k)
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        out.append(reinterpret_cast<const char*>(p), extra + 1);
        p += extra + 1;
    }
    return out;
}

std::string new_uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 1
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(a >> 32),
        static_cast<unsigned>((a >> 16) & 0xFFFF),
        static_cast<unsigned>(a & 0xFFFF),
        static_cast<unsigned>(b >> 48),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string{buf};
}

// ── agentty conversation → Responses `input[]` array ───────────────────────
//
// Rebuilds the whole turn history each call (the Codex backend runs
// store:false, so state is client-side). Every assistant tool_call becomes a
// `function_call` item immediately followed by its `function_call_output`.
json build_input(const provider::Request& req) {
    json input = json::array();
    for (const auto& m : req.messages) {
        if (m.role == Role::System) continue;   // folded into `instructions`

        const std::string text = m.attachments.empty()
            ? m.text
            : attachment::expand(m.text, m.attachments);

        if (m.role == Role::User) {
            if (text.empty()) continue;
            input.push_back({
                {"type", "message"}, {"role", "user"},
                {"content", json::array({
                    json{{"type", "input_text"}, {"text", scrub_utf8(text)}}})},
            });
            continue;
        }

        // Assistant turn: emit its prose (if any) as an output_text message,
        // then each tool call as function_call + function_call_output.
        if (!text.empty()) {
            input.push_back({
                {"type", "message"}, {"role", "assistant"},
                {"content", json::array({
                    json{{"type", "output_text"}, {"text", scrub_utf8(text)}}})},
            });
        }
        for (const auto& tc : m.tool_calls) {
            std::string args = tc.args.is_null() ? "{}" : tc.args.dump();
            input.push_back({
                {"type", "function_call"},
                {"call_id", tc.id.value},
                {"name", tc.name.value},
                {"arguments", scrub_utf8(args)},
            });
            // The result the host produced for this call (may be pending if
            // the turn is still in flight — then we skip, the model re-requests).
            if (tc.is_terminal()) {
                input.push_back({
                    {"type", "function_call_output"},
                    {"call_id", tc.id.value},
                    {"output", scrub_utf8(tc.output())},
                });
            }
        }
    }
    return input;
}

json build_tools(const provider::Request& req) {
    json tools = json::array();
    for (const auto& t : req.tools) {
        // Responses API function tool is FLAT (name/description/parameters at
        // top level), unlike Chat Completions' nested {function:{...}}.
        tools.push_back({
            {"type", "function"},
            {"name", t.name},
            {"description", t.description},
            {"parameters", t.input_schema.is_null()
                ? json{{"type", "object"}, {"properties", json::object()}}
                : t.input_schema},
        });
    }
    return tools;
}

json build_body(const provider::Request& req) {
    json body{
        {"model", req.model.empty() || req.model == "codex-cli-default"
                      ? "gpt-5-codex" : req.model},
        {"instructions", scrub_utf8(req.system_prompt)},
        {"input", build_input(req)},
        {"tool_choice", "auto"},
        {"parallel_tool_calls", true},
        {"store", false},
        {"stream", true},
        {"include", json::array()},
    };
    if (auto tools = build_tools(req); !tools.empty()) body["tools"] = tools;
    if (!req.effort.empty())
        body["reasoning"] = json{{"effort", req.effort}, {"summary", "auto"}};
    else
        body["reasoning"] = json{{"summary", "auto"}};
    return body;
}

// ── SSE dispatch state ─────────────────────────────────────────────────────
struct StreamCtx {
    EventSink sink;
    wire::SseFramer sse;
    // item.id (fc_…) → call_id (call_…) so argument deltas keyed by item_id
    // can be forwarded under the correlation id the result must echo.
    std::unordered_map<std::string, std::string> call_ids;
    std::string open_tool_item;   // item.id of the function_call currently open
    bool text_block_open = false;
    bool saw_function_call = false;
    bool terminated = false;
    StopReason stop = StopReason::EndTurn;
};

void close_open_tool(StreamCtx& ctx) {
    if (!ctx.open_tool_item.empty()) {
        ctx.sink(StreamToolUseEnd{});
        ctx.open_tool_item.clear();
    }
}

void emit_usage(StreamCtx& ctx, const json& usage) {
    if (!usage.is_object()) return;
    StreamUsage su;
    su.input_tokens  = usage.value("input_tokens", 0);
    su.output_tokens = usage.value("output_tokens", 0);
    if (usage.contains("input_tokens_details"))
        su.cache_read_input_tokens =
            usage["input_tokens_details"].value("cached_tokens", 0);
    ctx.sink(su);
}

void dispatch(StreamCtx& ctx, std::string_view data) {
    if (data.empty() || data == "[DONE]") return;
    json j;
    try { j = json::parse(data); } catch (...) { return; }

    const auto type = j.value("type", std::string{});

    if (type == "response.output_text.delta") {
        if (!ctx.text_block_open) ctx.text_block_open = true;
        ctx.sink(StreamTextDelta{j.value("delta", std::string{})});
        return;
    }
    if (type == "response.reasoning_summary_text.delta"
        || type == "response.reasoning_text.delta") {
        ctx.sink(StreamThinkingDelta{j.value("delta", std::string{}), {}});
        return;
    }
    if (type == "response.output_item.added") {
        const auto& item = j.value("item", json::object());
        const auto itype  = item.value("type", std::string{});
        if (itype == "function_call") {
            // A new tool call opens. Close any prior text block first so the
            // reveal cursor snaps before the card (matches Anthropic seam).
            if (ctx.text_block_open) {
                ctx.text_block_open = false;
                ctx.sink(StreamTextBlockClosed{});
            }
            close_open_tool(ctx);
            const std::string item_id = item.value("id", std::string{});
            const std::string call_id = item.value("call_id", item_id);
            const std::string name    = item.value("name", std::string{});
            ctx.call_ids[item_id] = call_id;
            ctx.open_tool_item    = item_id;
            ctx.saw_function_call = true;
            ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
            // Some backends deliver the whole args string up-front on `added`.
            if (const auto a = item.value("arguments", std::string{}); !a.empty())
                ctx.sink(StreamToolUseDelta{a});
        }
        return;
    }
    if (type == "response.function_call_arguments.delta") {
        ctx.sink(StreamToolUseDelta{j.value("delta", std::string{})});
        return;
    }
    if (type == "response.output_item.done") {
        const auto& item = j.value("item", json::object());
        if (item.value("type", std::string{}) == "function_call")
            close_open_tool(ctx);
        else if (ctx.text_block_open) {
            ctx.text_block_open = false;
            ctx.sink(StreamTextBlockClosed{});
        }
        return;
    }
    if (type == "response.completed") {
        close_open_tool(ctx);
        const auto& resp = j.value("response", json::object());
        emit_usage(ctx, resp.value("usage", json::object()));
        // No finish_reason on the wire — a function_call in the output means
        // the model wants tool results before continuing.
        ctx.stop = ctx.saw_function_call ? StopReason::ToolUse : StopReason::EndTurn;
        ctx.sink(StreamFinished{ctx.stop});
        ctx.terminated = true;
        return;
    }
    if (type == "response.incomplete") {
        close_open_tool(ctx);
        const auto& resp = j.value("response", json::object());
        emit_usage(ctx, resp.value("usage", json::object()));
        const auto reason = resp.value("incomplete_details", json::object())
                                .value("reason", std::string{});
        ctx.stop = reason == "max_output_tokens" ? StopReason::MaxTokens
                                                  : StopReason::EndTurn;
        ctx.sink(StreamFinished{ctx.stop});
        ctx.terminated = true;
        return;
    }
    if (type == "response.failed" || type == "error") {
        close_open_tool(ctx);
        std::string msg;
        if (type == "error") msg = j.value("message", std::string{"stream error"});
        else msg = j.value("response", json::object())
                       .value("error", json::object())
                       .value("message", std::string{"Codex request failed"});
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }
    // response.created / in_progress / content_part.* / *_summary_part.* etc.
    // are structural — nothing to render. Bump liveness so the stall watchdog
    // knows the wire is healthy during a long reasoning pass.
    ctx.sink(StreamHeartbeat{});
}

} // namespace

bool responses_available() {
    auto c = load_codex_credentials();
    return c && !c->access_token.empty();
}

void stream_responses(provider::Request req, provider::EventSink sink) {
    sink(StreamStarted{});

    auto creds = codex_fresh_credentials();   // auto-refreshes if stale
    if (!creds || creds->access_token.empty()) {
        sink(StreamError{"not signed in to ChatGPT — run `agentty login` and "
                         "choose ChatGPT, or use --provider with an API key"});
        return;
    }

    http::Request hr;
    hr.method = http::HttpMethod::Post;
    hr.host   = "chatgpt.com";
    hr.port   = 443;
    hr.path   = "/backend-api/codex/responses";
    hr.headers = {
        {"authorization",     "Bearer " + creds->access_token},
        {"content-type",      "application/json"},
        {"accept",            "text/event-stream"},
        {"openai-beta",       "responses=experimental"},
        {"originator",        OAuthConfig::originator},
        {"session_id",        new_uuid_v4()},
        {"user-agent",        "codex_cli_rs/" AGENTTY_VERSION},
    };
    if (!creds->account_id.empty())
        hr.headers.push_back({"chatgpt-account-id", creds->account_id});

    try {
        hr.body = build_body(req).dump();
    } catch (const std::exception& e) {
        sink(StreamError{std::string{"could not encode request: "} + e.what()});
        return;
    }

    StreamCtx ctx;
    ctx.sink = sink;
    int http_status = 0;
    std::string error_body;

    http::StreamHandler cbs;
    cbs.on_headers = [&](int status, const http::Headers&) { http_status = status; };
    cbs.on_chunk = [&](std::string_view chunk) -> bool {
        if (http_status >= 400) { error_body.append(chunk); return true; }
        ctx.sse.feed(chunk.data(), chunk.size(),
            [&](std::string_view, std::string_view payload, char*) {
                dispatch(ctx, payload);
            });
        return !ctx.terminated;
    };

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(15'000);
    tos.total   = std::chrono::milliseconds(600'000);

    auto result = http::default_client().stream(hr, cbs, tos, req.cancel);

    if (req.cancel && req.cancel->is_cancelled()) {
        sink(StreamError{"cancelled"});
        return;
    }
    if (http_status >= 400) {
        std::string msg = "Codex backend returned HTTP " + std::to_string(http_status);
        try {
            auto j = json::parse(error_body);
            if (j.contains("error"))
                msg = j["error"].value("message", msg);
            else if (j.contains("detail"))
                msg = j.value("detail", msg);
        } catch (...) {}
        if (http_status == 401)
            msg += " — session expired; run `agentty login` and sign in to ChatGPT again";
        sink(StreamError{msg, std::nullopt});
        return;
    }
    if (!result) {
        sink(StreamError{result.error().render()});
        return;
    }
    if (!ctx.terminated) {
        // Stream closed cleanly without response.completed (proxy cutoff).
        sink(StreamFinished{ctx.stop});
    }
}

// ── Test seams ────────────────────────────────────────────────────
nlohmann::json build_body_for_test(const provider::Request& req) {
    return build_body(req);
}

std::vector<Msg> parse_sse_for_test(const std::vector<std::string>& sse_data_lines) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.sink = [&](Msg m) { out.push_back(std::move(m)); };
    for (const auto& line : sse_data_lines) dispatch(ctx, line);
    return out;
}

} // namespace agentty::provider::codex_cli
