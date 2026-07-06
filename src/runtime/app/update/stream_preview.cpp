// Live tool-args preview decoder, split out of stream.cpp.
//
// update_stream_preview runs on every input_json_delta tick (throttled by the
// caller) and incrementally decodes the growing tool-args wire buffer into
// tc.args so the tool card can render path / content / edits / todos as they
// stream. sync_todo_state_from_args mirrors a completed todo call's items into
// the UI plan state. Both are declared in internal.hpp (cross-TU); the shared
// pure leaf helpers they lean on live in stream_args.hpp.
//
// This file carries the O(N)-per-tick decode hot path (write's cached-offset
// content decode, edit's edits[] mirror). Keeping it out of stream.cpp shrinks
// the reducer TU and isolates the preview logic from the finalize/error state
// machine it has no dependency on.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update/stream_args.hpp"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agentty/tool/util/partial_json.hpp"

namespace agentty::app::detail {

using json = nlohmann::json;

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
                    tc.stream_decode_through = *p;
                    break;
                }
            }
        }
        if (tc.stream_sniff_offset != 0) {
            // Incremental decode: each tick consumes only the new bytes
            // since stream_decode_through, then trims the accumulated
            // value back to ~2× the preview cap so memory stays bounded
            // on huge writes. Cumulative cost is O(args_streaming.size())
            // across the whole stream, not O(N²).
            agentty::tools::util::decode_string_append(
                tc.args_streaming,
                &tc.stream_decode_through,
                tc.stream_decoded_value);
            constexpr std::size_t kKeepCap = 2 * kStreamingPreviewCap;
            if (tc.stream_decoded_value.size() > kKeepCap) {
                // Trim the head, keep the freshest 2×cap bytes. The
                // preview only ever displays the trailing cap, so the
                // head we drop was already invisible.
                tc.stream_decoded_value.erase(
                    0, tc.stream_decoded_value.size() - kKeepCap);
            }
            const auto& v = tc.stream_decoded_value;
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
                    set_arg("content", v);
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
          || n == "git_log"    || n == "git_commit") {
        if (n == "git_commit") try_set("message");
        pull_desc();
    }
    else if (n == "todo") {
        pull_desc();
        // Mirror the partial todos[] array into tc.args["todos"] as it
        // streams so the inline plan card fills in row-by-row (and the
        // persistent plan state below tracks the in-progress item) the
        // moment each item lands -- not only once the full array arrives
        // at StreamToolUseEnd. Mirrors Zed's live update_plan element.
        const auto& parsed = get_parsed();
        if (parsed) {
            if (auto it = parsed->find("todos");
                it != parsed->end() && it->is_array() && !it->empty())
            {
                json arr = json::array();
                for (const auto& td : *it) {
                    if (!td.is_object()) continue;
                    auto c = td.find("content");
                    if (c == td.end() || !c->is_string()) continue;
                    json item = json::object();
                    item["content"] = c->get<std::string>();
                    if (auto s = td.find("status");
                        s != td.end() && s->is_string())
                        item["status"] = s->get<std::string>();
                    else
                        item["status"] = "pending";
                    arr.push_back(std::move(item));
                }
                if (!arr.empty()) {
                    if (!tc.args.is_object()) tc.args = json::object();
                    auto& cur = tc.args["todos"];
                    bool changed = !cur.is_array() || cur.size() != arr.size();
                    if (!changed) {
                        for (std::size_t i = 0; i < arr.size(); ++i) {
                            if (arr[i].value("content", std::string{})
                                    != cur[i].value("content", std::string{})
                             || arr[i].value("status", std::string{})
                                    != cur[i].value("status", std::string{})) {
                                changed = true; break;
                            }
                        }
                    }
                    if (changed) { cur = std::move(arr); tc.mark_args_dirty(); }
                }
            }
        }
    }
}

void sync_todo_state_from_args(Model& m, const nlohmann::json& args) {
    if (!args.is_object()) return;
    auto it = args.find("todos");
    if (it == args.end() || !it->is_array() || it->empty()) return;

    std::vector<TodoItem> items;
    items.reserve(it->size());
    for (const auto& td : *it) {
        if (!td.is_object()) continue;
        auto c = td.find("content");
        if (c == td.end() || !c->is_string()) continue;
        TodoItem item;
        item.content = c->get<std::string>();
        auto st = td.value("status", std::string{"pending"});
        item.status = st == "completed"   ? TodoStatus::Completed
                    : st == "in_progress" ? TodoStatus::InProgress
                                           : TodoStatus::Pending;
        items.push_back(std::move(item));
    }
    if (items.empty()) return;
    m.ui.todo.items = std::move(items);
}

} // namespace agentty::app::detail
