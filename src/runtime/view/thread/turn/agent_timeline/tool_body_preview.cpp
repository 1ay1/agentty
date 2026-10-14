// tool_body_preview_config — the ToolUse → ToolBodyPreview::Config
// adapter. Structured tool names dispatch to purpose-built renderers; one
// lifecycle fallback then guarantees a useful body for every state, including
// native tools without a custom renderer and dynamically discovered MCP tools.
// Shared parsing/windowing helpers live in tool_body_common.{hpp,cpp}.
//
// Dispatch order remains load-bearing: failed todo calls bypass the checkbox
// renderer so their error is shown, while edit/write/bash/task retain their
// specialized output whenever it is meaningful.

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include <maya/runtime.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "tool_body_common.hpp"

#include <cctype>
#include <functional>

#include <nlohmann/json.hpp>

#include "agentty/tool/util/utf8.hpp"

namespace agentty::ui {

// ── Frozen-build scope ─────────────────────────────────────
// Retained as a no-op-by-default phase flag for callers that still ask
// `building_frozen()`. The body is now IDENTICAL in both phases (full,
// seam-safe), so no renderer path branches on it for content; freeze_range
// still scopes it for clarity and so a future divergent-build path has a
// hook.
namespace {
// The render-phase flag is LOOP-ONLY: FrozenBuildScope is taken and released
// inside one view() build, and view() runs on the kernel's loop thread.
//
// It was a thread_local, which enforced nothing — a worker asking
// building_frozen() would have read its own untouched `false` and silently
// taken the wrong branch. loop_bound<bool> makes that an abort naming the
// rule instead of a quietly wrong render. See jaal/kernel/loop.hpp.
maya::loop_bound<bool> g_frozen_build;

bool frozen_build_get() noexcept {
    return g_frozen_build.with([](const bool& v) { return v; });
}
void frozen_build_set(bool v) noexcept {
    g_frozen_build.with([v](bool& slot) { slot = v; });
}

bool text_is_visible(std::string_view text) {
    const auto stripped = tools::util::strip_terminal_controls(text);
    for (unsigned char c : stripped) {
        if (!std::isspace(c) && c >= 0x20) return true;
    }
    return false;
}

bool has_visible_body(const maya::ToolBodyPreview::Config& body) {
    using Kind = maya::ToolBodyPreview::Kind;
    switch (body.kind) {
    case Kind::None:      return false;
    case Kind::EditDiff:  return !body.hunks.empty();
    case Kind::TodoList:  return !body.todos.empty();
    // Whitespace is meaningful in file bodies because their gutter/prefix and
    // footer make it visible (an empty-line-only file is still real output).
    case Kind::FileRead:
    case Kind::FileWrite: return !body.text.empty();
    default:              return text_is_visible(body.text);
    }
}

// A JSON tool result as readable lines: objects become "key: value" with
// nested values indented, arrays of scalars join on one line, arrays of
// objects become one entry each. "" when the text isn't a JSON object or
// array (or is fenced/prefixed: then it's shown as-is). Bounded output.
std::string json_as_lines(std::string_view text) {
    auto a = text.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos || (text[a] != '{' && text[a] != '[')) return {};
    auto j = nlohmann::json::parse(text.substr(a), nullptr, false);
    if (j.is_discarded() || (!j.is_object() && !j.is_array())) return {};
    std::string out;
    constexpr std::size_t kMaxOut = 16 * 1024;
    auto scalar = [](const nlohmann::json& v) -> std::string {
        if (v.is_string()) return v.get<std::string>();
        if (v.is_null()) return "\xe2\x80\x94";
        return v.dump();
    };
    auto all_scalar = [](const nlohmann::json& arr) {
        for (auto& e : arr)
            if (e.is_object() || e.is_array()) return false;
        return true;
    };
    std::function<void(const nlohmann::json&, int)> walk = [&](const nlohmann::json& v, int depth) {
        if (out.size() > kMaxOut) return;
        std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
        if (v.is_object()) {
            for (auto& [k, x] : v.items()) {
                if (out.size() > kMaxOut) return;
                if (x.is_object() && !x.empty()) {
                    out += pad + k + ":\n";
                    walk(x, depth + 1);
                } else if (x.is_array() && !x.empty() && !all_scalar(x)) {
                    out += pad + k + ": (" + std::to_string(x.size()) + ")\n";
                    walk(x, depth + 1);
                } else if (x.is_array()) {
                    std::string s;
                    for (auto& e : x) s += (s.empty() ? "" : ", ") + scalar(e);
                    out += pad + k + ": " + (s.empty() ? "(none)" : s) + "\n";
                } else if (x.is_object()) {
                    out += pad + k + ": {}\n";
                } else {
                    auto s = scalar(x);
                    // multi-line strings keep their lines, indented under the key
                    if (s.find('\n') != std::string::npos) {
                        out += pad + k + ":\n";
                        std::size_t p = 0;
                        while (p <= s.size() && out.size() <= kMaxOut) {
                            auto e = s.find('\n', p);
                            out += pad + "  " + s.substr(p, e == std::string::npos ? std::string::npos : e - p) + "\n";
                            if (e == std::string::npos) break;
                            p = e + 1;
                        }
                    } else {
                        out += pad + k + ": " + s + "\n";
                    }
                }
            }
        } else if (v.is_array()) {
            if (v.empty()) { out += pad + "(none)\n"; return; }
            if (all_scalar(v)) {
                for (auto& e : v) out += pad + "\xe2\x80\xa2 " + scalar(e) + "\n";
                return;
            }
            for (auto& e : v) {
                if (out.size() > kMaxOut) return;
                if (e.is_object()) {
                    // one entry per element: its fields on one line when short
                    std::string line;
                    bool flat = true;
                    for (auto& [k, x] : e.items()) {
                        if (x.is_object() || (x.is_array() && !all_scalar(x))) { flat = false; break; }
                        std::string s = x.is_array() ? std::to_string(x.size()) + " items" : scalar(x);
                        if (s.find('\n') != std::string::npos) { flat = false; break; }
                        line += (line.empty() ? "" : " \xc2\xb7 ") + k + ": " + s;
                    }
                    if (flat && line.size() <= 160) {
                        out += pad + "\xe2\x80\xa2 " + line + "\n";
                    } else {
                        out += pad + "\xe2\x80\xa2\n";
                        walk(e, depth + 1);
                    }
                } else {
                    walk(e, depth + 1);
                }
            }
        }
    };
    walk(j, 0);
    if (out.size() > kMaxOut) out += "\xe2\x80\xa6\n";
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

bool supports_generic_text_body(std::string_view name) {
    // Native and MCP registry names are canonical lowercase/snake identifiers.
    // Provider-specific legacy labels (for example ACP's historical "Bash")
    // were always header-only; retroactively giving them progress/output rows
    // changes committed card geometry in old threads and parallel-tool turns.
    for (unsigned char c : name)
        if (c >= 'A' && c <= 'Z') return false;
    return true;
}

// No tool card should ever be an unexplained blank. Structured renderers get
// first refusal; this lifecycle fallback covers every native tool as well as
// dynamically discovered MCP tools, including tools added after this code was
// compiled. Live progress wins, terminal output is preserved, and genuinely
// silent states receive a short explicit explanation.
void generic_lifecycle_body(const ToolUse& tc,
                            maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (has_visible_body(out)) return;

    const bool generic_text = supports_generic_text_body(tc.name.value);
    out.text_color = text_tertiary;
    if (tc.is_failed()) {
        if (text_is_visible(tc.output())) {
            out.kind = Kind::Failure;
            out.text = tc.output();
            out.chrome_color = status_error;
        }
        return;
    }
    if (tc.is_running()) {
        if (!generic_text) return;
        // Real progress earns a body; a synthetic "waiting" row does not.
        // Tool insertion can happen after a long assistant prefix has entered
        // native scrollback, and adding even one placeholder row there shifts
        // committed content before any settle-reconciliation window exists.
        // The event header already shows spinner + argument detail for silent
        // tools, so it remains useful without destabilising frame geometry.
        if (text_is_visible(tc.progress_text())) {
            out.kind = Kind::CodeBlock;
            out.text = tc.progress_text();
            out.is_streaming = true;
        }
        return;
    }
    if (tc.is_pending() || tc.is_approved()) {
        // Input streaming / permission state is fully described by the event
        // header. Keep the body at zero rows until structured args or genuine
        // progress arrive, for the same committed-scrollback invariant above.
        return;
    }
    if (generic_text && tc.is_done() && text_is_visible(tc.output())) {
        out.kind = Kind::CodeBlock;
        // MCP tools answer in JSON; the card shows it as "key: value" lines
        // (what Claude Code and Zed do with structured results) rather than
        // braces and quotes. The model still gets the exact JSON.
        auto readable = json_as_lines(tc.output());
        out.text = readable.empty() ? tc.output() : std::move(readable);
        // Settled output for the tools that land here (repo_map, outline,
        // read_filter, search_*, extract, json_query, MCP tools…) is
        // HEAD-heavy: the answer is at the top, the tail is pagination /
        // truncation chatter. tail_only stays true only for LIVE progress
        // above (newest lines pinned bottom, terminal-like).
        out.tail_only = false;
    }
    // Empty success/failure/rejection is already explicit in the event header.
    // Inventing a body row only at settle would shift committed transcript
    // rows; the full-output overlay supplies a safe "no output" explanation.
    return;
}
} // namespace

FrozenBuildScope::FrozenBuildScope() noexcept
    : prev_(frozen_build_get()) { frozen_build_set(true); }
FrozenBuildScope::~FrozenBuildScope() { frozen_build_set(prev_); }
bool building_frozen() noexcept { return frozen_build_get(); }

GrepHits collect_grep_hits(std::span<const ToolUse> tool_calls) {
    GrepHits out;
    for (const auto& tc : tool_calls) {
        if (tc.name.value != "grep") continue;
        const auto& body = tc.output();
        if (body.empty()) continue;
        detail::accumulate_grep_hits(body, out);
    }
    return out;
}

maya::ToolBodyPreview::Config tool_body_preview_config(
    const ToolUse& tc, const GrepHits* grep_hits)
{
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // Chrome (line-number gutter, pipe separator, elision marker) reads
    // in the tool's category color so the body's structure visually
    // matches the header NAME color. Body content itself stays
    // text_tertiary (dim) so the colored chrome frames it without
    // competing with prose. Each per-tool renderer may override
    // chrome_color (e.g. the Failure paths flip it to red).
    out.chrome_color = tool_category_color(n);

    // ── Structured renderers get first refusal. Every return passes through
    //    the generic lifecycle fallback so even silent/empty tools explain
    //    what they are waiting for or how they settled. ───────────────────
    if (n == "edit" || n == "apply_patch") {
        // apply_patch emits the SAME diff-fenced output shape as edit (both
        // route through diff::render_unified); the fence-parse path gives it
        // the colored −/+ diff card instead of a dim generic code block.
        detail::edit_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "shell" || n == "diagnostics" || n == "test"
        || n == "process_start" || n == "process_poll"
        || n == "process_stop") {
        // The process_* trio IS terminal output — tail-heavy, ANSI-laden,
        // same reading question as bash ("what did it print / did it
        // fail?"), so it shares BashOutput's structured-extraction chain
        // (test summaries, compiler diagnostics) and tail window.
        detail::bash_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "write") {
        detail::write_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }

    if (n == "git_diff") {
        if (detail::git_diff_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "read" || n == "find_definition") {
        if (detail::read_body(tc, grep_hits, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "web_fetch") {
        if (detail::web_fetch_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "grep") {
        if (detail::grep_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "replace" || n == "rewrite_structural") {
        if (detail::diff_preview_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "glob" || n == "list_dir"
        || n == "web_search"
        || n == "git_status" || n == "git_log" || n == "git_commit") {
        if (detail::generic_list_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }

    if (n == "task") {
        detail::task_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }

    // Preserve the deliberate ordering: a failed todo displays the error,
    // not its stale checkbox list. All other todo states keep the structured
    // list when one is available.
    if (tc.is_failed()) {
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "todo" && detail::todo_body(tc, out)) {
        generic_lifecycle_body(tc, out);
        return out;
    }

    generic_lifecycle_body(tc, out);
    return out;
}

} // namespace agentty::ui
