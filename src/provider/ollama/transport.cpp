// agentty::provider::ollama — native /api/chat transport for a local Ollama
// server. See transport.hpp for the rationale. Modelled on zed-industries/zed
// crates/ollama/src/ollama.rs: NDJSON stream, structured message.tool_calls,
// object-form tool arguments, keep_alive + options.num_predict.
//
// We prefer Ollama's structured tool_calls channel, but weak local models
// (qwen2.5-coder, codellama, etc.) routinely leak tool calls as bare JSON in
// `content` instead. So content is held-then-classified at the START of a
// response: if it parses as a tool-call object/array (or ```json / <tool_call>
// wrapper) naming an advertised tool, we salvage it into a structured call;
// otherwise it flushes as ordinary prose. Once any structured tool_calls frame
// arrives, salvage is disabled (the model can drive the real channel).

#include "agentty/provider/ollama/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/util/base64.hpp"

namespace agentty::provider::ollama {

using json = nlohmann::json;
using auth::AuthHeader;
using auth::ApiKeyHeader;
using auth::BearerHeader;

namespace {

// ── UTF-8 scrub ─────────────────────────────────────────────────────────────
// A tool output or pasted blob can carry invalid UTF-8; nlohmann's dump()
// throws on it. Replace every malformed byte with U+FFFD.
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

http::Headers build_request_headers(const AuthHeader& auth) {
    http::Headers h;
    h.push_back({"accept", "application/json"});
    h.push_back({"content-type", "application/json"});
    h.push_back({"user-agent", "agentty/" AGENTTY_VERSION});
    // Ollama needs no auth by default, but a remote / proxied instance may
    // sit behind a bearer key — emit it the same way the OpenAI family does.
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

bool is_assistant_with_results(const Message& m) {
    return m.role == Role::Assistant && !m.tool_calls.empty();
}

// ── Stream state ─────────────────────────────────────────────────────────────
struct StreamCtx {
    EventSink   sink;
    std::string buf;          // NDJSON line buffer
    std::size_t read_pos = 0;
    int         tool_seq = 0; // uniquifies synthesised tool-call ids
    StopReason  stop_reason = StopReason::Unspecified;
    bool        terminated  = false;

    // ── Leaked-tool-call salvage (weak local models) ────────────────────
    // Content frames are held while the response COULD still be a bare
    // tool-call JSON. Once we decide it's prose we flush + stop holding;
    // once a structured tool_calls frame arrives salvage is disabled.
    std::string text_hold;                 // buffered content under suspicion
    bool        holding         = true;    // still buffering potential tool JSON
    bool        salvage_eligible = true;   // can still be a tool call (start)
    bool        any_text_flushed = false;
    bool        any_structured_tool = false;
    int         salvage_seq     = 0;       // uniquifies salvaged call ids
    std::vector<std::string> known_tools;  // tool names we may salvage to
    std::string full_content;              // every content byte (rescue scan)

    // JSON-protocol mode (agent-zero style, very weak models). No native
    // tools array was sent; the WHOLE response is expected to be one
    // `{"thoughts":[...],"tool_name":"...","tool_args":{...}}` object (or
    // plain prose for a chat-only turn). Enables the forgiving first-`{`..
    // last-`}` extraction in the terminal salvage so leading/trailing
    // narration around the object doesn't defeat the tool call.
    bool        json_protocol   = false;

    StreamCtx() { buf.reserve(64 * 1024); }
};

constexpr std::size_t kCompactThreshold = 256 * 1024;

// ── Salvage helpers (ported from openai/transport.cpp) ──────────────────

// Could `s` be the START of a leaked tool-call? Only meaningful at the start
// of a response (salvage_eligible). Detects: bare `{`, `[{`, `<tool_call>`,
// or a ```json fence. Anything else is prose.
[[nodiscard]] bool could_be_tool_json(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
    if (i >= s.size()) return true;            // only whitespace — undecided
    std::string_view rest = s.substr(i);

    if (rest.front() == '{') {
        if (rest.size() == 1) return true;
        std::string_view after = rest.substr(1);
        std::size_t k = 0;
        while (k < after.size() && (after[k]==' '||after[k]=='\t'
               ||after[k]=='\n'||after[k]=='\r')) ++k;
        if (k == after.size()) return true;
        return after[k] == '"' || after[k] == '}';
    }
    if (rest.starts_with("[{") || rest.starts_with("[ {")) return true;
    if (rest == "[" || rest == "[ ") return true;

    if (rest.starts_with("<tool_call>")) return true;
    constexpr std::string_view kTag = "<tool_call>";
    if (rest.size() < kTag.size() && rest.front() == '<') {
        for (std::size_t j = 0; j < rest.size(); ++j)
            if (rest[j] != kTag[j]) return false;
        return true;
    }

    if (rest.starts_with("```json")) return true;
    if (rest.starts_with("```")) {
        if (rest.size() <= 3) return true;
        std::string_view after = rest.substr(3);
        return after.empty() || after[0]=='\n' || after[0]=='\r'
            || after[0]==' ' || after[0]=='\t' || after[0]=='j';
    }
    return false;
}

// Strip <tool_call>…</tool_call> and ```json…``` wrappers + surrounding ws.
[[nodiscard]] std::string_view strip_tool_call_wrappers(std::string_view sv) noexcept {
    auto ltrim = [](std::string_view& s) {
        while (!s.empty() && (s.front()==' '||s.front()=='\t'
                              ||s.front()=='\n'||s.front()=='\r')) s.remove_prefix(1);
    };
    auto rtrim = [](std::string_view& s) {
        while (!s.empty() && (s.back()==' '||s.back()=='\t'
                              ||s.back()=='\n'||s.back()=='\r')) s.remove_suffix(1);
    };
    ltrim(sv); rtrim(sv);
    if (sv.starts_with("<tool_call>")) {
        sv.remove_prefix(std::string_view{"<tool_call>"}.size());
        if (auto p = sv.rfind("</tool_call>"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        ltrim(sv); rtrim(sv);
    }
    if (sv.starts_with("```")) {
        sv.remove_prefix(3);
        // Skip an optional language tag on the fence line (json, sh, bash,
        // tool_code, …) up to the first newline. Weak models wrap a leaked
        // tool call in whatever fence they feel like.
        if (auto nl = sv.find('\n'); nl != std::string_view::npos) {
            std::string_view first = sv.substr(0, nl);
            bool word = !first.empty();
            for (char c : first)
                if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-')) { word=false; break; }
            if (word) sv.remove_prefix(nl + 1);   // drop the lang tag line
        } else if (sv.starts_with("json")) {
            sv.remove_prefix(4);
        }
        ltrim(sv);
        if (auto p = sv.rfind("```"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        rtrim(sv);
    }
    return sv;
}

// Held buffer opens like a tool-call object but doesn't parse (wire cut off),
// or is a wrapper-only remnant / empty `{}` — drop it rather than show garbage.
[[nodiscard]] bool hold_is_truncated_tool_json(std::string_view sv) noexcept {
    std::string_view raw = sv;
    sv = strip_tool_call_wrappers(sv);
    if (sv.empty() && raw != sv) return true;
    if (sv == "json" || sv == "```" || sv == "`") return true;
    if (sv == "{}") return true;
    if (sv.empty() || sv.front() != '{') return false;
    try { auto _ = json::parse(sv); (void)_; return false; }
    catch (...) { return true; }
}

// Complete tool-call-shaped object naming an UNADVERTISED tool — drop it.
[[nodiscard]] bool hold_is_unknown_tool_call(
        std::string_view sv, const std::vector<std::string>& known) noexcept {
    if (known.empty()) return false;
    sv = strip_tool_call_wrappers(sv);
    if (sv.empty() || sv.front() != '{') return false;
    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object()) return false;
    std::string name;
    if (j.contains("name") && j["name"].is_string())
        name = j["name"].get<std::string>();
    else if (j.contains("function") && j["function"].is_string())
        name = j["function"].get<std::string>();
    else if (j.contains("tool_name") && j["tool_name"].is_string())
        name = j["tool_name"].get<std::string>();
    else if (j.contains("tool") && j["tool"].is_string())
        name = j["tool"].get<std::string>();
    else
        return false;
    if (!j.contains("arguments") && !j.contains("parameters")
            && !j.contains("tool_args") && !j.contains("args")) return false;
    // Strip any `tool:action` suffix before the known-tool check.
    if (auto colon = name.find(':'); colon != std::string::npos)
        name = name.substr(0, colon);
    for (const auto& t : known) if (t == name) return false;
    return true;
}

// Emit one salvaged tool call from a JSON object. Returns true if consumed
// (emitted OR deliberately swallowed as a never-salvage footgun tool).
//
// Accepts every shape weak models produce for a tool request:
//   • native-ish:   {"name":"bash","arguments":{...}}  (or "function")
//   • agent-zero:   {"tool_name":"bash","tool_args":{...}}
//   • loose:        {"tool":"bash","args":{...}}
//   • action suffix:{"tool_name":"x:run",...} → args.action="run"
// This mirrors agent-zero's normalize_tool_request: the model only has to
// emit SOME recognisable {name, args} object, not the exact native schema.
[[nodiscard]] bool emit_salvaged_tool(StreamCtx& ctx, std::string_view sv) {
    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object()) return false;
    std::string name;
    if (j.contains("name") && j["name"].is_string())
        name = j["name"].get<std::string>();
    else if (j.contains("function") && j["function"].is_string())
        name = j["function"].get<std::string>();
    else if (j.contains("tool_name") && j["tool_name"].is_string())
        name = j["tool_name"].get<std::string>();
    else if (j.contains("tool") && j["tool"].is_string())
        name = j["tool"].get<std::string>();
    else
        return false;

    // `tool_name` may carry a `tool:action` suffix (agent-zero method dispatch).
    // Split it off; the action becomes an arg the tool reads.
    std::string action_suffix;
    if (auto colon = name.find(':'); colon != std::string::npos
            && colon > 0 && colon + 1 < name.size()) {
        action_suffix = name.substr(colon + 1);
        name = name.substr(0, colon);
    }

    bool known = false;
    for (const auto& t : ctx.known_tools) if (t == name) { known = true; break; }
    if (!known) return false;
    // Never auto-run footgun tools from a leaked guess: swallow the JSON.
    static constexpr std::string_view kNeverSalvage[] = {
        "remember", "forget", "wipe_memory", "skill"};
    for (auto t : kNeverSalvage) if (t == name) return true;

    // Arguments under any of the accepted keys; default to {}.
    json args_obj = json::object();
    bool have_args = false;
    auto take_args = [&](const char* key) {
        if (have_args || !j.contains(key)) return;
        const auto& a = j[key];
        if (a.is_string()) {
            try { args_obj = json::parse(a.get<std::string>()); have_args = true; }
            catch (...) {}
        } else if (a.is_object()) {
            args_obj = a; have_args = true;
        } else if (!a.is_null()) {
            args_obj = a; have_args = true;
        }
    };
    take_args("arguments");
    take_args("tool_args");
    take_args("args");
    take_args("parameters");
    if (!action_suffix.empty() && args_obj.is_object()
            && !args_obj.contains("action"))
        args_obj["action"] = action_suffix;
    std::string args = args_obj.is_object() || args_obj.is_array()
        ? args_obj.dump() : "{}";

    std::string call_id = "call_salvaged_" + std::to_string(ctx.salvage_seq++);
    ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
    ctx.sink(StreamToolUseDelta{args});
    ctx.sink(StreamToolUseEnd{});
    ctx.stop_reason = StopReason::ToolUse;
    return true;
}

// Emit salvaged calls from a JSON array [{...},{...}]. Returns true if any.
[[nodiscard]] bool emit_salvaged_tool_array(StreamCtx& ctx, std::string_view sv) {
    json arr;
    try { arr = json::parse(sv); } catch (...) { return false; }
    if (!arr.is_array()) return false;
    bool any = false;
    for (const auto& item : arr)
        if (item.is_object() && emit_salvaged_tool(ctx, item.dump())) any = true;
    return any;
}

// Try to salvage the held content as a tool call (object or array, after
// stripping wrappers). Returns true if a call was emitted/consumed.
[[nodiscard]] bool try_salvage_hold(StreamCtx& ctx) {
    std::string_view inner = strip_tool_call_wrappers(ctx.text_hold);
    if (inner.empty()) return false;
    if (inner.front() == '[') return emit_salvaged_tool_array(ctx, inner);
    if (inner.front() == '{') return emit_salvaged_tool(ctx, inner);
    return false;
}

// Flush held content as prose (dropping leaked/garbage tool JSON) and stop
// holding. Called once we decide the hold is not a salvageable tool call.
void flush_text_hold(StreamCtx& ctx) {
    ctx.holding = false;
    if (ctx.text_hold.empty()) return;
    if (hold_is_truncated_tool_json(ctx.text_hold)) { ctx.text_hold.clear(); return; }
    if (hold_is_unknown_tool_call(ctx.text_hold, ctx.known_tools)) {
        ctx.text_hold.clear(); return;
    }
    ctx.sink(StreamTextDelta{ctx.text_hold});
    ctx.any_text_flushed = true;
    ctx.text_hold.clear();
}

// Last-resort: a weak model narrated prose AND buried a leaked tool call in a
// fenced ```…``` block mid-response (so it was never held at the start). At
// terminal close, if NOTHING was salvaged or structured, scan full_content for
// the FIRST fenced block whose body is an advertised tool-call object and
// salvage it. Conservative: only fires when no other call happened, only on
// fenced blocks, only for advertised tools. Returns true if a call fired.
bool rescue_tool_from_prose(StreamCtx& ctx) {
    if (ctx.any_structured_tool) return false;
    if (ctx.stop_reason == StopReason::ToolUse) return false;  // already salvaged
    std::string_view sv{ctx.full_content};
    std::size_t pos = 0;
    while ((pos = sv.find("```", pos)) != std::string_view::npos) {
        std::size_t open_end = sv.find('\n', pos + 3);
        if (open_end == std::string_view::npos) break;
        std::size_t close = sv.find("```", open_end + 1);
        if (close == std::string_view::npos) break;
        std::string_view body = sv.substr(open_end + 1, close - open_end - 1);
        // trim
        while (!body.empty() && (body.front()==' '||body.front()=='\t'
               ||body.front()=='\n'||body.front()=='\r')) body.remove_prefix(1);
        while (!body.empty() && (body.back()==' '||body.back()=='\t'
               ||body.back()=='\n'||body.back()=='\r')) body.remove_suffix(1);
        if (!body.empty() && (body.front()=='{' || body.front()=='[')) {
            bool ok = body.front()=='['
                ? emit_salvaged_tool_array(ctx, body)
                : emit_salvaged_tool(ctx, body);
            if (ok) return true;
        }
        pos = close + 3;
    }
    return false;
}

// Find the first BALANCED top-level JSON object substring in `sv`, honouring
// string literals + escapes (so a `}` inside a quoted value doesn't close the
// object early). Returns the [first '{' .. matching '}'] span, or empty if no
// balanced object exists. This is agent-zero's extract_json_root_string: weak
// models in JSON-protocol mode wrap their `{tool_name,tool_args}` object in
// stray narration ("Sure! {"tool_name":...}  Let me know!"); pulling the
// balanced object out makes the call survive that noise.
[[nodiscard]] std::string_view extract_first_json_object(std::string_view sv) noexcept {
    std::size_t start = sv.find('{');
    if (start == std::string_view::npos) return {};
    int depth = 0;
    bool in_str = false, esc = false;
    for (std::size_t i = start; i < sv.size(); ++i) {
        char c = sv[i];
        if (in_str) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            continue;
        }
        if (c == '"')      in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return sv.substr(start, i - start + 1);
        }
    }
    return {};  // never balanced (wire cut mid-object)
}

// JSON-protocol terminal rescue (very weak models, agent-zero style). The
// whole response was meant to be ONE {thoughts, tool_name, tool_args} object.
// Pull the first balanced object out of full_content and try to emit it as a
// tool call. If it carries a `thoughts`/`headline` field but no usable tool,
// surface those as prose so the user still sees the model's reasoning. Returns
// true if a tool call fired.
bool rescue_json_protocol(StreamCtx& ctx) {
    if (!ctx.json_protocol) return false;
    if (ctx.any_structured_tool) return false;
    if (ctx.stop_reason == StopReason::ToolUse) return false;
    std::string_view obj = extract_first_json_object(ctx.full_content);
    if (obj.empty()) return false;
    json j;
    try { j = json::parse(obj); } catch (...) { return false; }
    if (!j.is_object()) return false;

    // A tool request? emit it.
    const bool has_tool =
        (j.contains("tool_name") && j["tool_name"].is_string()) ||
        (j.contains("tool")      && j["tool"].is_string())      ||
        (j.contains("name")      && j["name"].is_string());
    if (has_tool && emit_salvaged_tool(ctx, obj)) return true;
    return false;
}

// Handle one native `message` object from an NDJSON frame.
void handle_message(StreamCtx& ctx, const json& message) {
    // Structured tool calls. Ollama returns them fully-formed in one frame
    // (not streamed char-by-char), so emit Start+Delta+End back to back.
    if (message.contains("tool_calls") && message["tool_calls"].is_array()
        && !message["tool_calls"].empty()) {
        int idx = 0;
        for (const auto& tc : message["tool_calls"]) {
            std::string name, args = "{}";
            std::string call_id;
            if (tc.contains("id") && tc["id"].is_string())
                call_id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    name = fn["name"].get<std::string>();
                if (fn.contains("arguments")) {
                    const auto& a = fn["arguments"];
                    if (a.is_string())     args = a.get<std::string>();
                    else if (!a.is_null()) args = a.dump();
                }
            }
            if (name.empty()) continue;
            if (call_id.empty())
                call_id = "call_ollama_" + std::to_string(ctx.tool_seq++)
                        + "_" + std::to_string(idx);
            ++idx;
            ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
            ctx.sink(StreamToolUseDelta{args});
            ctx.sink(StreamToolUseEnd{});
            ctx.stop_reason = StopReason::ToolUse;
            ctx.any_structured_tool = true;
            ctx.salvage_eligible    = false;  // model drives the real channel
        }
        // A real structured call arrived — any held content is genuine prose
        // that preceded it; flush it now.
        if (ctx.holding) flush_text_hold(ctx);
    }

    // Assistant prose. Held-and-classified while salvage is still eligible so
    // a leaked tool-call JSON can be recovered instead of dumped as text.
    if (message.contains("content") && message["content"].is_string()) {
        const auto& s = message["content"].get_ref<const std::string&>();
        if (s.empty()) return;
        ctx.full_content += s;
        if (!ctx.holding) { ctx.sink(StreamTextDelta{s}); return; }
        ctx.text_hold += s;
        // Still could be a tool call? keep holding until `done` decides.
        if (ctx.salvage_eligible && could_be_tool_json(ctx.text_hold)) return;
        // Decided it's prose — release the buffer and stream the rest live.
        flush_text_hold(ctx);
    }
}

void dispatch_line(StreamCtx& ctx, std::string_view line) {
    if (line.empty()) return;
    json j;
    try { j = json::parse(line); } catch (...) { return; }

    if (j.contains("error")) {
        std::string msg = j["error"].is_string()
            ? j["error"].get<std::string>() : j["error"].dump();
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }
    if (j.contains("message") && j["message"].is_object())
        handle_message(ctx, j["message"]);

    if (j.value("done", false)) {
        // JSON-protocol (very weak models): the ENTIRE reply is meant to be
        // one {thoughts, tool_name, tool_args} object. Try to pull a tool
        // call out of the full content FIRST (before flushing the hold as
        // prose) so leading narration like "Sure, I'll do that:" doesn't
        // get emitted ahead of — and visually compete with — the action.
        bool jp_fired = false;
        if (ctx.json_protocol) {
            jp_fired = rescue_json_protocol(ctx);
            if (jp_fired) { ctx.text_hold.clear(); ctx.holding = false; }
        }
        // Final decision on any still-held content: try to salvage it as a
        // leaked tool call, else flush as prose (dropping garbage JSON).
        if (!jp_fired && ctx.holding && !ctx.text_hold.empty()) {
            if (!try_salvage_hold(ctx)) flush_text_hold(ctx);
            else { ctx.text_hold.clear(); ctx.holding = false; }
        } else if (ctx.holding) {
            ctx.holding = false;
        }
        // Mid-prose buried tool call (narration + ```fenced``` JSON).
        rescue_tool_from_prose(ctx);
        StreamUsage su;
        su.input_tokens  = j.value("prompt_eval_count", 0);
        su.output_tokens = j.value("eval_count", 0);
        if (su.input_tokens || su.output_tokens) ctx.sink(su);
        auto reason = j.value("done_reason", std::string{"stop"});
        if (ctx.stop_reason != StopReason::ToolUse)
            ctx.stop_reason = (reason == "length") ? StopReason::MaxTokens
                                                   : StopReason::EndTurn;
    }
}

void feed_ndjson(StreamCtx& ctx, const char* data, std::size_t len) {
    ctx.buf.append(data, len);
    std::string_view buf{ctx.buf};
    while (true) {
        const auto nl = buf.find('\n', ctx.read_pos);
        if (nl == std::string_view::npos) break;
        std::string_view line = buf.substr(ctx.read_pos, nl - ctx.read_pos);
        ctx.read_pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        dispatch_line(ctx, line);
    }
    if (ctx.read_pos >= kCompactThreshold) {
        ctx.buf.erase(0, ctx.read_pos);
        ctx.read_pos = 0;
    }
}

// ── CLAUDE.md memory tiers (user-authored, concise) ─────────────────────────
std::filesystem::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return std::filesystem::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h)
        return std::filesystem::path{h};
#endif
    return {};
}

std::string read_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    if (s.size() > 64 * 1024) s.resize(64 * 1024);
    return s;
}

std::string memory_blocks() {
    std::string user    = read_file(home_dir() / "CLAUDE.md");
    std::string project = read_file(std::filesystem::path{"CLAUDE.md"});
    std::string local   = read_file(std::filesystem::path{"CLAUDE.local.md"});
    if (user.empty() && project.empty() && local.empty()) return {};

    std::string m = "\n\n<memory>\n"
        "Project-specific guidance the user has authored. Treat these as "
        "persistent context for THIS workspace and user.\n";
    if (!user.empty())    m += "<user-memory>\n"    + user    + "\n</user-memory>\n";
    if (!project.empty()) m += "<project-memory>\n" + project + "\n</project-memory>\n";
    if (!local.empty())   m += "<local-memory>\n"   + local   + "\n</local-memory>\n";
    m += "</memory>";
    return m;
}

} // namespace

// ── Messages array (native shape) ───────────────────────────────────────────
// Ollama wants tool arguments as a JSON OBJECT (not a serialized string) and
// tool results as role:"tool" with `tool_name`.
json build_messages(const std::vector<Message>& msgs) {
    json arr = json::array();
    for (const auto& m : msgs) {
        const bool has_text  = !m.text.empty();
        const bool has_tools = is_assistant_with_results(m);
        bool has_images = false;
        if (m.role == Role::User)
            for (const auto& img : m.images)
                if (!img.bytes.empty()) { has_images = true; break; }

        if (has_text || has_images || has_tools) {
            json msg;
            msg["role"] = (m.role == Role::User) ? "user" : "assistant";
            std::string wire_text = m.attachments.empty()
                ? m.text : attachment::expand(m.text, m.attachments);
            msg["content"] = scrub_utf8(wire_text);
            if (has_images) {
                json imgs = json::array();
                for (const auto& img : m.images)
                    if (!img.bytes.empty())
                        imgs.push_back(agentty::util::base64_encode(img.bytes));
                if (!imgs.empty()) msg["images"] = std::move(imgs);
            }
            if (has_tools) {
                json calls = json::array();
                for (const auto& tc : m.tool_calls) {
                    calls.push_back({
                        {"id", tc.id.value},
                        {"function", {
                            {"name", tc.name.value},
                            {"arguments", tc.args.is_null() ? json::object()
                                                            : tc.args},
                        }},
                    });
                }
                msg["tool_calls"] = std::move(calls);
            }
            arr.push_back(std::move(msg));
        }
        if (has_tools) {
            for (const auto& tc : m.tool_calls) {
                std::string out = tc.output();
                if (out.empty()) {
                    if (tc.is_rejected())       out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                }
                arr.push_back({
                    {"role", "tool"},
                    {"tool_name", tc.name.value},
                    {"content", scrub_utf8(out)},
                });
            }
        }
    }
    return arr;
}

namespace {
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

// JSON-protocol addendum (agent-zero style) for very weak models. Appended to
// the system prompt INSTEAD of sending a native `tools` array. Inlines the
// tool catalog (name + description + parameter names) and pins the exact
// single-object response format the extractor parses. Kept terse — tiny
// models drown in long schemas, so we give names + a one-line param hint, not
// the full JSON Schema.
std::string json_protocol_addendum(const std::vector<provider::ToolSpec>& tools) {
    std::string s;
    s += "\n\n## How to act (IMPORTANT — read carefully)\n";
    s += "You do NOT have a function-calling API. To use a tool you MUST reply "
         "with ONE single JSON object and NOTHING else — no prose before or "
         "after it, no markdown fences. The JSON object has exactly these "
         "fields:\n";
    s += "  - \"thoughts\": array of short strings, your reasoning\n";
    s += "  - \"tool_name\": the EXACT name of one tool from the list below\n";
    s += "  - \"tool_args\": an object of arguments for that tool\n\n";
    s += "Example (run a shell command):\n";
    s += "{\"thoughts\":[\"I need to list files\"],\"tool_name\":\"bash\","
         "\"tool_args\":{\"command\":\"ls -la\"}}\n\n";
    s += "Rules:\n";
    s += "- Output the JSON object ALONE, valid JSON, double quotes.\n";
    s += "- Use ONE tool per reply, then wait for its result in the next "
         "message before the next step.\n";
    s += "- `tool_name` must be one of the listed names, never an action verb "
         "like read/write/run.\n";
    s += "- If you do NOT need a tool (a greeting, a question you can answer "
         "from the conversation), reply in plain text instead — no JSON.\n\n";
    s += "## Available tools\n";
    for (const auto& t : tools) {
        s += "- " + t.name;
        if (!t.description.empty()) {
            // First sentence / first line of the description keeps it short.
            std::string d = t.description;
            if (auto nl = d.find('\n'); nl != std::string::npos) d = d.substr(0, nl);
            if (d.size() > 160) d = d.substr(0, 160);
            s += ": " + d;
        }
        // Parameter name hints from the JSON Schema's `properties`.
        if (t.input_schema.is_object() && t.input_schema.contains("properties")
                && t.input_schema["properties"].is_object()) {
            std::string params;
            for (auto it = t.input_schema["properties"].begin();
                 it != t.input_schema["properties"].end(); ++it) {
                if (!params.empty()) params += ", ";
                params += it.key();
            }
            if (!params.empty()) s += "  (args: " + params + ")";
        }
        s += "\n";
    }
    return s;
}
} // namespace

std::string system_prompt() {
    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch (...) {}

#if defined(_WIN32)
    const char* os_name = "Windows";
    const char* shell   = "cmd.exe";
#elif defined(__APPLE__)
    const char* os_name = "macOS";
    const char* shell   = "sh";
#else
    const char* os_name = "Linux";
    const char* shell   = "sh";
#endif

    std::string out;
    out += "You are agentty, a terminal coding assistant. You are helpful, "
           "direct, and act on requests instead of asking which option to "
           "pick. Keep replies concise.\n\n";

    out += "CONVERSATION MEMORY\n"
           "- The full conversation so far is in the messages above. ALWAYS "
           "use earlier messages to answer follow-up questions (names, files, "
           "decisions the user already gave you).\n"
           "- If the user told you a fact earlier (e.g. their name), recall it "
           "from the conversation; never say you don't have it.\n\n";

    out += "TOOLS\n"
           "- Tools let you read/edit files and run commands. Call a tool ONLY "
           "when the task needs it. For greetings, chit-chat, or questions you "
           "can answer from the conversation, reply in plain text \xE2\x80\x94 "
           "do NOT call a tool.\n"
           "- When a task DOES need an action (rename/move/delete a file, run a "
           "shell command, read or edit code), you MUST actually call the tool. "
           "NEVER describe the command in prose or a code block and claim it "
           "ran \xE2\x80\x94 that does nothing. NEVER say a file was created, "
           "renamed, or deleted unless a tool you called returned that result.\n"
           "- To run a shell command (mv, rm, mkdir, git, etc.) call the `bash` "
           "tool with a `command` argument. There is NO `git` or `mv` tool \xE2\x80\x94 "
           "use `bash`. To edit an existing file use `edit`; use `write` only to "
           "create a new file.\n"
           "- Emit tool calls through the tool-call channel, NOT as JSON or a "
           "```code block``` in your reply.\n"
           "- Make ONE tool call at a time and wait for its result. Never "
           "invent a tool result.\n"
           "- Never call remember/forget/wipe_memory unless the user asks you "
           "to remember or forget something.\n\n";

    out += "OUTPUT\n"
           "- Output is rendered as GitHub-flavoured markdown in a terminal. "
           "Use fenced code blocks for code. Keep tables small.\n\n";

    out += "ENVIRONMENT\n";
    out += "- os: ";    out += os_name; out += "\n";
    out += "- shell: "; out += shell;   out += "\n";
    if (!cwd.empty()) { out += "- cwd: "; out += cwd; out += "\n"; }

    out += memory_blocks();
    return out;
}

// ── Streaming entry point ────────────────────────────────────────────────────
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    StreamCtx ctx;
    ctx.sink = std::move(sink);
    // Salvage may only synthesise calls to tools we actually advertised.
    ctx.known_tools.reserve(req.tools.size());
    for (const auto& t : req.tools) ctx.known_tools.push_back(t.name);
    ctx.json_protocol = req.json_protocol && !req.tools.empty();
    // No tools advertised → nothing to salvage to; treat all content as prose.
    if (req.tools.empty()) { ctx.holding = false; ctx.salvage_eligible = false; }
    // JSON-protocol: the whole reply is one tool-request object, so always
    // hold + stay salvage-eligible (no native channel will arrive).
    if (ctx.json_protocol) { ctx.holding = true; ctx.salvage_eligible = true; }

    auto emit_terminal = [](StreamCtx& c, std::optional<std::string> err,
                            std::optional<std::chrono::seconds> retry_after = {}) {
        if (c.terminated) return;
        // Stream ended without a `done` frame (e.g. wire cut) but content may
        // still be held — salvage or flush it before the terminal event.
        if (!err) {
            bool jp_fired = false;
            if (c.json_protocol) {
                jp_fired = rescue_json_protocol(c);
                if (jp_fired) { c.text_hold.clear(); c.holding = false; }
            }
            if (!jp_fired && c.holding && !c.text_hold.empty()) {
                if (!try_salvage_hold(c)) flush_text_hold(c);
                else { c.text_hold.clear(); c.holding = false; }
            }
            rescue_tool_from_prose(c);
        }
        if (err) c.sink(StreamError{*err, retry_after});
        else     c.sink(StreamFinished{c.stop_reason});
        c.terminated = true;
    };

    // ── Build /api/chat body ─────────────────────────────────────────────────
    json body;
    body["model"]  = req.model;
    body["stream"] = true;
    // Keep the model resident between turns so the next prompt is instant.
    body["keep_alive"] = "10m";
    // num_predict = max output tokens (Ollama default is a low ~128, which
    // truncates real answers). num_ctx is left to the model's Modelfile /
    // server default so we don't shrink a model's window by guessing.
    body["options"] = {{"num_predict", req.max_tokens}};

    json messages = json::array();
    std::string sys = req.system_prompt;
    // JSON-protocol: append the tool catalog + single-object response spec to
    // the system prompt, and do NOT send a native `tools` array below.
    if (ctx.json_protocol)
        sys += json_protocol_addendum(req.tools);
    if (!sys.empty())
        messages.push_back({{"role", "system"},
                            {"content", scrub_utf8(sys)}});
    for (auto& m : build_messages(req.messages))
        messages.push_back(std::move(m));
    body["messages"] = std::move(messages);

    // Native tools array ONLY when not in JSON-protocol mode. Weak models get
    // the inline catalog instead (the native schema confuses them).
    if (!req.tools.empty() && !ctx.json_protocol)
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

    // ── HTTP request ─────────────────────────────────────────────────────────
    http::Request hreq;
    hreq.method    = http::HttpMethod::Post;
    hreq.host      = req.endpoint.host;
    hreq.port      = req.endpoint.port;
    hreq.path      = "/api/chat";
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

    http::StreamHandler handler;
    handler.on_headers = [&](int status, const http::Headers&) {
        http_status = status;
        is_success  = (status >= 200 && status < 300);
    };
    handler.on_chunk = [&](std::string_view chunk) -> bool {
        if (is_success) {
            feed_ndjson(ctx, chunk.data(), chunk.size());
        } else if (error_body.size() < 64 * 1024) {
            error_body.append(chunk.data(),
                std::min(chunk.size(), 64 * 1024 - error_body.size()));
        }
        return true;
    };

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(0);   // streaming unbounded
    tos.ping    = std::chrono::milliseconds(15'000);
    tos.idle    = std::chrono::milliseconds(120'000);  // local gen can be slow

    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    if (!result) {
        std::string msg = std::string{"http: "} + result.error().render();
        msg += "  (is Ollama running? start it with 'ollama serve', or check "
               "--provider host:port)";
        emit_terminal(ctx, std::move(msg));
        return;
    }

    if (!is_success) {
        std::string msg = "HTTP " + std::to_string(http_status);
        try {
            auto j = json::parse(error_body);
            if (j.contains("error") && j["error"].is_string())
                msg += ": " + j["error"].get<std::string>();
            else if (!error_body.empty())
                msg += ": " + error_body.substr(0, 300);
        } catch (...) {
            if (!error_body.empty()) msg += ": " + error_body.substr(0, 300);
        }
        if (http_status == 404)
            msg += "  (model not loaded — run 'ollama pull " + req.model + "')";
        emit_terminal(ctx, std::move(msg));
        return;
    }

    // 2xx — guarantee a terminal event even if `done` never arrived.
    emit_terminal(ctx, std::nullopt);
}

std::vector<Msg> parse_ndjson_for_test(std::string_view ndjson_bytes,
                                       std::vector<std::string> known_tools,
                                       bool json_protocol) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    ctx.known_tools = std::move(known_tools);
    ctx.json_protocol = json_protocol && !ctx.known_tools.empty();
    if (ctx.known_tools.empty()) { ctx.holding = false; ctx.salvage_eligible = false; }
    if (ctx.json_protocol) { ctx.holding = true; ctx.salvage_eligible = true; }
    feed_ndjson(ctx, ndjson_bytes.data(), ndjson_bytes.size());
    // Mirror run_stream_sync's terminal flush so tests see the live sequence.
    if (!ctx.terminated) {
        bool jp_fired = false;
        if (ctx.json_protocol) {
            jp_fired = rescue_json_protocol(ctx);
            if (jp_fired) { ctx.text_hold.clear(); ctx.holding = false; }
        }
        if (!jp_fired && ctx.holding && !ctx.text_hold.empty()) {
            if (!try_salvage_hold(ctx)) flush_text_hold(ctx);
            else { ctx.text_hold.clear(); ctx.holding = false; }
        }
        rescue_tool_from_prose(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

} // namespace agentty::provider::ollama
