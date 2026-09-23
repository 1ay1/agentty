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
// The observation tables: every way this spec-less wire spells a field lives
// there, not here. See lens.hpp for why a Lens and not an acp::Codec.
#include "agentty/provider/openai/dialect.hpp"

#include <map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <array>
#include <chrono>
#include <format>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "agentty/provider/debug.hpp"   // wire dump → logx `wire` channel
#include "agentty/provider/dialect.hpp"  // chat-vs-Responses routing (SSOT)
#include "agentty/provider/openai/responses_site.hpp"
#include "agentty/provider/registry.hpp" // endpoint columns: from_spec's SSOT
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/stream_scaffold.hpp"
#include "agentty/provider/usage.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/provider/wire/streamed.hpp"
#include "agentty/provider/wire/tool_calls.hpp"
#include "agentty/provider/wire_supersede.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // util::workspace_root/project_root — AGENTS.md anchor + walk start
#include "agentty/util/base64.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::openai {

using json = nlohmann::json;

namespace {

// ── UTF-8 scrub (same defence as the Anthropic transport) ───────────────────
// A tool output or pasted blob can carry invalid UTF-8; nlohmann's dump()
// throws on it. Replace every malformed byte with U+FFFD so the request
// builds instead of the turn dying with a json type_error. Shared strict
// implementation (rejects overlong encodings + surrogates) — see
// wire::scrub_utf8.
using wire::scrub_utf8;

// ── Per-tool-call streaming accumulator ─────────────────────────────────────
// One slot per OpenAI tool_calls[].index. We need to remember the id+name we
// saw on the opening delta so StreamToolUseEnd / salvage paths have them.
struct ToolCallSlot {
    std::string id;
    std::string name;
    // Everything decoded so far for this call. `delta.tool_calls[].function
    // .arguments` USUALLY carries fragments, but a coalescing proxy may repeat
    // the COMPLETE arguments in each chunk — blind concatenation turns that
    // into `{"a":1}{"a":1}`, which fails as "invalid args" exactly like the
    // Responses `.done` bug did. Keeping the running value lets unseen()
    // recognise a repeat and emit nothing.
    std::string args;
    bool started = false;   // StreamToolUseStart already emitted
    bool ended   = false;   // StreamToolUseEnd already emitted
};

struct StreamCtx {
    EventSink sink;
    // Model id, diagnostics only — stamps the Model-channel log lines so a
    // salvage/drop event names which model produced it.
    std::string model_id;

    // Mirrors req.show_reasoning. When the user has reasoning HIDDEN we
    // convert reasoning text deltas into heartbeats: the wire streams
    // reasoning_content unconditionally on this dialect, and capturing it
    // invisibly just bloats Message::thinking + the persisted session for
    // bytes never displayed and never replayed on this wire. Defaults true
    // so the test harnesses keep capture semantics.
    bool show_reasoning = true;

    // Byte-level stream framing lives in the shared wire helpers (see
    // include/agentty/provider/wire.hpp). Exactly one is driven per request:
    // `sse` for the /v1/chat/completions compat path, `ndjson` for Ollama's
    // native /api/chat. Both own their own buffer + read cursor + compaction;
    // the transport only supplies the per-event/per-line dispatch.
    wire::SseFramer  sse;
    wire::LineFramer ndjson;

    // Reused simdjson ondemand parser for the content-delta fast path
    // (see dispatch_data_fast). Stateful — caches its scratch across
    // iterate() calls so the hot path avoids a malloc per SSE frame.
    simdjson::ondemand::parser simd_parser;

    // Tool-call streaming state. Attribution (which chunk belongs to which
    // call) lives in wire::ToolCallTracker — it keys on (id, index) jointly
    // because neither alone is reliable across providers, and it is unit
    // tested without any SSE plumbing. Calls may interleave, so each has its
    // own lifecycle; there is no provider-global "active" call.
    wire::ToolCallTracker tools;
    bool any_structured_tool = false; // a real tool_calls[] delta arrived
    // An unattributable chunk poisoned the tool state: we cannot know which
    // call the bytes belonged to, so every call in flight may be missing
    // arguments. Set once, reported once, and it stops us emitting tool
    // events built from state we no longer trust.
    bool tool_attribution_failed = false;

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
    // Memory tools are normally too destructive to infer from leaked JSON.
    // The request setup enables only the operation the latest user explicitly
    // asked for, so weak/local models can honor "remember this" without making
    // unsolicited memory mutations possible.
    bool allow_remember_salvage = false;
    bool allow_forget_salvage = false;
    bool allow_wipe_salvage = false;

    // Mistral / Magistral (and some vLLM reasoning parsers) don't send a
    // separate reasoning_content field — they wrap the model's thinking in
    // [THINK]…[/THINK] (Mistral) or <think>…</think> (DeepSeek-R1 / Qwen /
    // QwQ / most local models) tags INSIDE `content`. We strip those inline
    // and re-route the enclosed text as reasoning (StreamThinkingDelta), so it
    // renders in the reasoning block instead of leaking into the answer (and
    // so the leading `[`/`<` never trips the tool-call salvage). Mirrors
    // vLLM's BaseThinkingReasoningParser contract:
    //   • in_think: currently inside a reasoning span (across deltas).
    //   • think_carry: a tag split across two content deltas ("[TH"|"INK]").
    //   • think_closed: the FIRST close tag was seen — everything after is
    //     content, even a later stray open tag (no re-entry).
    //   • think_seen_open: an OPEN tag was seen. Many models (Magistral) begin
    //     in reasoning with NO open tag — handled by reason_by_default below.
    //   • think_probing: still deciding whether the stream opens with a tag /
    //     starts in reasoning (buffers the very first bytes until sure).
    bool        in_think = false;
    bool        think_closed = false;
    bool        think_seen_open = false;
    bool        think_emitted_content = false;   // any non-think prose committed
    bool        think_probing = true;            // still deciding open-vs-default
    // Whether THIS model is known to begin reasoning with NO open tag
    // (Magistral, and DeepSeek-R1 which prepends <think> only sometimes). When
    // false, we NEVER treat leading text as implicit reasoning — a stray
    // "</think>"/"[/THINK]" in ordinary answer prose is kept verbatim. Set from
    // the model id. Explicit open+close tags are always honored regardless.
    bool        reason_by_default = false;
    std::string think_carry;   // partial open/close tag across deltas
    std::string think_probe;   // leading bytes buffered until reason-by-default decided

    // Incremental JSON parse state for salvage.
    int  brace_depth = 0;           // nested {} depth in current JSON object
    int  bracket_depth = 0;         // nested [] depth (for arrays of calls)
    bool in_string = false;         // inside a JSON string literal
    bool escape_next = false;       // next char is escaped in string
    std::size_t json_start = std::string::npos;  // offset where current JSON began
    bool saw_wrapper = false;  // have we seen any tool wrapper (```, <tool_call>) this hold?

    StopReason stop_reason = StopReason::Unspecified;
    bool terminated = false;
};

// Could `s` be the START of a leaked tool-call? This is only called at the
// beginning of a response (salvage_eligible=true). Once prose is detected,
// salvage is disabled permanently for this response. Shared classifier — see
// wire::could_be_tool_json (single source of truth; was duplicated here and in
// the ollama transport).
using wire::could_be_tool_json;

[[nodiscard]] StopReason parse_openai_finish(std::string_view fr) noexcept {
    // OpenAI finish_reason: "stop" | "length" | "tool_calls" |
    // "content_filter" | "function_call" (legacy). Map onto agentty's enum.
    if (fr == "stop")           return StopReason::EndTurn;
    if (fr == "length")         return StopReason::MaxTokens;
    if (fr == "tool_calls")     return StopReason::ToolUse;
    if (fr == "function_call")  return StopReason::ToolUse;
    return StopReason::Unspecified;
}

// Close every still-open tool call exactly once. OpenAI Chat can interleave
// argument deltas by tool_calls[].index and only gives a turn-level finish, so
// closing a call merely because another index emitted would truncate siblings.
void close_open_tools(StreamCtx& ctx) {
    // Attribution failed earlier in this response: we know some argument
    // bytes went unplaced, but not which call lost them. Announcing these
    // calls now would hand the model a set of tool invocations that look
    // complete and are not. The StreamError already went out; stay quiet.
    if (ctx.tool_attribution_failed) return;
    for (auto& slot : ctx.tools.all()) {
        // A call whose name never resolved to a known tool was never
        // announced (the start gate waits for a resolvable name so a
        // fragmented name can finish accumulating). Announce it now under
        // whatever accumulated, so it reaches the model as "unknown tool:
        // <full name>" instead of silently vanishing mid-turn.
        if (!slot.started && !slot.name.empty()) {
            if (slot.id.empty()) slot.id = "call_late";
            ctx.sink(StreamToolUseStart{ToolCallId{slot.id},
                                        ToolName{slot.name}});
            if (!slot.args.empty())
                ctx.sink(StreamToolUseDelta{ToolCallId{slot.id}, slot.args});
            slot.started = true;
        }
        if (!slot.started || slot.ended || slot.id.empty()) continue;
        ctx.sink(StreamToolUseEnd{ToolCallId{slot.id}});
        slot.ended = true;
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

// Split a content delta on inline reasoning tags and route the interior to the
// reasoning block. Handles BOTH delimiter families — Mistral [THINK]…[/THINK]
// and DeepSeek-R1/Qwen <think>…</think> — and the three real-world shapes
// vLLM's BaseThinkingReasoningParser handles:
//   1. explicit open+close:      "[THINK]…[/THINK]answer"
//   2. NO open tag (reason-by-default, common on Magistral): the stream begins
//      in reasoning and the FIRST close tag ends it — "…thoughts…[/THINK]answer"
//   3. no tags at all: pure content.
// After the first close tag EVERYTHING is content (a later stray open never
// re-enters reasoning). Tags split across deltas are carried in think_carry.
// The interior is emitted as StreamThinkingDelta (heartbeat when reasoning is
// hidden, matching the reasoning_content path); the returned string is the
// content prose the caller feeds into the tool-call salvage.
std::string extract_think_spans(StreamCtx& ctx, std::string_view in) {
    // Once reasoning is closed, everything is plain content — fast path.
    if (ctx.think_closed) {
        std::string s{in};
        if (!s.empty()) ctx.think_emitted_content = true;
        return s;
    }

    static constexpr std::string_view kOpens[]  = {"[THINK]", "<think>"};
    static constexpr std::string_view kCloses[] = {"[/THINK]", "</think>"};

    std::string prose;
    std::string buf = ctx.think_carry + std::string{in};   // prepend carried partial
    ctx.think_carry.clear();

    auto emit_reason = [&](std::string_view r) {
        if (r.empty()) return;
        if (ctx.show_reasoning) ctx.sink(StreamThinkingDelta{std::string{r}, ""});
        else                    ctx.sink(StreamThinkingDelta{"", ""});   // heartbeat
    };
    auto find_any = [&](const std::string_view (&set)[2], std::size_t from,
                        int& which) -> std::size_t {
        std::size_t best = std::string::npos; which = -1;
        for (int t = 0; t < 2; ++t) {
            std::size_t p = buf.find(set[t], from);
            if (p < best) { best = p; which = t; }
        }
        return best;
    };
    auto tail_is_partial = [&](const std::string_view (&set)[2],
                               std::size_t from) -> std::size_t {
        // Longest suffix of buf[from:] that is a proper prefix of a tag.
        std::size_t keep = buf.size();
        for (int t = 0; t < 2; ++t) {
            const auto& tag = set[t];
            for (std::size_t back = 1;
                 back < tag.size() && back <= buf.size() - from; ++back) {
                if (std::string_view{buf}.substr(buf.size() - back)
                        == tag.substr(0, back))
                    keep = std::min(keep, buf.size() - back);
            }
        }
        return keep;
    };

    // The core split is an inner lambda so EVERY exit flows through the
    // think_emitted_content update below (the probe must only fire at start).
    auto run = [&]() {
        std::size_t i = 0;
        while (i < buf.size()) {
            if (ctx.in_think) {
                int w = -1;
                const std::size_t pos = find_any(kCloses, i, w);   // CLOSE tag
                if (pos == std::string::npos) {
                    const std::size_t keep = tail_is_partial(kCloses, i);
                    emit_reason(std::string_view{buf.data() + i, keep - i});
                    ctx.think_carry.assign(buf, keep, std::string::npos);
                    return;
                }
                emit_reason(std::string_view{buf.data() + i, pos - i});
                ctx.in_think = false;
                ctx.think_closed = true;          // latch: no re-entry
                i = pos + kCloses[w].size();
                prose += std::string_view{buf.data() + i, buf.size() - i};
                return;
            }

            // REASON-BY-DEFAULT probe. At the very start we don't yet know if
            // this model opens with a tag, starts in reasoning with no tag
            // (Magistral), or has no reasoning at all. Buffer the leading bytes
            // in think_probe until the decision is forced by:
            //   • a CLOSE before any OPEN  → the buffer was implicit reasoning;
            //   • an OPEN                  → buffer (up to it) was content;
            //   • the buffer exceeding a cap → assume plain content (flush).
            // A bounded cap keeps a non-reasoning model from stalling.
            if (ctx.think_probing) {
                // Models NOT known to reason-by-default never treat leading
                // text as implicit reasoning — skip straight to explicit-tag
                // mode (no buffering, no stray-close false positives).
                if (!ctx.reason_by_default) { ctx.think_probing = false; continue; }
                int ow = -1, cw = -1;
                const std::size_t open  = find_any(kOpens, i, ow);
                const std::size_t close = find_any(kCloses, i, cw);
                if (close != std::string::npos
                    && (open == std::string::npos || close < open)) {
                    // Implicit reasoning: probe buffer + text up to close.
                    ctx.think_probing = false;
                    ctx.think_closed = true;
                    emit_reason(ctx.think_probe);
                    emit_reason(std::string_view{buf.data() + i, close - i});
                    ctx.think_probe.clear();
                    i = close + kCloses[cw].size();
                    prose += std::string_view{buf.data() + i, buf.size() - i};
                    return;
                }
                if (open != std::string::npos) {
                    // Explicit open: probe buffer + text before it was content.
                    ctx.think_probing = false;
                    prose += ctx.think_probe; ctx.think_probe.clear();
                    prose += std::string_view{buf.data() + i, open - i};
                    ctx.in_think = true;
                    ctx.think_seen_open = true;
                    i = open + kOpens[ow].size();
                    continue;
                }
                // Neither tag yet. Buffer, minus a possible partial tag tail.
                constexpr std::size_t kProbeCap = 256;
                const std::size_t keep = std::min(tail_is_partial(kOpens, i),
                                                  tail_is_partial(kCloses, i));
                ctx.think_probe.append(buf, i, keep - i);
                ctx.think_carry.assign(buf, keep, std::string::npos);
                if (ctx.think_probe.size() > kProbeCap) {
                    // Long leading run with no tag → this model isn't wrapping
                    // reasoning; commit the buffer as content and stop probing.
                    ctx.think_probing = false;
                    prose += ctx.think_probe; ctx.think_probe.clear();
                }
                return;
            }

            // Past the probe: explicit-tag mode only (open re-enters reasoning
            // until the first close latches think_closed).
            int w = -1;
            const std::size_t open = find_any(kOpens, i, w);
            if (open == std::string::npos) {
                const std::size_t keep = tail_is_partial(kOpens, i);
                prose += std::string_view{buf.data() + i, keep - i};
                ctx.think_carry.assign(buf, keep, std::string::npos);
                return;
            }
            prose += std::string_view{buf.data() + i, open - i};
            ctx.in_think = true;
            ctx.think_seen_open = true;
            i = open + kOpens[w].size();
        }
    };
    run();
    if (!prose.empty()) ctx.think_emitted_content = true;
    return prose;
}

void flush_text_hold(StreamCtx& ctx) {
    ctx.holding = false;
    if (ctx.text_hold.empty()) return;
    if (hold_is_truncated_tool_json(ctx.text_hold)) {
        // Weak-model heterogeneity event: the model tried to call a tool by
        // leaking JSON into content and got cut off. The bytes are dropped
        // (correct), but the ATTEMPT must be visible — this is what "the
        // model ignores tools" actually looks like on the wire.
        AGT_LOG(Model, Warn, "salvage.dropped_truncated",
                "model={} bytes={} head={}", ctx.model_id, ctx.text_hold.size(),
                std::string_view{ctx.text_hold}.substr(0, 200));
        ctx.text_hold.clear();   // truncated leaked tool call — drop
        return;
    }
    if (hold_is_unknown_tool_call(ctx.text_hold, ctx.known_tools)) {
        AGT_LOG(Model, Warn, "salvage.dropped_unknown_tool",
                "model={} bytes={} head={}", ctx.model_id, ctx.text_hold.size(),
                std::string_view{ctx.text_hold}.substr(0, 200));
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

    // Sensitive meta-tools are only salvaged when the latest user message
    // explicitly requested that exact class of operation. This keeps the old
    // greeting/small-talk safety gate while fixing weak models that can emit
    // tools only as JSON-in-content: their explicit "remember this" calls used
    // to be swallowed every time.
    if (name == "remember" && !ctx.allow_remember_salvage) return true;
    if (name == "forget" && !ctx.allow_forget_salvage) return true;
    if (name == "wipe_memory" && !ctx.allow_wipe_salvage) return true;
    if (name == "skill") return true;

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
    // The model bypassed the structured tool channel and we recovered it
    // from leaked content JSON. Works, but it's a MODEL CAPABILITY smell:
    // frequent salvage on a given (model, host) means its native tool
    // grammar is broken or disabled ("grammar set to none") — the exact
    // heterogeneity fact worth reporting upstream.
    AGT_LOG(Model, Info, "salvage.tool_call",
            "model={} tool={} args_bytes={} id={}",
            ctx.model_id, name, args.size(), call_id);
    ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
    ctx.sink(StreamToolUseDelta{ToolCallId{call_id}, args});
    ctx.sink(StreamToolUseEnd{ToolCallId{call_id}});
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
    // Reasoning / chain-of-thought text. Reasoning-capable models on the Chat
    // wire stream their thinking in a field PARALLEL to `content`, and the
    // ecosystem does not agree on its name — DeepSeek/vLLM say
    // `reasoning_content`, OpenAI's Responses API and some OpenRouter
    // passthroughs say `reasoning`.
    //
    // WHICH SPELLINGS EXIST IS NOT DECIDED HERE. dialect::reasoning_delta() is
    // the single source of truth for that, and it also encodes the ordering
    // rule (prefer the specific key, but fall through when it is EMPTY — some
    // proxies send an empty reasoning_content beside a populated reasoning,
    // and stopping at the first key that merely exists drops all reasoning
    // silently). Adding a provider's spelling is a one-line change there,
    // covered by openai_dialect_test.cpp against captured fixtures.
    //
    // Emit as StreamThinkingDelta — the SAME event the Anthropic transport
    // emits for thinking_delta — so the reducer / UI render it identically.
    // Also a liveness signal during a long reasoning pause before any visible
    // content.
    if (auto r = dialect::reasoning_delta()(delta)) {
        if (ctx.show_reasoning)
            ctx.sink(StreamThinkingDelta{*r, {}});
        else
            ctx.sink(StreamHeartbeat{});  // liveness only, no capture
    }

    // Structured content-parts array (Mistral reasoning models): `content`
    // arrives as a LIST of typed parts instead of a string —
    //   {"content":[{"type":"thinking",
    //                "thinking":[{"type":"text","text":"…"}]}]}
    // for chain-of-thought, and {"type":"text","text":"…"} parts for prose.
    // (Probed live on mistral-small-latest with reasoning_effort=high; the
    // final answer still arrives as a plain content STRING, handled below.)
    // Route thinking parts to the same StreamThinkingDelta the
    // reasoning_content path emits, and text parts into the normal prose
    // handling — without this branch the whole array form was silently
    // DROPPED: reasoning invisible and no liveness heartbeat during it.
    if (delta.contains("content") && delta["content"].is_array()) {
        for (const auto& part : delta["content"]) {
            if (!part.is_object()) continue;
            const std::string type = part.value("type", "");
            if (type == "thinking") {
                // Nested: thinking: [{type:"text", text:"…"}, …]
                std::string chunk;
                if (auto th = part.find("thinking");
                    th != part.end() && th->is_array()) {
                    for (const auto& seg : *th)
                        if (seg.is_object() && seg.value("type", "") == "text")
                            chunk += seg.value("text", "");
                } else if (auto tx = part.find("text");
                           tx != part.end() && tx->is_string()) {
                    chunk = tx->get<std::string>();   // flat variant
                }
                // A thinking PART arrived — that is itself a liveness signal,
                // even when its accumulated text is empty. Mistral streams
                // near-empty thinking segments during a long reasoning pass;
                // with reasoning hidden (show_reasoning=false) and no visible
                // prose yet, dropping these silently starves the stall
                // watchdog. Emit the reasoning when shown; otherwise ALWAYS
                // pulse a heartbeat for the part, empty text or not.
                if (ctx.show_reasoning) {
                    if (!chunk.empty())
                        ctx.sink(StreamThinkingDelta{chunk, {}});
                    else
                        ctx.sink(StreamHeartbeat{});
                } else {
                    ctx.sink(StreamHeartbeat{});
                }
            } else if (type == "text") {
                const std::string t = part.value("text", "");
                if (!t.empty()) {
                    // Prose from a parts array is never leaked tool JSON
                    // (structured emitters use the real tool_calls channel),
                    // so emit directly — mirrors the salvage-disabled path.
                    ctx.sink(StreamTextDelta{t});
                    ctx.any_text_flushed = true;
                    ctx.salvage_eligible = false;
                }
            }
        }
    }

    // Plain assistant text.
    if (delta.contains("content") && delta["content"].is_string()) {
        const auto& raw = delta["content"].get_ref<const std::string&>();
        // Mistral/Magistral inline their reasoning as [THINK]…[/THINK] inside
        // `content`. Peel those spans off as reasoning FIRST; `s` is only the
        // non-think prose. (If the model never emits THINK tags this is a
        // cheap identity pass — no tag found, prose == raw.) Doing it here also
        // stops a leading `[` of `[THINK]` from tripping the tool-call salvage.
        const std::string s = extract_think_spans(ctx, raw);
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
        int array_pos = -1;
        const std::size_t array_len = delta["tool_calls"].size();
        for (const auto& tc : delta["tool_calls"]) {
            ++array_pos;
            // `index` is how Chat Completions distinguishes PARALLEL calls:
            // fragments for different calls interleave in one stream and the
            // index is the only thing separating them. A provider that omits
            // it used to collapse every parallel call onto slot 0, so the
            // second call's arguments were appended to the first and both
            // were lost. Falling back to the payload's own array position
            // keeps them apart, which is what the field means when present.
            const bool has_wire_index =
                tc.contains("index") && tc["index"].is_number_integer();
            const int index = has_wire_index ? tc["index"].get<int>() : array_pos;
            if (index < 0) continue;
            // `index` is server-controlled. A hostile or buggy endpoint
            // can send an absurd one, so cap it before it reaches the
            // tracker — otherwise we allocate per-call state for it.
            //
            // When the field is ABSENT we fall back to the payload's own
            // array position. Chat Completions interleaves fragments for
            // parallel calls in one delta, and a provider that omits the
            // index leaves position as the only thing separating them: the
            // opening delta lists both calls, and the continuation delta
            // lists their fragments in the SAME order with no id and no
            // index. Without the fallback both fragments look like "no
            // identity at all", which with two calls in flight is correctly
            // reported as ambiguous — correct, but it drops arguments a
            // well-ordered server did give us enough to place.
            //
            // That reasoning only holds while position is EVIDENCE rather
            // than a guess: it requires the delta to restate every call in
            // flight, so slot N really is the Nth call. A delta carrying
            // FEWER elements than there are open calls has no such
            // correspondence — element 0 could be any of them — and using
            // position there silently appends one call's arguments to
            // another. In that case send no index at all and let the tracker
            // report it as unattributable.
            constexpr std::size_t kMaxToolSlots = 1024;
            const bool position_is_evidence =
                has_wire_index || ctx.tools.size() <= 1
                || array_len >= ctx.tools.size();
            std::optional<std::size_t> slot_index;
            if (position_is_evidence
                && static_cast<std::size_t>(index) < kMaxToolSlots)
                slot_index = static_cast<std::size_t>(index);

            // WHO does this chunk belong to?
            //
            // Delegated to wire::ToolCallTracker, which keys on (id, index)
            // jointly and fails rather than guessing when a chunk carries
            // neither. That rule is not obvious and we got it wrong twice:
            // index-only keying merged two parallel calls that reused index
            // 0, and an empty-id restatement wiped the call's identity so it
            // was never closed. Both are pinned in
            // tests/tool_call_identity_test.cpp, against the seam rather
            // than against an SSE fixture.
            std::string_view wire_id;
            if (tc.contains("id") && tc["id"].is_string())
                wire_id = tc["id"].get_ref<const std::string&>();

            const auto who = ctx.tools.attribute(
                wire::ChunkIdentity{wire_id, slot_index});
            if (!who) {
                // Ambiguous: no id, no index, several calls in flight. Any
                // pick is a coin flip, and the losing side appends these
                // bytes to another call's arguments — producing JSON that
                // still parses, so the corruption reaches a tool
                // invocation silently.
                //
                // Dropping the fragment avoids the corruption but keeps the
                // silence: the remaining arguments still parse, so the turn
                // runs a tool with SOME of its arguments missing and nothing
                // says so. A chunk we cannot attribute means every call in
                // flight is now suspect, so fail the turn — the same choice
                // zed makes (ToolCallAccumulator::entry returns
                // AmbiguousToolCallChunk and the mapper surfaces it as a
                // completion error rather than a best guess). Report once:
                // later chunks would otherwise each raise their own error.
                if (!ctx.tool_attribution_failed) {
                    ctx.tool_attribution_failed = true;
                    AGT_LOG(Wire, Warn, "openai.tool_args_unattributable",
                            "calls_in_flight={}", ctx.tools.size());
                    ctx.sink(StreamError{
                        "the provider sent a tool-call fragment that names "
                        "neither an id nor an index while "
                            + std::to_string(ctx.tools.size())
                            + " calls were in flight — it cannot be placed "
                              "without risking corrupted arguments",
                        std::nullopt});
                }
                continue;
            }

            // An index reused for a DIFFERENT id means the previous call at
            // that index is over. Close it here: an unclosed call leaves a
            // tool_use with no tool_result and hangs the turn, so splitting
            // without closing trades one bug for a worse one.
            if (who->displaced) {
                auto& old = ctx.tools.displaced(*who);
                if (old.started && !old.ended && !old.id.empty()) {
                    ctx.sink(StreamToolUseEnd{ToolCallId{old.id}});
                    old.ended = true;
                }
            }

            auto& slot = ctx.tools.at(*who);

            std::string fn_name;
            std::string fn_args;
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    fn_name = fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string())
                    fn_args = fn["arguments"].get<std::string>();
            }
            // The NAME can arrive in fragments too, not just the arguments.
            //
            // The spec's own accumulator (`accumulated.function.name ??=`)
            // takes the first non-null and never appends, which is right for
            // servers that send the whole name once. But real gateways split
            // it: `"re"` then `"ad"` for `read` (opencode #24137). Taking
            // only the first fragment dispatches a tool called `re`, which
            // fails "unknown tool" three times and kills the turn.
            //
            // So: append until the call is ANNOUNCED. The announce gate
            // below waits for a name that resolves to a real tool, which is
            // what makes appending safe — a server that sends the whole name
            // up front matches on the first fragment and never accumulates a
            // second, and one that splits it builds up until it does.
            //
            // An empty name on a continuation is IGNORED, never assigned:
            // several gateways (Azure, local proxies) re-send
            // `"name": ""` on argument chunks, and letting that through
            // wipes an established name (A3S-Lab/Code #139).
            if (!fn_name.empty()) {
                if (slot.started) {
                    // Already announced under a resolved name — a later
                    // fragment cannot rename the call.
                } else if (slot.name.empty()) {
                    slot.name = fn_name;
                } else if (slot.name != fn_name) {
                    slot.name += fn_name;
                }
            }

            // Announce once the name names something we can actually run.
            //
            // Waiting for a KNOWN tool is what lets the accumulation above
            // terminate: a complete name resolves immediately, a partial one
            // does not and we keep collecting. A name that never resolves is
            // announced by close_open_tools at end of turn instead, so the
            // call still reaches the model as "unknown tool: <full name>"
            // rather than vanishing.
            const bool name_is_known = [&] {
                if (ctx.known_tools.empty()) return true;   // nothing declared
                for (const auto& t : ctx.known_tools)
                    if (t == slot.name) return true;
                return false;
            }();
            if (!slot.started && !slot.name.empty() && name_is_known) {
                if (slot.id.empty())
                    slot.id = "call_" + std::to_string(index);
                ctx.sink(StreamToolUseStart{ToolCallId{slot.id},
                                            ToolName{slot.name}});
                slot.started = true;
            }

            if (!fn_args.empty() && slot.started) {
                // A snapshot is when the server restates the COMPLETE arguments
                // (longer than what we have, or equal to it on retransmission).
                // A fragment is never longer than the accumulated buffer yet.
                // This distinguishes:
                //  (1) Snapshot longer: fn_args > slot.args and slot.args is a prefix
                //  (2) Idempotent repeat: fn_args == slot.args exactly
                //  (3) Fragment: fn_args.size() == 1 (e.g., "{" from token-by-token)
                //      that happens to match the start of our accumulated buffer —
                //      this is legitimate new data, not a retransmission.
                const bool snapshot = (fn_args.size() > slot.args.size()
                                    && fn_args.compare(0, slot.args.size(), slot.args) == 0)
                                   || fn_args == slot.args;
                const auto fresh = wire::unseen(slot.args, fn_args, snapshot);
                if (!fresh.empty())
                    ctx.sink(StreamToolUseDelta{ToolCallId{slot.id},
                                                std::string{fresh}});
            }
        }
    }
}

// Fast path for the dominant OpenAI SSE frame: a single choice carrying only
// a `delta.content` string, once salvage is no longer in play. That's the
// bulk of a streaming turn's volume. simdjson ondemand walks the bytes in
// place (over the SseFramer's padded buffer) and emits one StreamTextDelta
// without materialising an nlohmann DOM.
//
// Tri-state, for the SAME reason as Anthropic's fast path: simdjson unescapes
// strings in place, mutating the buffer, so once we've read the content string
// the caller must NOT re-parse.
//
//   Unparseable — didn't match the exact simple shape BEFORE any string was
//                 extracted; buffer pristine, caller falls back to nlohmann.
//   Handled     — emitted the content delta; done.
//
// Bails (→ Unparseable) on ANYTHING that needs the full path: salvage still
// eligible / currently holding, an `error`/`usage`/`finish_reason` field, a
// `tool_calls` delta, a missing/empty/non-string content, more than one
// choice-relevant field. Correctness first — a false Handled would drop a
// tool call or a usage frame. The guards are cheap key probes (no unescape).
static_assert(wire::kSseSimdPadding == simdjson::SIMDJSON_PADDING,
              "wire::kSseSimdPadding must equal simdjson::SIMDJSON_PADDING");
enum class FastData { Unparseable, Handled };
FastData dispatch_data_fast(StreamCtx& ctx, std::string_view data, char* padded) {
    if (!padded) return FastData::Unparseable;
    // Only safe once prose is flowing and no leaked-tool-call hold is open.
    // In any other salvage state handle_delta's hold machinery must run.
    if (ctx.holding || ctx.salvage_eligible) return FastData::Unparseable;
    // A usage frame (stream_options.include_usage) or a mid-body error frame
    // both need the full path; a cheap substring probe rejects them before we
    // even parse (these are rare, so the probe cost is paid seldom).
    if (data.find("\"usage\"") != std::string_view::npos
        || data.find("\"error\"") != std::string_view::npos
        || data.find("\"finish_reason\"") != std::string_view::npos
        || data.find("\"tool_calls\"") != std::string_view::npos
        // Reasoning text (reasoning_content / reasoning) rides PARALLEL to
        // content and may precede it in the object; the forward-only fast path
        // can't safely peek a field it hasn't reached, so hand any
        // reasoning-bearing frame to handle_delta (random-access). The probe
        // matches both field names via the common "reasoning prefix.
        || data.find("\"reasoning") != std::string_view::npos
        // Mistral (Small/Medium 3.5+ with reasoning_effort=high) streams the
        // model's chain-of-thought as a STRUCTURED content array:
        //   delta.content = [{"type":"thinking","thinking":[{"type":"text",…}]}]
        // and only later flips content to a plain answer STRING. The fast path
        // reads content as a string field, so a thinking-array frame would be
        // silently dropped (no reasoning render, no liveness heartbeat → a long
        // post-tool reasoning pass can trip the stall watchdog). Once prose is
        // flowing (salvage disabled) this frame reaches the fast path on later
        // turns / interleaved thinking, so probe for the thinking-part marker
        // and hand any structured-content frame to handle_delta.
        || data.find("\"thinking\"") != std::string_view::npos)
        return FastData::Unparseable;

    const std::size_t cap = data.size() + simdjson::SIMDJSON_PADDING;
    simdjson::ondemand::document doc;
    if (ctx.simd_parser.iterate(padded, data.size(), cap).get(doc))
        return FastData::Unparseable;

    simdjson::ondemand::array choices;
    if (doc.find_field_unordered("choices").get_array().get(choices))
        return FastData::Unparseable;
    auto it = choices.begin();
    if (it == choices.end()) return FastData::Unparseable;
    simdjson::ondemand::object choice;
    if ((*it).get_object().get(choice)) return FastData::Unparseable;

    simdjson::ondemand::object delta;
    if (choice.find_field_unordered("delta").get_object().get(delta))
        return FastData::Unparseable;

    // Extracting the content string may unescape into the buffer — this is the
    // commit point; never return Unparseable past here.
    std::string_view content;
    if (delta.find_field_unordered("content").get_string().get(content))
        return FastData::Unparseable;
    if (!content.empty()) {
        ctx.sink(StreamTextDelta{std::string{content}});
        ctx.any_text_flushed = true;
    }
    return FastData::Handled;
}

// Parse + dispatch one SSE `data:` payload.
void dispatch_data(StreamCtx& ctx, std::string_view data) {
    if (data.empty()) return;
    if (data == "[DONE]") {
        close_open_tools(ctx);
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
        // Same SSOT as the HTTP-status path below: dialect::error_message()
        // knows every envelope this wire uses, so neither site re-derives it.
        std::string msg = dialect::error_message()(j).value_or("unknown error");
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }
    // Bare error shape ({"code":500,"message":"…","type":"…"}) — llama.cpp
    // emits this WITHOUT an "error" wrapper on some versions. A frame with
    // no choices, a numeric error code ≥400 and a message is a failure, not
    // a delta; dropping it silently was part of the local-model dead loop.
    // Guard: real OpenAI-dialect frames always carry "object" (e.g.
    // "chat.completion.chunk") — its presence means content, not an error,
    // however error-shaped the other keys look.
    if (!j.contains("choices") && !j.contains("object")
        && j.contains("message")
        && j["message"].is_string() && j.contains("code")
        && j["code"].is_number_integer()
        && j["code"].get<int>() >= 400) {
        StreamError err{j["message"].get<std::string>(), std::nullopt};
        const int code = j["code"].get<int>();
        if (code <= 599) err.http_status = code;   // typed classification
        ctx.sink(std::move(err));
        ctx.terminated = true;
        return;
    }

    // Usage can arrive on a final frame (when stream_options.include_usage
    // is set) OR be attached to the last choices frame. Shared extractor — see
    // usage::from_openai (single source of truth for the OpenAI-compat shape).
    if (j.contains("usage")) {
        if (auto su = usage::from_openai(j["usage"])) ctx.sink(*su);
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
        close_open_tools(ctx);
    }
}

// SSE line feeder. OpenAI streams `data: {json}\n\n` frames (no `event:`
// lines), so the parser is simpler than Anthropic's: accumulate `data:`
// lines, dispatch on the blank-line terminator. The framing itself is the
// shared wire::SseFramer; OpenAI just ignores the (always-empty) event name.
void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
    ctx.sse.feed(data, len, [&](std::string_view event, std::string_view payload,
                                char* padded) {
        // llama.cpp (and some proxies) deliver mid-stream failures as a
        // NAMED SSE event — `event: error` + a JSON body — under HTTP 200.
        // OpenAI itself never names events, so the old parser ignored the
        // name entirely: the error frame fell through to dispatch_data,
        // whose j.contains("error") check only catches the {"error":…}
        // TOP-LEVEL shape — llama.cpp's named-event body is the bare error
        // object ({"code":500,"message":…,"type":…}), so it was silently
        // DROPPED: the stream then closed clean, the turn fabricated
        // "(empty response)", and the retry machinery re-fired forever —
        // the local-model dead loop. Surface it as a real StreamError.
        if (event == "error" && !ctx.terminated) {
            std::string msg{payload.empty() ? std::string_view{"stream error"}
                                            : payload};
            int code = 0;   // numeric error code → typed classification
            try {
                auto j = json::parse(payload);
                if (j.is_object()) {
                    // Envelope shapes live in dialect::error_message() — the
                    // SSOT for how this spec-less wire spells an error. Three
                    // exist ({"error":{"message"}}, {"error":"…"} flattened by
                    // some gateways, and llama.cpp's bare {"message","code"}),
                    // and an unrecognised one surfaces to the user as "the
                    // model stopped for no reason", which is the worst
                    // possible diagnostic. Parsing them here too would be a
                    // second partial list that drifts from the first.
                    if (auto m = dialect::error_message()(j)) msg = std::move(*m);
                    // llama.cpp stamps the would-be HTTP status in `code`
                    // (500 template failure, 400 bad request, 503 busy).
                    // Carrying it into StreamError.http_status routes the
                    // reducer through the TYPED classifier — without it the
                    // string sniff can misfile a deterministic per-request
                    // failure ("failed to read connection") as Transient
                    // and re-loop.
                    if (j.contains("code") && j["code"].is_number_integer())
                        code = j["code"].get<int>();
                    else if (j.contains("error") && j["error"].is_object()
                             && j["error"].contains("code")
                             && j["error"]["code"].is_number_integer())
                        code = j["error"]["code"].get<int>();
                }
            } catch (...) { /* keep raw payload as the message */ }
            StreamError err{std::move(msg), std::nullopt};
            if (code >= 400 && code <= 599) err.http_status = code;
            ctx.sink(std::move(err));
            ctx.terminated = true;
            return;
        }
        // [DONE] and empty frames go straight to the full path (terminal
        // handling lives there). Otherwise try the content-delta fast path;
        // it returns Unparseable — leaving `payload` pristine — for anything
        // that needs salvage/usage/tool handling.
        if (!payload.empty() && payload != "[DONE]"
            && dispatch_data_fast(ctx, payload, padded) == FastData::Handled)
            return;
        dispatch_data(ctx, payload);
    });
}

// True iff an assistant message carries any tool_calls (whose results must
// follow as `role:"tool"` messages in OpenAI's format).
[[nodiscard]] bool is_assistant_with_results(const Message& m) noexcept {
    return wire::is_assistant_with_results(m);
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
    // Count terminal-carrying tool results so each can be assigned a recency
    // rank (0 = newest) for the shared age-tiered wire budget. See
    // wire::cap_tool_result_aged — old dumps fade to a tight head+tail so a
    // 60 KiB read from 30 calls ago stops replaying in full every turn.
    int total_tool_results = 0;
    for (const auto& m : msgs)
        if (is_assistant_with_results(m))
            total_tool_results += static_cast<int>(m.tool_calls.size());
    int tool_results_emitted = 0;
    const auto superseded = wire::superseded_read_ids(msgs);
    for (const auto& m : msgs) {
        const bool has_text  = !m.text.empty();
        const bool has_tools = is_assistant_with_results(m);
        bool has_images = false;
        if (m.role == Role::User)
            for (const auto& img : m.images)
                if (!img.bytes().empty()) { has_images = true; break; }

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
                    if (!img.bytes().empty())
                        imgs.push_back(agentty::util::base64_encode(img.bytes()));
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
                const int recency_rank = total_tool_results - 1 - tool_results_emitted;
                ++tool_results_emitted;
                std::string out = tc.output();
                bool is_error = !tc.is_terminal() || tc.is_failed() || tc.is_rejected();
                if (out.empty()) {
                    if (tc.is_rejected())       out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                } else if (!is_error && superseded.count(tc.id.value)) {
                    out = std::string{wire::kSupersededReadPointer};
                } else {
                    out = wire::cap_tool_result_aged(out, recency_rank, is_error);
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
            ctx.sink(StreamToolUseDelta{ToolCallId{id}, args});
            ctx.sink(StreamToolUseEnd{ToolCallId{id}});
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
        // Final frame carries usage + done_reason. Shared extractor — see
        // usage::from_ollama.
        if (auto su = usage::from_ollama(j)) ctx.sink(*su);
        auto reason = j.value("done_reason", std::string{"stop"});
        if (ctx.stop_reason != StopReason::ToolUse)
            ctx.stop_reason = (reason == "length") ? StopReason::MaxTokens
                                                   : StopReason::EndTurn;
    }
}

// NDJSON line feeder for /api/chat (newline-delimited JSON, not SSE).
void feed_ndjson(StreamCtx& ctx, const char* data, size_t len) {
    ctx.ndjson.feed(data, len, [&](std::string_view line) {
        dispatch_native_line(ctx, line);
    });
}

} // namespace

// ── Endpoint presets ────────────────────────────────────────────────────────
Endpoint Endpoint::from_spec(std::string_view spec) {
    auto eq = [](std::string_view a, const char* b) {
        return a == std::string_view{b};
    };
    if (eq(spec, "codex")) {
        // "codex" is a LEGACY ALIAS: it used to be its own "Codex API" picker
        // row, but that was byte-identical to `openai` (same host, same
        // /v1/chat/completions, same bearer auth — only the key env var
        // differed). The row was dropped and CODEX_API_KEY folded into the
        // `openai` preset's auth_env; the spec still resolves here so
        // `--provider codex` and persisted configs keep working. The genuinely
        // distinct Codex path is `chatgpt` (Responses API over ChatGPT OAuth).
        // Resolved through the registry so it can't drift from `openai`.
        spec = "openai";
    }
    // Empty spec = the default OpenAI-family host, resolved through the
    // registry like every other preset (no second copy of its host/path).
    if (spec.empty()) spec = "openai";
    if (eq(spec, "chatgpt") || eq(spec, "codex-cli")) {
        // The native ChatGPT path never dials this endpoint: main.cpp
        // dispatches this label to chatgpt::ChatGptProvider. Keep a
        // local-shaped sentinel so generic OpenAI-family selection
        // invariants stay true. The label is canonicalised to "chatgpt";
        // "codex-cli" is still accepted as a legacy spec (persisted configs,
        // `--provider codex-cli`) but normalises to the same endpoint.
        return Endpoint{"localhost", 0, "/", "/", false, "chatgpt"};
    }
    // NOTE `copilot` resolves from its registry row below. The native
    // Copilot path (copilot::CopilotProvider) overrides the host at request
    // time from the token exchange's `endpoints.api` (Individual/Business/
    // Enterprise all differ); the row is the sentinel + label.

    // ── Preset rows: ONE lookup over the provider registry ──────────────
    // This used to be a 14-arm if-chain that re-stated host/path facts the
    // registry row already half-carried. The two drifted (the `openai` row
    // claimed Wire::OpenAIResponses while this function dialled
    // /v1/chat/completions), so the endpoint columns now live on the row and
    // this is a pure projection of them. Adding a provider touches the table
    // only — there is no arm here to forget.
    if (const auto* row = provider::preset_for(spec);
        row && row->http_dialled()) {
        Endpoint ep{std::string{row->host}, row->port, std::string{row->path},
                    std::string{row->models_path}, row->use_tls,
                    std::string{row->id}};
        ep.native_api = row->native_api;
        return ep;
    }

    if (eq(spec, "llama.cpp")) {
        // Not a preset row (a llama.cpp/vLLM/LM Studio server is just a
        // generic OpenAI-compatible host — see the registry NOTE), but kept
        // as a convenience alias: OpenAI REST dialect on /v1 (NOT Ollama's
        // native /api/chat), port 8080, no auth, plain HTTP (localhost).
        return Endpoint{"localhost", 8080, "/v1/chat/completions",
                         "/v1/models", false, "llama.cpp"};
    }
    // Full URL form: "https://host[:port][/path]" or "http://host[:port][/path]".
    // The path is a BASE PREFIX — agentty appends /chat/completions and /models
    // to it, so "https://gw.example.com/api" → path "/api/chat/completions".
    // No path after scheme://authority → empty prefix → "/chat/completions".
    // This lets users reach servers that don't serve on /v1 (e.g. gateways
    // proxying to /api/chat/completions). Bare "host[:port]" specs (no scheme)
    // fall through to the legacy handler below and keep the /v1 default.
    if (spec.starts_with("https://") || spec.starts_with("http://")) {
        Endpoint ep;
        ep.label = std::string{spec};
        std::string_view s{spec};
        // Optional "#name" FRAGMENT: names this entry so several accounts on
        // the SAME endpoint stay distinct ("https://ollama.com/v1#work" and
        // "…/v1#personal" are two settings keys → two API keys, two saved
        // models, two picker rows). The fragment is a pure local tag —
        // stripped here so it never reaches the wire; kept in `label` so
        // rows/badges display it.
        if (auto hash = s.rfind('#'); hash != std::string_view::npos)
            s = s.substr(0, hash);
        const bool tls = spec.starts_with("https://");
        ep.use_tls = tls;
        s.remove_prefix(tls ? 8 : 7);   // "https://" = 8, "http://" = 7
        ep.port = tls ? 443 : 80;

        // Split authority from path at the first '/'.
        auto slash = s.find('/');
        std::string_view authority = (slash == std::string_view::npos)
            ? s : s.substr(0, slash);
        // Strip userinfo: "user:pass@host" dials `host`, not the whole
        // string. Left in, it became the SNI name and the Host header, so
        // TLS failed with a name mismatch that read as "certificate
        // problem" rather than "your URL has credentials in it".
        if (auto at = authority.rfind('@'); at != std::string_view::npos)
            authority = authority.substr(at + 1);

        std::string prefix;   // base path prefix, may be empty
        if (slash != std::string_view::npos) {
            prefix = std::string{s.substr(slash)};
            // Drop ?query and #fragment — neither belongs in a base path.
            // Without this, "/v1?key=x" produced the path
            // "/v1?key=x/chat/completions", which every server 404s.
            if (auto q = prefix.find_first_of("?#"); q != std::string::npos)
                prefix.resize(q);
            // Strip trailing '/' so "/api/" → "/api", and "/" → "".
            while (prefix.size() > 1 && prefix.back() == '/')
                prefix.pop_back();
            if (prefix == "/") prefix.clear();
            // Collapse duplicate slashes: a hand-edited or concatenated URL
            // yields "//v1", and "//v1/chat/completions" is a different
            // path to most routers.
            for (std::size_t i = 1; i + 1 <= prefix.size();) {
                if (prefix[i] == '/' && prefix[i - 1] == '/') prefix.erase(i, 1);
                else ++i;
            }
            // THE ONE USERS ACTUALLY HIT. The docs say the path is a
            // PREFIX and we append /chat/completions — but what a provider
            // hands you, and what you therefore paste, is the full
            // endpoint URL. Appending to that gives
            // "/v1/chat/completions/chat/completions": a 404 on every
            // request, from a URL that is visibly correct.
            //
            // So accept both spellings. A prefix that already ends in the
            // completions path IS the endpoint; take the part before it.
            for (std::string_view tail : {"/chat/completions", "/completions"}) {
                if (prefix.size() > tail.size()
                    && std::string_view{prefix}.ends_with(tail)) {
                    prefix.resize(prefix.size() - tail.size());
                    break;
                }
            }
        }
        // BARE URL → /v1 DEFAULT. This endpoint speaks the OpenAI dialect,
        // and that dialect lives under /v1 everywhere — api.openai.com/v1,
        // llama.cpp, vLLM, LM Studio, Ollama's compat shim all serve
        // /v1/chat/completions; the OpenAI SDK convention is that base_url
        // CONTAINS /v1. Deriving a bare "/chat/completions" from
        // "http://host:8080/" produced a path almost no server answers —
        // the user got a 404 on every request (or worse, a streamed error
        // the old parser dropped) and reported it as "the model locks".
        // An EXPLICIT path ("/api", "/openai/v1") is always kept verbatim,
        // so unusual gateways stay expressible.
        if (prefix.empty()) prefix = "/v1";

        // Split host from port. IPv6 literals are bracketed — "[::1]" or
        // "[::1]:8080" — so the port colon is the one AFTER "]", never a colon
        // inside the address. For a plain host the port is after the last ':'.
        if (!authority.empty() && authority.front() == '[') {
            auto close = authority.find(']');
            if (close != std::string_view::npos) {
                ep.host = std::string{authority.substr(1, close - 1)};  // strip [ ]
                auto rest = authority.substr(close + 1);                // "" or ":8080"
                if (rest.size() > 1 && rest.front() == ':') {
                    try {
                        int port_int = std::stoi(std::string{rest.substr(1)});
                        ep.port = (port_int > 0 && port_int <= 65535)
                            ? static_cast<std::uint16_t>(port_int)
                            : (tls ? 443 : 80);
                    } catch (...) { ep.port = tls ? 443 : 80; }
                }
            } else {
                ep.host = std::string{authority};  // malformed; dial as-is
            }
        } else if (auto colon = authority.rfind(':');
                   colon != std::string_view::npos) {
            ep.host = std::string{authority.substr(0, colon)};
            try {
                int port_int = std::stoi(std::string{authority.substr(colon + 1)});
                ep.port = (port_int > 0 && port_int <= 65535)
                    ? static_cast<std::uint16_t>(port_int)
                    : (tls ? 443 : 80);
            }
            catch (...) { ep.port = tls ? 443 : 80; }
        } else {
            ep.host = std::string{authority};
        }

        // Empty host (e.g. "https:///path" or "https://:443/") → fall back
        // to the default endpoint rather than dialing an empty host.
        if (ep.host.empty()) return Endpoint{};

        // Hostnames are case-insensitive (RFC 4343), but we compare them as
        // strings in several places — SNI, the Host header, and the
        // per-provider credential lookup keyed on the endpoint. "API.Foo.com"
        // and "api.foo.com" are the same server, and a user who capitalises
        // one should not get a second, key-less identity.
        for (char& c : ep.host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        ep.path       = prefix + "/chat/completions";
        ep.models_path = prefix + "/models";
        return ep;
    }
    // Treat anything else as a raw "host[:port]" — defaults to https on 443,
    // plain http if a non-443 port is given (a local server convention).
    // The label carries the raw spec so the model badge / provider readout
    // shows the actual endpoint ("my-box.lan:8080") rather than a generic
    // placeholder.
    Endpoint ep;
    ep.label = std::string{spec};
    std::string s{spec};
    // "#name" fragment: local multi-account tag, never dialed (see the URL
    // branch above). Strip BEFORE the port split so "host:8080#work" parses
    // the port, not "8080#work".
    if (auto hash = s.rfind('#'); hash != std::string::npos)
        s.resize(hash);
    // Same normalisation as the URL arm: a bare spec can carry userinfo
    // ("user@my-box.lan:8080") and can be capitalised. Both reach the same
    // dialling code, so both need the same cleanup — having one arm accept a
    // shape the other rejects is its own bug report.
    if (auto at = s.rfind('@'); at != std::string::npos)
        s.erase(0, at + 1);
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
    for (char& c : ep.host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
    // Recency ranks for the shared age-tiered wire budget (0 = newest tool
    // result). Same policy as Anthropic — see wire::cap_tool_result_aged.
    int total_tool_results = 0;
    for (const auto& m : t.messages)
        if (is_assistant_with_results(m))
            total_tool_results += static_cast<int>(m.tool_calls.size());
    int tool_results_emitted = 0;
    // Earlier reads whose file a later turn re-read/edited are collapsed to a
    // one-line pointer instead of their full body — see wire::superseded_read_ids.
    const auto superseded = wire::superseded_read_ids(t);
    for (const auto& m : t.messages) {
        const bool has_text   = !m.text.empty();
        // Skip empty-bytes images (a drained draft attachment that leaked
        // into the wrong thread) — a "data:...;base64," with no payload
        // makes the server reject the request.
        bool has_images = false;
        if (m.role == Role::User)
            for (const auto& img : m.images)
                if (!img.bytes().empty()) { has_images = true; break; }
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
                    if (img.bytes().empty()) continue;
                    std::string url = "data:" + img.media_type + ";base64,"
                                    + agentty::util::base64_encode(img.bytes());
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
                const int recency_rank = total_tool_results - 1 - tool_results_emitted;
                ++tool_results_emitted;
                // Send whatever output we have; non-terminal calls (rare on
                // the OpenAI path) still need a paired tool message or the
                // next request 400s on an unanswered tool_call_id.
                std::string out = tc.output();
                bool is_error = !tc.is_terminal() || tc.is_failed() || tc.is_rejected();
                if (out.empty()) {
                    if (tc.is_rejected())      out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                } else if (!is_error && superseded.count(tc.id.value)) {
                    out = std::string{wire::kSupersededReadPointer};
                } else {
                    out = wire::cap_tool_result_aged(out, recency_rank, is_error);
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

// ── Header builder ───────────────────────────────────────────────────────────
http::Headers build_request_headers(const AuthHeader& auth,
                                    const Endpoint& endpoint) {
    http::Headers h;
    h.push_back({"accept", "application/json"});
    h.push_back({"content-type", "application/json"});
    h.push_back({"user-agent", "agentty/" AGENTTY_VERSION});
    // Ask for an UNCOMPRESSED body. agentty's HTTP client does not decode
    // content-encodings (no gzip/deflate/br inflater), so a gateway that
    // gzips its JSON response — z.ai's GLM Coding Plan does this on /models
    // even with no Accept-Encoding sent — hands us bytes we can't parse: the
    // model list comes back empty and the picker shows nothing (issue #30).
    // The streaming path already forced `identity` via append_sse_no_buffer;
    // emitting it HERE makes every OpenAI-family request (models listing,
    // /api/show, the /v1 probe, the stream) uniformly safe — no request can
    // forget it. Harmless duplicate with append_sse_no_buffer on the stream
    // (same name+value; servers honour the first/identical directive).
    h.push_back({"accept-encoding", "identity"});
    // Both arms carry a plain secret; the OpenAI family sends it as a Bearer
    // token (or a custom raw header). See auth::bearer_token — the single
    // source of truth for OpenAI-family token extraction.
    std::string key = auth::bearer_token(auth);
    // Diagnostic: which credential is going out (length + last-4 only, never
    // the full key). Enable with AGENTTY_LOG=debug,chan=openai.auth to debug a
    // 401 — an empty/short key means the provider switch didn't install this
    // host's credential (wrong active auth header).
    util::dbglog("openai.auth",
        "host=" + endpoint.host + " key_len=" + std::to_string(key.size())
        + (key.size() >= 4 ? " tail=" + key.substr(key.size() - 4) : ""));
    if (key.empty()) return h;
    if (!endpoint.auth_header_name.empty()) {
        // Custom header name (--auth-header): key goes out raw, no "Bearer "
        // prefix, for gateways that authenticate with e.g. `X-API-Key`.
        // Lowercased — header names are case-insensitive and every other
        // header agentty sends is lowercase.
        std::string name = endpoint.auth_header_name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        h.push_back({std::move(name), std::move(key)});
    } else {
        // Default: `Authorization: Bearer <token>` for the whole OpenAI
        // family — OpenAI/Groq/OpenRouter all use bearer keys. (ApiKeyHeader's
        // raw `sk-...` value goes out the same way; there's no `x-api-key`
        // here.)
        h.push_back({"authorization", "Bearer " + key});
    }
    // Endpoint-specific static headers (e.g. Copilot's editor-identification
    // block). Appended last so they can't be clobbered by the defaults above.
    for (const auto& kv : endpoint.extra_headers)
        h.push_back({kv.first, kv.second});
    return h;
}

// Infer only explicit memory-tool intent from the latest real user message.
// This is a safety gate for JSON-in-content salvage, not a general NLP policy:
// false negatives merely leave the call swallowed; false positives could mutate
// memory, so keep the vocabulary narrow and action-specific.
void set_memory_salvage_intent(StreamCtx& ctx, const Request& req) {
    std::string text;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role == Role::User && !it->is_proactive_context()) {
            text = it->text;
            break;
        }
    }
    std::ranges::transform(text, text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](std::string_view s) { return text.find(s) != std::string::npos; };

    ctx.allow_remember_salvage = has("remember") || has("don't forget")
        || has("do not forget") || has("keep in mind") || has("from now on");
    ctx.allow_forget_salvage = has("forget") || has("remove the memory")
        || has("remove memory") || has("drop the memory");
    ctx.allow_wipe_salvage = has("wipe memory") || has("wipe your memory")
        || has("forget everything") || has("clean slate");
}

// ── Streaming entry point ────────────────────────────────────────────────────
provider::StreamResult run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    // ── Dialect fork ─────────────────────────────────────────────────────
    // This is the ONE chokepoint every OpenAI-family turn passes through
    // (Copilot and Kimi delegate here too), so the chat-vs-Responses choice
    // belongs here rather than duplicated in each caller.
    //
    // It is not an optimisation. Per OpenAI's reasoning guide, Chat
    // Completions rejects tool calling for GPT-5.4+ at any reasoning_effort
    // other than `none`, and GPT-6-class models drop chat function calling
    // entirely — agentty always sends tools, so for those models this fork is
    // the difference between a working turn and a 400. dialect_for() owns the
    // decision (see provider/dialect.hpp) and self-corrects from live 404s.
    //
    // Deliberately AFTER nothing and BEFORE the auth check: the Responses
    // path does its own credential resolution with its own host-phrased
    // error, so falling through to the chat check first would report the
    // wrong endpoint in the message.
    if (dialect_for(req.endpoint.label, req.model) == Dialect::Responses) {
        openai::ResponsesEndpoint rep;
        if (responses_endpoint_for(req.endpoint.label, rep)) {
            rep.provider_id   = req.endpoint.label;
            rep.extra_headers = req.endpoint.extra_headers;
            // openai::Request and provider::Request are distinct types (the
            // former carries chat-only fields like weak_model/native_api), so
            // project across explicitly rather than assuming layout overlap.
            provider::Request rreq;
            rreq.model          = std::move(req.model);
            rreq.system_prompt  = std::move(req.system_prompt);
            rreq.messages       = std::move(req.messages);
            rreq.tools          = std::move(req.tools);
            rreq.max_tokens     = req.max_tokens;
            rreq.context_window = req.context_window;
            rreq.auth           = std::move(req.auth);
            rreq.retry_count    = req.retry_count;
            rreq.cancel         = std::move(cancel);
            rreq.effort         = req.effort;
            rreq.show_reasoning = req.show_reasoning;
            rreq.session_key    = req.session_key;
            return stream_responses(rep, std::move(rreq), std::move(sink));
        }
        // Row advertises no /responses (a custom host, or the user overrode
        // the endpoint). Fall through to chat: a degraded turn beats none.
    }

    // Ollama and other local servers accept an empty key. Only error out when
    // the endpoint is a TLS/hosted one that needs auth.
    if (req.endpoint.use_tls && is_empty(req.auth)) {
        sink(StreamError{"not authenticated — set the provider's API key "
                         "(e.g. OPENAI_API_KEY) or run 'agentty login'"});
        return provider::StreamResult::failed("not authenticated");
    }

    StreamCtx ctx;
    ctx.sink = std::move(sink);
    ctx.model_id = req.model;
    ctx.show_reasoning = req.show_reasoning;
    // Models that BEGIN in reasoning with no open tag: Magistral (Mistral's
    // reasoning line) and the DeepSeek-R1 family. For these, leading content
    // before the first close tag is implicit reasoning. Other models only get
    // EXPLICIT [THINK]/<think> spans routed — never leading text — so a stray
    // close tag in ordinary prose is kept verbatim.
    // The family list is a MODEL fact and lives in the catalog
    // (reasons_by_default) — one authority for every transport.
    ctx.reason_by_default = agentty::reasons_by_default(req.model);
    set_memory_salvage_intent(ctx, req);
    // Tools we advertised this turn — the salvage path only converts a
    // leaked-JSON "tool call" into a real one when it names one of these.
    ctx.known_tools.reserve(req.tools.size());
    for (const auto& t : req.tools) ctx.known_tools.push_back(t.name);

    // ── Build the request body ──────────────────────────────────────────
    // One pure function, asserted directly in openai_request_body_test.cpp.
    // Keeping it out of this hot path is what lets the conformance tiers be
    // a contract rather than a comment.
    const bool native = req.endpoint.native_api;
    json body = build_request_body(req);

    std::string body_str;
    try {
        body_str = body.dump();
    } catch (const nlohmann::json::exception& e) {
        ctx.sink(StreamError{std::string{"request build failed (invalid UTF-8): "}
                             + e.what()});
        ctx.sink(StreamFinished{StopReason::Unspecified});
        return provider::StreamResult::failed("request build failed: invalid UTF-8");
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
    hreq.headers = build_request_headers(req.auth, req.endpoint);
    http::append_sse_no_buffer(hreq.headers);
    hreq.body    = std::move(body_str);

    // Request metadata + the full body land on the `wire` channel of the one
    // structured log (see provider/debug.hpp). Body is NOT truncated: a
    // request that a server rejects is exactly the case where the last bytes
    // matter, and a clipped dump sends you back to guessing.
    AGT_LOG(Wire, Debug, "openai.request",
            "POST {}://{}:{}{} model={} native={} bytes={}",
            req.endpoint.use_tls ? "https" : "http", req.endpoint.host,
            static_cast<unsigned>(req.endpoint.port), req.endpoint.path,
            req.model, native ? 1 : 0, hreq.body.size());
    AGT_LOG(Wire, Trace, "openai.request.body", "raw={}", hreq.body);

    provider::StreamScaffold sc;
    sc.dialect = native ? "ollama-native" : "openai-chat";
    sc.sink    = ctx.sink;
    sc.feed    = [&](std::string_view chunk) {
        if (native) feed_ndjson(ctx, chunk.data(), chunk.size());
        else        feed_sse(ctx, chunk.data(), chunk.size());
        return true;
    };
    http::StreamHandler handler = sc.handler();

    // Standard ladder, with the ONE legitimate per-transport knob: LOCAL
    // (plaintext) servers send nothing during prompt processing — llama.cpp
    // on consumer hardware can grind for minutes before the first token, and
    // a 90 s idle cut mid-processing forced a retry that re-processes the
    // same prompt from scratch, forever (the local dead loop). 10 min for
    // plaintext endpoints; Esc still cancels instantly.
    http::Timeouts tos = provider::stream_timeouts(
        req.endpoint.use_tls ? std::chrono::milliseconds(90'000)
                             : std::chrono::milliseconds(600'000));

    // Keep a copy of the cancel token: moved into the stream call, but
    // finish_stream needs it to distinguish a user cancel from a transport
    // error at the post-loop.
    http::CancelTokenPtr cancel_for_end = cancel;
    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    // One end-of-turn line + the raw error body on failure — the pair every
    // "why did my provider fail?" report needs. Mirrors the Anthropic
    // transport's anthropic.response/anthropic.error.body seam so a shared
    // log reads the same regardless of which backend was active.
    // Uniform end-of-turn pair via the scaffold (Debug summary + Warn raw
    // error body), host + terminated appended as dialect detail.
    sc.log_result(bool(result),
                  result ? std::string_view{} : result.error().render(),
                  std::format("host={} terminated={}",
                              req.endpoint.host, ctx.terminated ? 1 : 0));

    // Whole post-loop through the SHARED epilogue: one terminal event, correct
    // precedence, identical to every other provider. on_any_end closes an open
    // tool block on both paths; before_finish (success only) salvages a
    // leaked-JSON tool call / flushes held text / guarantees a non-empty turn.
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
                if (j.contains("error") && j["error"].is_object()
                    && j["error"].contains("message"))
                    msg += ": " + j["error"]["message"].get<std::string>();
                else if (j.contains("message"))
                    msg += ": " + j["message"].get<std::string>();
                else if (!sc.error_body.empty())
                    msg += ": " + sc.error_body.substr(0, 300);
            } catch (...) {
                if (!sc.error_body.empty()) msg += ": " + sc.error_body.substr(0, 300);
            }
            if (sc.http_status == 401 || sc.http_status == 403)
                msg += "  (check the provider API key)";
            // A 404 on a local OpenAI-compatible server is one of TWO things:
            // the chat path doesn't exist on this server (custom-host spec
            // missing its /v1 prefix — llama.cpp only serves /v1/…), or the
            // model id isn't loaded/known. Name both, server-neutrally — the
            // old hint said 'ollama pull' to llama.cpp/vLLM users.
            if (sc.http_status == 404 && !req.endpoint.use_tls)
                msg += "  (404 from " + req.endpoint.host + ": either the "
                       "path '" + req.endpoint.path + "' doesn't exist on "
                       "this server \xe2\x80\x94 most need the spec to end in /v1 \xe2\x80\x94 "
                       "or the model '" + req.model + "' isn't loaded. "
                       "Check with Ctrl-P \xe2\x86\x92 Custom host, or pick a listed "
                       "model with ^/)";
            return msg;
        },
        .retry_after = sc.retry_after_hint,
        .transport_error_message = [&]() -> std::string {
            std::string msg = std::string{"http: "} + result.error().render();
            // Local backend unreachable — the daemon almost certainly isn't
            // running. Name the concrete fix, without assuming WHICH server
            // (ollama serve / llama-server / vllm serve all apply).
            if (!req.endpoint.use_tls)
                msg += "  (is the local server running on "
                     + req.endpoint.host + ":"
                     + std::to_string(req.endpoint.port)
                     + "? start it, or fix the host with Ctrl-P \xe2\x86\x92 "
                       "Custom host)";
            return msg;
        },
        .before_finish = [&ctx]() {
            if (!try_salvage_tool_call(ctx)) flush_text_hold(ctx);
            ensure_nonempty_turn(ctx);
        },
        .on_any_end = [&ctx]() {
            close_open_tools(ctx);
        },
    });
}

namespace {

// CLAUDE.md tiers only. The Anthropic prompt also injects agent-authored
// learned-memory (load_recent_*) and the skills catalog, but those can run to
// thousands of tokens and demonstrably confuse small local models on simple
// prompts (a 14b answered "hi" with "I didn't understand" once the learned
// facts were present). Local models get the concise user-authored CLAUDE.md
// guidance and nothing else. The user/project/local wrapper is the shared
// wire::claude_md_blocks (also used by the Ollama transport). AGENTS.md
// (AAIF standard, project-scoped) is prepended via wire::agents_md_block
// so the standardized public project guidance appears BEFORE the personal
// CLAUDE.md tiers — same ordering the Anthropic prompt uses.
[[nodiscard]] std::string local_memory_blocks() {
    return wire::agents_md_block(
               "Project guidance following the open AGENTS.md standard "
               "(agents.md, stewarded by the Agentic AI Foundation under "
               "the Linux Foundation). Treat as authoritative public "
               "project conventions.",
               tools::util::workspace_root(),
               tools::util::project_root(),
               wire::resolve_global_agents_md())
         + wire::claude_md_blocks(
               "Project-specific guidance the user has authored. Treat these as "
               "persistent context for THIS workspace and user.");
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
    try { cwd = std::filesystem::current_path().string(); }
    catch (const std::exception& e) { util::dbglog("openai.local_prompt.cwd", e.what()); }
    catch (...) { util::dbglog("openai.local_prompt.cwd", "non-std exception"); }

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
           "Use fenced code blocks for code. Keep tables small. LaTeX math "
           "renders too: `$…$` inline, `$$…$$` or a ```math fence for "
           "display (fractions, roots, sums, matrices).\n\n";

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
    // Ollama's /api/show capabilities[] also advertises "thinking" for
    // reasoning-capable models (deepseek-r1, qwen3, qwq, gpt-oss, magistral
    // …) — a LIVE, per-model, authoritative declaration. nullopt = probe
    // failed / older ollama without the field.
    std::optional<bool> supports_thinking;
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
    hreq.headers    = build_request_headers(auth, ep);
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
            bool has_thinking = false;
            for (const auto& cap : j["capabilities"])
                if (cap.is_string()) {
                    const auto& s = cap.get_ref<const std::string&>();
                    if (s == "tools")    has_tools = true;
                    if (s == "thinking") has_thinking = true;
                }
            out.supports_tools    = has_tools;
            out.supports_thinking = has_thinking;
        }
        // Real context window. /api/show returns model_info as a flat map of
        // arch-prefixed keys; the window lives under "<arch>.context_length"
        // (e.g. "qwen2.context_length": 32768, "llama.context_length": 8192).
        // The arch prefix varies per model, so scan for any key ending in
        // ".context_length" rather than hard-coding the architecture. This
        // mirrors what Ollama's own CLI does in cmd/cmd.go's showInfo.
        //
        // Accept a FLOAT as well as an integer. Ollama itself sends a plain
        // integer — verified against a real 0.34.2 daemon, which answers
        // "qwen3.context_length":40960 — but ModelInfo is typed
        // map[string]any, so nothing in the protocol guarantees that, and
        // LiteLLM already emits this class of field as a float (16385.0).
        // Being strict here buys nothing and silently drops a window.
        if (j.contains("model_info") && j["model_info"].is_object()) {
            for (auto it = j["model_info"].begin(); it != j["model_info"].end(); ++it) {
                const std::string& key = it.key();
                if (key.size() >= 15
                    && key.compare(key.size() - 15, 15, ".context_length") == 0
                    && it.value().is_number()) {
                    const double d = it.value().get<double>();
                    if (d > 0 && d < 2e9) {
                        out.context_window = static_cast<int>(d);
                        break;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        util::dbglog("openai.probe_ollama_model.parse", e.what());
    } catch (...) {
        util::dbglog("openai.probe_ollama_model.parse", "non-std exception");
    }
    return out;
}
} // namespace

// The exact JSON agentty puts on the wire. Pure: Request in, json out.
// See the header for why this is not inline in the stream path, and
// conformance.hpp for the evidence behind each tier decision below.
json build_request_body(const Request& req) {
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
        if (!req.tools.empty()) {
            body["tools"] = wire::openai_chat_tools(req.tools);
            body["tool_choice"] = "auto";
        }
        return body;
    }

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

    if (!req.tools.empty()) {
        body["tools"] = wire::openai_chat_tools(req.tools);
        body["tool_choice"] = "auto";
    }

    // Prompt-cache routing. OpenAI auto-caches prefixes >=1024 tokens;
    // sending a stable prompt_cache_key pins a conversation's identical
    // system+tools+history prefix to one cache node. HOSTED TIER: local
    // servers reject the field and their KV cache is prefix-automatic
    // anyway, so there is nothing to gain and a 400 to lose.
    if (req.endpoint.use_tls && !req.session_key.empty())
        body["prompt_cache_key"] = req.session_key;

    // Reasoning effort. PROBED TIER: the catalog already excluded models
    // that can't (or must not) take it — e.g. Mistral MAGISTRAL reasons
    // natively and REJECTS reasoning_effort (422), so the catalog excludes
    // it and effort arrives empty here. So this needs no capability
    // re-check. Hosted TLS only; local servers reject the field. NOTE:
    // reasoning TEXT still streams for excluded models — the response-side
    // reasoning handler is unconditional (see handle_delta).
    if (req.endpoint.use_tls && !req.effort.empty())
        body["reasoning_effort"] = req.effort;

    return body;
}

// ── Model listing ────────────────────────────────────────────────────────────
// One actionable sentence per failure. The taxonomy and the wording live
// together so they cannot drift.
std::string HostProbe::explain() const {
    switch (failure) {
        case Failure::None:
            return {};
        case Failure::Unreachable:
            return "nothing listening \xe2\x80\x94 is the server running on "
                   "that host:port?";
        case Failure::NeedsKey:
            // NOT a typo in the address. Say so, or the user starts editing a
            // URL that was already correct.
            return "HTTP " + std::to_string(http_status)
                 + " \xe2\x80\x94 the endpoint is right, it needs an API key";
        case Failure::NotAnApi:
            // The single most common custom-host mistake: pasting the
            // dashboard / docs URL instead of the API base.
            return "that's a web page, not an API \xe2\x80\x94 use the API "
                   "base URL (usually ends in /v1)";
        case Failure::NoModelList:
            return "reachable, but no model list at any known path "
                   "\xe2\x80\x94 check the path prefix (often /v1)";
        case Failure::HttpError:
            return "HTTP " + std::to_string(http_status)
                 + " \xe2\x80\x94 no model list at any known path";
    }
    return "probe failed";
}

HostProbe probe_host(const AuthHeader& auth, const Endpoint& endpoint) {
    HostProbe out;
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(3'000);
    tos.total   = std::chrono::milliseconds(6'000);

    // Worst failure seen so far, so the LAST attempt cannot erase the most
    // informative diagnosis. Ordering matters: a 401 on the configured path
    // is far more useful than a 404 on the fallback, because it means the
    // address was right all along.
    auto note = [&](HostProbe::Failure f) {
        const auto rank = [](HostProbe::Failure x) {
            switch (x) {
                case HostProbe::Failure::NeedsKey:    return 5;  // most useful
                case HostProbe::Failure::NotAnApi:    return 4;
                case HostProbe::Failure::NoModelList: return 3;
                case HostProbe::Failure::HttpError:   return 2;
                case HostProbe::Failure::Unreachable: return 1;
                case HostProbe::Failure::None:        return 0;
            }
            return 0;
        };
        if (rank(f) > rank(out.failure)) out.failure = f;
    };

    auto attempt = [&](const std::string& path) -> bool {
        http::Request r;
        r.method    = http::HttpMethod::Get;
        r.host      = endpoint.host;
        r.port      = endpoint.port;
        r.path      = path;
        r.plaintext = !endpoint.use_tls;
        if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
            r.dial_host = ov.host;
            r.dial_port = ov.port;
        }
        r.headers        = build_request_headers(auth, endpoint);
        r.max_body_bytes = 2ull * 1024 * 1024;
        const auto t0   = std::chrono::steady_clock::now();
        auto resp       = http::default_client().send(r, tos);
        const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!resp) { note(HostProbe::Failure::Unreachable); return false; }
        out.http_status = resp->status;
        if (resp->status == 401 || resp->status == 403) {
            // The address is CORRECT — it answered, and it wants credentials.
            // Treat as the most informative outcome short of success.
            note(HostProbe::Failure::NeedsKey);
            return false;
        }
        if (resp->status != 200) { note(HostProbe::Failure::HttpError); return false; }

        // 200 + HTML is the dashboard-URL mistake. Detect it BEFORE trying to
        // parse, so the diagnosis is "that's a web page" instead of a JSON
        // error the user cannot act on. Verified live: https://yolo-auto.com
        // /models returns 200 text/html with a full SPA.
        const std::string_view body{resp->body};
        const auto first = body.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos
            && (body.compare(first, 1, "<") == 0)) {
            note(HostProbe::Failure::NotAnApi);
            return false;
        }

        // Count models from either shape. {"data":[…]} = OpenAI dialect;
        // {"models":[…]} = Ollama /api/tags.
        try {
            auto j = json::parse(resp->body);
            if (j.contains("data") && j["data"].is_array()) {
                out.dialect     = HostProbe::Dialect::OpenAiCompat;
                out.model_count = static_cast<int>(j["data"].size());
            } else if (j.contains("models") && j["models"].is_array()) {
                out.dialect     = HostProbe::Dialect::OllamaNative;
                out.model_count = static_cast<int>(j["models"].size());
            } else {
                note(HostProbe::Failure::NoModelList);
                return false;                    // 200 but not a model list
            }
        } catch (...) { note(HostProbe::Failure::NoModelList); return false; }
        out.models_path = path;
        out.latency_ms  = static_cast<long>(ms);
        out.failure     = HostProbe::Failure::None;
        return true;
    };

    // 1. The configured path (explicit prefix honoured). 2. The /v1 default.
    // 3. Ollama's native /api/tags — a bare daemon on 11434 or a proxy.
    if (attempt(endpoint.models_path)) return out;
    if (endpoint.models_path != "/v1/models" && attempt("/v1/models"))
        return out;
    if (attempt("/api/tags")) return out;
    return out;   // dialect None; failure carries the best diagnosis
}

namespace detail {

// The context window a gateway ADVERTISES for one /v1/models row, or 0 when
// it says nothing.
//
// There is no standard field here — OpenAI's own /v1/models returns only
// {id, object, created, owned_by}, so every gateway that wants to publish a
// window invented its own place to put it. Reading all of them is the
// difference between a 1M-token model working out of the box and it being
// silently clamped to a default:
//
//   LiteLLM     model_info.max_input_tokens  (also .max_tokens as a
//               fallback; a config's `model_info:` block passes straight
//               through to /v1/models and /model/info)
//   OpenRouter  context_length, and top_provider.context_length — the
//               latter is the window of the endpoint that will actually
//               serve the request, so it wins when the two disagree
//   vLLM        max_model_len (added to /v1/models in vllm#4643)
//   llama.cpp / n_ctx, and the meta.n_ctx_train an ollama-style probe
//   others      reports
//
// Order matters where a row carries more than one: prefer the field that
// describes THIS deployment (top_provider, max_model_len) over a catalog
// figure for the model family, because a gateway routinely serves a model
// at less than its theoretical maximum.
//
// Returns 0 rather than a default so the caller can distinguish "the
// gateway told us" from "nobody knows" — those deserve different
// treatment, and conflating them is what pinned every custom-host model to
// the built-in default.
[[nodiscard]] inline int advertised_context_window(const nlohmann::json& m) {
    auto as_int = [](const nlohmann::json& v) -> int {
        if (v.is_number_integer())  return v.get<int>();
        if (v.is_number_unsigned()) return static_cast<int>(v.get<std::uint64_t>());
        if (v.is_number_float())    return static_cast<int>(v.get<double>());
        // Some proxies stringify numeric metadata.
        if (v.is_string()) {
            try { return std::stoi(v.get<std::string>()); } catch (...) {}
        }
        return 0;
    };
    auto pick = [&](const nlohmann::json& obj, const char* key) -> int {
        if (!obj.is_object()) return 0;
        const auto it = obj.find(key);
        return it == obj.end() ? 0 : as_int(*it);
    };

    // Deployment-specific first.
    if (const auto tp = m.find("top_provider");
        tp != m.end() && tp->is_object())
        if (const int w = pick(*tp, "context_length"); w > 0) return w;
    if (const int w = pick(m, "max_model_len"); w > 0) return w;

    // LiteLLM's model_info block.
    if (const auto mi = m.find("model_info");
        mi != m.end() && mi->is_object()) {
        if (const int w = pick(*mi, "max_input_tokens"); w > 0) return w;
        if (const int w = pick(*mi, "max_tokens");       w > 0) return w;
        if (const int w = pick(*mi, "context_window");   w > 0) return w;
    }

    // llama-server's per-model `meta` block.
    //
    // Verified against llama.cpp tools/server/server-context.cpp
    // (get_res_model_info): each /v1/models row carries
    //
    //     "meta": { "n_ctx": slot_n_ctx, "n_ctx_train": …, … }
    //
    // n_ctx is the SERVED window — what the server was actually started
    // with (-c), divided across slots — and n_ctx_train is the model's
    // train-time maximum. The served one is the number that bounds this
    // request, so it wins; n_ctx_train is the fallback for a build that
    // omits it.
    //
    // The flat ladder below already lists n_ctx/n_ctx_train, but llama.cpp
    // never puts them at the top level of a row — only inside `meta` — so
    // every llama-server model fell through to the 200k default. That is
    // issue #49: a 8k or 32k local model whose gauge claimed 200k, so
    // compaction never fired and the server truncated the prompt instead.
    if (const auto meta = m.find("meta");
        meta != m.end() && meta->is_object()) {
        if (const int w = pick(*meta, "n_ctx");       w > 0) return w;
        if (const int w = pick(*meta, "n_ctx_train"); w > 0) return w;
    }

    // GGUF arch-prefixed keys: "<arch>.context_length".
    //
    // This is how the window is spelled in a model's own metadata —
    // "qwen2.context_length", "llama.context_length", "gemma3.context_length"
    // — and it is exactly what Ollama's own CLI reads in cmd/cmd.go's
    // showInfo(). The prefix varies per architecture, so scan for the
    // suffix rather than hard-coding a list that would go stale with every
    // new model family.
    //
    // Any nested object is searched, because the key appears at the top
    // level on some servers and under "model_info" on Ollama.
    {
        const auto scan = [](const nlohmann::json& obj) -> int {
            if (!obj.is_object()) return 0;
            constexpr std::string_view kSuffix = ".context_length";
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                const std::string& key = it.key();
                if (key.size() <= kSuffix.size()) continue;
                if (key.compare(key.size() - kSuffix.size(),
                                kSuffix.size(), kSuffix) != 0) continue;
                if (!it.value().is_number()) continue;
                const double d = it.value().get<double>();
                if (d > 0 && d < 2e9) return static_cast<int>(d);
            }
            return 0;
        };
        if (const auto mi = m.find("model_info");
            mi != m.end() && mi->is_object())
            if (const int w = scan(*mi); w > 0) return w;
        if (const int w = scan(m); w > 0) return w;
    }

    // Flat spellings, in the order a row is most likely to carry them.
    //
    // Every hosted OpenAI-compatible API picked its own name for the same
    // number, and a spelling we do not read is not a small miss: the model
    // silently drops to the 200k default, the gauge misreads, and
    // auto-compaction fires at the wrong point. So this list is the
    // ecosystem, not a guess — one entry per vendor we can actually name.
    //
    //   context_length     Together, Fireworks, DeepSeek, Perplexity,
    //                      openrouter's flat rows
    //   context_window     Groq
    //   max_context_length Mistral (api.mistral.ai) — its own spelling,
    //                      matching nothing else in the ecosystem
    //   max_input_tokens   Qwen/DashScope and several others that report
    //                      the input half separately; that is the number
    //                      bounding the prompt, which is what a context
    //                      gauge measures
    //   n_ctx / n_ctx_train  llama.cpp (served window, then train-time)
    //
    // NOTE what is deliberately absent: `max_tokens`, `max_output_tokens`,
    // `max_completion_tokens`. Those are OUTPUT caps, an order of
    // magnitude smaller, and reading one as the window produces a
    // plausible wrong number rather than a visible failure — the agent
    // then "forgets" constantly on a model with a huge window.
    for (const char* key : {"context_length", "context_window",
                            "max_context_length",
                            "max_input_tokens", "n_ctx", "n_ctx_train"})
        if (const int w = pick(m, key); w > 0) return w;

    return 0;
}

// ── Window probe: ask the gateway what its catalog did not say ──────────
//
// The /v1/models ladder above recovers a window from every shape that
// CARRIES one (openrouter's top_provider, vLLM's max_model_len, LiteLLM's
// max_input_tokens, llama.cpp's n_ctx, nested model_info, stringified
// numbers). What it cannot do is invent one for a gateway whose rows are
// bare `{"id":..,"object":"model"}` — and that is most of them: stock
// LiteLLM before it grew max_input_tokens, Ollama's /v1 shim, LM Studio,
// vanilla OpenAI-compat servers.
//
// Those models then fall all the way to kDefaultContextWindow (200k), which
// is why a user behind LiteLLM serving 1M-context models sees 200k and
// reasonably concludes 200k is a cap.
//
// Static data cannot fix this. A self-hosted gateway's URL is unknowable to
// any third-party catalog: models.dev carries 7824 models and exactly 18 of
// them sit behind a loopback origin, with no `litellm` entry at all. The
// only thing that knows a private deployment's window is the deployment.
//
// So ASK it. Two endpoints, both cheap, both optional:
//
//   /v1/model/info   LiteLLM's management route. Returns per-model
//                    max_input_tokens resolved from its own cost map —
//                    authoritative, and the exact figure the proxy will
//                    enforce.
//   /props           llama.cpp's server introspection: default_generation_
//                    settings.n_ctx is the window the server was STARTED
//                    with, which beats the model's train-time n_ctx_train.
//
// Probed ONCE per endpoint per catalog load, not per model, and only when
// at least one row came back window-less. A gateway that already answers in
// /v1/models pays nothing; one that does not pays a single request.
struct WindowProbe {
    // model id -> window. Empty when the endpoint offers neither route.
    std::map<std::string, int> per_model;
    // A single window that applies to the whole server (llama.cpp serves one
    // model). Used when per_model has no entry for an id.
    int server_wide = 0;
    // Is per_model a MEASUREMENT of what the server allocated, or another
    // party's DECLARATION of what a model supports?
    //
    // The distinction decides whether this may shrink a window the row
    // already advertised. A measurement may: llama.cpp's meta.n_ctx and LM
    // Studio's loaded_instances[].config.context_length are the size the
    // running process actually allocated, so a request genuinely cannot
    // exceed them.
    //
    // A declaration may NOT. LiteLLM's /v1/model/info is a config file the
    // proxy was handed; it can be stale, conservative, or a default the
    // admin never edited, and letting it override a larger advertised
    // window would silently cost users context they really have. Same for
    // any future catalog-shaped route.
    //
    // Getting this wrong is a quiet, expensive bug: too-small means we
    // compact a conversation that never needed compacting, and the user
    // sees history disappear for no reason.
    bool measured = false;
};

// Hosts the user has explicitly opted into runtime probing for.
//
// Installed by the runtime from Settings::probe_hosts — pushed IN rather
// than read out, because this layer takes no dependency on the settings
// store (the whole transport is constructed from an Endpoint and an auth
// header, which is what makes it testable without a filesystem).
std::set<std::string>& probe_opt_in() {
    static std::set<std::string> s;
    return s;
}
std::shared_mutex& probe_opt_in_mu() {
    static std::shared_mutex m;
    return m;
}

// Is this endpoint a server we should probe unconditionally?
//
// The probe is up to four extra round-trips and every route it tries
// (/api/v1/models, /props, /api/ps) is local-server-specific, so against a
// hosted API it buys guaranteed 404s per refresh. On a self-hosted server
// they are cheap and carry the only accurate answer — the RUNTIME window —
// which no catalog can know.
//
// "Local" therefore means "a server you run", not "a server on this
// machine". The first version of this checked loopback only, which was
// wrong the moment anyone put llama-server on the box with the GPU and
// talked to it over their LAN — reported on #49 within hours. A private
// address is the honest generalisation: RFC1918 and friends are not
// routable on the internet, so nothing behind one is a hosted API.
//
// Host-based, not TLS-based: `https://llama.lan:8443` is still yours, and a
// plain-HTTP remote gateway is still somebody else's.
[[nodiscard]] bool is_local_endpoint(const Endpoint& ep) {
    const std::string& h = ep.host;
    if (h.empty()) return false;

    // Explicit opt-in, for anything the heuristics cannot see: a server on a
    // public address, behind a VPN with its own DNS, or reached through a
    // tunnel. Set in the app (the add-host toast offers it when it matters)
    // and persisted in Settings::probe_hosts; AGENTTY_PROBE_HOSTS adds to it
    // for one run without persisting.
    //
    // This exists because the alternative is telling someone their setup is
    // unsupported — the probe is the only way to learn a runtime window, so
    // refusing to run it makes the window permanently wrong.
    {
        std::shared_lock lk(probe_opt_in_mu());
        if (probe_opt_in().count(h) > 0) return true;
    }
    if (const char* v = std::getenv("AGENTTY_PROBE_HOSTS")) {
        std::string_view list{v};
        while (!list.empty()) {
            const auto comma = list.find(',');
            auto item = list.substr(0, comma);
            // trim spaces
            while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
            while (!item.empty() && item.back()  == ' ') item.remove_suffix(1);
            if (!item.empty() && item == h) return true;
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
    }

    if (h == "localhost" || h == "::1" || h == "[::1]") return true;
    // The docker-desktop / podman bridge alias for the host machine.
    if (h == "host.docker.internal") return true;
    // A .local name is mDNS — link-local by definition (RFC 6762).
    if (h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;

    // Private / non-routable IPv4. Parsed rather than prefix-matched, so
    // "17.2.3.4" (Apple) is not mistaken for 172.16/12, and a hostname that
    // merely starts with digits is not mistaken for an address at all.
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (std::sscanf(h.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4
        && a < 256 && b < 256 && c < 256 && d < 256) {
        if (a == 127) return true;                     // loopback
        if (a == 10)  return true;                     // 10/8
        if (a == 192 && b == 168) return true;         // 192.168/16
        if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16/12
        if (a == 169 && b == 254) return true;         // link-local
        if (a == 100 && b >= 64 && b <= 127) return true;  // CGNAT — tailscale
        return false;   // a real routable address
    }

    // IPv6 unique-local (fc00::/7) and link-local (fe80::/10).
    if (h.size() > 2) {
        const std::string_view v6{h.front() == '[' ? h.c_str() + 1 : h.c_str()};
        if (v6.size() >= 2) {
            const char c0 = static_cast<char>(std::tolower(v6[0]));
            const char c1 = static_cast<char>(std::tolower(v6[1]));
            if (c0 == 'f' && (c1 == 'c' || c1 == 'd')) return true;
            if (c0 == 'f' && c1 == 'e' && v6.size() >= 3) {
                const char c2 = static_cast<char>(std::tolower(v6[2]));
                if (c2 >= '8' && c2 <= '9') return true;
                if (c2 == 'a' || c2 == 'b') return true;
            }
        }
    }

    // A bare single-label hostname ("gpubox", "nas") has no dots and cannot
    // resolve on the public internet without a search domain — it is a
    // machine on your network.
    if (h.find('.') == std::string::npos && h.find(':') == std::string::npos)
        return true;

    return false;
}

// Percent-encode one query-string value.
//
// Model ids routinely contain '/' and ':' ("qwen/qwen3-coder-30b",
// "llama3.2:latest"), and llama-server's router matches on the exact name.
// Sending those raw would either truncate the value at the first reserved
// character or miss the model entirely.
[[nodiscard]] inline std::string url_encode(std::string_view in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}

[[nodiscard]] inline WindowProbe probe_endpoint_windows(const AuthHeader& auth,
                                                       const Endpoint& ep,
                                                       const std::vector<std::string>& ids = {}) {
    WindowProbe out;
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(1'500);
    tos.total   = std::chrono::milliseconds(4'000);

    auto get = [&](std::string_view path) -> std::optional<std::string> {
        http::Request hreq;
        hreq.method    = http::HttpMethod::Get;
        hreq.host      = ep.host;
        hreq.port      = ep.port;
        hreq.path      = std::string{path};
        hreq.plaintext = !ep.use_tls;
        hreq.headers   = build_request_headers(auth, ep);
        hreq.max_body_bytes = 2ull * 1024 * 1024;
        if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
            hreq.dial_host = ov.host;
            hreq.dial_port = ov.port;
        }
        auto resp = http::default_client().send(hreq, tos);
        if (!resp || resp->status != 200) return std::nullopt;
        return resp->body;
    };

    // 1. LiteLLM /v1/model/info — {"data":[{"model_name":..,
    //    "model_info":{"max_input_tokens":1048576}}, ...]}
    if (auto body = get("/v1/model/info")) {
        try {
            auto j = json::parse(*body);
            for (const auto& row : j.value("data", json::array())) {
                if (!row.is_object()) continue;
                // The id lives under model_name here (it is the PUBLIC alias,
                // which is what /v1/models listed and what we dispatch on).
                std::string id = row.value("model_name", std::string{});
                if (id.empty()) id = row.value("id", std::string{});
                if (id.empty()) continue;
                // Reuse the same tolerant ladder as the catalog path: the
                // window may be flat on the row or nested under model_info,
                // and LiteLLM emits it as a FLOAT (16385.0).
                int w = advertised_context_window(row);
                if (w <= 0) continue;
                out.per_model[id] = w;
            }
        } catch (const std::exception& e) {
            util::dbglog("openai.window_probe.model_info", e.what());
        } catch (...) {}
    }
    // NOT measured: /v1/model/info is the proxy's own config, a declaration
    // like the row's, so it fills holes but never shrinks a larger one.
    if (!out.per_model.empty()) return out;

    // 2. LM Studio /api/v1/models — the NATIVE API, not the /v1 shim.
    //
    // LM Studio serves two APIs on one port. The OpenAI-compatible /v1 one
    // reports `max_context_length`: what the model ARCHITECTURE supports.
    // The native one additionally reports, per running instance:
    //
    //     "loaded_instances": [ { "config": { "context_length": 16384 } } ]
    //
    // which is the window the instance was actually loaded with. Those are
    // different numbers and only the second one bounds a request — a model
    // capable of 128k loaded at 16k will still refuse at 16k.
    //
    // This is the second half of issue #49: the gauge showed the
    // architectural maximum, so compaction sat idle while the server
    // truncated the prompt.
    //
    // NOTE the asymmetry, which matters: an UNLOADED model gets no entry
    // here at all. LM Studio does not expose the saved load config of an
    // unloaded model, so its real window is unknowable until it loads — and
    // substituting max_context_length would size prompts against a window
    // that was never allocated, which is the bug we are fixing. Leaving it
    // absent lets the existing ladder fall back to max_context_length as a
    // declared ceiling, which is honest: it is the best bound available
    // until the model loads.
    if (auto body = get("/api/v1/models")) {
        try {
            auto j = json::parse(*body);
            for (const auto& row : j.value("models", json::array())) {
                if (!row.is_object()) continue;
                const std::string id = row.value("key", std::string{});
                if (id.empty()) continue;
                const auto li = row.find("loaded_instances");
                if (li == row.end() || !li->is_array()) continue;
                // Several instances of one model can be loaded at different
                // sizes. The smallest is the only one every request is
                // guaranteed to fit, so it is the safe bound.
                int best = 0;
                for (const auto& inst : *li) {
                    if (!inst.is_object()) continue;
                    const auto cfg = inst.find("config");
                    if (cfg == inst.end() || !cfg->is_object()) continue;
                    const int w = advertised_context_window(*cfg);
                    if (w > 0 && (best == 0 || w < best)) best = w;
                }
                if (best > 0) out.per_model[id] = best;
            }
        } catch (const std::exception& e) {
            util::dbglog("openai.window_probe.lmstudio", e.what());
        } catch (...) {}
    }
    if (!out.per_model.empty()) {
        // MEASURED: this is the size the loaded instance allocated.
        out.measured = true;
        return out;
    }

    // 3. llama.cpp /props — the RUNTIME window (-c on the command line),
    //    which is the number that actually applies; n_ctx_train is the
    //    architectural ceiling and is often much larger.
    //
    //    Two server shapes:
    //
    //    SINGLE-MODEL (the classic `llama-server -m model.gguf`): a bare
    //    /props answers for the one model it serves, so the result is
    //    server-wide.
    //
    //    ROUTER (`llama-server --models-dir ...`, multi-model): a bare
    //    /props returns a DUMMY with n_ctx: 0 — literally a placeholder so
    //    the web UI doesn't break (verified in tools/server/server-models.cpp,
    //    get_router_props). The real answer needs ?model=<name>.
    //
    //    So a 0 here is not "no answer", it is "ask again per model".
    if (auto body = get("/props")) {
        try {
            auto j = json::parse(*body);
            if (auto dg = j.find("default_generation_settings");
                dg != j.end() && dg->is_object()) {
                if (const int w = advertised_context_window(*dg); w > 0)
                    out.server_wide = w;
            }
            if (out.server_wide <= 0)
                out.server_wide = advertised_context_window(j);
            // MEASURED: default_generation_settings.n_ctx is meta.slot_n_ctx,
            // the window this process actually allocated (verified in
            // llama.cpp tools/server/server-context.cpp).
            if (out.server_wide > 0) out.measured = true;

            // Router mode: ask per model for the ones we were given.
            //
            // `autoload=false` is NOT optional. The router's models_autoload
            // defaults to TRUE, so a plain /props?model=X LOADS that model —
            // walking the list would pull every model on the server into
            // memory just to read a number. With autoload=false an unloaded
            // model answers "model is not loaded" and we simply skip it,
            // which is the correct outcome anyway: a model that is not
            // resident has no allocated window to report.
            if (j.value("role", std::string{}) == "router" && !ids.empty()) {
                for (const auto& id : ids) {
                    auto pb = get("/props?model=" + url_encode(id)
                                  + "&autoload=false");
                    if (!pb) continue;
                    try {
                        auto pj = json::parse(*pb);
                        int w = 0;
                        if (auto dg = pj.find("default_generation_settings");
                            dg != pj.end() && dg->is_object())
                            w = advertised_context_window(*dg);
                        if (w <= 0) w = advertised_context_window(pj);
                        if (w > 0) out.per_model[id] = w;
                    } catch (...) {}
                }
                if (!out.per_model.empty()) out.measured = true;
            }
        } catch (const std::exception& e) {
            util::dbglog("openai.window_probe.props", e.what());
        } catch (...) {}
    }
    if (out.measured) return out;

    // 4. Ollama /api/ps — what is loaded RIGHT NOW, and at what size.
    //
    // /api/tags (the route list_models uses for Ollama) carries no window
    // at all — verified against ollama api/types.go ListModelResponse,
    // which is {name, model, modified_at, size, digest, details,
    // capabilities}. So an Ollama row arrives here at 0 and the runtime
    // falls back to effective_num_ctx()'s 8k floor / 32k ceiling.
    //
    // That fallback is a guess. /api/ps reports, per LOADED model,
    // ProcessModelResponse.context_length — the actual num_ctx the daemon
    // allocated, which is the same class of fact as llama.cpp's n_ctx and
    // strictly better than a heuristic. Someone running `ollama serve` with
    // OLLAMA_CONTEXT_LENGTH=65536 currently gets clamped to 32k by our
    // ceiling despite having asked for more.
    //
    // Only loaded models appear, which is correct: an unloaded model has no
    // allocated window to report, and inventing one is the #49 bug.
    if (auto body = get("/api/ps")) {
        try {
            auto j = json::parse(*body);
            for (const auto& row : j.value("models", json::array())) {
                if (!row.is_object()) continue;
                const int w = row.value("context_length", 0);
                if (w <= 0) continue;
                // Ollama answers to both "llama3.2" and "llama3.2:latest";
                // index whichever names the row carries so the lookup in
                // list_models hits regardless of how the user typed it.
                for (const char* key : {"model", "name"}) {
                    const auto id = row.value(key, std::string{});
                    if (!id.empty()) out.per_model[id] = w;
                }
            }
            if (!out.per_model.empty()) out.measured = true;
        } catch (const std::exception& e) {
            util::dbglog("openai.window_probe.ollama_ps", e.what());
        } catch (...) {}
    }
    return out;
}

}  // namespace detail

void install_probe_hosts(std::set<std::string> hosts) {
    std::unique_lock lk(detail::probe_opt_in_mu());
    detail::probe_opt_in() = std::move(hosts);
}

// Public forwarder — see the header for why the 0 is a contract.
int advertised_context_window(const nlohmann::json& model_row) {
    return detail::advertised_context_window(model_row);
}

std::vector<ModelInfo> list_models(const AuthHeader& auth, const Endpoint& endpoint,
                                   bool force_probe) {
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
    hreq.headers        = build_request_headers(auth, endpoint);
    hreq.max_body_bytes = 2ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5'000);
    tos.total   = std::chrono::milliseconds(10'000);

    auto resp = http::default_client().send(hreq, tos);
    // PATH-PREFIX PROBE (custom hosts): a spec like http://host:8080/ derives
    // models_path "/models", but most local OpenAI-compatible servers
    // (llama.cpp, vLLM, LM Studio) only serve under "/v1". A 404 here — with
    // the server plainly reachable — is almost always that missing prefix,
    // and it was the root of the "custom host dead loop": the picker showed
    // no models, the user prompted anyway, and every turn 404'd. Retry once
    // with /v1 prepended; on success adopt the corrected paths for THIS
    // process (Endpoint::from_spec output is rebuilt per selection, so the
    // correction also has to happen at request time — see run_stream_sync's
    // matching fallback note).
    if (resp && resp->status == 404
        && !endpoint.models_path.starts_with("/v1/")
        && !endpoint.native_api) {
        http::Request retry = hreq;
        retry.path = "/v1/models";
        auto second = http::default_client().send(retry, tos);
        if (second && second->status == 200) resp = std::move(second);
    }
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
                // Ollama DECLARES per-model reasoning in /api/show
                // capabilities[] ("thinking") — record it scoped to this
                // endpoint so resolved_caps' live-catalog layer covers local
                // models the id heuristics don't know (qwq, qwen3, new
                // community models). Ollama's `think` accepts graded
                // low|medium|high, so declare exactly that ladder; the
                // encode side folds minimal/xhigh/max inward to match.
                if (probe.supports_thinking.value_or(false)) {
                    set_catalog_reasoning(endpoint.label + "/" + id, true);
                    set_catalog_effort_set(
                        endpoint.label + "/" + id,
                        static_cast<std::uint8_t>(
                            effort_bit(Effort::Low) | effort_bit(Effort::Medium)
                            | effort_bit(Effort::High)));
                } else if (probe.supports_thinking.has_value()) {
                    set_catalog_reasoning(endpoint.label + "/" + id, false);
                }
                result.push_back(std::move(info));
            }
        } else {
            for (const auto& m : j.value("data", json::array())) {
                auto id = m.value("id", "");
                if (id.empty()) continue;
                // A raw /v1/models dump lists every asset the key can touch —
                // embeddings, image/audio/moderation endpoints, rerankers —
                // none of which an agent can drive. Drop them so they never
                // pollute the picker OR get chosen by the subagent router.
                if (!is_dispatchable_model(id)) continue;
                // Providers that DECLARE per-model reasoning support in the
                // catalog (Mistral: capabilities.reasoning) get that truth
                // recorded — resolved_caps() folds it in over id inference,
                // so the effort ladder tracks the provider's live dispatch
                // table instead of our static guesses (which drift: dated
                // mistral-medium revisions reject reasoning_effort while
                // magistral now accepts it).
                if (auto ci = m.find("capabilities");
                    ci != m.end() && ci->is_object()
                    && ci->contains("reasoning")
                    && (*ci)["reasoning"].is_boolean()) {
                    // Provider-scoped key: the same bare id on another host
                    // (gpt-oss on Groq vs Cerebras) may have a different
                    // contract, so never let one host's declaration bleed.
                    set_catalog_reasoning(endpoint.label + "/" + id,
                                          (*ci)["reasoning"].get<bool>());
                }
                result.push_back(ModelInfo{
                    .id           = ModelId{id},
                    .display_name = m.value("display_name", id),
                    .provider     = endpoint.label,
                    .context_window = detail::advertised_context_window(m),
                });
            }
            // Any row the catalog said nothing about gets ONE chance to be
            // answered by the gateway itself (LiteLLM /v1/model/info,
            // llama.cpp /props). Deferred to here — after the whole list is
            // parsed — so the probe fires once per load rather than per
            // model, and not at all when every row already carried a window.
            //
            // This is the seam that fixes "my 1M-context models show 200k":
            // a private gateway's window is knowable only from the gateway,
            // never from a third-party catalog keyed by URL.
            // Probe whenever ANY row is unknown, and also — for a LOCAL
            // server — whenever the rows already declare something, because
            // what they declare may not be the runtime window.
            //
            // Gating the probe on `any_unknown` alone was wrong for the two
            // biggest local servers, because both declare something:
            //
            //   * LM Studio's /v1 rows carry `max_context_length` — what the
            //     ARCHITECTURE supports. A model capable of 128k but loaded
            //     at 16k still refuses at 16k. The row looked "known", so
            //     the probe never ran and the gauge showed 128k forever.
            //   * llama-server rows carry meta.n_ctx_train alongside the
            //     served meta.n_ctx.
            //
            // A DECLARED ceiling and a MEASURED runtime window are different
            // facts, and the measured one is the only one a request has to
            // fit inside — so when every row is "known" we still probe, and
            // a per-model result OVERRIDES a declared window if it is
            // smaller. (That is also why #49 reported "values don't update
            // on refresh": a refresh re-read the declared ceiling and the
            // probe result had nowhere to land.)
            //
            // Smaller-only, deliberately. A probe larger than the declared
            // ceiling would mean we misread something; raising the window on
            // that basis risks building a prompt the server rejects, while
            // lowering it only costs headroom. Fail toward the smaller
            // number.
            //
            // LOCAL-ONLY, also deliberately. The probe is up to three extra
            // HTTP round-trips, and none of its endpoints exist on a hosted
            // API — running it against api.openai.com on every refresh would
            // buy three guaranteed 404s. Hosted rows keep the old rule:
            // probe only to fill a hole.
            const bool any_unknown =
                std::any_of(result.begin(), result.end(),
                            [](const ModelInfo& mi) { return mi.context_window <= 0; });
            const bool local = detail::is_local_endpoint(endpoint);
            if (any_unknown || local || force_probe) {
                // The ids let the probe answer per model where the endpoint
                // requires it (llama-server router mode's /props?model=).
                std::vector<std::string> ids;
                ids.reserve(result.size());
                for (const auto& mi : result) ids.push_back(mi.id.value);
                const auto probe =
                    detail::probe_endpoint_windows(auth, endpoint, ids);
                if (!probe.per_model.empty() || probe.server_wide > 0) {
                    for (auto& mi : result) {
                        const auto it = probe.per_model.find(mi.id.value);
                        if (it != probe.per_model.end()) {
                            // Fill a hole always; SHRINK only on a real
                            // measurement (see WindowProbe::measured).
                            // Letting a declaration shrink a larger
                            // advertised window would quietly cost users
                            // context they actually have — and the symptom,
                            // a conversation compacting early for no visible
                            // reason, is one nobody would trace back here.
                            if (mi.context_window <= 0
                                || (probe.measured && it->second < mi.context_window))
                                mi.context_window = it->second;
                        } else if (mi.context_window <= 0 && probe.server_wide > 0) {
                            // server_wide is NOT applied over a declared
                            // window: on a multi-model gateway it describes
                            // whichever model is loaded, not this row.
                            mi.context_window = probe.server_wide;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        util::dbglog("openai.list_models.parse", e.what());
    } catch (...) {
        util::dbglog("openai.list_models.parse", "non-std exception");
    }

    return result;
}

// ── Test harness ─────────────────────────────────────────────────────────────
std::vector<Msg> parse_sse_for_test(std::string_view sse_bytes,
                                   std::vector<std::string> known_tools,
                                   bool allow_memory_salvage,
                                   bool reason_by_default,
                                   bool show_reasoning) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.show_reasoning = show_reasoning;
    ctx.known_tools = std::move(known_tools);
    ctx.allow_remember_salvage = allow_memory_salvage;
    ctx.allow_forget_salvage = allow_memory_salvage;
    ctx.allow_wipe_salvage = allow_memory_salvage;
    ctx.reason_by_default = reason_by_default;
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    feed_sse(ctx, sse_bytes.data(), sse_bytes.size());
    // Mirror run_stream_sync's terminal guarantee: if [DONE] never arrived,
    // synthesise the close so a test sees a StreamFinished.
    if (!ctx.terminated) {
        close_open_tools(ctx);
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
                                       std::vector<std::string> known_tools,
                                       bool allow_memory_salvage) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.known_tools = std::move(known_tools);
    ctx.allow_remember_salvage = allow_memory_salvage;
    ctx.allow_forget_salvage = allow_memory_salvage;
    ctx.allow_wipe_salvage = allow_memory_salvage;
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    feed_ndjson(ctx, ndjson_bytes.data(), ndjson_bytes.size());
    if (!ctx.terminated) {
        close_open_tools(ctx);
        if (!try_salvage_tool_call(ctx)) flush_text_hold(ctx);
        ensure_nonempty_turn(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

} // namespace agentty::provider::openai
