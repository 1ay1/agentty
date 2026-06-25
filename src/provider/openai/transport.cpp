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
#include <filesystem>
#include <fstream>
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

    // ── Incremental leaked-tool-call salvage (local models) ─────────────
    // Many Ollama/llama.cpp models (qwen2.5-coder, hermes, mistral, etc.)
    // emit tool calls as bare JSON in `content` instead of the structured
    // `tool_calls[]` channel. We parse INCREMENTALLY: as soon as a complete
    // JSON object is recognized, emit it as a real tool call immediately
    // (so the card appears during streaming, not after [DONE]).
    //
    // IMPORTANT: Salvage ONLY applies at the START of a response. Once we've
    // seen that the model is outputting prose (not a tool call), we disable
    // salvage permanently. This prevents false positives on code like
    // `int main() {` or `#include <iostream>`.
    //
    // The hold buffer accumulates content that COULD still be tool JSON.
    // We track brace depth to know when a JSON object is complete.
    std::string text_hold;          // buffered text under suspicion
    bool        holding   = false;  // currently buffering potential tool JSON
    bool        salvage_eligible = true;  // can still be a tool call (start of response)
    bool        any_text_flushed = false;
    int         salvage_seq = 0;    // uniquifies synthesised salvage call ids
    std::vector<std::string> known_tools;  // tool names we may salvage to

    // Incremental JSON parse state for salvage.
    int  brace_depth = 0;           // nested {} depth in current JSON object
    int  bracket_depth = 0;         // nested [] depth (for arrays of calls)
    bool in_string = false;         // inside a JSON string literal
    bool escape_next = false;       // next char is escaped in string
    std::size_t json_start = std::string::npos;  // offset where current JSON began
    bool saw_wrapper = false;  // have we seen any tool wrapper (```, <tool_call>) this hold?

    StopReason stop_reason = StopReason::Unspecified;
    bool terminated = false;

    StreamCtx() { buf.reserve(64 * 1024); data_accum.reserve(8 * 1024); }
};

// Could `s` be the START of a leaked tool-call? This is only called at the
// beginning of a response (salvage_eligible=true). Once prose is detected,
// salvage is disabled permanently for this response.
//
// We detect these patterns that weak local models use:
// 1. Bare JSON object: {"name": "...", "arguments": {...}}
// 2. JSON array of objects: [{...}, {...}]
// 3. <tool_call> tag wrapper: <tool_call>{...}</tool_call>
// 4. ```json fence wrapper: ```json\n{...}\n```
//
// For robustness: if content starts with ANYTHING ELSE, it's prose.
[[nodiscard]] bool could_be_tool_json(std::string_view s) noexcept {
    // Skip leading whitespace.
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'
                            || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size()) return true;  // only whitespace so far, still undecided
    
    std::string_view rest = s.substr(i);
    
    // Pattern 1: Bare JSON object starting with {
    // Accept {, {whitespace, or {" — the brace may be followed by newlines
    // before the first key (pretty-printed JSON from local models).
    if (rest.front() == '{') {
        if (rest.size() == 1) return true;  // just { so far
        // Check what follows the {
        std::string_view after_brace = rest.substr(1);
        // Skip whitespace after {
        std::size_t k = 0;
        while (k < after_brace.size() && (after_brace[k] == ' ' || after_brace[k] == '\t'
               || after_brace[k] == '\n' || after_brace[k] == '\r')) ++k;
        // If only whitespace after {, keep waiting
        if (k == after_brace.size()) return true;
        // If next non-ws char is " (start of key) or } (empty object), valid JSON
        if (after_brace[k] == '"' || after_brace[k] == '}') return true;
        // Otherwise not valid JSON object start
        return false;
    }
    
    // Pattern 2: JSON array starting with [{ or [
    if (rest.starts_with("[{") || rest.starts_with("[ {")) return true;
    if (rest == "[" || rest == "[ ") return true;
    
    // Pattern 3: <tool_call> tag
    if (rest.starts_with("<tool_call>")) return true;
    // Incomplete prefix of <tool_call>
    constexpr std::string_view kTag = "<tool_call>";
    if (rest.size() < kTag.size() && rest.front() == '<') {
        for (std::size_t j = 0; j < rest.size(); ++j) {
            if (rest[j] != kTag[j]) return false;  // diverged
        }
        return true;  // exact prefix match
    }
    
    // Pattern 4: ```json fence
    if (rest.starts_with("```json")) return true;
    if (rest.starts_with("```")) {
        if (rest.size() <= 3) return true;  // just ``` so far
        std::string_view after = rest.substr(3);
        // Could still be ```json or ```\n{
        if (after.empty() || after[0] == '\n' || after[0] == '\r' ||
            after[0] == ' ' || after[0] == '\t' || after[0] == 'j') {
            return true;
        }
        // It's ```cpp or similar - NOT a tool wrapper
        return false;
    }
    
    // Anything else is prose.
    return false;
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

// Strip the wrappers weak local models put around a leaked tool call so what
// remains is (ideally) a bare JSON object: surrounding whitespace, a single
// <tool_call>…</tool_call> tag pair, and/or a ```json … ``` code fence. Order
// is tag-then-fence-then-trim, applied leniently (a missing closing tag/fence
// is tolerated — the wire may have cut off). Returns the inner slice.
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
    // <tool_call> … </tool_call>
    if (sv.starts_with("<tool_call>")) {
        sv.remove_prefix(std::string_view{"<tool_call>"}.size());
        if (auto p = sv.rfind("</tool_call>"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        ltrim(sv); rtrim(sv);
    }
    // ```json … ```  (or a bare ``` fence)
    if (sv.starts_with("```")) {
        sv.remove_prefix(3);
        if (sv.starts_with("json")) sv.remove_prefix(4);
        ltrim(sv);
        if (auto p = sv.rfind("```"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        rtrim(sv);
    }
    return sv;
}

// True iff the held buffer opens like a bare tool-call JSON object but is
// INCOMPLETE — it begins with `{` (after optional whitespace / a ```json
// fence / a <tool_call> tag) yet does not parse as a complete JSON value. That
// is the signature of a tool call the model leaked into `content` whose wire
// cut off mid-body ("upstream cut off"). A COMPLETE object that simply named
// an unadvertised tool is NOT incomplete — it still surfaces as text so the
// user sees what the model meant.
[[nodiscard]] bool hold_is_truncated_tool_json(std::string_view sv) noexcept {
    std::string_view raw = sv;
    sv = strip_tool_call_wrappers(sv);
    // A hold that is NOTHING BUT a tool-call wrapper — a bare ``` / ```json
    // fence or a <tool_call> tag with no JSON body after stripping — is a
    // leaked wrapper whose body never arrived (or a fence-only opener like
    // qwen emitting just "```json" then stopping). It is never legitimate
    // prose; surfacing it dumps a stray "json" / "```" into the reply (the
    // bug where "hi" was answered with the literal text "json"). Drop it.
    if (sv.empty() && raw != sv) return true;
    // Likewise a fence-word-only remnant the stripper left (e.g. the model
    // emitted exactly "json" or "```json" with nothing parseable).
    if (sv == "json" || sv == "```" || sv == "`") return true;
    // Empty object "{}" — qwen2.5-coder:14b outputs this when tools are
    // passed but it doesn't want to call any. It's garbage, not prose.
    if (sv == "{}") return true;
    if (sv.empty() || sv.front() != '{') return false;
    try { auto _ = json::parse(sv); (void)_; return false; }   // complete — keep as text
    catch (...) { return true; }                    // truncated — drop
}

// A COMPLETE JSON object shaped EXACTLY like a tool call ({"name"|"function":
// "<str>", "arguments"|"parameters": {...}}) that names a tool NOT in
// known_tools. Weak local models leak these constantly with a mistyped tool
// name (e.g. "read_file" instead of "read"); surfacing the raw JSON dumps
// garbage into the reply and primes a re-leak next turn. It is never prose a
// user wants to read. Only consulted when we ARE advertising tools.
[[nodiscard]] bool hold_is_unknown_tool_call(
        std::string_view sv, const std::vector<std::string>& known) noexcept {
    if (known.empty()) return false;            // not in agentic mode
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
    else if (j.contains("tool") && j["tool"].is_string())
        name = j["tool"].get<std::string>();
    else
        return false;                           // no tool-name key — real prose
    // Must ALSO carry an args/params field to count as a tool-call shape.
    if (!j.contains("arguments") && !j.contains("parameters")) return false;
    for (const auto& t : known) if (t == name) return false;  // advertised
    return true;                                 // tool-shaped, unknown name
}

// Emit a slice of text_hold as ordinary prose. Used when we've determined
// some prefix isn't tool JSON (e.g., leading prose before a JSON object).
void flush_text_slice(StreamCtx& ctx, std::size_t len) {
    if (len == 0) return;
    std::string_view slice{ctx.text_hold.data(), len};
    ctx.sink(StreamTextDelta{std::string{slice}});
    ctx.any_text_flushed = true;
    ctx.text_hold.erase(0, len);
    // Reset JSON parse state since we removed prefix.
    ctx.json_start = std::string::npos;
    ctx.brace_depth = 0;
    ctx.bracket_depth = 0;
    ctx.in_string = false;
    ctx.escape_next = false;
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
    if (hold_is_unknown_tool_call(ctx.text_hold, ctx.known_tools)) {
        ctx.text_hold.clear();   // complete leak naming an unadvertised tool
        return;
    }
    ctx.sink(StreamTextDelta{ctx.text_hold});
    ctx.any_text_flushed = true;
    ctx.text_hold.clear();
}

// Guard against a SILENTLY EMPTY turn. Weak local models sometimes leak a
// truncated tool-call JSON into `content` and nothing else; flush_text_hold
// then drops it (correctly — see above) but the assistant message ends up
// with zero text and zero tool calls, so the user sees an empty bubble. If
// the whole stream produced no text, no salvaged tool, and no structured
// tool call, emit one short line so the turn isn't a blank void. Call this
// AFTER salvage/flush at every terminal close, BEFORE StreamFinished.
void ensure_nonempty_turn(StreamCtx& ctx) {
    if (ctx.any_text_flushed) return;
    if (ctx.any_structured_tool) return;
    if (ctx.stop_reason == StopReason::ToolUse) return;  // salvaged a call
    ctx.sink(StreamTextDelta{
        "(the model returned an empty or unparseable response)"});
    ctx.any_text_flushed = true;
}

// Try to emit a single tool call from a JSON object. Returns true if emitted.
// The JSON must have {"name": "...", "arguments": {...}} or use "function"
// as the name key (some models use this variant).
[[nodiscard]] bool emit_salvaged_tool(StreamCtx& ctx, std::string_view sv) {
    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object()) return false;

    // Extract tool name — models use different keys:
    //   - "name": "tool_name"           (standard)
    //   - "function": "tool_name"       (some models)
    //   - "tool": "tool_name"           (rare)
    std::string name;
    if (j.contains("name") && j["name"].is_string()) {
        name = j["name"].get<std::string>();
    } else if (j.contains("function") && j["function"].is_string()) {
        name = j["function"].get<std::string>();
    } else if (j.contains("tool") && j["tool"].is_string()) {
        name = j["tool"].get<std::string>();
    } else {
        return false;  // no recognizable name key
    }
    // Only salvage to a tool we actually advertised — never invent a call.
    bool known = false;
    for (const auto& t : ctx.known_tools) if (t == name) { known = true; break; }
    if (!known) return false;

    // Tools we NEVER auto-run from a leaked-content guess. Weak local models
    // reflexively emit these on greetings / small talk:
    //   - remember/forget/wipe_memory: mutate the user's memory store; only
    //     ever run on an EXPLICIT user request, never the model's initiative.
    //   - skill: a meta-tool the model hallucinates from the catalog block in
    //     its prompt (e.g. {"name":"skill","arguments":{"name":"greeting"}}),
    //     which then fails "not found" and loops.
    // Swallow the leaked JSON so no card is ever born (return true consumes it
    // from the hold); surfacing it just to fail it flashes/loops (bad UX).
    static constexpr std::string_view kNeverSalvage[] = {
        "remember", "forget", "wipe_memory", "skill"};
    for (auto t : kNeverSalvage) if (t == name) return true;

    // arguments may be an object (qwen) or a JSON string (some templates).
    std::string args = "{}";
    if (j.contains("arguments")) {
        const auto& a = j["arguments"];
        if (a.is_string())      args = a.get<std::string>();
        else if (a.is_object()) args = a.dump();
        else if (a.is_array())  args = a.dump();  // some models use array
    } else if (j.contains("parameters")) {
        const auto& p = j["parameters"];
        if (p.is_object())      args = p.dump();
        else if (p.is_string()) args = p.get<std::string>();
    }

    std::string call_id = "call_salvaged_" + std::to_string(ctx.salvage_seq++);
    ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
    ctx.sink(StreamToolUseDelta{args});
    ctx.sink(StreamToolUseEnd{});
    ctx.stop_reason = StopReason::ToolUse;
    return true;
}

// Process an array of tool calls (some models emit [{...}, {...}]).
// Returns true if at least one tool was emitted.
[[nodiscard]] bool emit_salvaged_tool_array(StreamCtx& ctx, std::string_view sv) {
    json arr;
    try { arr = json::parse(sv); } catch (...) { return false; }
    if (!arr.is_array()) return false;
    bool any = false;
    for (const auto& item : arr) {
        if (item.is_object() && emit_salvaged_tool(ctx, item.dump()))
            any = true;
    }
    return any;
}

// Scan text_hold for complete JSON objects/arrays that could be tool calls.
// Emits them immediately as they're found. Returns true if any were emitted.
// This is the core incremental salvage logic — tool cards appear DURING
// streaming, not after [DONE].
[[nodiscard]] bool try_incremental_salvage(StreamCtx& ctx) {
    if (ctx.any_structured_tool) return false;  // real tool_calls[] wins
    if (ctx.text_hold.empty()) return false;

    bool any_emitted = false;

    // Strip wrapper prefixes (opening tags/fences) and suffixes (closing)
    // that weak local models wrap around leaked tool calls. ONLY strip when
    // we see an ACTUAL wrapper — don't strip ordinary whitespace or content.
    // This is called at the start of incremental salvage and after a JSON
    // object is extracted (to consume trailing wrappers).
    auto skip_wrapper_prefix = [&]() {
        std::string_view sv{ctx.text_hold};
        std::size_t orig = sv.size();
        
        // Helper: trim leading whitespace from sv.
        auto ltrim = [&sv]() {
            while (!sv.empty() && (sv.front()==' '||sv.front()=='\t'
                                   ||sv.front()=='\n'||sv.front()=='\r')) {
                sv.remove_prefix(1);
            }
        };
        
        // Keep stripping wrappers until we can't find any more.
        // This handles cases like: <tool_call>```json\n{...}
        bool found_any = false;
        bool found_this_pass = true;
        while (found_this_pass) {
            found_this_pass = false;
            ltrim();
            
            // <tool_call> opening tag
            if (sv.starts_with("<tool_call>")) {
                sv.remove_prefix(11);
                found_this_pass = true;
                found_any = true;
                continue;
            }
            // </tool_call> closing tag
            if (sv.starts_with("</tool_call>")) {
                sv.remove_prefix(12);
                found_this_pass = true;
                found_any = true;
                continue;
            }
            // ```json fence
            if (sv.starts_with("```json")) {
                sv.remove_prefix(7);
                found_this_pass = true;
                found_any = true;
                continue;
            }
            // Bare ``` fence (with or without "json" suffix). The JSON body
            // follows after whitespace. Strip it unconditionally — a tool call
            // wrapper is NEVER followed by non-JSON content that matters.
            if (sv.starts_with("```")) {
                sv.remove_prefix(3);
                // Also strip "json" if present (model might send it separately).
                ltrim();
                if (sv.starts_with("json")) sv.remove_prefix(4);
                found_this_pass = true;
                found_any = true;
                continue;
            }
            // Bare "json" prefix (leftover from a streamed ```json fence where
            // ``` was stripped on an earlier delta). Strip it.
            if (sv.starts_with("json")) {
                sv.remove_prefix(4);
                found_this_pass = true;
                found_any = true;
                continue;
            }
        }
        
        if (!found_any) {
            // No wrapper found THIS call — but if we saw one on a PREVIOUS
            // call (saw_wrapper), still strip leading whitespace (the gap
            // between ```json and {).
            if (!ctx.saw_wrapper) return;
            // Fall through to strip whitespace after previously-seen wrapper.
        } else {
            ctx.saw_wrapper = true;
        }
        
        // Strip final whitespace after wrappers.
        ltrim();
        
        std::size_t removed = orig - sv.size();
        if (removed > 0) {
            ctx.text_hold.erase(0, removed);
            // Reset ALL parse state since we removed content.
            ctx.json_start = std::string::npos;
            ctx.brace_depth = 0;
            ctx.bracket_depth = 0;
            ctx.in_string = false;
            ctx.escape_next = false;
        }
    };

    skip_wrapper_prefix();
    if (ctx.text_hold.empty()) return false;

    // Scan character by character to find complete JSON objects/arrays.
    // We track brace/bracket depth to know when {} or [] is complete.
    for (std::size_t i = 0; i < ctx.text_hold.size(); ++i) {
        char c = ctx.text_hold[i];

        if (ctx.escape_next) {
            ctx.escape_next = false;
            continue;
        }

        if (ctx.in_string) {
            if (c == '\\') ctx.escape_next = true;
            else if (c == '"') ctx.in_string = false;
            continue;
        }

        // Not in string.
        if (c == '"') {
            ctx.in_string = true;
            continue;
        }

        if (c == '{') {
            if (ctx.brace_depth == 0 && ctx.bracket_depth == 0) {
                // Starting a new JSON object.
                if (ctx.json_start == std::string::npos) {
                    // Flush any prose BEFORE this JSON.
                    if (i > 0) {
                        flush_text_slice(ctx, i);
                        i = 0;  // Continue from new start.
                    }
                    ctx.json_start = 0;
                }
            }
            ctx.brace_depth++;
        } else if (c == '}') {
            if (ctx.brace_depth > 0) ctx.brace_depth--;
            // Complete object?
            if (ctx.brace_depth == 0 && ctx.bracket_depth == 0
                && ctx.json_start != std::string::npos) {
                std::size_t end = i + 1;
                std::string_view candidate{ctx.text_hold.data() + ctx.json_start,
                                           end - ctx.json_start};
                if (emit_salvaged_tool(ctx, candidate)) {
                    any_emitted = true;
                    // Remove the emitted JSON from hold.
                    ctx.text_hold.erase(0, end);
                    ctx.json_start = std::string::npos;
                    ctx.brace_depth = 0;
                    ctx.bracket_depth = 0;
                    // Skip trailing </tool_call> or ``` if present.
                    skip_wrapper_prefix();
                    // Restart scan from beginning of remaining hold.
                    i = static_cast<std::size_t>(-1);  // will be 0 after ++
                    continue;
                } else {
                    // Complete object that isn't a salvageable call. If it's
                    // tool-SHAPED but names an unadvertised tool (weak-model
                    // mistype), drop it instead of dumping raw JSON. Otherwise
                    // it's prose — flush as text.
                    if (hold_is_unknown_tool_call(candidate, ctx.known_tools)) {
                        ctx.text_hold.erase(0, end);
                        ctx.json_start = std::string::npos;
                        ctx.brace_depth = 0;
                        ctx.bracket_depth = 0;
                        skip_wrapper_prefix();
                    } else {
                        flush_text_slice(ctx, end);
                    }
                    i = static_cast<std::size_t>(-1);
                    continue;
                }
            }
        } else if (c == '[') {
            if (ctx.brace_depth == 0 && ctx.bracket_depth == 0) {
                // Starting a JSON array (some models emit [{...}, {...}]).
                if (ctx.json_start == std::string::npos) {
                    if (i > 0) {
                        flush_text_slice(ctx, i);
                        i = 0;
                    }
                    ctx.json_start = 0;
                }
            }
            ctx.bracket_depth++;
        } else if (c == ']') {
            if (ctx.bracket_depth > 0) ctx.bracket_depth--;
            // Complete array?
            if (ctx.bracket_depth == 0 && ctx.brace_depth == 0
                && ctx.json_start != std::string::npos) {
                std::size_t end = i + 1;
                std::string_view candidate{ctx.text_hold.data() + ctx.json_start,
                                           end - ctx.json_start};
                if (emit_salvaged_tool_array(ctx, candidate)) {
                    any_emitted = true;
                    ctx.text_hold.erase(0, end);
                    ctx.json_start = std::string::npos;
                    ctx.brace_depth = 0;
                    ctx.bracket_depth = 0;
                    skip_wrapper_prefix();
                    i = static_cast<std::size_t>(-1);
                    continue;
                } else {
                    // Not valid tool calls — flush as text.
                    flush_text_slice(ctx, end);
                    i = static_cast<std::size_t>(-1);
                    continue;
                }
            }
        }
    }

    return any_emitted;
}

// At finish: try to salvage any remaining held text as a tool call.
// This handles the case where the JSON was incomplete mid-stream but
// completes by [DONE]. Returns true if salvaged.
[[nodiscard]] bool try_salvage_tool_call(StreamCtx& ctx) {
    if (ctx.any_structured_tool) return false;
    if (ctx.text_hold.empty())   return false;

    // Final attempt at incremental salvage (might complete now).
    if (try_incremental_salvage(ctx)) return true;

    // Legacy path: try the whole hold as one object.
    std::string_view sv = strip_tool_call_wrappers(ctx.text_hold);
    if (sv.empty()) return false;

    // Could be a single object or an array.
    if (sv.front() == '{') {
        if (emit_salvaged_tool(ctx, sv)) {
            ctx.text_hold.clear();
            ctx.holding = false;
            return true;
        }
    } else if (sv.front() == '[') {
        if (emit_salvaged_tool_array(ctx, sv)) {
            ctx.text_hold.clear();
            ctx.holding = false;
            return true;
        }
    }
    return false;
}

// Handle one choices[0].delta object.
void handle_delta(StreamCtx& ctx, const json& delta) {
    // Plain assistant text.
    if (delta.contains("content") && delta["content"].is_string()) {
        const auto& s = delta["content"].get_ref<const std::string&>();
        if (!s.empty()) {
            // SIMPLE LOGIC:
            // 1. If we're already holding, append and check if it's still valid
            // 2. If salvage is still possible (start of response), check if this
            //    STARTS a tool call pattern. If not, emit and disable salvage.
            // 3. Once any prose is emitted, salvage is permanently disabled.
            
            if (ctx.holding) {
                // Accumulating a potential tool call.
                ctx.text_hold += s;
                // Try to extract complete JSON tool calls.
                (void)try_incremental_salvage(ctx);
                // If still holding, check if it can still be a tool call.
                if (ctx.holding && !could_be_tool_json(ctx.text_hold)) {
                    // Not a tool call — flush as prose.
                    flush_text_hold(ctx);
                    ctx.salvage_eligible = false;  // no more salvage attempts
                }
            } else if (ctx.salvage_eligible && !ctx.any_structured_tool) {
                // Start of response or between tool calls. Check if this could
                // be the beginning of a leaked tool call.
                if (could_be_tool_json(s)) {
                    // Start holding.
                    ctx.holding = true;
                    ctx.text_hold = s;
                    ctx.json_start = std::string::npos;
                    ctx.brace_depth = 0;
                    ctx.bracket_depth = 0;
                    ctx.in_string = false;
                    ctx.escape_next = false;
                    ctx.saw_wrapper = false;
                    (void)try_incremental_salvage(ctx);
                    // If it was a complete tool call, try_incremental_salvage
                    // already emitted it and cleared the hold.
                } else {
                    // Not a tool call pattern — emit as prose.
                    ctx.sink(StreamTextDelta{s});
                    ctx.any_text_flushed = true;
                    ctx.salvage_eligible = false;  // prose detected, no more salvage
                }
            } else {
                // Salvage disabled — just emit text directly.
                ctx.sink(StreamTextDelta{s});
                ctx.any_text_flushed = true;
            }
        }
    }

    // Tool-call fragments (structured).
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        if (!delta["tool_calls"].empty()) {
            ctx.any_structured_tool = true;
            ctx.holding = false;
            ctx.text_hold.clear();
            ctx.salvage_eligible = false;  // real tool call, no need for salvage
        }
        for (const auto& tc : delta["tool_calls"]) {
            const int index = tc.value("index", 0);
            if (index < 0) continue;
            if (static_cast<std::size_t>(index) >= ctx.tool_slots.size())
                ctx.tool_slots.resize(index + 1);
            auto& slot = ctx.tool_slots[index];

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

            if (index != ctx.active_tool_index) {
                close_active_tool(ctx);
            }

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
            ensure_nonempty_turn(ctx);
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
    //
    // If we've already salvaged a leaked tool call (stop_reason == ToolUse),
    // don't let the server's "stop" overwrite it — weak local models report
    // finish_reason=stop even when they emitted a tool call as JSON in content.
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        auto server_reason = parse_openai_finish(
            choice["finish_reason"].get<std::string_view>());
        if (ctx.stop_reason != StopReason::ToolUse)
            ctx.stop_reason = server_reason;
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

// ── Ollama native /api/chat protocol ────────────────────────────────────────
// The OpenAI-compat shim (/v1/chat/completions) makes weak local models leak
// tool calls as raw JSON in `content`. The native endpoint applies the model's
// chat template, returns structured `message.tool_calls`, and chats cleanly on
// a bare greeting. Request body shape differs (messages with tool_calls inline,
// tool results as role:"tool"), and the response is NDJSON: one JSON object per
// line, each `{"message":{...},"done":bool}`.

// Build the native `messages` array. Like the OpenAI shape but Ollama wants
// tool arguments as a JSON OBJECT (not a serialized string) and tool results
// as role:"tool" with `tool_name`.
[[nodiscard]] json build_native_messages(const std::vector<Message>& msgs) {
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
                // Ollama native: images is an array of base64 strings.
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

// Handle one native message delta (the `message` object of an NDJSON frame).
void handle_native_message(StreamCtx& ctx, const json& message) {
    // Structured tool calls win — no salvage needed on the native endpoint.
    if (message.contains("tool_calls") && message["tool_calls"].is_array()
        && !message["tool_calls"].empty()) {
        ctx.any_structured_tool = true;
        ctx.holding = false;
        ctx.text_hold.clear();
        int idx = 0;
        for (const auto& tc : message["tool_calls"]) {
            std::string name, args = "{}";
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    name = fn["name"].get<std::string>();
                if (fn.contains("arguments")) {
                    const auto& a = fn["arguments"];
                    if (a.is_string())      args = a.get<std::string>();
                    else if (!a.is_null())  args = a.dump();
                }
            }
            if (name.empty()) continue;
            std::string id = "call_native_"
                + std::to_string(ctx.salvage_seq++) + "_"
                + std::to_string(idx++);
            ctx.sink(StreamToolUseStart{ToolCallId{id}, ToolName{name}});
            ctx.sink(StreamToolUseDelta{args});
            ctx.sink(StreamToolUseEnd{});
            ctx.stop_reason = StopReason::ToolUse;
        }
    }
    // Assistant text. Ollama's native /api/chat is SUPPOSED to route tool
    // calls into the structured tool_calls[] channel above — but qwen2.5-coder
    // and friends emit a bare {"name":..,"arguments":..} that doesn't match
    // the chat template's <tool_call> wrapper, so Ollama leaves it in
    // `content`. Without salvage these models can't call ANY tool. So we run
    // content through the same hold/salvage machinery as the OpenAI-compat
    // path: a complete JSON object naming an ADVERTISED tool is executed; a
    // greeting leak / unknown-tool / memory tool is dropped (never shown as
    // raw JSON, never looped). handle_delta is the single source of truth.
    if (message.contains("content") && message["content"].is_string()) {
        const auto& s = message["content"].get_ref<const std::string&>();
        // Feed ONLY the content field — structured tool_calls were handled
        // above; passing the whole message would double-process them.
        if (!s.empty()) handle_delta(ctx, json{{"content", s}});
    }
}

// Parse one NDJSON line from /api/chat.
void dispatch_native_line(StreamCtx& ctx, std::string_view line) {
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
        handle_native_message(ctx, j["message"]);
    if (j.value("done", false)) {
        // Final frame carries usage + done_reason.
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

// NDJSON line feeder for /api/chat (newline-delimited JSON, not SSE).
void feed_ndjson(StreamCtx& ctx, const char* data, size_t len) {
    ctx.buf.append(data, len);
    auto& read_pos = ctx.read_pos;
    std::string_view buf{ctx.buf};
    while (true) {
        const auto nl = buf.find('\n', read_pos);
        if (nl == std::string_view::npos) break;
        std::string_view line = buf.substr(read_pos, nl - read_pos);
        read_pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        dispatch_native_line(ctx, line);
    }
    if (read_pos >= kSseCompactThreshold) {
        ctx.buf.erase(0, read_pos);
        read_pos = 0;
    }
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
        Endpoint ep{"localhost", 11434, "/api/chat",
                    "/api/tags", false, "ollama"};
        ep.native_api = true;
        return ep;
    }
    // Treat anything else as a raw "host[:port]" — defaults to https on 443,
    // plain http if a non-443 port is given (a local server convention).
    Endpoint ep;
    ep.label = "openai-compatible";
    std::string s{spec};
    if (auto colon = s.rfind(':'); colon != std::string::npos) {
        ep.host = s.substr(0, colon);
        try {
            int port_int = std::stoi(s.substr(colon + 1));
            ep.port = (port_int > 0 && port_int <= 65535)
                ? static_cast<std::uint16_t>(port_int)
                : 443;   // out of range → fall back to https default
        }
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
        // Skip empty-bytes images (a drained draft attachment that leaked
        // into the wrong thread) — a "data:...;base64," with no payload
        // makes the server reject the request.
        bool has_images = false;
        if (m.role == Role::User)
            for (const auto& img : m.images)
                if (!img.bytes.empty()) { has_images = true; break; }
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
                    if (img.bytes.empty()) continue;
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
            ensure_nonempty_turn(c);
            c.sink(StreamFinished{c.stop_reason});
        }
        c.terminated = true;
    };

    // ── Build the request body ──────────────────────────────────────────────
    const bool native = req.endpoint.native_api;
    json body;
    body["model"]  = req.model;
    body["stream"] = true;

    if (native) {
        // Ollama native /api/chat: system prompt as a role:"system" message,
        // structured tool_calls, NDJSON response.
        // num_predict = max output tokens (Ollama default is low ~128).
        body["options"] = {{"num_predict", req.max_tokens}};
        json messages = json::array();
        if (!req.system_prompt.empty())
            messages.push_back({{"role", "system"},
                                {"content", scrub_utf8(req.system_prompt)}});
        for (auto& m : build_native_messages(req.messages))
            messages.push_back(std::move(m));
        body["messages"] = std::move(messages);
        if (!req.tools.empty()) body["tools"] = build_tools(req.tools);
    } else {
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
    }

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
            if (native) feed_ndjson(ctx, chunk.data(), chunk.size());
            else        feed_sse(ctx, chunk.data(), chunk.size());
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

namespace {

[[nodiscard]] std::filesystem::path lm_home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return std::filesystem::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h)
        return std::filesystem::path{h};
#endif
    return {};
}

[[nodiscard]] std::string lm_read_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    if (s.size() > 64 * 1024) s.resize(64 * 1024);
    return s;
}

// CLAUDE.md tiers only. The Anthropic prompt also injects agent-authored
// learned-memory (load_recent_*) and the skills catalog, but those can run to
// thousands of tokens and demonstrably confuse small local models on simple
// prompts (a 14b answered "hi" with "I didn't understand" once the learned
// facts were present). Local models get the concise user-authored CLAUDE.md
// guidance and nothing else.
[[nodiscard]] std::string local_memory_blocks() {
    std::string user    = lm_read_file(lm_home_dir() / "CLAUDE.md");
    std::string project = lm_read_file(std::filesystem::path{"CLAUDE.md"});
    std::string local   = lm_read_file(std::filesystem::path{"CLAUDE.local.md"});

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

std::string_view local_model_prompt_addendum() {
    return
        "\n\nMost messages are answered in plain words — greetings, small "
        "talk, and questions you already know do NOT use a tool. Only call a "
        "tool when the user asks you to touch files, run a command, or look "
        "something up; then make exactly ONE call and wait for its result. "
        "Never call remember/forget/wipe_memory on your own.";
}

std::string local_model_system_prompt() {
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

    // Tuned for OpenAI-compatible / local models (Ollama, llama.cpp, vLLM).
    // Plainer and firmer than the Claude prompt: local models follow short
    // imperative rules better than long prose, and they read recent history
    // less reliably so the recall reminder is explicit.
    std::string out;
    out += "You are agentty, a terminal coding assistant. You are helpful, "
           "direct, and act on requests instead of asking which option to "
           "pick. Keep replies concise.\n\n";

    out += "CONVERSATION MEMORY\n"
           "- The full conversation so far is provided in the messages. "
           "ALWAYS use earlier messages to answer follow-up questions "
           "(names, files, decisions the user already gave you).\n"
           "- If the user told you a fact earlier (e.g. their name), recall "
           "it from the conversation; do not say you don't have it.\n\n";

    out += "TOOLS\n"
           "- Tools let you read/edit files and run commands. Call a tool "
           "ONLY when the task needs it (touch files, run a command, search "
           "the codebase). For greetings, chit-chat, or questions you can "
           "answer from the conversation, reply in plain text \u2014 do NOT call "
           "a tool.\n"
           "- To edit an existing file use `edit` (targeted change). Use "
           "`write` only to create a new file.\n"
           "- Make ONE tool call at a time and wait for its result before "
           "the next. Never invent a tool result.\n"
           "- Never call remember/forget/wipe_memory unless the user asks "
           "you to remember or forget something.\n\n";

    out += "OUTPUT\n"
           "- Output is rendered as GitHub-flavoured markdown in a terminal. "
           "Use fenced code blocks for code. Keep tables small.\n\n";

    out += "ENVIRONMENT\n";
    out += "- os: "; out += os_name; out += "\n";
    out += "- shell: "; out += shell; out += "\n";
    if (!cwd.empty()) { out += "- cwd: "; out += cwd; out += "\n"; }

    // User/project CLAUDE.md tiers only. Skills catalog + learned-memory are
    // omitted for local models (token bloat + confusion on small models);
    // skills still activate explicitly via /skill-name.
    out += local_memory_blocks();
    return out;
}

// ── Ollama /api/show probe ───────────────────────────────────────────────────
// Fetches a model's capabilities from Ollama. Returns std::nullopt on failure.
// Zed-style: only models that report "tools" in capabilities[] get tools.
namespace {
struct OllamaProbe {
    std::optional<bool> supports_tools;
    int context_window = 0;  // real model window from model_info.*.context_length
};

OllamaProbe probe_ollama_model(const AuthHeader& auth,
                               const Endpoint& ep,
                               const std::string& model_name) {
    OllamaProbe out;
    http::Request hreq;
    hreq.method     = http::HttpMethod::Post;
    hreq.host       = ep.host;
    hreq.port       = ep.port;
    hreq.path       = "/api/show";
    hreq.plaintext  = !ep.use_tls;
    hreq.headers    = build_request_headers(auth);
    hreq.body       = json{{"model", model_name}}.dump();
    hreq.max_body_bytes = 512 * 1024;  // /api/show can be large (modelfile)

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(2'000);
    tos.total   = std::chrono::milliseconds(5'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp || resp->status != 200) return out;

    try {
        auto j = json::parse(resp->body);
        // Tool capability (Zed-style): only models advertising "tools" get the
        // native function-call channel.
        if (j.contains("capabilities") && j["capabilities"].is_array()) {
            bool has_tools = false;
            for (const auto& cap : j["capabilities"])
                if (cap.is_string() && cap.get<std::string>() == "tools")
                    has_tools = true;
            out.supports_tools = has_tools;
        }
        // Real context window. /api/show returns model_info as a flat map of
        // arch-prefixed keys; the window lives under "<arch>.context_length"
        // (e.g. "qwen2.context_length": 32768, "llama.context_length": 8192).
        // The arch prefix varies per model, so scan for any key ending in
        // ".context_length" rather than hard-coding the architecture.
        if (j.contains("model_info") && j["model_info"].is_object()) {
            for (auto it = j["model_info"].begin(); it != j["model_info"].end(); ++it) {
                const std::string& key = it.key();
                if (key.size() >= 15
                    && key.compare(key.size() - 15, 15, ".context_length") == 0
                    && it.value().is_number_integer()) {
                    out.context_window = it.value().get<int>();
                    break;
                }
            }
        }
    } catch (...) {}
    return out;
}
} // namespace

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
        if (endpoint.native_api) {
            // Ollama /api/tags: {"models":[{"name":"qwen2.5-coder:7b",...}]}
            // Collect model names first, then probe /api/show for each to
            // determine tool support (Zed-style capability check).
            std::vector<std::string> names;
            for (const auto& m : j.value("models", json::array())) {
                auto id = m.value("name", "");
                if (!id.empty()) names.push_back(id);
            }
            // Probe each model's capabilities via /api/show. This adds a
            // round-trip per model but runs concurrently in the background
            // fetch. Without it we'd have no way to know if a model really
            // supports structured tool calls (Ollama's capabilities[].
            // "tools") vs. just leaking tool JSON into content.
            for (const auto& id : names) {
                auto probe = probe_ollama_model(auth, endpoint, id);
                ModelInfo info{
                    .id             = ModelId{id},
                    .display_name   = id,
                    .provider       = endpoint.label,
                    .supports_tools = probe.supports_tools,
                };
                if (probe.context_window > 0)
                    info.context_window = probe.context_window;
                result.push_back(std::move(info));
            }
        } else {
            for (const auto& m : j.value("data", json::array())) {
                auto id = m.value("id", "");
                if (id.empty()) continue;
                result.push_back(ModelInfo{
                    .id           = ModelId{id},
                    .display_name = id,
                    .provider     = endpoint.label,
                });
            }
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
        ensure_nonempty_turn(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

// NDJSON (native /api/chat) test harness. Drives feed_ndjson then mirrors
// run_stream_sync's terminal salvage/flush so a test observes the same Msg
// sequence the live native path produces.
std::vector<Msg> parse_ndjson_for_test(std::string_view ndjson_bytes,
                                       std::vector<std::string> known_tools) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.known_tools = std::move(known_tools);
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    feed_ndjson(ctx, ndjson_bytes.data(), ndjson_bytes.size());
    if (!ctx.terminated) {
        if (ctx.in_tool_use) { ctx.sink(StreamToolUseEnd{}); ctx.in_tool_use = false; }
        if (!try_salvage_tool_call(ctx)) flush_text_hold(ctx);
        ensure_nonempty_turn(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

} // namespace agentty::provider::openai
