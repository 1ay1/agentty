// Stream-side helpers for the update reducer: live-preview salvage during
// input_json_delta, partial-JSON closing + truncation guards, and the
// finalize_turn state-machine handoff from Streaming → Idle / Permission /
// ExecutingTool. Kept out of update.cpp so the reducer orchestrator stays
// easy to read.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <span>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/provider/error_class.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/partial_json.hpp"

namespace agentty::app::detail {

using json = nlohmann::json;
using agentty::tools::util::sniff_string;
using agentty::tools::util::sniff_string_progressive;
using maya::Cmd;

namespace {

// Keys models sometimes emit in place of our canonical field name. Mirrors
// the ArgReader alias table — keep in sync.
constexpr std::string_view kPathAliases[]    = {"path", "file_path", "filepath", "filename"};
constexpr std::string_view kOldStrAliases[]  = {"old_string", "old_str", "oldStr"};
constexpr std::string_view kNewStrAliases[]  = {"new_string", "new_str", "newStr"};
constexpr std::string_view kContentAliases[] = {"content", "file_text", "text",
                                                 "file_content", "contents",
                                                 "body", "data"};
constexpr std::string_view kDisplayDescription = "display_description";

// Cap on transparent retries per user turn before we give up and surface the
// truncation as a real Error to the model. Two attempts rides out intermittent
// edge idle-timeouts; more would loop on a genuinely broken upstream.
constexpr int kMaxTruncationRetries = 2;

// Hard cap on the live content preview shown during streaming. The widget
// only renders the first ~6 lines of `content` while the model is mid-write;
// re-extracting / re-laying-out a multi-KB body 8x/sec was what made big
// writes "feel" stuck even when bytes were arriving. 4 KiB covers ~50 wide
// lines — far more than the widget shows — and bounds per-tick work.
constexpr std::size_t kStreamingPreviewCap = 4 * 1024;

std::optional<std::string> sniff_any(const std::string& raw,
                                     std::span<const std::string_view> keys,
                                     bool partial) {
    for (auto k : keys) {
        auto v = partial ? sniff_string_progressive(raw, k)
                         : sniff_string(raw, k);
        if (v) return v;
    }
    return std::nullopt;
}

// Attempt to parse the streaming buffer via the partial-JSON closer. Returns
// an object when the result is a parseable object, otherwise nullopt. Strictly
// more capable than the regex sniffer — handles nested objects (edits[].old_text)
// and escaped quotes — but we still fall back to the sniffer for fields the
// partial closer can't yet expose (e.g. when the current field's value is a
// partial string that won't close until later).
std::optional<json> try_parse_partial(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    try {
        auto closed = agentty::tools::util::close_partial_json(raw);
        auto parsed = json::parse(closed, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> get_string_any(const json& obj,
                                          std::span<const std::string_view> keys) {
    for (auto k : keys) {
        auto it = obj.find(std::string{k});
        if (it == obj.end()) continue;
        if (it->is_string()) return it->get<std::string>();
    }
    return std::nullopt;
}

// Truncation guard: after the stream parses/salvages tool args, verify the
// minimum fields the target tool actually needs. A common failure mode is the
// wire dropping between `display_description`'s closing `"` and the
// `"content":` that should follow — close_partial_json then strips the
// dangling `,` and produces a well-formed but content-less object. Running
// the tool on that would silently produce an empty file and the model would
// retry on a cryptic "content required" loop.
std::string_view missing_required_field(std::string_view tool_name,
                                        const json& args) {
    if (!args.is_object()) return "(args)";
    auto is_nonempty_string_any = [&](std::span<const std::string_view> keys) {
        for (auto k : keys) {
            auto it = args.find(std::string{k});
            if (it == args.end() || !it->is_string()) continue;
            if (!it->get_ref<const std::string&>().empty()) return true;
        }
        return false;
    };
    auto is_nonempty_string = [&](std::string_view key) {
        auto it = args.find(std::string{key});
        return it != args.end() && it->is_string()
            && !it->get_ref<const std::string&>().empty();
    };

    // Closed-set dispatch via spec::Kind. Tools not in the catalog
    // (kind_of returns nullopt) get treated as "no required fields" so
    // an unknown future tool isn't blocked by this guard — the runtime
    // dispatcher will reject the unknown name with a typed error first.
    auto kind = tools::spec::kind_of(tool_name);
    if (!kind) return {};

    using K = tools::spec::Kind;
    switch (*kind) {
        case K::Write:
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            if (!is_nonempty_string_any(kContentAliases)) return "content";
            return {};
        case K::Edit: {
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            auto it = args.find("edits");
            if (it != args.end() && it->is_array() && !it->empty()) return {};
            if (!is_nonempty_string_any(kOldStrAliases))  return "old_string";
            if (!is_nonempty_string_any(kNewStrAliases))  return "new_string";
            return {};
        }
        case K::Bash:
        case K::Diagnostics:
            if (!is_nonempty_string("command")) return "command";
            return {};
        case K::Grep:
            if (!is_nonempty_string("pattern")) return "pattern";
            return {};
        case K::FindDefinition:
            if (!is_nonempty_string("symbol")) return "symbol";
            return {};
        case K::WebFetch:
            if (!is_nonempty_string("url")) return "url";
            return {};
        case K::GitCommit:
            if (!is_nonempty_string("message")) return "message";
            return {};
        // `path` is nice-to-have but not strictly required for these
        // (list_dir/glob default to cwd; read without path is already
        // a tool error — surfacing it from the tool itself preserves
        // the typed ToolError chain instead of converting to a stream-
        // level salvage failure here).
        case K::Read:
        case K::ListDir:
        case K::Glob:
        case K::GitDiff:
        case K::GitLog:
        case K::GitStatus:
        case K::WebSearch:
        case K::Todo:
            return {};
    }
    return {};   // unreachable: switch is exhaustive over Kind
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

void update_stream_preview(ToolUse& tc) {
    // Cheap early-out: if the streaming buffer hasn't grown since the
    // last preview, re-parsing gives identical output. The 120 ms
    // throttle in the caller limits rate; this limits *work* when the
    // model pauses mid-stream (empty deltas, heartbeat gaps) — zero-copy,
    // zero-alloc skip.
    if (tc.args_streaming.size() == tc.stream_sniff_size
        && tc.stream_sniff_size != 0) {
        return;
    }
    tc.stream_sniff_size = tc.args_streaming.size();

    auto set_arg = [&](std::string_view key, std::string v) {
        if (!tc.args.is_object()) tc.args = json::object();
        auto& cur = tc.args[std::string{key}];
        // Cheap "did it change?" — full byte compare on a multi-KB content
        // string was ~half the per-tick cost. Same-size + same-bookend is a
        // very strong signal of "unchanged" during append-only streaming.
        if (cur.is_string()) {
            const auto& s = cur.get_ref<const std::string&>();
            if (s.size() == v.size()
                && (s.empty()
                    || (s.front() == v.front() && s.back() == v.back())))
                return;
        }
        cur = std::move(v);
        tc.mark_args_dirty();
    };
    auto try_set = [&](std::string_view canon,
                       std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/false)) {
            set_arg(canon, *v); return true;
        }
        return false;
    };
    auto try_set_partial = [&](std::string_view canon,
                               std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/true)) {
            set_arg(canon, *v); return true;
        }
        return false;
    };
    // Lazy: try_parse_partial is O(|args_streaming|) per call — on a
    // multi-KB write body, running it every 120 ms is quadratic over
    // the turn. Only the `edit` branch actually needs the structured
    // view (to walk `edits[]`). Every other branch works with sniffer
    // scalars + the cached-offset content decode, and skips this cost
    // entirely. The optional is populated on first access.
    std::optional<json> parsed_cache;
    bool                parsed_attempted = false;
    auto get_parsed = [&]() -> const std::optional<json>& {
        if (!parsed_attempted) {
            parsed_cache = try_parse_partial(tc.args_streaming);
            parsed_attempted = true;
        }
        return parsed_cache;
    };
    auto try_struct = [&](std::string_view canon,
                          std::span<const std::string_view> keys) -> bool {
        const auto& p = get_parsed();
        if (!p) return false;
        if (auto s = get_string_any(*p, keys)) {
            set_arg(canon, *s);
            return true;
        }
        return false;
    };
    auto try_struct_first_edit = [&](std::string_view canon,
                                     std::string_view field) -> bool {
        const auto& p = get_parsed();
        if (!p) return false;
        auto it = p->find("edits");
        if (it == p->end() || !it->is_array() || it->empty()) return false;
        const auto& first = (*it)[0];
        if (!first.is_object()) return false;
        auto f = first.find(std::string{field});
        if (f == first.end() || !f->is_string()) return false;
        set_arg(canon, f->get<std::string>());
        return true;
    };
    auto pull_desc = [&] {
        // display_description is a short scalar field — sniffer is
        // strictly faster than a full parse for it. Skip try_struct
        // so we don't force-materialize `parsed` just for a 40-char
        // description on the hot write/bash/grep paths.
        (void)try_set("display_description", std::span{&kDisplayDescription, 1});
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") {
        // path is a short scalar — sniffer is faster than a full parse.
        try_set("path", kPathAliases);
        pull_desc();
    }
    else if (n == "write") {
        // Write's fast path. General try_struct / try_parse_partial closes
        // and re-parses the *entire* growing args buffer on every tick —
        // fine for tiny tools, quadratic on a multi-KB write body (a 50 KB
        // content field at 8 ticks/sec is 400 KB of O(N) work per second,
        // rising to megabytes by the tail of the stream). Path + desc are
        // located near the head of the buffer and cheap; content uses a
        // *cached* start-of-value offset so each tick only decodes the
        // newly-appended bytes, not the cumulative buffer.
        try_set("path", kPathAliases);
        pull_desc();

        // Locate the opening `"` of content's value ONCE, cache the
        // offset, then on subsequent ticks resume decoding from there.
        // args_streaming grows append-only, so the offset stays valid
        // for the tool call's lifetime. We probe each alias until one
        // hits, then stop probing (stream_sniff_offset != 0).
        if (tc.stream_sniff_offset == 0) {
            for (auto k : kContentAliases) {
                if (auto p = agentty::tools::util::locate_string_value(
                        tc.args_streaming, k)) {
                    tc.stream_sniff_offset = *p;
                    break;
                }
            }
        }
        if (tc.stream_sniff_offset != 0) {
            auto v = agentty::tools::util::decode_string_from(
                tc.args_streaming, tc.stream_sniff_offset);
            if (!v.empty()) {
                if (v.size() > kStreamingPreviewCap) {
                    // Preview shows the tail — the newest bytes are the
                    // most useful confirmation that data is still flowing.
                    // Build the capped preview in one shot instead of
                    // concat + substr (which allocates the full N-byte
                    // intermediate only to throw it away).
                    std::string tail;
                    tail.reserve(kStreamingPreviewCap + 32);
                    tail.append("\xe2\x80\xa6 (showing tail) \xe2\x80\xa6\n");
                    tail.append(v, v.size() - kStreamingPreviewCap,
                                kStreamingPreviewCap);
                    set_arg("content", std::move(tail));
                } else {
                    set_arg("content", std::move(v));
                }
            }
        }
    }
    else if (n == "edit") {
        // Edit is the only branch that genuinely needs the structured
        // parse — `edits[]` is an array of objects whose fields we want
        // to mirror into tc.args in order. try_struct/try_struct_first_edit
        // will lazily materialize `parsed` on first call.
        if (!try_struct("path", kPathAliases)) try_set("path", kPathAliases);
        pull_desc();
        // Mirror the FULL edits array into tc.args["edits"] as it grows so
        // the card can render every edit during streaming, not just the
        // first. Each entry is a partial object — old_text may be present
        // before new_text starts — we keep them ordered so the renderer's
        // "edit N/M" labels stay stable as more edits land.
        bool wrote_edits_array = false;
        const auto& parsed = get_parsed();
        if (parsed) {
            if (auto it = parsed->find("edits");
                it != parsed->end() && it->is_array() && !it->empty())
            {
                json arr = json::array();
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    json out = json::object();
                    if (auto o = e.find("old_text"); o != e.end() && o->is_string())
                        out["old_text"] = o->get<std::string>();
                    else if (auto o2 = e.find("old_string"); o2 != e.end() && o2->is_string())
                        out["old_text"] = o2->get<std::string>();
                    if (auto nv = e.find("new_text"); nv != e.end() && nv->is_string())
                        out["new_text"] = nv->get<std::string>();
                    else if (auto nv2 = e.find("new_string"); nv2 != e.end() && nv2->is_string())
                        out["new_text"] = nv2->get<std::string>();
                    arr.push_back(std::move(out));
                }
                if (!arr.empty()) {
                    if (!tc.args.is_object()) tc.args = json::object();
                    auto& cur = tc.args["edits"];
                    bool changed = !cur.is_array() || cur.size() != arr.size();
                    if (!changed) {
                        for (std::size_t i = 0; i < arr.size(); ++i) {
                            const auto& a = arr[i];
                            const auto& b = cur[i];
                            auto a_old = a.value("old_text", std::string{});
                            auto b_old = b.value("old_text", std::string{});
                            auto a_new = a.value("new_text", std::string{});
                            auto b_new = b.value("new_text", std::string{});
                            if (a_old.size() != b_old.size() || a_new.size() != b_new.size()) {
                                changed = true; break;
                            }
                        }
                    }
                    if (changed) {
                        cur = std::move(arr);
                        tc.mark_args_dirty();
                    }
                    wrote_edits_array = true;
                }
            }
        }
        if (!wrote_edits_array) {
            if (!try_struct_first_edit("old_string", "old_text"))
                try_set_partial("old_string", kOldStrAliases);
            if (!try_struct_first_edit("new_string", "new_text"))
                try_set_partial("new_string", kNewStrAliases);
        }
    }
    else if (n == "bash")  { try_set("command"); pull_desc(); }
    else if (n == "grep")  { try_set("pattern"); try_set("path", kPathAliases); pull_desc(); }
    else if (n == "glob")  { try_set("pattern"); pull_desc(); }
    else if (n == "find_definition") { try_set("symbol"); pull_desc(); }
    else if (n == "web_fetch")       { try_set("url");    pull_desc(); }
    else if (n == "web_search")      { try_set("query");  pull_desc(); }
    else if (n == "diagnostics")     { try_set("command"); pull_desc(); }
    else if (n == "git_status" || n == "git_diff"
          || n == "git_log"    || n == "git_commit"
          || n == "todo")        { if (n == "git_commit") try_set("message"); pull_desc(); }
}

bool guard_truncated_tool_args(ToolUse& tc) {
    auto missing = missing_required_field(tc.name.value, tc.args);
    if (missing.empty()) return false;
    auto now = std::chrono::steady_clock::now();
    tc.status = ToolUse::Failed{
        tc.started_at(),
        now,
        std::string{"Tool call arguments look incomplete — `"}
            + std::string{missing}
            + "` is missing. This usually means the stream was truncated "
              "before the full tool input arrived. Please emit a fresh "
              "tool call with every required field populated (including `"
            + std::string{missing} + "`).",
    };
    return true;
}

json salvage_args(const ToolUse& tc) {
    if (auto parsed = try_parse_partial(tc.args_streaming)) {
        if (!parsed->empty()) return *parsed;
    }
    json out = json::object();
    auto pick = [&](std::string_view canon,
                    std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/true))
            out[std::string{canon}] = *v;
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") { pick("path", kPathAliases); }
    else if (n == "write") { pick("path", kPathAliases); pick("content", kContentAliases); }
    else if (n == "edit")  { pick("path", kPathAliases);
                             pick("old_string", kOldStrAliases);
                             pick("new_string", kNewStrAliases); }
    else if (n == "bash")  { pick("command"); }
    else if (n == "grep")  { pick("pattern"); pick("path", kPathAliases); }
    else if (n == "glob")  { pick("pattern"); }
    else if (n == "find_definition") { pick("symbol"); }
    else if (n == "web_fetch")       { pick("url"); }
    else if (n == "web_search")      { pick("query"); }
    else if (n == "diagnostics")     { pick("command"); }
    else if (n == "git_commit")      { pick("message"); }
    pick("display_description", std::span{&kDisplayDescription, 1});
    return out;
}

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    using maya::Cmd;
    // Stream is over — drop the cancel handle so a stale Esc can't trip
    // the next turn's stream the moment it launches. Phase transitions
    // below (or in kick_pending_tools) drive whether active() flips off.
    if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

    // Compaction completion. The CompactContext handler appended a
    // synthetic User "summarise per spec" message + Assistant
    // placeholder to messages[0..compact_pre_synth_count); the
    // assistant has now produced the summary text in that placeholder.
    //
    // CC-style stitching (mirrors Claude Code's `T5_` / `iU9` reactive
    // compact, binary near offset 77409806):
    //   1. Lift the summary text out of the trailing assistant.
    //   2. Drop the synth user + assistant placeholder.
    //   3. From the original prefix [0, compact_pre_synth_count),
    //      preserve a TAIL of the most recent turn-groups verbatim
    //      (typically the last 1-2 user→assistant pairs). The user's
    //      most recent work stays on screen and the model keeps an
    //      anchor for "where we left off".
    //   4. Build the final messages vector as
    //      [summary_msg(is_compact_summary=true), ...preserved_tail].
    //   5. Reset thread_view_start to 0 so the view shows the new
    //      (much shorter) conversation in full — without this the
    //      user-visible window stays anchored at the now-out-of-bounds
    //      pre-compact offset and the conversation appears empty.
    if (m.s.compacting) {
        std::string summary;
        if (!m.d.current.messages.empty()
            && m.d.current.messages.back().role == Role::Assistant) {
            auto& last = m.d.current.messages.back();
            if (!last.pending_stream.empty()) {
                last.streaming_text += last.pending_stream;
                last.pending_stream.clear();
            }
            summary = std::move(last.text);
            if (!last.streaming_text.empty()) summary += last.streaming_text;
        }
        constexpr std::string_view kOpen = "<summary>";
        constexpr std::string_view kClose = "</summary>";
        if (auto a_pos = summary.find(kOpen); a_pos != std::string::npos) {
            auto body = a_pos + kOpen.size();
            auto b_pos = summary.find(kClose, body);
            if (b_pos != std::string::npos) {
                summary = summary.substr(body, b_pos - body);
            }
        }
        auto is_space = [](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; };
        while (!summary.empty() && is_space(summary.front())) summary.erase(summary.begin());
        while (!summary.empty() && is_space(summary.back()))  summary.pop_back();
        if (summary.empty()) summary = "[compaction produced no text]";

        // Preserve a recent tail from the original prefix. Walk
        // backwards over [0, compact_pre_synth_count) collecting
        // complete turn-groups (a User followed by 0+ Assistants up
        // to the next User). Stop once we have either:
        //   • 2 turn-groups, OR
        //   • a token estimate ≥ 25% of context_max (don't preserve
        //     so much that we re-trip compaction on the next turn).
        // The tail must START with a User to satisfy Anthropic's
        // wire-format requirement that the message sequence after
        // the summary's User leads with an Assistant or another User
        // — easiest enforced by always anchoring on a User boundary.
        std::vector<Message> preserved_tail;
        const std::size_t prefix_n = std::min(
            m.s.compact_pre_synth_count, m.d.current.messages.size());
        if (prefix_n > 0) {
            std::size_t groups = 0;
            std::size_t bytes  = 0;
            const std::size_t kMaxBytes = (m.s.context_max > 0)
                ? static_cast<std::size_t>(m.s.context_max) * 3   // ~25% of ctx in token-bytes
                : 200000;
            constexpr std::size_t kMaxGroups = 2;
            for (std::size_t i = prefix_n; i-- > 0;) {
                bytes += m.d.current.messages[i].text.size();
                for (const auto& tc : m.d.current.messages[i].tool_calls) {
                    bytes += tc.output().size();
                }
                if (m.d.current.messages[i].role == Role::User) {
                    ++groups;
                    if (groups >= kMaxGroups || bytes >= kMaxBytes) {
                        preserved_tail.assign(
                            std::make_move_iterator(m.d.current.messages.begin()
                                + static_cast<std::ptrdiff_t>(i)),
                            std::make_move_iterator(m.d.current.messages.begin()
                                + static_cast<std::ptrdiff_t>(prefix_n)));
                        break;
                    }
                }
                if (i == 0 && groups > 0) {
                    preserved_tail.assign(
                        std::make_move_iterator(m.d.current.messages.begin()),
                        std::make_move_iterator(m.d.current.messages.begin()
                            + static_cast<std::ptrdiff_t>(prefix_n)));
                }
            }
        }

        Message summary_msg;
        summary_msg.role = Role::User;
        summary_msg.is_compact_summary = true;
        // Continuation directive at the end of the body is the same
        // trick CC uses (`Continue the conversation from where it
        // left off without asking the user any further questions...`,
        // binary near offset 77409806) — without it the model often
        // replies with a "let me recap what we were doing" preamble
        // that wastes the first turn after compaction.
        summary_msg.text = "This session is being continued from a previous "
                           "conversation that ran out of context. The summary "
                           "below covers the earlier portion of the "
                           "conversation; recent messages are preserved "
                           "verbatim after this summary.\n\nSummary:\n"
                         + summary
                         + "\n\nContinue the work from where it left off "
                           "without re-acknowledging this summary or recapping "
                           "what was happening. Pick up the last task as if "
                           "the break never happened.";

        m.d.current.messages.clear();
        m.d.current.messages.push_back(std::move(summary_msg));
        for (auto& msg : preserved_tail) {
            m.d.current.messages.push_back(std::move(msg));
        }
        m.d.current.updated_at = std::chrono::system_clock::now();

        // Reset the view's slicing window. thread_view_start was
        // anchored at some offset into the OLD long conversation;
        // after compaction the new conversation is much shorter, so
        // the old offset would either point past the end (rendering
        // nothing — the user's "UI resets and next turns are not
        // visible" bug) or skip the summary itself.
        m.ui.thread_view_start      = 0;
        m.ui.thread_view_start_turn = 0;
        m.ui.thread_scroll          = 0;

        // Rapid-refill breaker bookkeeping. If this compact landed
        // within `kRapidRefillTurns` assistant turns of the previous
        // one, it counts toward the breaker; otherwise the streak
        // resets. Crossing `kRapidRefillCount` flips the disable
        // flag so the auto-trigger in modal.cpp stops firing.
        constexpr int kRapidRefillTurns = 3;
        constexpr int kRapidRefillCount = 3;
        if (m.s.turns_since_last_compact <= kRapidRefillTurns) {
            ++m.s.recent_compacts;
        } else {
            m.s.recent_compacts = 1;
        }
        m.s.turns_since_last_compact = 0;
        if (m.s.recent_compacts >= kRapidRefillCount) {
            m.s.autocompact_disabled = true;
        }

        m.s.compact_pre_synth_count = 0;
        m.s.compacting    = false;
        m.s.phase         = phase::Idle{};
        m.s.tokens_in     = 0;
        m.s.tokens_out    = 0;
        m.s.status        = m.s.autocompact_disabled
            ? "auto-compact disabled (rapid refill); use /compact manually"
            : "context compacted";
        auto now_ts = std::chrono::steady_clock::now();
        m.s.status_until  = now_ts + std::chrono::seconds{4};
        deps().save_thread(m.d.current);
        // Hand the freshly-freed arenas back to the OS. On glibc malloc
        // (the common Linux case) the dropped tool-output strings would
        // otherwise sit on the free list indefinitely — the user sees
        // "context compacted" but their RSS doesn't budge, which makes
        // the feature feel broken on the only metric they can observe.
        // No-op on musl / macOS / Windows; eager on mimalloc.
        release_to_kernel();

        // Drain any messages the user queued during compaction. Same
        // shape as the post-StreamFinished drain at the end of this
        // function, but tailored: we're already Idle, no pending tools
        // to kick.
        if (!m.ui.composer.queued.empty()) {
            m.ui.composer.text = m.ui.composer.queued.front();
            m.ui.composer.queued.erase(m.ui.composer.queued.begin());
            auto [mm, sub_cmd] = submit_message(std::move(m));
            m = std::move(mm);
            return sub_cmd;
        }
        auto stamp = m.s.status_until;
        return Cmd<Msg>::after(std::chrono::seconds{4}
                               + std::chrono::milliseconds{50},
                               Msg{ClearStatus{stamp}});
    }
    bool any_truncated = false;
    const bool max_tokens_hit = (stop_reason == StopReason::MaxTokens);
    if (!m.d.current.messages.empty()) {
        auto& last = m.d.current.messages.back();
        // Drain any text still in the smoothing buffer before committing
        // — message_stop should leave no in-flight bytes invisible.
        if (last.role == Role::Assistant && !last.pending_stream.empty()) {
            last.streaming_text += last.pending_stream;
            last.pending_stream.clear();
        }
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            if (last.text.empty()) last.text = std::move(last.streaming_text);
            else                   last.text += std::move(last.streaming_text);
            std::string{}.swap(last.streaming_text);
        }
        // Flush any tool_calls whose StreamToolUseEnd never fired — Anthropic
        // normally sends content_block_stop per tool block, but proxies /
        // message_stop cut-offs can skip it.
        for (auto& tc : last.tool_calls) {
            if (!tc.args_streaming.empty() && tc.is_pending()) {
                try {
                    tc.args = json::parse(tc.args_streaming);
                    tc.mark_args_dirty();
                } catch (const std::exception& ex) {
                    auto salvaged = salvage_args(tc);
                    if (!salvaged.empty()) {
                        tc.args = std::move(salvaged);
                        tc.mark_args_dirty();
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            std::string{"tool args never closed: "} + ex.what()};
                    }
                }
            }
            std::string{}.swap(tc.args_streaming);
            if (tc.is_pending()) {
                if (guard_truncated_tool_args(tc)) any_truncated = true;
            }
        }
    }

    // ── max_tokens cutoff handling ──
    //
    // When stop_reason == max_tokens, generation halted at the model's
    // output cap. If a tool block was being streamed at that moment, its
    // input_json is truncated — which has two failure modes:
    //
    //   (a) The truncation is "syntactically obvious" (the parser can't
    //       close the JSON, or required schema fields are missing).
    //       guard_truncated_tool_args above sets `any_truncated` and
    //       fails the tool with a generic incomplete-args message.
    //
    //   (b) The truncation lands at a syntactically valid point — common
    //       for `write`/`edit` whose `content` field happens to close
    //       cleanly mid-body. The args parse, schema validation passes,
    //       guard returns false. The tool would dispatch and write a
    //       half-truncated file, surfacing as either silent corruption
    //       or a confusing tool-runner error that misleads the model
    //       into thinking its argument shape was wrong.
    //
    // Both need the same treatment: fail the tool with a message that
    // names the real cause (max_tokens cap) and points the model at
    // the actionable workaround (smaller payload / `edit` over `write` /
    // multiple calls). Don't retry — the budget is the same next time
    // and the model would burn it the same way. The retry guard below
    // already enforces this via `!max_tokens_hit`.
    //
    // We force-fail *every* still-pending tool here, not just ones
    // guard caught, to cover case (b) cleanly. Tools already in a
    // terminal state (Done from an earlier sub-turn, etc.) are left
    // alone.
    if (max_tokens_hit
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        constexpr std::string_view kMaxTokensExplanation =
            "Output token cap (max_tokens) was reached before the tool "
            "input finished streaming, so the call was cut off. Even if "
            "the args parsed, the body is likely truncated. Retry with a "
            "smaller payload: prefer `edit` over `write` for long files, "
            "or split the change across multiple calls.";
        const auto now = std::chrono::steady_clock::now();
        for (auto& tc : m.d.current.messages.back().tool_calls) {
            if (tc.is_pending()) {
                tc.status = ToolUse::Failed{
                    tc.started_at(), now, std::string{kMaxTokensExplanation}};
            } else if (auto* f = std::get_if<ToolUse::Failed>(&tc.status);
                       f && f->output.starts_with("Tool call arguments look incomplete")) {
                // guard_truncated_tool_args already failed this one with
                // a generic message. Upgrade it to the max_tokens-aware
                // version so the model gets a single coherent story.
                f->output.assign(kMaxTokensExplanation);
            }
        }
        (void)any_truncated;  // both (a) and (b) covered above
    }

    // Transparent retry on upstream truncation — libcurl's TCP keepalive
    // can't prevent an edge LB from closing an idle connection. When that
    // happens mid-tool-input we silently re-launch on the same context,
    // capped at kMaxTruncationRetries.
    if (auto* a = active_ctx(m.s.phase);
        a && any_truncated
        && !max_tokens_hit
        && a->truncation_retries < kMaxTruncationRetries
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        auto& last = m.d.current.messages.back();
        const bool has_committed_work =
            !last.text.empty() ||
            std::ranges::any_of(last.tool_calls, [](const auto& tc) {
                return tc.is_done() || tc.is_running();
            });
        if (!has_committed_work) {
            ++a->truncation_retries;
            m.d.current.messages.pop_back();
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            // Streaming → Streaming (truncation retry). Reuse the
            // same ctx — the just-incremented truncation_retries
            // counter persists so the kMaxTruncationRetries cap
            // works across retries within the turn.
            auto ctx = take_active_ctx(std::move(m.s.phase)).value();
            m.s.phase = phase::Streaming{std::move(ctx)};
            m.s.status = "retrying (upstream cut off)…";
            return cmd::launch_stream(m);
        }
    }

    // Bump the rapid-refill breaker's turn counter on every settled
    // assistant turn (whether or not a compact happened this round).
    // The compact-finalize branch above resets it to 0; non-compact
    // turns increment it, so a quiet stretch eventually re-arms
    // auto-compaction by letting `recent_compacts` reset on the next
    // trigger. Cap at INT_MAX/2 to avoid overflow on very long sessions.
    if (m.s.turns_since_last_compact < 1000000) ++m.s.turns_since_last_compact;
    if (m.s.autocompact_disabled
        && m.s.turns_since_last_compact > 10) {
        // Long quiet stretch — re-enable auto-compact. The user has
        // either stopped triggering huge tool outputs or the
        // conversation naturally drifted away from the thrash trigger.
        m.s.autocompact_disabled = false;
        m.s.recent_compacts      = 0;
    }

    deps().save_thread(m.d.current);
    auto kp = cmd::kick_pending_tools(m);

    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        m.ui.composer.text = m.ui.composer.queued.front();
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }
    return kp;
}

// ============================================================================
// stream_update — reducer for `msg::StreamMsg`
// ============================================================================
// Every event handler bumps `last_event_at` so the Tick-based stall watchdog
// can tell "stream is alive but quiet" from "stream is stalled."

Step stream_update(Model m, msg::StreamMsg sm) {
    using maya::overload;

    return std::visit(overload{
        [&](StreamStarted) -> Step {
            auto now = std::chrono::steady_clock::now();
            // The phase variant guarantees a non-null ctx when active;
            // StreamStarted only fires after submit_message / retry has
            // already moved us into Streaming.
            if (auto* a = active_ctx(m.s.phase)) {
                a->started        = now;
                a->last_event_at  = now;
                a->retry          = retry::Fresh{};   // fresh stream → re-arm watchdog
                // Reset the live-rate accumulator so each sub-turn
                // (post-tool) measures its own generation speed instead
                // of polluting the CURRENT-rate display with the previous
                // turn's bytes. The sparkline ring (rate_history) lives
                // on StreamState — it carries across sub-turns / tool
                // gaps so the user sees a continuous trace of generation
                // rate over the whole session, not a fresh empty bar
                // after every tool call.
                a->live_delta_bytes       = 0;
                a->first_delta_at         = {};
                a->rate_last_sample_at    = {};
                a->rate_last_sample_bytes = 0;
            }
            // Fresh stream is alive — wipe any leftover toast (retry
            // countdown, "error: …", "cancelled") from the previous
            // attempt so the status row doesn't show a stale message
            // on top of a healthy connection.
            m.s.status.clear();
            m.s.status_until = {};
            return done(std::move(m));
        },
        [&](StreamTextDelta& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) {
                a->last_event_at = now;
                if (!e.text.empty()) {
                    if (a->first_delta_at.time_since_epoch().count() == 0)
                        a->first_delta_at = now;
                    a->live_delta_bytes += e.text.size();
                }
            }
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                // Append to the smoothing buffer rather than directly to
                // streaming_text — the Tick handler drips it out at a
                // controlled rate so server bursts don't visually jump.
                // Cap is on the COMBINED visible + buffered size so
                // smoothing can't push past the per-message budget.
                auto& msg = m.d.current.messages.back();
                const std::size_t in_flight =
                    msg.streaming_text.size() + msg.pending_stream.size();
                if (in_flight < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - in_flight;
                    if (e.text.size() <= room) msg.pending_stream += e.text;
                    else                       msg.pending_stream.append(e.text, 0, room);
                }
            }
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) a->last_event_at = now;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.args = json::object();
                // Stamp start now so the card shows a live timer during the
                // args-streaming phase too — lets the user tell "model hasn't
                // started emitting" from "execution is slow" at a glance.
                tc.status = ToolUse::Pending{now};
                m.d.current.messages.back().tool_calls.push_back(std::move(tc));
                // Args-stream watchdog removed at user request — no
                // automatic Pending → Failed timeout. A tool whose
                // tool_use_start streams in but never gets its End
                // event will stay Pending until the user cancels via
                // Esc.  Stream-level errors still surface via the
                // StreamError handler, which clears the in-flight tools.
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) {
                a->last_event_at = now;
                if (!e.partial_json.empty()) {
                    if (a->first_delta_at.time_since_epoch().count() == 0)
                        a->first_delta_at = now;
                    a->live_delta_bytes += e.partial_json.size();
                }
            }
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Bounded append — beyond the cap we drop further bytes so
                // the salvage path at StreamToolUseEnd runs on whatever
                // scalars sniffed cleanly.
                if (tc.args_streaming.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - tc.args_streaming.size();
                    if (e.partial_json.size() <= room) tc.args_streaming += e.partial_json;
                    else tc.args_streaming.append(e.partial_json, 0, room);
                }
                // Throttle the live preview. First delta runs unconditionally
                // so the header paints immediately, then subsequent re-parses
                // are spaced ~120 ms. StreamToolUseEnd always runs the full
                // parse, so the final state is exact.
                using clock = std::chrono::steady_clock;
                constexpr auto kPreviewInterval = std::chrono::milliseconds{120};
                auto now2 = clock::now();
                if (tc.last_preview_at.time_since_epoch().count() == 0
                    || now2 - tc.last_preview_at >= kPreviewInterval) {
                    update_stream_preview(tc);
                    tc.last_preview_at = now2;
                }
            }
            return done(std::move(m));
        },
        [&](StreamToolUseEnd) -> Step {
            if (auto* a = active_ctx(m.s.phase))
                a->last_event_at = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Empty args_streaming is legitimate for argumentless tools;
                // args was seeded to {} at StreamToolUseStart.
                if (!tc.args_streaming.empty()) {
                    try {
                        tc.args = json::parse(tc.args_streaming);
                        tc.mark_args_dirty();
                        std::string{}.swap(tc.args_streaming);
                    } catch (const std::exception& ex) {
                        // Parse failed — typically an SSE cutoff mid-content.
                        // Salvage whatever scalar fields we can so the tool
                        // still has a shot at running instead of nuking the
                        // whole turn.
                        auto salvaged = salvage_args(tc);
                        if (!salvaged.empty()) {
                            tc.args = std::move(salvaged);
                            tc.mark_args_dirty();
                            std::string{}.swap(tc.args_streaming);
                        } else {
                            auto now = std::chrono::steady_clock::now();
                            tc.status = ToolUse::Failed{
                                tc.started_at(), now,
                                std::string{"invalid tool arguments: "} + ex.what()
                                    + "\nraw: " + tc.args_streaming};
                            std::string{}.swap(tc.args_streaming);
                        }
                    }
                }
                // Required-field check is deferred to finalize_turn so the
                // turn-level retry logic owns the single decision point.
            }
            return done(std::move(m));
        },
        [&](StreamUsage& e) -> Step {
            if (auto* a = active_ctx(m.s.phase))
                a->last_event_at = std::chrono::steady_clock::now();
            // `input_tokens` from Anthropic is the FULL prefix for this
            // request, NOT the delta. Accumulating across turns triple-counted
            // by turn 5. Replace, don't add. Cache fields are excluded from
            // `input_tokens` per the API but still occupy the context window,
            // so the true "tokens in context" is the sum.
            if (e.input_tokens || e.cache_read_input_tokens
                || e.cache_creation_input_tokens) {
                m.s.tokens_in = e.input_tokens
                                   + e.cache_read_input_tokens
                                   + e.cache_creation_input_tokens;
            }
            if (e.output_tokens) m.s.tokens_out = e.output_tokens;
            return done(std::move(m));
        },
        [&](StreamHeartbeat) -> Step {
            // Wire-alive signal from the transport (SSE `ping` or
            // `thinking_delta`). No payload, no UI effect — we just
            // bump last_event_at so the stall watchdog knows the
            // connection is healthy. Critical during extended-thinking
            // passes where the model reasons silently for 60-120 s
            // between visible deltas; without this the watchdog would
            // fire on every non-trivial opus turn.
            if (auto* a = active_ctx(m.s.phase))
                a->last_event_at = std::chrono::steady_clock::now();
            return done(std::move(m));
        },
        [&](StreamFinished e) -> Step {
            auto cmd = finalize_turn(m, e.stop_reason);
            // No force_redraw arming. The previous version armed
            // needs_force_redraw here so the next user input would
            // trigger maya's case-(B) soft redraw — meant to flush
            // any prev_cells/wire desync left by streaming. That
            // desync's only real source was the shrink path's
            // \r\n\x1b[2K loop scrolling at viewport bottom, which is
            // now a single \x1b[J in maya/src/render/serialize.cpp;
            // prev_cells stays in sync with the wire on its own.
            //
            // Re-arming case (B) on every first keypress was actively
            // harmful: when the shrink left the cursor mid-viewport
            // (cursor at content_rows - 1 < term_h - 1), case (B)
            // pulled the cursor back to viewport bottom, shifting the
            // canvas-to-buffer mapping by the same delta. The first
            // re-emitted row landed one buffer row below its original
            // streaming position, so the original row stayed stranded
            // in scrollback as a duplicate of the row right at viewport
            // top. With the arming gone, the keypress takes the normal
            // diff path and the composer stays where the shrink left
            // it (no "pull down," no duplicate).
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            // Dedupe: when the stall watchdog fired, it tripped the
            // cancel token, which causes the worker thread to unwind
            // and emit its own StreamError("cancelled") shortly after.
            // The first error already scheduled a retry; ignore any
            // subsequent ones that arrive before that retry runs,
            // otherwise we'd race two worker threads into the same
            // session.
            if (m.s.in_scheduled()) return done(std::move(m));

            // Compaction failed mid-flight (rate limit, network drop,
            // etc.). Pop the synthetic User "summarise per spec" + the
            // assistant placeholder we appended in the CompactContext
            // handler, restoring the conversation to the state the user
            // saw before they hit "Compact context". Then let the
            // normal error path finish (it'll surface "transient —
            // retrying…" or terminal text). Without this rewind, a
            // failed compaction leaves the summary prompt + a partially-
            // streamed assistant reply permanently in the transcript.
            if (m.s.compacting) {
                m.s.compacting = false;
                if (m.d.current.messages.size() >= 2
                    && m.d.current.messages.back().role == Role::Assistant) {
                    m.d.current.messages.pop_back();
                    if (!m.d.current.messages.empty()
                        && m.d.current.messages.back().role == Role::User) {
                        m.d.current.messages.pop_back();
                    }
                }
            }

            // Worker thread is unwinding; drop the token so the next turn
            // (or scheduled retry) mints a fresh one. Reaches into the
            // ctx (rather than dropping the whole ctx) because we may
            // still need the retry counters for the can_retry check.
            if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

            // Move any partial streaming_text into the message body so
            // the assistant's in-flight output isn't lost regardless of
            // what we do next (retry or terminal). Drain the smoothing
            // buffer first so the error path preserves every received
            // byte even if the Tick pacer hadn't revealed it yet.
            Message* last = nullptr;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                last = &m.d.current.messages.back();
                if (!last->pending_stream.empty()) {
                    last->streaming_text += last->pending_stream;
                    last->pending_stream.clear();
                }
                if (!last->streaming_text.empty()) {
                    if (last->text.empty()) last->text = std::move(last->streaming_text);
                    else                    last->text += std::move(last->streaming_text);
                    std::string{}.swap(last->streaming_text);
                }
            }

            // Classify and decide retry vs terminal.
            auto klass = provider::classify(e.message);
            // If the stall watchdog fired this turn, the worker thread
            // will eventually unwind and emit StreamError("cancelled")
            // — that's our doing, not the user's. Force-classify it as
            // Transient so the retry machinery treats it as a recoverable
            // upstream stall, not a user cancel.
            if (m.s.in_stall_fired()
                && klass == provider::ErrorClass::Cancelled) {
                klass = provider::ErrorClass::Transient;
            }

            // "Committed work" gating for retry: only Done/Running tool
            // calls + finalized text body block a retry. A Pending tool
            // (StreamToolUseStart fired, args may have been mid-streaming
            // when the stall hit) is NOT committed — re-running gives
            // the model a chance to re-emit it cleanly. Same definition
            // the truncation-retry path uses (see above).
            bool has_committed = false;
            if (last) {
                has_committed = !last->text.empty() ||
                    std::ranges::any_of(last->tool_calls, [](const auto& tc) {
                        return tc.is_done() || tc.is_running();
                    });
            }
            const phase::Active* err_ctx = active_ctx(m.s.phase);
            int prior_transient = err_ctx ? err_ctx->transient_retries : 0;
            bool can_retry = (klass == provider::ErrorClass::Transient
                           || klass == provider::ErrorClass::RateLimit)
                          && prior_transient < provider::kMaxRetries
                          && !has_committed
                          && err_ctx;   // can't retry from Idle (no ctx)

            if (can_retry) {
                int attempt = prior_transient;
                std::chrono::milliseconds delay;
                if (e.retry_after.has_value()) {
                    auto s = e.retry_after->count();
                    if (s < 1)   s = 1;
                    if (s > 120) s = 120;
                    delay = std::chrono::seconds(s);
                } else {
                    delay = provider::backoff_with_jitter(klass, attempt);
                }
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    delay + std::chrono::milliseconds{999}).count();
                m.s.status = std::string{provider::to_string(klass)}
                           + " — retrying in " + std::to_string(secs) + "s"
                           + " (attempt " + std::to_string(attempt + 1)
                           + "/" + std::to_string(provider::kMaxRetries) + ")…";
                m.s.status_until = std::chrono::steady_clock::now()
                                 + delay + std::chrono::milliseconds{1500};
                auto ctx = take_active_ctx(std::move(m.s.phase)).value();
                ctx.transient_retries = attempt + 1;
                ctx.retry             = retry::Scheduled{};
                m.s.phase = phase::Streaming{std::move(ctx)};
                if (last) m.d.current.messages.pop_back();
                Message placeholder;
                placeholder.role = Role::Assistant;
                m.d.current.messages.push_back(std::move(placeholder));
                return {std::move(m),
                    Cmd<Msg>::after(delay, Msg{RetryStream{}})};
            }

            // Terminal path — discard the source ctx and drop to Idle.
            m.s.phase = phase::Idle{};
            if (klass == provider::ErrorClass::Cancelled) {
                m.s.status = "cancelled";
            } else {
                m.s.status = "error: " + e.message;
            }
            if (last) {
                if (klass != provider::ErrorClass::Cancelled)
                    last->error = e.message;
                std::string fail_msg = "stream ended before tool args "
                                       "closed: " + e.message;
                auto contains_ci = [](std::string_view hay,
                                      std::string_view needle) noexcept {
                    if (needle.size() > hay.size()) return false;
                    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                        bool ok = true;
                        for (std::size_t j = 0; j < needle.size(); ++j) {
                            char a = hay[i + j], b = needle[j];
                            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                            if (a != b) { ok = false; break; }
                        }
                        if (ok) return true;
                    }
                    return false;
                };
                if (contains_ci(e.message, "filtering policy")
                    || contains_ci(e.message, "content filter")
                    || contains_ci(e.message, "blocked by content")) {
                    fail_msg =
                        "Anthropic's safety classifier blocked the tool "
                        "input mid-stream (\"" + e.message + "\"). This "
                        "is the upstream policy on the OAuth path "
                        "tripping on generated content; it is "
                        "probabilistic. Try once more if the content is "
                        "innocuous (lorem ipsum, JSON, code) — most "
                        "false positives clear on retry. If it blocks "
                        "again, write a short stub file via `write` and "
                        "build the rest with successive `edit` calls. "
                        "(Direct API-key callers usually bypass this "
                        "filter entirely; a long chain of safety "
                        "blocks on OAuth is a hint to switch auth.)";
                }
                for (auto& tc : last->tool_calls) {
                    if (tc.is_pending()) {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now, fail_msg};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
            }
            {
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{
                    klass == provider::ErrorClass::Cancelled ? 3 : 6};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}})};
            }
        },
        [&](RetryStream) -> Step {
            // Scheduled retry fired. If the user cancelled during the
            // wait (Esc → CancelStream dropped phase to Idle), do
            // nothing. Otherwise transition retry back to Fresh on
            // the in-flight ctx so the freshly-launched stream's own
            // errors flow through the normal classifier path.
            if (auto* a = active_ctx(m.s.phase)) a->retry = retry::Fresh{};
            if (m.s.is_idle()) return done(std::move(m));
            return {std::move(m), cmd::launch_stream(m)};
        },
        [&](CancelStream) -> Step {
            // Esc — full synchronous teardown.
            if (auto* a = active_ctx(m.s.phase); a && a->cancel) a->cancel->cancel();

            // Compaction-cancel: pop the synthetic "summarise per spec"
            // user message + the assistant placeholder so the transcript
            // returns to its pre-compaction shape.
            if (m.s.compacting) {
                m.s.compacting = false;
                if (!m.d.current.messages.empty()
                    && m.d.current.messages.back().role == Role::Assistant) {
                    m.d.current.messages.pop_back();
                }
                if (!m.d.current.messages.empty()
                    && m.d.current.messages.back().role == Role::User) {
                    m.d.current.messages.pop_back();
                }
            }

            // Salvage partial assistant work and finalise in-flight tool calls.
            auto now = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& last = m.d.current.messages.back();
                if (!last.pending_stream.empty()) {
                    last.streaming_text += last.pending_stream;
                    last.pending_stream.clear();
                }
                if (!last.streaming_text.empty()) {
                    if (last.text.empty()) last.text = std::move(last.streaming_text);
                    else                   last.text += std::move(last.streaming_text);
                    std::string{}.swap(last.streaming_text);
                }
                for (auto& tc : last.tool_calls) {
                    if (!tc.is_terminal()) {
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now, "cancelled"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
                if (last.text.empty() && last.tool_calls.empty()) {
                    m.d.current.messages.pop_back();
                }
            }

            m.d.pending_permission.reset();
            m.s.phase = phase::Idle{};
            m.s.status = "cancelled";
            {
                auto ttl = std::chrono::seconds{3};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}})};
            }
        },
    }, sm);
}

} // namespace agentty::app::detail
