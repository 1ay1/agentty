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
#include <unordered_set>
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
        case K::Remember:
            // remember requires the `text` body. `scope` is optional.
            if (!is_nonempty_string("text")) return "text";
            return {};
        case K::Forget:
            // forget accepts either `id` or `substring` — the tool's
            // own parser surfaces the "need one of" error with a richer
            // message than this guard could, so don't gate here.
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
    // Refuse salvage when the wire stopped mid-string. close_partial_json
    // would synthesise a closing `"` and produce a parseable object whose
    // last string value is truncated at an arbitrary byte boundary;
    // dispatching `write`/`edit`/`bash` on that silently corrupts the
    // target file or runs a half-typed command. The caller's parse-failed
    // path (`StreamToolUseEnd`) already has the right shape — we just
    // need to keep it from accepting a synthesised-quote salvage. See
    // Finding 3 in docs/corruption-analysis.md.
    //
    // Path-only tools (read, list_dir, glob) would also be affected but
    // their values are tiny and rarely truncated mid-string in practice;
    // applying the rule uniformly keeps the policy reviewable in one
    // place at the cost of a few false refusals on degenerate streams.
    if (agentty::tools::util::ended_inside_string(tc.args_streaming)) {
        return json::object();
    }
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

    // Compaction completion. The compaction stream wrote its summary
    // text into `m.s.compaction_buffer` (NOT into messages — the
    // transcript is immutable across compaction events). Now:
    //   1. Lift the summary text out of the buffer.
    //   2. Strip <summary>…</summary> tags / trim whitespace.
    //   3. Push a Thread::CompactionRecord describing
    //      [0, compaction_target_index) so future wire payloads
    //      built by `cmd_factory::wire_messages_for` substitute the
    //      summary for the raw prefix.
    //
    // The user-visible transcript stays exactly as it was before
    // CompactContext fired. No swap, no "reset to a shorter conversation",
    // no thread_view_start fiddling. The only UI signal is the status
    // banner flipping to "context compacted" and a divider the view
    // draws between messages[up_to_index-1] and messages[up_to_index].
    if (m.s.compacting) {
        std::string summary = std::move(m.s.compaction_buffer);
        m.s.compaction_buffer.clear();

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

        // Clamp the target index to the current transcript size in
        // case messages were queued/submitted during compaction
        // (queue-on-compact path drains AFTER this branch runs, but a
        // future code path could in principle land messages earlier).
        // The invariant we care about: up_to_index never exceeds
        // messages.size() so the wire-substitution helper stays
        // well-defined.
        const std::size_t up_to = std::min(m.s.compaction_target_index,
                                           m.d.current.messages.size());
        Thread::CompactionRecord rec;
        rec.up_to_index = up_to;
        rec.summary     = std::move(summary);
        rec.created_at  = std::chrono::system_clock::now();
        m.d.current.compactions.push_back(std::move(rec));
        m.d.current.updated_at = std::chrono::system_clock::now();

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

        m.s.compaction_target_index = 0;
        m.s.compacting    = false;
        m.s.phase         = phase::Idle{};
        // tokens_in / tokens_out are intentionally NOT touched: the
        // StreamUsage handler skipped writes during compaction, so
        // these still hold the values from the last real (non-
        // compaction) turn. Resetting them to 0 would produce a
        // visible flicker on the context gauge between this point
        // and the next StreamUsage event.
        // Smooth UX: when a user message is queued behind compaction,
        // it's about to fire below — the user will immediately see
        // their request go out, so a "context compacted" toast is
        // noise on top of their answer streaming in. Skip it entirely.
        // Only surface the rapid-refill disable warning (it's actionable
        // info: tells the user to run /compact manually next time)
        // and only show "context compacted" when there's no queued
        // work and the user might still be looking at the spinner.
        // Even then, keep it brief (1.5s) instead of the prior 4s
        // dwell that competed for attention with the next thing the
        // user actually wants to read.
        const bool queued_about_to_fire = !m.ui.composer.queued.empty();
        auto now_ts = std::chrono::steady_clock::now();
        if (m.s.autocompact_disabled) {
            m.s.status        = "auto-compact disabled (rapid refill); use /compact manually";
            m.s.status_until  = now_ts + std::chrono::seconds{6};
        } else if (queued_about_to_fire) {
            // Silent path — the queued message becomes the next visible
            // event in the UI, no toast needed.
            m.s.status.clear();
            m.s.status_until = {};
        } else {
            m.s.status        = "context compacted";
            m.s.status_until  = now_ts + std::chrono::milliseconds{1500};
        }
        deps().save_thread(m.d.current);
        // Hand the freshly-freed arenas back to the OS. Less to free
        // now that we don't drop tool outputs — the only freed bytes
        // are the compaction stream's input/output buffers — but
        // still cheap and keeps the historical "compact → RSS dips"
        // user signal alive on glibc. No-op on musl/macOS/Windows.
        release_to_kernel();

        // Drain any messages the user queued during compaction. Same
        // shape as the post-StreamFinished drain at the end of this
        // function, but tailored: we're already Idle, no pending tools
        // to kick.
        if (queued_about_to_fire) {
            auto& head = m.ui.composer.queued.front();
            m.ui.composer.text        = std::move(head.text);
            m.ui.composer.attachments = std::move(head.attachments);
            m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
            m.ui.composer.queued.erase(m.ui.composer.queued.begin());
            auto [mm, sub_cmd] = submit_message(std::move(m));
            m = std::move(mm);
            return sub_cmd;
        }
        if (m.s.status.empty()) return Cmd<Msg>::none();
        auto stamp = m.s.status_until;
        auto ttl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            m.s.status_until - now_ts);
        return Cmd<Msg>::after(ttl_ms + std::chrono::milliseconds{50},
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
                        // Distinguish "never closed" (parse failed AND no
                        // mid-string truncation) from "truncated mid-string"
                        // (salvage_args refused because close_partial_json
                        // would have synthesised a closing quote on a
                        // half-written value). The model needs the
                        // mid-string story to know its file body never
                        // finished arriving, not just "args malformed".
                        const bool mid_string =
                            agentty::tools::util::ended_inside_string(
                                tc.args_streaming);
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            mid_string
                                ? std::string{"tool args truncated "
                                    "mid-string \u2014 the wire cut off "
                                    "inside a string value (likely "
                                    "`content` / `command` / `new_text`), "
                                    "so the body is incomplete and the "
                                    "call was refused. Re-emit the tool "
                                    "with the full payload."}
                                : std::string{"tool args never closed: "}
                                    + ex.what()};
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
        auto& head = m.ui.composer.queued.front();
        m.ui.composer.text        = std::move(head.text);
        m.ui.composer.attachments = std::move(head.attachments);
        m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }

    // Post-turn idle auto-compaction.
    //
    // The turn finished, no pending tools, no queued messages. This
    // is the natural pause moment — the user is reading the model's
    // output, not typing. If the wire estimate is over the threshold
    // (~80% of context_max with output-reserve headroom), kick off
    // compaction in the background. By the time the user types their
    // next message, the wire view has been replaced with a summary
    // and the next request goes out at a fraction of the size.
    //
    // Threshold mirrors the old submit-time formula
    // (context_max - kOutputReserve - kCompactSlack) since the
    // intent is the same: "is the NEXT turn at risk of hitting the
    // ceiling?" The difference is WHEN we check — we check at the
    // post-turn idle boundary, not at submit — so the compaction
    // round happens while the user is reading, not blocking them.
    //
    // Soft-trim on the normal-turn path already handles "the next
    // request would overflow" by silently dropping oldest raw turns,
    // so this trigger is purely a quality optimisation: summary >
    // truncation. If compaction fails (rate-limited, network drop)
    // the user still gets through their next turn via soft-trim.
    if (m.s.is_idle()
        && !m.s.compacting
        && !m.s.autocompact_disabled
        && m.s.context_max > 0
        && m.ui.composer.queued.empty()
        && !m.d.current.messages.empty()) {
        constexpr int kOutputReserve = 13000;
        constexpr int kCompactSlack  = 4000;
        const int threshold = std::max(0,
            m.s.context_max - kOutputReserve - kCompactSlack);
        const int est = cmd::estimate_wire_tokens(m.d.current);
        if (std::max(m.s.tokens_in, est) > threshold) {
            // Dispatch CompactContext as an async Msg so it goes
            // through the same reducer arm /compact uses; that arm
            // is the single source of truth for compaction kickoff
            // (target_index capture, buffer clear, phase transition).
            // The user sees the banner flip to "compacting context…"
            // while they're reading; their next submit (when they
            // get to it) either lands after compaction finishes
            // (queueing for max ~1-2 turn-times) OR Esc-cancels
            // compaction and fires immediately.
            auto compact_cmd = Cmd<Msg>::task(
                [](std::function<void(Msg)> dispatch) {
                    dispatch(CompactContext{});
                });
            return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(kp), std::move(compact_cmd)});
        }
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
                    if (a->first_delta_at.time_since_epoch().count() == 0) {
                        a->first_delta_at = now;
                        // First real byte of this stream attempt —
                        // the connection demonstrably works. Reset
                        // the retry budget so a later mid-stream
                        // failure gets a fresh ladder instead of
                        // inheriting whatever attempts the prior
                        // (now-succeeded) connect-time retries
                        // consumed. Without this, an opus turn that
                        // survived 2 transient connect failures and
                        // then hit a mid-stream stall 90 s in could
                        // only retry once before being marked
                        // terminal — even though the wire-level
                        // health was perfect for 90 s of streaming.
                        a->transient_retries = 0;
                    }
                    a->live_delta_bytes += e.text.size();
                }
            }
            // Compaction routing: during a compaction stream the
            // transcript has no synthetic Assistant placeholder to
            // stream into — the summary lives off-transcript in
            // `m.s.compaction_buffer` and only ever surfaces as a
            // CompactionRecord at finalize_turn. Route every byte
            // there instead of into messages.back(), with the same
            // per-stream byte cap so a runaway summary can't OOM.
            if (m.s.compacting) {
                if (m.s.compaction_buffer.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - m.s.compaction_buffer.size();
                    if (e.text.size() <= room) m.s.compaction_buffer += e.text;
                    else m.s.compaction_buffer.append(e.text, 0, room);
                }
                return done(std::move(m));
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
                // First-byte inline reveal. The Tick pacer is
                // responsible for the steady fill animation, but its
                // cadence (33 ms on DEC-2026 terminals, 100 ms on
                // plain xterm / Apple Terminal / tmux-without-sync)
                // adds that much latency to the very first paint of
                // every turn. Reveal a small head slice synchronously
                // here so time-to-first-paint is bounded by SSE
                // arrival, not by tick cadence. The pacer takes over
                // for the rest — this just unblocks the renderer for
                // the leading edge.
                //
                // Gated on streaming_text being empty so we only fire
                // once per assistant message; subsequent deltas flow
                // through the pacer unchanged.
                if (msg.streaming_text.empty() && !msg.pending_stream.empty()) {
                    constexpr std::size_t kFirstReveal = 256;
                    std::size_t n = std::min(kFirstReveal, msg.pending_stream.size());
                    // UTF-8 safety — same rule as the Tick pacer.
                    // Bound the continuation walk at 3 bytes so a
                    // malformed buffer can't push us through the
                    // whole pending_stream.
                    {
                        std::size_t walked = 0;
                        while (n < msg.pending_stream.size()
                               && walked < 3
                               && (static_cast<unsigned char>(msg.pending_stream[n]) & 0xC0) == 0x80) {
                            ++n;
                            ++walked;
                        }
                    }
                    msg.streaming_text.append(msg.pending_stream, 0, n);
                    msg.pending_stream.erase(0, n);
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
                    if (a->first_delta_at.time_since_epoch().count() == 0) {
                        a->first_delta_at = now;
                        // See StreamTextDelta — same rationale.
                        a->transient_retries = 0;
                    }
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
                        // whole turn.  salvage_args refuses to salvage when
                        // the wire stopped mid-string (Finding 3) since
                        // close_partial_json would synthesise a closing
                        // quote on a half-written body — in that case we
                        // surface a clear truncated-mid-string failure
                        // instead of "invalid tool arguments".
                        const bool mid_string =
                            agentty::tools::util::ended_inside_string(
                                tc.args_streaming);
                        auto salvaged = salvage_args(tc);
                        if (!salvaged.empty()) {
                            tc.args = std::move(salvaged);
                            tc.mark_args_dirty();
                            std::string{}.swap(tc.args_streaming);
                        } else if (mid_string) {
                            auto now = std::chrono::steady_clock::now();
                            tc.status = ToolUse::Failed{
                                tc.started_at(), now,
                                "tool args truncated mid-string \u2014 the "
                                "wire cut off inside a string value (likely "
                                "`content` / `command` / `new_text`), so "
                                "the body is incomplete and the call was "
                                "refused. Re-emit the tool with the full "
                                "payload."};
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
            // Suppress token writes during compaction. The compaction
            // stream's `input_tokens` reflects the SUMMARISATION request
            // (full prefix + the verbose summarisation prompt) and its
            // `output_tokens` reflects the model's summary length —
            // neither belongs in the user-facing context gauge or the
            // "tokens generated" counter. Without this guard the gauge
            // briefly jumps to the wire-payload size mid-compaction and
            // resets to 0 in finalize_turn, producing a visible flicker
            // on the status bar. The PRIOR turn's values are still on
            // m.s.tokens_in/out from before compaction started; leaving
            // them in place gives a more honest "what's the model
            // actually carrying right now" reading until the next real
            // turn settles.
            if (m.s.compacting) return done(std::move(m));
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
            // etc.). Compaction is wire-only — nothing was ever pushed
            // into `messages` — so we don't have to rewind any transcript
            // state. Either retry on a backoff (transient / rate-limit
            // with budget remaining) or surface a clear actionable toast
            // and return to Idle. Either way the user-visible transcript
            // is identical to its pre-compaction shape because it was
            // never mutated.
            //
            // Workflow protection: retry transparently on transient and
            // rate-limit failures so a flaky network doesn't force the
            // user to babysit /compact. The summarisation request is
            // stateless from the server's POV — every retry rebuilds
            // the same wire payload from the immutable transcript — so
            // there's nothing to corrupt across attempts. On terminal
            // failure (auth, content filter, retry budget exhausted)
            // we drop to Idle, clear the buffer, and rely on the
            // launch_stream soft-trim to keep subsequent NORMAL turns
            // working even though compaction couldn't finish. The
            // user can re-invoke /compact later when conditions improve.
            if (m.s.compacting) {
                if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

                auto klass = provider::classify(e.message);
                if (m.s.in_stall_fired()
                    && klass == provider::ErrorClass::Cancelled) {
                    klass = provider::ErrorClass::Transient;
                }

                const phase::Active* cctx = active_ctx(m.s.phase);
                int prior = cctx ? cctx->transient_retries : 0;
                const bool can_retry_compact =
                    cctx
                    && (klass == provider::ErrorClass::Transient
                        || klass == provider::ErrorClass::RateLimit)
                    && prior < provider::kMaxRetries;

                if (can_retry_compact) {
                    std::chrono::milliseconds delay;
                    if (e.retry_after.has_value()) {
                        auto s = e.retry_after->count();
                        if (s < 1) s = 1;
                        if (s > 120) s = 120;
                        delay = std::chrono::seconds(s);
                    } else {
                        delay = provider::backoff_with_jitter(klass, prior);
                    }
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        delay + std::chrono::milliseconds{999}).count();
                    // Keep the user-facing wording calm: "context
                    // compacting" is already on the banner; we just
                    // append the retry hint so the user knows the
                    // operation is still alive, not silently wedged.
                    m.s.status = "compacting — retrying in " + std::to_string(secs) + "s";
                    m.s.status_until = std::chrono::steady_clock::now()
                                     + delay + std::chrono::milliseconds{1500};
                    // Reset only the streamed bytes — keep `compacting`
                    // and `compaction_target_index` so launch_stream
                    // rebuilds the same compaction request shape on
                    // RetryStream.
                    m.s.compaction_buffer.clear();
                    auto ctx = take_active_ctx(std::move(m.s.phase)).value();
                    ctx.transient_retries = prior + 1;
                    ctx.retry             = retry::Scheduled{};
                    m.s.phase = phase::Streaming{std::move(ctx)};
                    return {std::move(m),
                            Cmd<Msg>::after(delay, Msg{RetryStream{}})};
                }

                // Terminal compaction failure. Drop everything compaction-
                // related and surface ONE clear toast. Soft-trim on the
                // normal-turn path keeps the agent working at reduced
                // wire size; the user can /compact again later.
                m.s.compacting              = false;
                m.s.compaction_target_index = 0;
                m.s.compaction_buffer.clear();
                m.s.phase = phase::Idle{};
                m.s.status = (klass == provider::ErrorClass::Cancelled)
                    ? "compaction cancelled"
                    : ("compaction failed: " + e.message
                       + " — retry with /compact");
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{
                    klass == provider::ErrorClass::Cancelled ? 3 : 8};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                // Drain queued messages even though compaction failed —
                // launch_stream's soft-trim will fit them onto the
                // wire. Better to give the user their reply than to
                // leave their typing stuck behind a dead compaction.
                if (!m.ui.composer.queued.empty()) {
                    auto& head = m.ui.composer.queued.front();
                    m.ui.composer.text        = std::move(head.text);
                    m.ui.composer.attachments = std::move(head.attachments);
                    m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
                    m.ui.composer.queued.erase(m.ui.composer.queued.begin());
                    auto [mm, sub_cmd] = submit_message(std::move(m));
                    m = std::move(mm);
                    return {std::move(m), Cmd<Msg>::batch(
                        std::move(status_cmd), std::move(sub_cmd))};
                }
                return {std::move(m), std::move(status_cmd)};
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
            //
            // `prepended_into_committed_text` records the corruption-
            // adjacent case the renderer cares about: a multi-sub-turn
            // assistant message where `last->text` was ALREADY non-empty
            // before this error, and we just appended more bytes onto
            // it. StreamingMarkdown's `set_content` had been fed the
            // prior streaming_text alone; the new content does NOT have
            // that as a prefix, so the next frame's `set_content` hits
            // the replace path, calls `clear()`, and re-emits the body
            // through fresh block parsing. The inline-tail layout and
            // the committed-block layout of the same bytes have
            // different per-block heights, which shifts content_rows.
            // Live frame repaints correctly via the diff; rows that
            // already overflowed into native scrollback retain the
            // OLD layout — visible as duplicated / missing / fragmented
            // border rows at the scrollback↔viewport seam. Force a
            // Divergent transition on the next render so the full
            // repaint resolves the seam (scrollback wipe is acceptable
            // — a stream error has already disturbed the user's flow).
            // See docs/corruption-analysis.md Finding 2.
            Message* last = nullptr;
            bool prepended_into_committed_text = false;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                last = &m.d.current.messages.back();
                if (!last->pending_stream.empty()) {
                    last->streaming_text += last->pending_stream;
                    last->pending_stream.clear();
                }
                if (!last->streaming_text.empty()) {
                    if (last->text.empty()) last->text = std::move(last->streaming_text);
                    else                  {
                        prepended_into_committed_text = true;
                        last->text += std::move(last->streaming_text);
                    }
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
                auto retry_cmd = Cmd<Msg>::after(delay, Msg{RetryStream{}});
                if (prepended_into_committed_text) {
                    // Resolve the scrollback↔viewport seam mismatch
                    // before the retry stream starts feeding new
                    // deltas through StreamingMarkdown.
                    return {std::move(m),
                        Cmd<Msg>::batch(std::move(retry_cmd),
                                        Cmd<Msg>::force_redraw())};
                }
                return {std::move(m), std::move(retry_cmd)};
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
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                if (prepended_into_committed_text) {
                    // Terminal-path counterpart of the retry-path fix
                    // above: the message body just grew by prepend, so
                    // StreamingMarkdown will re-parse from scratch on
                    // the next set_content and the scrollback tail
                    // disagrees with the new live frame. Force a full
                    // repaint on the next render.
                    return {std::move(m),
                        Cmd<Msg>::batch(std::move(status_cmd),
                                        Cmd<Msg>::force_redraw())};
                }
                return {std::move(m), std::move(status_cmd)};
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

            // Compaction-cancel: nothing was ever appended to the
            // transcript, so we just discard the off-transcript buffer
            // and clear the in-flight flag. The user sees their full
            // pre-compaction transcript stay intact.
            //
            // Workflow protection: a message queued behind the
            // cancelled compaction would otherwise sit dormant until
            // the user manually re-submits something. Drain it here
            // so cancellation feels like "keep going without compacting"
            // rather than "freeze my whole session." launch_stream's
            // soft-trim will fit the wire payload even though we
            // skipped the summarisation.
            const bool was_compacting = m.s.compacting;
            if (m.s.compacting) {
                m.s.compacting              = false;
                m.s.compaction_target_index = 0;
                m.s.compaction_buffer.clear();
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
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                // If the user cancelled a COMPACTION (not a normal
                // turn) and had queued work behind it, fire that
                // queued message immediately. Cancelling a normal
                // turn leaves the queue alone because the user just
                // killed their own in-flight request — they probably
                // want to type something different next, not have the
                // queue automatically drain. Compaction was a side
                // operation the user didn't ask for in the first
                // place; killing it shouldn't penalise their actual
                // request.
                if (was_compacting && !m.ui.composer.queued.empty()) {
                    auto& head = m.ui.composer.queued.front();
                    m.ui.composer.text        = std::move(head.text);
                    m.ui.composer.attachments = std::move(head.attachments);
                    m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
                    m.ui.composer.queued.erase(m.ui.composer.queued.begin());
                    auto [mm, sub_cmd] = submit_message(std::move(m));
                    m = std::move(mm);
                    return {std::move(m), Cmd<Msg>::batch(
                        std::move(status_cmd), std::move(sub_cmd))};
                }
                return {std::move(m), std::move(status_cmd)};
            }
        },
    }, sm);
}

} // namespace agentty::app::detail
