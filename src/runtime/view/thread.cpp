#include "moha/runtime/view/thread.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/git_commit_tool.hpp>
#include <maya/widget/git_graph.hpp>
#include <maya/widget/git_status.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/timeline.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/todo_list.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/permission.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// Cached markdown render for an assistant message body.  Completed messages
// are immutable (mutators in update.cpp must reset cached_md_element), so
// once built the Element is reused across every frame.  The streaming tail
// uses StreamingMarkdown — block-boundary cache → O(new_chars) per delta.
Element cached_markdown_for(const Message& msg) {
    if (msg.text.empty()) {
        if (!msg.stream_md)
            msg.stream_md = std::make_shared<maya::StreamingMarkdown>();
        msg.stream_md->set_content(msg.streaming_text);
        return msg.stream_md->build();
    }
    if (!msg.cached_md_element) {
        msg.cached_md_element =
            std::make_shared<Element>(maya::markdown(msg.text));
        msg.stream_md.reset();
    }
    return *msg.cached_md_element;
}

// ── Helpers ─────────────────────────────────────────────────────────

template <class W, class StatusEnum>
StatusEnum map_status(const ToolUse::Status& s, StatusEnum running, StatusEnum failed,
                      StatusEnum done) {
    return std::visit([&](const auto& v) -> StatusEnum {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, ToolUse::Pending>
                   || std::same_as<T, ToolUse::Running>) return running;
        else if constexpr (std::same_as<T, ToolUse::Failed>
                        || std::same_as<T, ToolUse::Rejected>) return failed;
        else /* Done | Approved */ return done;
    }, s);
}

maya::ToolCallStatus tc_status(const ToolUse::Status& s) {
    return map_status<maya::ToolCall>(s,
        ToolCallStatus::Running, ToolCallStatus::Failed, ToolCallStatus::Completed);
}

std::string safe_arg(const nlohmann::json& args, const char* key) {
    if (!args.is_object()) return {};
    return args.value(key, "");
}

// Pick the first non-empty string under any of the listed keys.
// Mirrors the alias-tolerant parsing the tool implementations themselves
// do (write/edit accept `path | file_path | filepath | filename`,
// `display_description | description`, etc.) — without this, the view
// reads a single canonical key and a model that picks an alias renders
// as a blank card even though the underlying tool ran fine.
std::string pick_arg(const nlohmann::json& args,
                     std::initializer_list<const char*> keys) {
    if (!args.is_object()) return {};
    for (const char* k : keys) {
        if (auto it = args.find(k); it != args.end() && it->is_string()) {
            const auto& s = it->get_ref<const std::string&>();
            if (!s.empty()) return s;
        }
    }
    return {};
}

int safe_int_arg(const nlohmann::json& args, const char* key, int def) {
    if (!args.is_object() || !args.contains(key)) return def;
    return args.value(key, def);
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') n++;
    return n + (!s.empty() && s.back() != '\n' ? 1 : 0);
}

// Prepend the model's `display_description` to a card title when set:
//   "Fix null-deref in auth.cpp  ·  src/auth.cpp"
// When desc is empty the title is returned unchanged, so callers don't need
// to branch at the call site.
std::string with_desc(std::string_view title, const std::string& desc) {
    if (desc.empty()) return std::string{title};
    return desc + "  \u00B7  " + std::string{title};
}

// Seconds spent on this tool call so far. For running tools, "now - started";
// for finished tools, "finished - started". Returns 0 if started_at is unset
// (tool still Pending). Called every Tick frame while a tool runs, so the
// elapsed counter on the card updates live (~30 fps).
float tool_elapsed(const ToolUse& tc) {
    auto zero = std::chrono::steady_clock::time_point{};
    auto started = tc.started_at();
    if (started == zero) return 0.0f;
    auto finished = tc.finished_at();
    auto end = finished == zero ? std::chrono::steady_clock::now() : finished;
    auto dt = end - started;
    return std::chrono::duration<float>(dt).count();
}

// Tool output from `bash` wraps the captured stdout+stderr in a ```…``` fence
// (so the raw bytes come back to the model inside a markdown code block, and
// the model sees an unambiguous "this is the literal output" boundary). The
// BashTool widget has its own monospace frame, so the fence chars would be
// rendered verbatim ("```" floating at the top of every card). Strip them —
// along with the elapsed/truncation trailers that follow — so the widget
// shows just the inner payload. Keep the raw, fenced string in tc.output
// for the model; only the view gets the stripped version.
std::string strip_bash_output_fence(const std::string& s) {
    std::string_view sv{s};
    // Trailing metadata lines we emit after the closing fence — drop them
    // first so the "```" we look for below actually ends the block.
    auto drop_trailer = [&](std::string_view marker) {
        auto pos = sv.rfind(marker);
        if (pos != std::string_view::npos) sv = sv.substr(0, pos);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'
                               || sv.back() == ' '  || sv.back() == '\t'))
            sv.remove_suffix(1);
    };
    drop_trailer("\n\n[elapsed:");
    drop_trailer("\n\n[output truncated");

    auto fence = sv.find("```");
    if (fence == std::string_view::npos) return std::string{sv};
    // Allow a leading "Command …\n\n" header before the fence — the failure
    // and timeout branches put one there.
    auto body_start = fence + 3;
    // Skip a language tag (we don't emit one, but be forgiving) and the
    // newline after the opening fence.
    while (body_start < sv.size() && sv[body_start] != '\n') ++body_start;
    if (body_start < sv.size() && sv[body_start] == '\n') ++body_start;

    auto close = sv.rfind("```");
    if (close == std::string_view::npos || close <= body_start)
        return std::string{sv.substr(body_start)};

    auto body_end = close;
    while (body_end > body_start
           && (sv[body_end - 1] == '\n' || sv[body_end - 1] == '\r'))
        --body_end;

    std::string header{sv.substr(0, fence)};
    while (!header.empty() && (header.back() == '\n' || header.back() == '\r'
                               || header.back() == ' '))
        header.pop_back();
    std::string body{sv.substr(body_start, body_end - body_start)};
    if (header.empty()) return body;
    if (body.empty()) return header;
    return header + "\n\n" + body;
}

int parse_exit_code(const std::string& output) {
    // Recognize both formats: the "[exit code N]" suffix from legacy_format
    // (used by diagnostics/git) and the "failed with exit code N" clause from
    // the Zed-style bash formatter. Whichever appears, pull the integer.
    struct Marker { const char* text; size_t skip; };
    static constexpr Marker markers[] = {
        {"failed with exit code ", 22},
        {"[exit code ",            11},
    };
    for (const auto& m : markers) {
        auto pos = output.rfind(m.text);
        if (pos == std::string::npos) continue;
        try { return std::stoi(output.substr(pos + m.skip)); }
        catch (...) { return 1; }
    }
    if (output.find("timed out") != std::string::npos) return 124;
    return 0;
}

Element tool_card(const std::string& name, ToolCallKind kind,
                  const std::string& desc, const ToolUse::Status& status,
                  bool expanded, const std::string& output,
                  float elapsed = 0.0f) {
    maya::ToolCall::Config cfg;
    cfg.tool_name = name;
    cfg.kind = kind;
    cfg.description = desc;
    maya::ToolCall card(cfg);
    card.set_expanded(expanded);
    card.set_status(tc_status(status));
    card.set_elapsed(elapsed);
    if (!output.empty())
        card.set_content(text(output, fg_of(muted)));
    return card.build();
}

Element parse_grep_result(const ToolUse& tc, const std::string& pattern, bool collapsed) {
    SearchResult sr(SearchKind::Grep, pattern);
    sr.set_expanded(!collapsed);
    sr.set_max_matches_per_file(2);
    sr.set_status(map_status<SearchResult>(tc.status,
        SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
    sr.set_elapsed(tool_elapsed(tc));
    if (tc.output().empty() || !tc.is_done()
        || tc.output().starts_with("No matches")) {
        return sr.build();
    }

    SearchFileGroup current_group;
    int total_groups = 0;
    auto flush = [&](SearchFileGroup& g) {
        if (!g.file_path.empty()) {
            sr.add_group(std::move(g));
            total_groups++;
            g = SearchFileGroup{};
        }
    };

    // Markdown format (new):
    //   ## Matches in {path}
    //   ### L{s}-{e}
    //   ```
    //   {context lines}
    //   ```
    // Legacy format (from find_definition which still uses path:line:content):
    //   {path}:{line}:{content}
    std::istringstream iss(tc.output());
    std::string line;
    int range_start = 0, range_end = 0;
    bool in_code = false;
    int code_line_no = 0;
    while (std::getline(iss, line)) {
        if (line.starts_with("## Matches in ")) {
            flush(current_group);
            if (total_groups >= 10) break;
            auto path = line.substr(14);
            if (path.starts_with("./")) path = path.substr(2);
            current_group = SearchFileGroup{std::move(path), {}};
            in_code = false;
            continue;
        }
        if (line.starts_with("### L")) {
            auto dash = line.find('-', 5);
            try {
                range_start = std::stoi(line.substr(5, dash - 5));
                range_end = dash != std::string::npos
                    ? std::stoi(line.substr(dash + 1)) : range_start;
            } catch (...) { range_start = range_end = 0; }
            continue;
        }
        if (line == "```") {
            if (!in_code) { in_code = true; code_line_no = range_start; }
            else          { in_code = false; }
            continue;
        }
        if (in_code && !current_group.file_path.empty()) {
            current_group.matches.push_back({code_line_no++, line});
            continue;
        }
        // Legacy fallback: path:line:content (find_definition still uses this).
        if (!in_code) {
            auto c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            auto c2 = line.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            std::string file = line.substr(0, c1);
            if (file.starts_with("./")) file = file.substr(2);
            int lineno = 0;
            try { lineno = std::stoi(line.substr(c1+1, c2-c1-1)); } catch(...) {}
            std::string content = line.substr(c2 + 1);
            while (!content.empty() && (content.front() == ' ' || content.front() == '\t'))
                content.erase(content.begin());
            if (current_group.file_path != file) {
                flush(current_group);
                if (total_groups >= 10) break;
                current_group = SearchFileGroup{file, {}};
            }
            current_group.matches.push_back({lineno, content});
        }
    }
    flush(current_group);
    return sr.build();
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// render_tool_call — every tool gets a bordered card with status icon
// ════════════════════════════════════════════════════════════════════════

Element render_tool_call_uncached(const ToolUse& tc);

// Terminal-state card cache. A chat with 40 tool calls rebuilds 40 borders
// + 40 Yoga layouts + 40 text runs every frame otherwise — even when
// nothing about those cards has changed in minutes. We only cache when the
// tool has reached a terminal status; running/pending tools rebuild so the
// live elapsed counter keeps ticking.
Element render_tool_call(const ToolUse& tc) {
    const bool terminal = tc.is_terminal();
    if (terminal) {
        auto key = tc.compute_render_key();
        if (tc.render_cache && tc.render_cache_key == key)
            return *tc.render_cache;
        auto built = render_tool_call_uncached(tc);
        tc.render_cache     = std::make_shared<Element>(built);
        tc.render_cache_key = key;
        return built;
    }
    return render_tool_call_uncached(tc);
}

Element render_tool_call_uncached(const ToolUse& tc) {
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    auto cmd  = safe_arg(tc.args, "command");
    auto desc = pick_arg(tc.args, {"display_description", "description"});

    bool done = tc.is_terminal();

    // Live elapsed — grows each frame while Running, freezes on terminal status.
    float elapsed = tool_elapsed(tc);

    // ── read ────────────────────────────────────────────────────────
    if (tc.name == "read") {
        ReadTool rt(with_desc(path.empty() ? "read" : path, desc));
        rt.set_expanded(!done);
        rt.set_start_line(safe_int_arg(tc.args, "offset", 1));
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        rt.set_elapsed(elapsed);
        if (done) {
            rt.set_content(tc.output());
            rt.set_total_lines(count_lines(tc.output()));
            rt.set_max_lines(6);
        }
        return rt.build();
    }

    // ── list_dir (same style as read) ───────────────────────────────
    if (tc.name == "list_dir") {
        auto dir = path.empty() ? safe_arg(tc.args, "path") : path;
        if (dir.empty()) dir = ".";
        ReadTool rt(with_desc(dir, desc));
        rt.set_expanded(tc.expanded);
        rt.set_start_line(0);
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        rt.set_elapsed(elapsed);
        if (!tc.output().empty()) {
            rt.set_content(tc.output());
            rt.set_total_lines(count_lines(tc.output()));
            rt.set_max_lines(8);
        }
        return rt.build();
    }

    // ── write ───────────────────────────────────────────────────────
    if (tc.name == "write") {
        // While streaming, fall back from path → description → "(streaming…)"
        // so the card never sits on a static "(no path)" — that read as
        // "stuck" even when the model was actively generating.
        std::string file_path;
        if (!path.empty())      file_path = path;
        else if (!desc.empty()) file_path = desc;
        else                    file_path = "(streaming\xe2\x80\xa6)";
        // On error, fall back to the generic card so the failure reason
        // from the tool (permission denied, disk full, etc.) is visible.
        // WriteTool has no error-text surface.
        if (tc.is_failed() || tc.is_rejected()) {
            return tool_card("write", ToolCallKind::Edit,
                with_desc(path.empty() ? std::string{"write"} : path, desc),
                tc.status, tc.expanded, tc.output(), elapsed);
        }
        WriteTool wt(file_path);
        // Only set description as a separate field when path is real;
        // otherwise we already promoted it into the title above.
        if (!desc.empty() && !path.empty()) wt.set_description(desc);
        // Auto-expand while the model is still streaming `content` (Pending)
        // or the tool is writing to disk (Running) so the user sees a live
        // preview of the file being produced. Collapses on Done; user can
        // still toggle open via tc.expanded.
        wt.set_expanded(!done || tc.expanded);
        // Mirror the alias chain in src/tool/tools/write.cpp so the preview
        // shows the body whichever key the model picked. (`content` is the
        // canonical schema field; the rest are common aliases models reach
        // for. Last-resort salvage is intentionally not attempted here —
        // that's the tool's job; the view just shows whatever's claimed.)
        wt.set_content(pick_arg(tc.args, {"content", "file_text", "text",
                                          "body", "data", "contents",
                                          "file_content"}));
        wt.set_max_preview_lines(6);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        wt.set_elapsed(elapsed);
        return wt.build();
    }

    // ── edit ────────────────────────────────────────────────────────
    if (tc.name == "edit") {
        // Same path → desc → "(streaming…)" fallback as write so the card
        // never reads as stuck while the model is mid-stream.
        std::string base;
        if (!path.empty())      base = path;
        else if (!desc.empty()) base = desc;
        else                    base = "(streaming\xe2\x80\xa6)";
        auto header = (!path.empty() && !desc.empty())
                          ? with_desc(base, desc) : base;
        if (tc.is_failed() || tc.is_rejected()) {
            return tool_card("edit", ToolCallKind::Edit,
                header, tc.status, tc.expanded, tc.output(), elapsed);
        }
        EditTool et(header);
        // Edit cards stay expanded permanently — the diff is the whole point
        // of the card and is usually small enough to leave visible. (Write
        // collapses on done because it's the whole file body.)
        et.set_expanded(true);
        // The tool accepts three input shapes (see src/tool/tools/edit.cpp):
        //   1. canonical:  edits: [{old_text, new_text, ...}, ...]
        //   2. Zed-legacy: old_text / new_text at top level
        //   3. moha-orig:  old_string / new_string at top level
        // For (1) we want to surface EVERY edit in the card — the streaming
        // preview now mirrors the full array into tc.args["edits"], so the
        // user can see all hunks land live instead of just the first.
        bool rendered_array = false;
        if (tc.args.is_object()) {
            if (auto it = tc.args.find("edits");
                it != tc.args.end() && it->is_array() && !it->empty())
            {
                std::vector<EditTool::EditPair> pairs;
                pairs.reserve(it->size());
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    std::string ot, nt;
                    if (auto v = e.find("old_text"); v != e.end() && v->is_string())
                        ot = v->get<std::string>();
                    else if (auto v2 = e.find("old_string"); v2 != e.end() && v2->is_string())
                        ot = v2->get<std::string>();
                    if (auto v = e.find("new_text"); v != e.end() && v->is_string())
                        nt = v->get<std::string>();
                    else if (auto v2 = e.find("new_string"); v2 != e.end() && v2->is_string())
                        nt = v2->get<std::string>();
                    pairs.push_back({std::move(ot), std::move(nt)});
                }
                if (!pairs.empty()) {
                    et.set_edits(std::move(pairs));
                    rendered_array = true;
                }
            }
        }
        if (!rendered_array) {
            // Legacy single-edit shape: top-level old_text/old_string and
            // their new_ counterparts. Only consulted when no edits array
            // is present so the renderer never double-shows the same diff.
            auto pick = [&](const char* legacy_key, const char* orig_key) -> std::string {
                auto v = safe_arg(tc.args, legacy_key);
                if (!v.empty()) return v;
                return safe_arg(tc.args, orig_key);
            };
            et.set_old_text(pick("old_text", "old_string"));
            et.set_new_text(pick("new_text", "new_string"));
        }
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        et.set_elapsed(elapsed);
        return et.build();
    }

    // ── bash ────────────────────────────────────────────────────────
    if (tc.name == "bash") {
        BashTool bt(with_desc(cmd.empty() ? "bash" : cmd, desc));
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(5);
        bt.set_status(map_status<BashTool>(tc.status,
            BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        bt.set_elapsed(elapsed);
        if (done) {
            int rc = parse_exit_code(tc.output());
            bt.set_exit_code(rc);
            if (rc != 0) bt.set_status(BashStatus::Failed);
            bt.set_output(strip_bash_output_fence(tc.output()));
        } else if (!tc.progress_text().empty()) {
            // Live stream: stdout+stderr captured so far. Shown verbatim
            // (no fence stripping — the fence is added only by the final
            // formatter once the process exits).
            bt.set_output(tc.progress_text());
        }
        return bt.build();
    }

    // ── diagnostics (same style as bash) ────────────────────────────
    if (tc.name == "diagnostics") {
        auto diag_cmd = safe_arg(tc.args, "command");
        BashTool bt(with_desc(diag_cmd.empty() ? "diagnostics" : diag_cmd, desc));
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(8);
        bt.set_elapsed(elapsed);
        if (done) {
            int rc = parse_exit_code(tc.output());
            bt.set_exit_code(rc);
            bt.set_status(rc == 0 ? BashStatus::Success : BashStatus::Failed);
            bt.set_output(strip_bash_output_fence(tc.output()));
        } else if (!tc.progress_text().empty()) {
            bt.set_output(tc.progress_text());
            bt.set_status(BashStatus::Running);
        } else {
            bt.set_status(map_status<BashTool>(tc.status,
                BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        }
        return bt.build();
    }

    // ── grep / find_definition (SearchResult widget) ────────────────
    if (tc.name == "grep" || tc.name == "find_definition") {
        auto pattern = tc.name == "grep"
            ? safe_arg(tc.args, "pattern")
            : safe_arg(tc.args, "symbol");
        bool collapsed = tc.is_done();
        return parse_grep_result(tc, pattern, collapsed);
    }

    // ── glob (SearchResult widget) ──────────────────────────────────
    if (tc.name == "glob") {
        auto pattern = safe_arg(tc.args, "pattern");
        SearchResult sr(SearchKind::Glob, with_desc(pattern, desc));
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        sr.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()
            && tc.output() != "no matches") {
            SearchFileGroup group{"", {}};
            std::istringstream iss(tc.output());
            std::string line;
            while (std::getline(iss, line)) {
                if (line.starts_with("./")) line = line.substr(2);
                if (!line.empty()) group.matches.push_back({0, line});
            }
            if (!group.matches.empty()) sr.add_group(std::move(group));
        }
        return sr.build();
    }

    // ── web_fetch (FetchTool widget) ────────────────────────────────
    if (tc.name == "web_fetch") {
        auto url = safe_arg(tc.args, "url");
        FetchTool ft(with_desc(url, desc));
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(6);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        ft.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            const auto& out = tc.output();
            auto first_nl = out.find('\n');
            if (first_nl != std::string::npos) {
                auto header = out.substr(0, first_nl);
                auto sp = header.find(' ');
                if (sp != std::string::npos) {
                    try { ft.set_status_code(std::stoi(header.substr(sp+1))); } catch(...) {}
                }
                auto paren = header.find('(');
                if (paren != std::string::npos) {
                    auto close = header.find(')', paren);
                    if (close != std::string::npos)
                        ft.set_content_type(header.substr(paren+1, close-paren-1));
                }
                auto body_start = out.find("\n\n");
                if (body_start != std::string::npos)
                    ft.set_body(out.substr(body_start + 2));
            }
        } else if (tc.is_failed()) {
            ft.set_body(tc.output());
        }
        return ft.build();
    }

    // ── web_search (FetchTool widget, same bordered style) ──────────
    if (tc.name == "web_search") {
        auto query = safe_arg(tc.args, "query");
        FetchTool ft(with_desc("search: " + query, desc));
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(8);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        ft.set_elapsed(elapsed);
        if (!tc.output().empty()) {
            ft.set_status_code(200);
            ft.set_body(tc.output());
        }
        return ft.build();
    }

    // ── git_status (GitStatusWidget inside a ToolCall card) ─────────
    if (tc.name == "git_status") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_status";
        cfg.kind = ToolCallKind::Other;
        if (!desc.empty()) cfg.description = desc;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            GitStatusWidget gs;
            gs.set_compact(false);
            int modified = 0, staged = 0, untracked = 0, deleted = 0;
            std::istringstream iss(tc.output());
            std::string line;
            while (std::getline(iss, line)) {
                if (line.starts_with("# branch.head "))
                    gs.set_branch(line.substr(14));
                else if (line.starts_with("# branch.ab ")) {
                    auto ab = line.substr(12);
                    auto sp = ab.find(' ');
                    if (sp != std::string::npos) {
                        try { gs.set_ahead(std::stoi(ab.substr(0, sp))); } catch(...) {}
                        try { gs.set_behind(-std::stoi(ab.substr(sp+1))); } catch(...) {}
                    }
                } else if (line.size() >= 2) {
                    if (line[0] == '?') { untracked++; continue; }
                    if (line[0] != '1' && line[0] != '2') continue;
                    if (line.size() < 4) continue;
                    char x = line[2], y = line[3];
                    if (x != '.') staged++;
                    if (y == 'M') modified++;
                    else if (y == 'D') deleted++;
                }
            }
            gs.set_dirty(modified, staged, untracked);
            gs.set_deleted(deleted);
            card.set_content(gs.build());
        }
        return card.build();
    }

    // ── git_log (GitGraph inside a ToolCall card) ───────────────────
    if (tc.name == "git_log") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_log";
        cfg.kind = ToolCallKind::Other;
        auto ref = safe_arg(tc.args, "ref");
        cfg.description = desc.empty() ? ref
                                       : (ref.empty() ? desc : desc + "  \u00B7  " + ref);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            GitGraph gg;
            gg.set_show_hash(true);
            gg.set_show_author(true);
            gg.set_show_time(true);
            std::istringstream iss(tc.output());
            std::string line;
            bool first = true;
            while (std::getline(iss, line)) {
                if (line.empty() || line[0] == ' ') continue;
                GitCommit gc;
                auto sp1 = line.find(' ');
                if (sp1 == std::string::npos) continue;
                gc.hash = line.substr(0, sp1);
                auto sp2 = line.find(' ', sp1 + 1);
                if (sp2 != std::string::npos) {
                    gc.time = line.substr(sp1 + 1, sp2 - sp1 - 1);
                    gc.author = line.substr(sp2 + 1);
                }
                std::string msg_line;
                if (std::getline(iss, msg_line)) {
                    while (!msg_line.empty() && msg_line.front() == ' ')
                        msg_line.erase(msg_line.begin());
                    gc.message = msg_line;
                }
                gc.is_head = first;
                first = false;
                gg.add_commit(std::move(gc));
            }
            card.set_content(gg.build());
        }
        return card.build();
    }

    // ── git_diff (DiffView inside a ToolCall card) ──────────────────
    if (tc.name == "git_diff") {
        auto ref = safe_arg(tc.args, "ref");
        auto diff_path = safe_arg(tc.args, "path");
        std::string body;
        if (!ref.empty()) body += ref;
        if (!diff_path.empty()) { if (!body.empty()) body += " "; body += diff_path; }

        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_diff";
        cfg.kind = ToolCallKind::Other;
        if (!desc.empty())
            cfg.description = body.empty() ? desc : desc + "  \u00B7  " + body;
        else
            cfg.description = body.empty() ? std::string{"working tree"} : body;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()
            && tc.output() != "no changes") {
            DiffView dv("", tc.output());
            card.set_content(dv.build());
        }
        return card.build();
    }

    // ── git_commit (GitCommitTool widget) ───────────────────────────
    if (tc.name == "git_commit") {
        auto msg = safe_arg(tc.args, "message");
        GitCommitTool gc(msg.empty() ? desc : msg);
        gc.set_expanded(tc.expanded);
        gc.set_status(map_status<GitCommitTool>(tc.status,
            GitCommitStatus::Running, GitCommitStatus::Failed, GitCommitStatus::Done));
        gc.set_elapsed(elapsed);
        if (!tc.output().empty()) gc.set_output(tc.output());
        return gc.build();
    }

    // ── todo (TodoListTool widget) ──────────────────────────────────
    if (tc.name == "todo") {
        TodoListTool tl;
        tl.set_description(desc);
        tl.set_elapsed(elapsed);
        tl.set_expanded(true);
        tl.set_status(map_status<TodoListTool>(tc.status,
            TodoListStatus::Running, TodoListStatus::Failed, TodoListStatus::Done));
        // Pull items straight from the model-supplied args so the card
        // reflects the intended state even while `run_todo` is still in-flight
        // (and so failure cards still show what was attempted).
        if (tc.args.is_object()) {
            if (auto it = tc.args.find("todos"); it != tc.args.end() && it->is_array()) {
                for (const auto& td : *it) {
                    if (!td.is_object()) continue;
                    TodoListItem item;
                    item.content = td.value("content", "");
                    auto s = td.value("status", std::string{"pending"});
                    item.status = s == "completed"   ? TodoItemStatus::Completed
                                : s == "in_progress" ? TodoItemStatus::InProgress
                                                     : TodoItemStatus::Pending;
                    tl.add(std::move(item));
                }
            }
        }
        return tl.build();
    }

    return tool_card(tc.name.value, ToolCallKind::Other,
        tc.args.is_object() && !tc.args.empty() ? tc.args_dump() : "",
        tc.status, tc.expanded, tc.output(), elapsed);
}

// ════════════════════════════════════════════════════════════════════════

// Per-speaker visual identity: brand color + glyph + display name.
// Centralized so the rail color, the header glyph, and the bottom
// streaming indicator stay in lockstep.
struct SpeakerStyle {
    Color       color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        // Cyan — distinct from every model brand color so user vs
        // assistant turns always read as different voices.
        return {highlight, "\xe2\x9d\xaf", "You"};                   // ❯
    }
    const auto& id = m.model_id.value;
    Color c;
    std::string label;
    if      (id.find("opus")   != std::string::npos) { c = accent;    label = "Opus";   }
    else if (id.find("sonnet") != std::string::npos) { c = info;      label = "Sonnet"; }
    else if (id.find("haiku")  != std::string::npos) { c = success;   label = "Haiku";  }
    else                                              { c = highlight; label = id;       }
    // Append a version like "4.7" if present in the id.
    for (std::size_t i = 0; i + 2 < id.size(); ++i) {
        char ch = id[i];
        if (ch >= '0' && ch <= '9') {
            char sep = id[i + 1];
            if ((sep == '-' || sep == '.') && id[i + 2] >= '0' && id[i + 2] <= '9') {
                std::size_t end = i + 3;
                while (end < id.size() && id[end] >= '0' && id[end] <= '9') ++end;
                auto ver = id.substr(i, end - i);
                for (auto& v : ver) if (v == '-') v = '.';
                label += " " + ver;
                break;
            }
        }
    }
    return {c, "\xe2\x9c\xa6", std::move(label)};                    // ✦
}

// Turn header: speaker glyph + name on the left (in the speaker color),
// dim metadata trailing right (timestamp · elapsed · turn N). The
// leading ▎ edge mark is gone — the continuous left rail (added at the
// turn-block level in render_message) already does that work and a
// double-bar would feel cluttered.
Element turn_header(Role role, int turn_num, const Message& msg,
                    const Model& m, std::optional<float> elapsed_secs) {
    auto style = speaker_style_for(role, m);

    // Trailing metadata: timestamp · elapsed · turn N
    std::string meta = timestamp_hh_mm(msg.timestamp);
    if (elapsed_secs && *elapsed_secs > 0.0f) {
        char buf[24];
        if      (*elapsed_secs < 1.0f)  std::snprintf(buf, sizeof(buf), " \xc2\xb7 %.0fms", *elapsed_secs * 1000.0);
        else if (*elapsed_secs < 60.0f) std::snprintf(buf, sizeof(buf), " \xc2\xb7 %.1fs", static_cast<double>(*elapsed_secs));
        else {
            int mins = static_cast<int>(*elapsed_secs) / 60;
            float secs = *elapsed_secs - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), " \xc2\xb7 %dm%.0fs", mins, static_cast<double>(secs));
        }
        meta += buf;
    }
    if (turn_num > 0) {
        meta += " \xc2\xb7 turn " + std::to_string(turn_num);
    }

    // `grow(1.0f)` on the header row is load-bearing: without it the row
    // shrinks to content width when a sibling element (like the active
    // turn's Timeline card) has its own intrinsic width, and the
    // `spacer()` inside collapses to 0 so the timestamp snaps left
    // against the speaker label instead of pinning the right edge.
    return (h(
        text(style.glyph, fg_of(style.color)),
        text(" ", {}),
        text(std::move(style.label), Style{}.with_fg(style.color).with_bold()),
        spacer(),
        text(std::move(meta), fg_dim(muted)),
        text(" ", {})
    ) | grow(1.0f)).build();
}

// Wrap a turn's full content (header + body + tools) in a left-only
// border colored by the speaker. The border becomes a continuous
// vertical rail running the entire height of the turn — the visual
// signature of polished chat UIs (Claude Code, Zed, Cursor, Linear).
// Color groups everything under one speaker; padding pushes content
// off the rail with breathing room. Bold border style → ┃ heavier
// vertical so the rail reads as a real divider, not a thin line.
Element with_turn_rail(Element content, Color rail_color) {
    return maya::detail::box()
        .direction(FlexDirection::Row)
        .border(BorderStyle::Bold, rail_color)
        .border_sides({.top = false, .right = false,
                       .bottom = false, .left = true})
        .padding(0, 0, 0, 2)
        .grow(1.0f)
      (std::move(content));
}

// Inter-turn divider: a thin dim horizontal rule across the gap.
// Replaces a bare blank line — the rule gives the eye a real handhold
// for "new turn starts here" without being heavy. Skipped for the very
// first turn since there's nothing above to divide from.
Element inter_turn_divider() {
    return Element{ComponentElement{
        .render = [](int w, int /*h*/) -> Element {
            std::string line;
            // Faded thin rule: dim · runs across with some breathing
            // space at the indent column so it doesn't crash into the
            // turn rail above. Reads as a quiet timeline tick.
            int indent = 3;
            for (int i = 0; i < indent; ++i) line += ' ';
            for (int i = indent; i < w; ++i) line += "\xe2\x94\x80";  // ─
            return Element{TextElement{
                .content = std::move(line),
                .style = Style{}.with_fg(Color::bright_black()).with_dim(),
            }};
        },
        .layout = {},
    }};
}

// User message body: plain text. Indent removed since the turn rail's
// padding already handles offset; this keeps the single-source-of-truth
// for "how far in" content sits.
Element user_message_body(const std::string& body) {
    return text(body, fg_of(fg));
}

// Brief "what this tool is doing" line for the Timeline view. Tool-
// specific so the user can read the sequence at a glance: paths for fs
// ops, the actual command for bash, the pattern for grep, etc. When
// the tool has settled (terminal status), the detail also folds in
// post-completion stats — line count for read/write, hunk + Δ for
// edit, match count for grep, exit code for bash, etc. — so the
// Timeline doubles as a compact result log without the user needing
// to expand individual cards.
std::string tool_timeline_detail(const ToolUse& tc) {
    auto safe = [&](const char* k) -> std::string { return safe_arg(tc.args, k); };
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    const auto& n = tc.name.value;

    // Pretty-print "src/foo/bar.cpp" rather than the absolute path when
    // the path lives under cwd. Uses the same trick the existing tool
    // cards do — strip a known-prefix.
    auto pretty_path = [&](std::string p) -> std::string {
        if (p.empty()) return p;
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec).string();
        if (!ec && !cwd.empty()
            && p.size() > cwd.size()
            && p.compare(0, cwd.size(), cwd) == 0
            && p[cwd.size()] == '/') {
            return p.substr(cwd.size() + 1);
        }
        // Drop the user's home prefix as `~/…`.
        if (const char* home = std::getenv("HOME"); home && *home) {
            std::string h{home};
            if (p.size() > h.size() && p.compare(0, h.size(), h) == 0
                && p[h.size()] == '/')
                return std::string{"~/"} + p.substr(h.size() + 1);
        }
        return p;
    };

    auto path_pp = pretty_path(path);

    if (n == "read") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (auto off = safe_int_arg(tc.args, "offset", 0); off > 0)
            detail += " @" + std::to_string(off);
        if (tc.is_done()) {
            // Output starts with the directory listing or "Read N lines..."
            // header. Just count newlines for a rough size hint.
            int lines = count_lines(tc.output());
            if (lines > 1) detail += "  \xc2\xb7  " + std::to_string(lines) + " lines";
        }
        return detail;
    }
    if (n == "write") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (tc.is_done()) {
            // Output line "Wrote/Overwrote N (+X -Y)" — just include +/− if
            // we can find them; otherwise fall back to char count of args.
            const auto& out = tc.output();
            if (auto plus = out.find('+'); plus != std::string::npos) {
                if (auto end = out.find(')', plus); end != std::string::npos) {
                    auto from = out.rfind('(', plus);
                    if (from != std::string::npos)
                        detail += "  " + out.substr(from, end - from + 1);
                }
            }
        }
        return detail;
    }
    if (n == "edit") {
        if (path_pp.empty()) return "\xe2\x80\xa6";
        std::string detail = path_pp;
        // Surface hunk count if the args carry an edits[] array.
        if (tc.args.is_object()) {
            auto it = tc.args.find("edits");
            if (it != tc.args.end() && it->is_array() && !it->empty())
                detail += "  \xc2\xb7  " + std::to_string(it->size()) + " edits";
        }
        if (tc.is_done()) {
            // Pull "(+X -Y)" out of the tool output if present.
            const auto& out = tc.output();
            if (auto from = out.find('('); from != std::string::npos) {
                if (auto end = out.find(')', from); end != std::string::npos
                    && (out.find('+', from) < end || out.find('-', from) < end))
                    detail += "  " + out.substr(from, end - from + 1);
            }
        }
        return detail;
    }
    if (n == "bash" || n == "diagnostics") {
        auto cmd = safe("command");
        if (cmd.empty()) return "\xe2\x80\xa6";
        if (auto nl = cmd.find('\n'); nl != std::string::npos)
            cmd = cmd.substr(0, nl) + " \xe2\x80\xa6";
        if (tc.is_done()) {
            int rc = parse_exit_code(tc.output());
            if (rc != 0) cmd += "  \xc2\xb7  exit " + std::to_string(rc);
        }
        return cmd;
    }
    if (n == "grep") {
        auto pat = safe("pattern");
        if (pat.empty()) return "\xe2\x80\xa6";
        std::string detail = path_pp.empty() ? pat : pat + "  in  " + path_pp;
        if (tc.is_done()) {
            int matches = count_lines(tc.output());
            if (matches > 0) detail += "  \xc2\xb7  " + std::to_string(matches) + " matches";
        }
        return detail;
    }
    if (n == "glob") {
        auto pat = safe("pattern");
        if (pat.empty()) return "\xe2\x80\xa6";
        std::string detail = pat;
        if (tc.is_done()) {
            int hits = count_lines(tc.output());
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits) + " hits";
        }
        return detail;
    }
    if (n == "list_dir") {
        std::string detail = path_pp.empty() ? std::string{"."} : path_pp;
        if (tc.is_done()) {
            int entries = count_lines(tc.output());
            if (entries > 0) detail += "  \xc2\xb7  " + std::to_string(entries) + " entries";
        }
        return detail;
    }
    if (n == "find_definition") return safe("symbol");
    if (n == "web_fetch")       return safe("url");
    if (n == "web_search")      return safe("query");
    if (n == "git_commit") {
        auto msg = safe("message");
        if (auto nl = msg.find('\n'); nl != std::string::npos)
            msg = msg.substr(0, nl);
        return msg;
    }
    if (n == "git_diff" || n == "git_log" || n == "git_status")
        return path_pp.empty() ? std::string{"."} : path_pp;
    if (n == "todo") {
        if (tc.args.is_object()) {
            auto it = tc.args.find("todos");
            if (it != tc.args.end() && it->is_array())
                return std::to_string(it->size()) + " items";
        }
        return "\xe2\x80\xa6";
    }
    return safe_arg(tc.args, "display_description");
}

// Map a ToolUse status to maya's TaskStatus. Failed/Rejected fold into
// Completed (it IS terminal); the failure is surfaced via the detail
// line so the timeline still reads as a clean sequence.
TaskStatus tool_task_status(const ToolUse& tc) {
    if (tc.is_pending() || tc.is_approved()) return TaskStatus::Pending;
    if (tc.is_running())                     return TaskStatus::InProgress;
    return TaskStatus::Completed; // Done / Failed / Rejected
}

// Pretty title-case for the tool name shown as the timeline event label.
// Maps moha's lowercase canonical names to the brand TitleCase forms
// (matches the names users see in CC / Zed agent panel).
std::string tool_display_name(const std::string& n) {
    if (n == "read")            return "Read";
    if (n == "write")           return "Write";
    if (n == "edit")            return "Edit";
    if (n == "bash")            return "Bash";
    if (n == "grep")            return "Grep";
    if (n == "glob")            return "Glob";
    if (n == "list_dir")        return "List";
    if (n == "todo")            return "Todo";
    if (n == "web_fetch")       return "Fetch";
    if (n == "web_search")      return "Search";
    if (n == "find_definition") return "Definition";
    if (n == "diagnostics")     return "Diag";
    if (n == "git_status")      return "Git Status";
    if (n == "git_diff")        return "Git Diff";
    if (n == "git_log")         return "Git Log";
    if (n == "git_commit")      return "Git Commit";
    return n;
}

// Format a duration as ms/s/m+s — short, glanceable, no surprising
// precision changes across magnitudes.
std::string format_duration(float secs) {
    char buf[24];
    if      (secs < 1.0f)  std::snprintf(buf, sizeof(buf), "%.0fms", secs * 1000.0);
    else if (secs < 60.0f) std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
    else {
        int mins = static_cast<int>(secs) / 60;
        float s = secs - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(s));
    }
    return buf;
}

// Status icon for a tool event in the rich timeline. Spinner advances
// in sync with the activity-bar spinner via the shared frame index.
Element rich_status_icon(const ToolUse& tc, int frame) {
    if (tc.is_running() || tc.is_approved()) {
        // Same braille spinner pattern as maya::Timeline / Spinner<Dots>.
        static constexpr const char* frames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
            "\xe2\xa0\x87", "\xe2\xa0\x8f",
        };
        return text(frames[frame % 10], Style{}.with_fg(info).with_bold());
    }
    if (tc.is_done())     return text("\xe2\x9c\x93", fg_bold(success));   // ✓
    if (tc.is_failed())   return text("\xe2\x9c\x97", fg_bold(danger));    // ✗
    if (tc.is_rejected()) return text("\xe2\x8a\x98", fg_bold(warn));      // ⊘
    return text("\xe2\x97\x8b", fg_dim(muted));                            // ○
}

// Split a string into lines (without owning them). Used by the head+
// tail truncator so we can pick lines from front and back of the body.
std::vector<std::string_view> split_lines_view(const std::string& s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.emplace_back(s.data() + start, i - start);
            start = i + 1;
        }
    }
    if (start < s.size()) out.emplace_back(s.data() + start, s.size() - start);
    return out;
}

// First N lines of `s` joined back, with a `… N more lines` footer when
// there were more. Used for tool body previews so a 1000-line bash output
// doesn't blow up the timeline card.
std::pair<std::string,int> head_lines(const std::string& s, int max_lines) {
    int kept = 0;
    int total = 0;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            ++total;
            if (kept < max_lines) { ++kept; cut = i + 1; }
        }
    }
    if (!s.empty() && s.back() != '\n') {
        ++total;
        if (kept < max_lines) { ++kept; cut = s.size(); }
    }
    return {s.substr(0, cut), std::max(0, total - kept)};
}

// Smart head+tail elision: for content longer than `cap_lines`, show
// `head` lines from the start, an elision marker, and `tail` lines from
// the end. Reads like a `git diff` smart-context block — far more
// useful than just showing the first N and dropping the conclusion.
// Returns the stitched preview and the count of elided lines (0 when
// nothing was elided).
struct ElidedPreview {
    std::vector<std::string> lines;
    int elided = 0;
};

ElidedPreview head_tail_lines(const std::string& s, int head, int tail) {
    auto all = split_lines_view(s);
    int total = static_cast<int>(all.size());
    ElidedPreview out;
    int cap = head + tail;
    if (total <= cap) {
        out.lines.reserve(static_cast<std::size_t>(total));
        for (auto v : all) out.lines.emplace_back(v);
        return out;
    }
    out.lines.reserve(static_cast<std::size_t>(cap));
    for (int i = 0; i < head; ++i) out.lines.emplace_back(all[static_cast<std::size_t>(i)]);
    out.elided = total - head - tail;
    for (int i = total - tail; i < total; ++i)
        out.lines.emplace_back(all[static_cast<std::size_t>(i)]);
    return out;
}

// Render compact body content for a single tool event — placed under the
// timeline event's `│` connector. Tool-specific so each row carries
// real, glanceable information: a few lines of read content, the diff
// hunks for an edit, the head of bash output, etc. Empty Element when
// nothing useful exists yet (still streaming) — caller handles spacing.
Element compact_tool_body(const ToolUse& tc) {
    const auto& n = tc.name.value;
    constexpr int kMaxLines = 6;

    auto code_line = [](std::string_view ln, Style st) {
        return text(std::string{ln}, st);
    };

    // ── Edit: compact diff (one - / + pair per hunk, capped) ───────
    if (n == "edit" && tc.args.is_object()) {
        std::vector<Element> rows;
        auto rem = Style{}.with_fg(danger);
        auto add = Style{}.with_fg(success);
        auto rem_pre = Style{}.with_fg(danger).with_dim();
        auto add_pre = Style{}.with_fg(success).with_dim();

        auto push_hunk = [&](std::string_view old_text, std::string_view new_text) {
            // Show first line of each side; "…" suffix when multi-line.
            auto first_line = [](std::string_view s) {
                auto nl = s.find('\n');
                bool more = (nl != std::string_view::npos);
                std::string ln = std::string{nl == std::string_view::npos ? s : s.substr(0, nl)};
                if (more) ln += "  \xe2\x80\xa6";   // …
                return ln;
            };
            if (!old_text.empty()) rows.push_back(h(
                text("- ", rem_pre), code_line(first_line(old_text), rem)
            ).build());
            if (!new_text.empty()) rows.push_back(h(
                text("+ ", add_pre), code_line(first_line(new_text), add)
            ).build());
        };

        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            int shown = 0;
            for (const auto& e : *it) {
                if (shown >= 3) {
                    rows.push_back(text("\xe2\x80\xa6 " + std::to_string(static_cast<int>(it->size()) - shown)
                                        + " more edits", fg_dim(muted)));
                    break;
                }
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                push_hunk(ot, nt);
                ++shown;
            }
        } else {
            // Top-level legacy single-edit shape.
            auto ot = safe_arg(tc.args, "old_text"); if (ot.empty()) ot = safe_arg(tc.args, "old_string");
            auto nt = safe_arg(tc.args, "new_text"); if (nt.empty()) nt = safe_arg(tc.args, "new_string");
            if (!ot.empty() || !nt.empty()) push_hunk(ot, nt);
        }
        if (rows.empty()) return text("");
        return v(std::move(rows)).build();
    }

    // Render an elided head+tail preview as a vertical stack with a
    // dim "··· N hidden ···" centered marker. Reads like `git diff`'s
    // smart context: top of file, gap, bottom of file — far more
    // informative than only the first N lines.
    auto preview_block = [&](const std::string& body, Style line_style) -> Element {
        constexpr int kHead = 4;
        constexpr int kTail = 3;
        auto p = head_tail_lines(body, kHead, kTail);
        std::vector<Element> rows;
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (p.elided > 0 && i == kHead) {
                rows.push_back(text("\xc2\xb7 \xc2\xb7 \xc2\xb7  "
                                    + std::to_string(p.elided) + " hidden  \xc2\xb7 \xc2\xb7 \xc2\xb7",
                                    fg_dim(muted)));
            }
            rows.push_back(text(p.lines[static_cast<std::size_t>(i)], line_style));
        }
        return v(std::move(rows)).build();
    };

    // ── Write: head+tail of the streaming/written content ──────────
    if (n == "write") {
        std::string content = safe_arg(tc.args, "content");
        if (content.empty()) return text("");
        return preview_block(content, fg_dim(fg));
    }

    // ── Bash / diagnostics: head+tail of output ────────────────────
    if ((n == "bash" || n == "diagnostics") && tc.is_terminal()) {
        auto out = strip_bash_output_fence(tc.output());
        if (out.empty()) return text("");
        return preview_block(out, fg_dim(fg));
    }
    // Live bash progress (running stdout snapshot).
    if (n == "bash" && tc.is_running() && !tc.progress_text().empty()) {
        return preview_block(tc.progress_text(), fg_dim(fg));
    }

    // ── Read / list_dir / grep / glob: head+tail of output ─────────
    if ((n == "read" || n == "list_dir" || n == "grep" || n == "glob")
        && tc.is_done())
    {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        return preview_block(out, fg_dim(fg));
    }

    // ── Failure: surface the error message inline so it isn't hidden
    if (tc.is_failed() && !tc.output().empty()) {
        // Failures use the danger color so the error stands out, but
        // still through the elided preview path so a 200-line stderr
        // dump doesn't take over the panel.
        return preview_block(tc.output(),
                             Style{}.with_fg(danger));
    }

    (void)kMaxLines;
    return text("");
}

// Color-code a tool's wall-clock duration so the eye finds slow steps
// without parsing numbers. Green = snappy (<250ms), dim = normal, warn
// = slow (>2s), danger = stalling (>15s).
Color duration_color(float secs) {
    if (secs < 0.25f) return success;
    if (secs < 2.0f)  return Color::bright_black();
    if (secs < 15.0f) return warn;
    return danger;
}

// Pick the body-connector color for an event based on its status. The
// connector is the visual "thread" running down each event's body —
// coloring it by status reinforces the icon at the head of the event
// without adding more chrome.
Color event_connector_color(const ToolUse& tc) {
    if (tc.is_failed())                                return danger;
    if (tc.is_rejected())                              return warn;
    if (tc.is_running() || tc.is_approved())           return info;
    if (tc.is_done())                                  return Color::bright_black();
    return Color::bright_black();   // pending
}

// Tool category color — semantic grouping so the eye can scan a
// timeline and instantly see "this turn was mostly inspect + one
// modify". Five buckets, each with a distinct hue:
//
//   inspect (read, grep, glob, list, find, diag, web)  → info (blue)
//   mutate  (edit, write)                              → accent (magenta)
//   execute (bash)                                     → success (green)
//   plan    (todo)                                     → warn (yellow)
//   vcs     (git_*)                                    → highlight (cyan)
//
// Used for the gutter number and the tool name so a glance at the
// timeline shows the *kind* of work happening, not just the order.
Color tool_category_color(const std::string& n) {
    if (n == "edit" || n == "write")        return accent;
    if (n == "bash")                        return success;
    if (n == "todo")                        return warn;
    if (n.rfind("git_", 0) == 0)            return highlight;
    return info;  // read, grep, glob, list_dir, find_definition,
                  // diagnostics, web_fetch, web_search
}

// Short category label for the stats header.
std::string_view tool_category_label(const std::string& n) {
    if (n == "edit" || n == "write")        return "mutate";
    if (n == "bash")                        return "execute";
    if (n == "todo")                        return "plan";
    if (n.rfind("git_", 0) == 0)            return "vcs";
    return "inspect";
}

// Build the assistant turn's "Actions" panel: one bordered card whose
// body is a continuous timeline of tool events. Each event has a
// gutter-numbered header (like code line numbers), a status icon, the
// tool name + brief detail + duration, and an indented body where the
// rich tool-specific content sits under a status-colored connector.
// Overview + detail in one cohesive, scannable view.
Element assistant_timeline(const Message& msg, int spinner_frame,
                           Color rail_color) {
    std::vector<Element> rows;
    int total = static_cast<int>(msg.tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;
    // Per-category counts for the stats header. Order kept stable so
    // the badges always appear in the same relative position.
    std::vector<std::pair<std::string, int>> cat_counts;
    auto bump_cat = [&](const std::string& cat) {
        for (auto& [k, n] : cat_counts) if (k == cat) { ++n; return; }
        cat_counts.emplace_back(cat, 1);
    };
    for (std::size_t i = 0; i < msg.tool_calls.size(); ++i) {
        const auto& tc = msg.tool_calls[i];
        if (tc.is_terminal()) {
            ++done;
            total_elapsed += tool_elapsed(tc);
        }
        if (running_idx < 0 && (tc.is_running() || tc.is_approved()))
            running_idx = static_cast<int>(i);
        bump_cat(std::string{tool_category_label(tc.name.value)});
    }

    // ── Stats header ───────────────────────────────────────────────
    // Quick TL;DR of the turn: small badges showing the category mix
    // (e.g. "inspect 3  ·  mutate 2  ·  execute 1"). Shown once at
    // the top of the panel so the eye can read "what kind of work
    // happened here" without scanning the events.
    if (total > 1) {
        std::vector<Element> stats;
        bool first = true;
        for (const auto& [cat, n] : cat_counts) {
            if (!first) stats.push_back(text("  \xc2\xb7  ", fg_dim(muted)));
            first = false;
            // Pick a color from a representative tool name in this
            // category — same map as tool_category_color so the badge
            // and the per-event gutter agree.
            Color cc = (cat == "mutate")  ? accent
                     : (cat == "execute") ? success
                     : (cat == "plan")    ? warn
                     : (cat == "vcs")     ? highlight
                                          : info;
            stats.push_back(text(cat, Style{}.with_fg(cc).with_bold()));
            stats.push_back(text(" " + std::to_string(n), fg_dim(muted)));
        }
        rows.push_back((h(std::move(stats)) | grow(1.0f)).build());
        rows.push_back(text(""));
    }

    // Two-digit zero-padded gutter numbers for ≤99 events, plain for
    // larger. Same convention code editors use.
    auto gutter = [&](std::size_t idx) {
        char buf[8];
        if (total <= 99) std::snprintf(buf, sizeof(buf), "%02zu", idx + 1);
        else             std::snprintf(buf, sizeof(buf), "%zu",   idx + 1);
        return std::string{buf};
    };

    for (std::size_t i = 0; i < msg.tool_calls.size(); ++i) {
        const auto& tc = msg.tool_calls[i];
        bool is_last   = (i + 1 == msg.tool_calls.size());
        bool is_active = tc.is_running() || tc.is_approved();

        // ── Header row ──────────────────────────────────────────────
        // Layout: `01  ▸ ✓ Read   src/auth.ts · 234 lines        879ms`
        //          ^^^ ^ ^^^ ^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        //          gut  m icon name + detail              spacer  dur
        Element marker = is_active
            ? text("\xe2\x96\xb8", Style{}.with_fg(info).with_bold())   // ▸
            : text(" ", {});

        Element icon = rich_status_icon(tc, spinner_frame);
        std::string name = tool_display_name(tc.name.value);
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty())
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};

        Color cat = tool_category_color(tc.name.value);
        // Name styling: failed/rejected stay in their status colors so
        // the eye catches them. Otherwise color by tool *category* so
        // a glance at the timeline reads "inspect inspect mutate
        // execute" by hue. Active tools get bold + bright; settled
        // tools stay dim so the running step pops.
        Style name_style;
        if      (tc.is_failed())   name_style = Style{}.with_fg(danger).with_bold();
        else if (tc.is_rejected()) name_style = Style{}.with_fg(warn).with_bold();
        else if (is_active)        name_style = Style{}.with_fg(cat).with_bold();
        else if (tc.is_done())     name_style = Style{}.with_fg(cat).with_dim();
        else                        name_style = Style{}.with_fg(cat).with_dim();

        std::vector<Element> hdr;
        // Gutter takes the category color (dimmed) so the leading
        // column reads as a vertical color stripe matching the work
        // category at each step.
        hdr.push_back(text(gutter(i), Style{}.with_fg(cat).with_dim()));
        hdr.push_back(text("  ", {}));
        hdr.push_back(marker);
        hdr.push_back(text(" ", {}));
        hdr.push_back(icon);
        hdr.push_back(text("  ", {}));
        hdr.push_back(text(std::move(name), name_style));
        hdr.push_back(text("  ", {}));
        hdr.push_back(text(std::move(detail), fg_dim(muted)));
        hdr.push_back(spacer());
        if (tc.is_terminal()) {
            float secs = tool_elapsed(tc);
            hdr.push_back(text(format_duration(secs),
                               Style{}.with_fg(duration_color(secs))));
        }
        rows.push_back((h(std::move(hdr)) | grow(1.0f)).build());

        // ── Body content under a status-colored ┊ connector ────────
        // Body sits aligned beneath the tool name (column = gutter
        // width 2 + 4 spaces). The connector is colored by the event's
        // status — green ┊ under a done tool, blue ┊ under a running
        // one, red under a failed one. Reinforces the status icon
        // without adding more text labels.
        Color cc = event_connector_color(tc);
        auto body_rule = h(
            text("    ", {}),                                        // gutter alignment
            text("\xe2\x94\x8a  ", Style{}.with_fg(cc).with_dim())   // ┊
        ).build();

        Element body_el = compact_tool_body(tc);
        bool body_has_content = false;
        if (auto* bx = maya::as_box(body_el)) {
            for (const auto& child : bx->children) {
                rows.push_back(h(body_rule, child).build());
                body_has_content = true;
            }
        } else if (auto* t = maya::as_text(body_el)) {
            if (!t->content.empty()) {
                rows.push_back(h(body_rule, body_el).build());
                body_has_content = true;
            }
        }

        // ── Continuation between events ────────────────────────────
        // A short colored connector below the body keeps the visual
        // thread running into the next event. Cleaner than a blank
        // row + much cleaner than the previous full-width ┈ rule.
        if (!is_last) {
            Color next_cc = event_connector_color(msg.tool_calls[i + 1]);
            rows.push_back(h(
                text("    ", {}),
                text("\xe2\x94\x8a", Style{}.with_fg(next_cc).with_dim())  // ┊
            ).build());
        }
        (void)body_has_content;
    }

    // ── Footer summary when settled ────────────────────────────────
    // Once all tools are terminal, append a one-line footer with
    // aggregate stats: "✓ done · 5 actions · 1.8s elapsed". Pinned
    // bottom so it reads as the closing summary of the panel.
    if (done == total && total > 0) {
        std::string verb = "\xe2\x9c\x93 done";    // ✓ done
        // If anything failed, lead with that count instead.
        int failed = 0, rejected = 0;
        for (const auto& tc : msg.tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        Color verb_color = success;
        if (failed > 0) {
            verb = "\xe2\x9c\x97 " + std::to_string(failed) + " failed";
            verb_color = danger;
        } else if (rejected > 0) {
            verb = "\xe2\x8a\x98 " + std::to_string(rejected) + " rejected";
            verb_color = warn;
        }

        rows.push_back(text(""));
        rows.push_back(h(
            text("    ", {}),
            text(verb, Style{}.with_fg(verb_color).with_bold()),
            text("  \xc2\xb7  ", fg_dim(muted)),
            text(std::to_string(total) + " actions", fg_dim(muted)),
            text("  \xc2\xb7  ", fg_dim(muted)),
            text(format_duration(total_elapsed) + " elapsed", fg_dim(muted))
        ).build());
    }

    // ── Card title: progress + active step name + elapsed ─────────
    std::string title = " Actions  \xc2\xb7  "
                      + std::to_string(done) + "/"
                      + std::to_string(total);
    if (running_idx >= 0) {
        title += "  \xc2\xb7  " + tool_display_name(
            msg.tool_calls[static_cast<std::size_t>(running_idx)].name.value);
    } else if (done == total && total > 0) {
        title += "  \xc2\xb7  " + format_duration(total_elapsed);
    }
    title += " ";

    return (v(std::move(rows))
            | border(BorderStyle::Round)
            | bcolor(rail_color)
            | btext(std::move(title), BorderTextPos::Top, BorderTextAlign::Start)
            | padding(0, 1, 0, 1)
           ).build();
}

Element render_message(const Message& msg, int turn_num, const Model& m) {
    std::vector<Element> rows;

    // Compute elapsed wall-clock for assistant turns: from the previous
    // user message's timestamp to this one. Skipped for the first turn
    // (nothing to compare against).
    std::optional<float> assistant_elapsed;
    if (msg.role == Role::Assistant) {
        // Walk back to the most recent user message timestamp.
        for (std::size_t i = m.current.messages.size(); i-- > 0;) {
            if (&m.current.messages[i] == &msg) continue;
            if (m.current.messages[i].role == Role::User) {
                auto dt = std::chrono::duration<float>(
                    msg.timestamp - m.current.messages[i].timestamp).count();
                if (dt > 0.0f && dt < 3600.0f) assistant_elapsed = dt;
                break;
            }
        }
    }

    // Build the turn body (header + content) without the rail; the
    // rail is applied as a left border to the entire stack so it runs
    // continuously top-to-bottom regardless of how many sub-rows the
    // turn produces.
    std::vector<Element> body;
    Color rail_color;

    if (msg.role == Role::User) {
        if (msg.checkpoint_id) rows.push_back(render_checkpoint_divider());
        rail_color = speaker_style_for(Role::User, m).color;
        body.push_back(turn_header(Role::User, turn_num, msg, m, std::nullopt));
        body.push_back(text(""));
        body.push_back(user_message_body(msg.text));
    } else if (msg.role == Role::Assistant) {
        rail_color = speaker_style_for(Role::Assistant, m).color;
        body.push_back(turn_header(Role::Assistant, turn_num, msg, m,
                                   assistant_elapsed));
        body.push_back(text(""));
        bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            body.push_back(cached_markdown_for(msg));
            if (!msg.tool_calls.empty()) body.push_back(text(""));
        }

        // Timeline view ALWAYS — both during the response and after it
        // settles. The Timeline is the higher-level "Actions" view: a
        // clean CI-pipeline-style log of what the assistant did, with
        // post-completion stats folded into each event's detail line
        // (line counts, hunk Δ, exit codes, match counts). Avoids the
        // wall of giant detailed cards that buried the conversation
        // every time the assistant ran a few tools.
        if (!msg.tool_calls.empty()) {
            int frame = m.stream.spinner.frame_index();
            body.push_back(assistant_timeline(msg, frame, rail_color));
            // Render any in-flight permission inline so the user can
            // approve without losing the timeline context above.
            for (const auto& tc : msg.tool_calls) {
                if (m.pending_permission && m.pending_permission->id == tc.id) {
                    body.push_back(text(""));
                    body.push_back(render_inline_permission(*m.pending_permission, tc));
                }
            }
        }
    }

    rows.push_back(with_turn_rail((v(std::move(body)) | grow(1.0f)).build(),
                                  rail_color));
    // Bottom breathing — a short blank then the next turn's divider.
    rows.push_back(text(""));
    return v(std::move(rows)).build();
}

Element thread_panel(const Model& m) {
    std::vector<Element> rows;
    // Virtualize: older messages live in the terminal's native scrollback
    // (their rows were committed via maya::Cmd::commit_scrollback).  We
    // preserve absolute turn numbering by counting finalized assistant
    // messages *before* the view window too, so a user seeing "Turn 42"
    // after scrolling back stays consistent.
    const std::size_t total = m.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1;
    for (std::size_t i = 0; i < start; ++i)
        if (m.current.messages[i].role == Role::Assistant) ++turn;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.current.messages[i];
        // Inter-turn divider: thin dim rule between consecutive
        // user/assistant messages. Skip before the first message in
        // the visible window (no prior turn to divide from).
        if (i > start) rows.push_back(inter_turn_divider());
        rows.push_back(render_message(msg, turn, m));
        if (msg.role == Role::Assistant) ++turn;
    }
    if (m.stream.active && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        // Suppress this bottom indicator when the active assistant turn
        // is already showing its Timeline card — the timeline's own
        // in-progress spinner + status bar's spinner together carry the
        // "still working" signal; an extra spinner here was duplicate
        // chrome stacked under the card.
        const auto& last = m.current.messages.back();
        bool tl_visible_above =
            !last.tool_calls.empty()
            && std::any_of(last.tool_calls.begin(), last.tool_calls.end(),
                           [](const auto& tc){ return !tc.is_terminal(); });
        if (!tl_visible_above) {
            // Match the assistant turn header's left-edge bar so the
            // spinner reads as "still typing" inline with the message
            // above, not as a detached notification floating at the bottom.
            const auto& mid = m.model_id.value;
            Color edge_color = (mid.find("opus")   != std::string::npos) ? accent
                             : (mid.find("sonnet") != std::string::npos) ? info
                             : (mid.find("haiku")  != std::string::npos) ? success
                                                                         : highlight;
            auto spin = m.stream.spinner;
            spin.set_style(Style{}.with_fg(edge_color).with_bold());
            std::string verb{phase_verb(m.stream.phase)};
            rows.push_back((h(
                text("\xe2\x96\x8e", fg_of(edge_color)),                // ▎
                text(" ", {}),
                spin.build(),
                text(" " + verb + "\u2026", fg_italic(muted))
            ) | padding(0, 0, 0, 1)).build());
        }
    }
    if (rows.empty()) {
        // Wordmark-style welcome — quiet brand presence + the details that
        // orient the user (which model, which profile, what to do next).
        // A blank thread is the loneliest screen in the app; give it a
        // focal point with real visual weight.
        //
        // The wordmark is built from box-drawing characters so it scales
        // with the user's font and renders in any UTF-8 terminal — no
        // image bitmap, no font dependency, no broken glyph fallbacks.

        auto centered_text = [](std::string s, Style st) {
            return h(spacer(), text(std::move(s), st), spacer()).build();
        };

        // ── Wordmark: m o h a ────────────────────────────────────────
        // 3 rows × ~26 cells. Spacing between letters chosen so each glyph
        // reads as a discrete shape; using outline (┌┐└┘) instead of solid
        // blocks keeps it elegant rather than chunky.
        auto mark_style = fg_bold(accent);
        std::vector<Element> mark_rows;
        mark_rows.push_back(centered_text(
            "\u250c\u252c\u2510\u250c\u2500\u2510\u252c\u0020\u252c\u250c\u2500\u2510",
            mark_style));  // ┌┬┐┌─┐┬ ┬┌─┐
        mark_rows.push_back(centered_text(
            "\u2502\u2502\u2502\u2502\u0020\u2502\u251c\u2500\u2524\u251c\u2500\u2524",
            mark_style));  // │││││ │├─┤├─┤
        mark_rows.push_back(centered_text(
            "\u2534\u0020\u2534\u2514\u2500\u2518\u2534\u0020\u2534\u2534\u0020\u2534",
            fg_dim(accent)));  // ┴ ┴└─┘┴ ┴┴ ┴

        // ── Tagline ──────────────────────────────────────────────────
        auto tagline = centered_text(
            "a calm middleware between you and the model",
            fg_italic(muted));

        // ── Model + profile chip row ─────────────────────────────────
        ModelBadge mb;
        mb.set_model(m.model_id.value);
        mb.set_compact(true);
        auto profile_color_v = profile_color(m.profile);
        auto profile_chip = h(
            text("\u258c", fg_of(profile_color_v)),                  // ▌
            text(" " + std::string{profile_label(m.profile)} + " ",
                 Style{}.with_fg(profile_color_v).with_inverse().with_bold()),
            text("\u2590", fg_of(profile_color_v))                   // ▐
        ).build();
        auto chips_row = h(
            spacer(),
            mb.build(),
            text("    ", {}),
            std::move(profile_chip),
            spacer()
        ).build();

        // ── Starter prompts ──────────────────────────────────────────
        // Three example asks framed as a quiet bordered card so the user
        // sees concrete affordances, not "type something". Each is dim so
        // the eye doesn't read them as already-typed input.
        auto starter = [](std::string text_) {
            return h(
                text("\u2022 ", fg_dim(accent)),                      // •
                text(std::move(text_), fg_dim(fg))
            ).build();
        };
        auto starters_card = (v(
            text(" Try ", fg_bold(muted)),
            text("", {}),
            starter("Implement a small feature"),
            starter("Refactor or clean up this file"),
            starter("Explain what this code does"),
            starter("Write tests for ...")
        ) | padding(0, 2, 0, 2)
          | border(BorderStyle::Round)
          | bcolor(muted)
        ).build();

        auto starters_row = h(spacer(), starters_card, spacer()).build();

        // ── Bottom hint row ──────────────────────────────────────────
        auto hint = h(spacer(),
            text("type to begin  \u00B7  ", fg_dim(muted)),
            text("^K", fg_bold(highlight)),
            text(" palette  \u00B7  ", fg_dim(muted)),
            text("^J", fg_bold(highlight)),
            text(" threads  \u00B7  ", fg_dim(muted)),
            text("^N", fg_bold(success)),
            text(" new", fg_dim(muted)),
            spacer()).build();

        rows.push_back((v(
            text(""), text(""),
            mark_rows[0], mark_rows[1], mark_rows[2],
            text(""),
            tagline,
            text(""), text(""),
            chips_row,
            text(""), text(""),
            starters_row,
            text(""), text(""),
            hint
        )).build());
    }
    return (v(std::move(rows)) | padding(0, 1) | grow(1.0f)).build();
}

} // namespace moha::ui
