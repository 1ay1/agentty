// agentty::acp::AgentServer — implementation. See server.hpp for the design.
//
// Headless analogue of the TUI turn loop (src/runtime/app/cmd_factory.cpp).
// Reuses the SAME provider, tools, wire shaping, and permission policy, driven
// synchronously on a worker thread and sent to the client as session/update
// notifications over an rpc::Peer.

#include "agentty/acp/server.hpp"
#include "agentty/util/digits.hpp"

#include "agentty/util/base64.hpp"
#include "agentty/util/logx.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <sstream>
#include <fstream>
#include <stop_token>

#include <maya/runtime.hpp>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentty/diff/diff.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/io/shared_file.hpp"
#include "agentty/provider/prompt_policy.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/catalog_sources.hpp"
#include "agentty/provider/error_class.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/policy.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/runtime/app/update/stream_args.hpp"  // canonify_tool_args (`cmd`→`command`)
#include "agentty/runtime/app/cmd_factory.hpp"        // wire_messages_for, compaction prompts
#include "agentty/tool/commands.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/runtime/slash.hpp"   // one resolver for /name
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/util/background.hpp"
#include "agentty/util/self_exe.hpp"
#if AGENTTY_MCP
#include "agentty/mcp/client.hpp"
#endif

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::acp {
namespace {

using nlohmann::json;
namespace a = ::acp;

// A call to the client that throws its RpcError, for the try/catch paths.
template <jsonrpc::IsMethod M>
typename M::result call(const maya::unlocked& cap, rpc::Peer& peer, const typename M::params& p) {
    auto r = peer.call<M>(cap, p);
    if (!r) throw r.error();
    return std::move(*r);
}

// The editor's additionalDirectories, checked: each must be absolute and an
// existing directory (the spec requires absolute; a missing one is an error
// the user should see, not a root that silently grants nothing).
std::vector<std::string> extra_roots_of(const a::Maybe<a::List<std::string>>& dirs) {
    std::vector<std::string> out;
    if (!dirs) return out;
    for (const auto& d : *dirs) {
        std::filesystem::path p{d};
        std::error_code ec;
        if (!p.is_absolute())
            throw a::RpcError(a::errc::InvalidParams,
                              "additionalDirectories: '" + d + "' is not absolute");
        if (!std::filesystem::is_directory(p, ec))
            throw a::RpcError(a::errc::InvalidParams,
                              "additionalDirectories: '" + d + "' is not a directory");
        out.push_back(std::filesystem::weakly_canonical(p, ec).string());
    }
    return out;
}

// Unix seconds as an ISO 8601 UTC timestamp, the format SessionInfo.updatedAt wants.
// Not std::format("{:%FT%TZ}"): chrono formatting pulls libstdc++'s locale
// encoding conversion, which needs iconv, and the static-libstdc++ macOS
// release link has no iconv (libSystem doesn't export it) -- it broke the
// 0.9.20 mac builds. gmtime + strftime is locale-free and identical here.
std::string iso8601(std::int64_t unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
#if defined(_WIN32)
    if (::gmtime_s(&tm, &t) != 0) return {};
#else
    if (!::gmtime_r(&t, &tm)) return {};
#endif
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf, n);
}

// One exact git-style patch for a change, for JetBrains AIR's diffPatch.
// nullopt when it can't be exact (a CR byte, no hunks), so the caller sends
// the plain diff instead, as the AIR contract asks.
std::optional<std::string> air_git_patch(const FileChange& ch) {
    if (ch.hunks.empty()) return std::nullopt;
    if (ch.original_contents.find('\r') != std::string::npos ||
        ch.new_contents.find('\r') != std::string::npos)
        return std::nullopt;
    // Path: no leading slash, forward slashes, git's default quoting.
    std::string rel = ch.path;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    if (rel.empty()) return std::nullopt;
    const auto quote = [](const std::string& prefix, const std::string& p) {
        bool needs = false;
        for (unsigned char c : p)
            if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) { needs = true; break; }
        const std::string full = prefix + p;
        if (!needs) return full;
        std::string q = "\"";
        for (unsigned char c : full) {
            if (c == '"' || c == '\\') { q += '\\'; q += static_cast<char>(c); }
            else if (c == '\n') q += "\\n";
            else if (c == '\t') q += "\\t";
            else if (c < 0x20 || c >= 0x7f) {
                char buf[5]; std::snprintf(buf, sizeof buf, "\\%03o", c); q += buf;
            } else q += static_cast<char>(c);
        }
        return q + "\"";
    };
    const bool added = ch.original_contents.empty();
    const std::string a = quote("a/", rel), b = quote("b/", rel);
    const std::string tab = rel.find(' ') != std::string::npos ? "\t" : "";
    std::string out = "diff --git " + a + " " + b + "\n";
    if (added) out += "new file mode 100644\n";
    out += added ? "--- /dev/null\n" : "--- " + a + tab + "\n";
    out += "+++ " + b + tab + "\n";
    const bool old_nl = ch.original_contents.empty() || ch.original_contents.back() == '\n';
    const bool new_nl = ch.new_contents.empty() || ch.new_contents.back() == '\n';
    // A missing final newline needs a "\ No newline" marker placed exactly;
    // rather than risk a wrong patch, send the plain diff (AIR's fallback).
    if (!old_nl || !new_nl) return std::nullopt;
    // Git-exact lines: content ends in '\n', so drop the empty piece after
    // it (the display diff keeps it as a phantom line).
    auto lines = [](const std::string& s) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos < s.size()) {
            const auto nl = s.find('\n', pos);
            out.push_back(s.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return out;
    };
    const auto fc = diff::compute(rel, ch.original_contents, ch.new_contents);
    const auto a_lines = lines(ch.original_contents);
    const auto b_lines = lines(ch.new_contents);
    for (const auto& h : fc.hunks) {
        // Rebuild each hunk's body from its ranges, clamped to real lines.
        std::string body;
        int old_len = 0, new_len = 0;
        std::istringstream in(h.patch);
        std::string ln;
        int ai = h.old_start - 1, bi = h.new_start - 1;
        while (std::getline(in, ln)) {
            if (ln.empty()) continue;
            const char tag = ln[0];
            if (tag == ' ') {
                if (ai >= static_cast<int>(a_lines.size()) || bi >= static_cast<int>(b_lines.size())) break;
                body += " " + a_lines[ai++] + "\n"; ++bi; ++old_len; ++new_len;
            } else if (tag == '-') {
                if (ai >= static_cast<int>(a_lines.size())) continue;   // the phantom line
                body += "-" + a_lines[ai++] + "\n"; ++old_len;
            } else if (tag == '+') {
                if (bi >= static_cast<int>(b_lines.size())) continue;   // the phantom line
                body += "+" + b_lines[bi++] + "\n"; ++new_len;
            }
        }
        if (old_len == 0 && new_len == 0) continue;
        const int old_start = old_len == 0 ? h.old_start - 1 : h.old_start;
        const int new_start = new_len == 0 ? h.new_start - 1 : h.new_start;
        out += "@@ -" + std::to_string(added ? 0 : old_start) + "," + std::to_string(old_len) +
               " +" + std::to_string(new_start) + "," + std::to_string(new_len) + " @@\n";
        out += body;
    }
    if (out.find("\n@@ ") == std::string::npos) return std::nullopt;
    return out;
}


// Construct a text ContentBlock.
a::ContentBlock text_block(std::string s) {
    return a::ContentBlock{a::TextContent{std::move(s), a::Nothing, json::object()}};
}

// The first line of `s`, trailing CR stripped — for one-line card titles
// built from a possibly-multiline command/body.
std::string first_line(std::string s) {
    if (auto nl = s.find('\n'); nl != std::string::npos) s.resize(nl);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

// agentty spec::Kind → acp::ToolKind.
a::ToolKind acp_tool_kind(std::string_view tool_name) {
    namespace sp = tools::spec;
    const auto* s = sp::lookup(tool_name);
    if (!s) return a::ToolKind::Other;
    switch (s->kind) {
        case sp::Kind::Read:           return a::ToolKind::Read;
        case sp::Kind::Edit:           return a::ToolKind::Edit;
        case sp::Kind::Write:          return a::ToolKind::Edit;
        case sp::Kind::Move:           return a::ToolKind::Edit;
        case sp::Kind::Remove:         return a::ToolKind::Edit;
        case sp::Kind::Bash:           return a::ToolKind::Execute;
        case sp::Kind::ProcessStart:   return a::ToolKind::Execute;
        case sp::Kind::ProcessPoll:    return a::ToolKind::Execute;
        case sp::Kind::ProcessStop:    return a::ToolKind::Execute;
        case sp::Kind::Diagnostics:    return a::ToolKind::Execute;
        case sp::Kind::Test:           return a::ToolKind::Execute;
        case sp::Kind::GitCommit:      return a::ToolKind::Execute;
        case sp::Kind::GitBranch:      return a::ToolKind::Execute;
        case sp::Kind::GitStash:       return a::ToolKind::Execute;
        case sp::Kind::GitRebase:      return a::ToolKind::Execute;
        case sp::Kind::GitCherryPick:  return a::ToolKind::Execute;
        case sp::Kind::Grep:           return a::ToolKind::Search;
        case sp::Kind::Glob:           return a::ToolKind::Search;
        case sp::Kind::FindDefinition: return a::ToolKind::Search;
        case sp::Kind::SearchDocs:     return a::ToolKind::Search;
        case sp::Kind::SearchCode:     return a::ToolKind::Search;
        case sp::Kind::RepoMap:        return a::ToolKind::Search;
        case sp::Kind::ListDir:        return a::ToolKind::Read;
        case sp::Kind::GitStatus:      return a::ToolKind::Read;
        case sp::Kind::GitDiff:        return a::ToolKind::Read;
        case sp::Kind::GitLog:         return a::ToolKind::Read;
        case sp::Kind::GitShow:        return a::ToolKind::Read;
        case sp::Kind::GitBlame:       return a::ToolKind::Read;
        case sp::Kind::WebFetch:       return a::ToolKind::Fetch;
        case sp::Kind::WebSearch:      return a::ToolKind::Fetch;
        case sp::Kind::Todo:           return a::ToolKind::Think;
        case sp::Kind::Remember:       return a::ToolKind::Other;
        case sp::Kind::Forget:         return a::ToolKind::Other;
        case sp::Kind::Wipe:           return a::ToolKind::Other;
        case sp::Kind::Task:           return a::ToolKind::Think;
        case sp::Kind::Skill:          return a::ToolKind::Read;
    }
    // No default:/fall-through return — the switch is exhaustive over Kind, so
    // adding a new tool Kind re-triggers -Wswitch here (build error under
    // -Werror) instead of silently mapping to Other. This is the DESIGN.md
    // "sum type gains an arm → build error, not runtime ghost" rule.
    return a::ToolKind::Other;   // unreachable
}

// Present `path` relative to the session's workspace root when it lives
// under it — Zed shows project-relative paths in tool cards, and absolute
// paths are noise. Mirrors claude-code-acp's toDisplayPath, but we key off
// the session cwd the client actually opened rather than process().cwd.
std::string display_path(std::string_view path, std::string_view cwd) {
    if (path.empty() || cwd.empty()) return std::string{path};
    std::string_view c = cwd;
    while (!c.empty() && c.back() == '/') c.remove_suffix(1);
    if (path.size() > c.size() + 1 && path.substr(0, c.size()) == c
        && path[c.size()] == '/')
        return std::string{path.substr(c.size() + 1)};
    return std::string{path};
}

// Reconstruct a ripgrep-style command line from a grep call so the card reads
// like what claude-code-acp shows: `grep -i --include="*.cpp" "pattern" path`.
// Flags first, then --include for the glob, then the quoted pattern, then the
// path — the exact order and quoting claude-code-acp's Grep case emits.
std::string grep_command(const json& args) {
    auto s = [&](const char* k) -> std::string {
        return (args.contains(k) && args[k].is_string()) ? args[k].get<std::string>() : std::string{};
    };
    auto b = [&](const char* k) -> bool {
        return args.contains(k) && args[k].is_boolean() && args[k].get<bool>();
    };
    std::string cmd = "grep";
    // Case-insensitive is the default; claude-code-acp only ever appends `-i`
    // (for its case_insensitive flag) — there is no `-s` arm. agentty's flag is
    // case_sensitive, so emit `-i` only when NOT case-sensitive.
    if (!b("case_sensitive")) cmd += " -i";
    const std::string glob = s("glob");
    if (!glob.empty()) cmd += " --include=\"" + glob + "\"";
    const std::string pat = s("pattern");
    if (!pat.empty()) cmd += " \"" + pat + "\"";
    const std::string path = s("path");
    if (!path.empty()) cmd += " " + path;
    return cmd;
}

// One-line human title for a tool call card. `cwd` (may be empty) lets file
// paths render project-relative. Dispatch on the typed spec::Kind so a new
// tool arm forces a decision here rather than silently falling to bare name.
std::string tool_title(const ToolUse& tc, std::string_view cwd = {}) {
    namespace sp = tools::spec;
    const auto& args = tc.args;
    const std::string& n = tc.name.value;
    auto str = [&](const char* k) -> std::string {
        if (args.contains(k) && args[k].is_string()) return args[k].get<std::string>();
        return {};
    };
    auto num = [&](const char* k) -> std::optional<std::int64_t> {
        if (args.contains(k) && args[k].is_number_integer())
            return args[k].get<std::int64_t>();
        return std::nullopt;
    };
    auto path_arg = [&]() {
        for (const char* k : {"path", "file_path", "filepath", "filename"}) {
            std::string p = str(k);
            if (!p.empty()) return display_path(p, cwd);
        }
        return std::string{};
    };
    auto clip = [](std::string s, std::size_t n) {
        if (auto nl = s.find('\n'); nl != std::string::npos) s.resize(nl);
        if (s.size() > n) { s.resize(n); s += "\xe2\x80\xa6"; }
        return s;
    };

    const auto* spec = sp::lookup(n);
    const sp::Kind kind = spec ? spec->kind : sp::Kind::Read;
    if (!spec) return n;

    switch (kind) {
        case sp::Kind::Read: {
            std::string p = path_arg();
            // claude-code-acp uses "File" as the no-path fallback, and formats
            // the range as " (offset - offset+limit-1)" / " (from line N)".
            auto off = num("offset"); if (!off) off = num("start_line");
            auto lim = num("limit");
            auto end = num("end_line");
            std::string label = p.empty() ? std::string{"File"} : p;
            if (off && end) return "Read " + label + " (" + std::to_string(*off) + " - " + std::to_string(*end) + ")";
            if (lim && *lim > 0) {
                std::int64_t o = off ? *off : 1;
                return "Read " + label + " (" + std::to_string(o) + " - " + std::to_string(o + *lim - 1) + ")";
            }
            if (off) return "Read " + label + " (from line " + std::to_string(*off) + ")";
            return "Read " + label;
        }
        case sp::Kind::Edit: {
            std::string p = path_arg();
            return p.empty() ? "Edit" : "Edit " + p;
        }
        case sp::Kind::Write: {
            std::string p = path_arg();
            return p.empty() ? "Write" : "Write " + p;
        }
        case sp::Kind::Move: {
            auto source = display_path(str("source"), cwd);
            auto destination = display_path(str("destination"), cwd);
            return source.empty() ? "Move" : "Move " + source + " → " + destination;
        }
        case sp::Kind::Remove: {
            auto path = path_arg();
            return path.empty() ? "Remove" : "Remove " + path;
        }
        case sp::Kind::Bash:
        case sp::Kind::Diagnostics:
        case sp::Kind::Test:
        case sp::Kind::ProcessStart: {
            // Match claude-code-acp: the command itself IS the title, shown
            // in full (the client single-line-clamps for display, and the
            // whole command also rides in the card's content block via
            // command_content). Only the first line — a heredoc/multiline
            // body would otherwise blow up the one-line card. No 96-char clip.
            std::string c = first_line(str("command"));
            if (!c.empty()) return c;
            std::string d = str("display_description");
            return d.empty() ? (kind == sp::Kind::Test ? "Run tests" : "Terminal") : d;
        }
        case sp::Kind::ProcessPoll:
        case sp::Kind::ProcessStop: {
            auto id = str("id");
            return (kind == sp::Kind::ProcessPoll ? "Poll process " : "Stop process ") + id;
        }
        case sp::Kind::GitCommit: {
            std::string m = str("message");
            return m.empty() ? "git commit" : "git commit: " + clip(m, 60);
        }
        case sp::Kind::Grep:
            return grep_command(args);
        case sp::Kind::Glob: {
            std::string p = str("pattern");
            std::string root = display_path(str("path"), cwd);
            std::string label = "Find";
            if (!root.empty()) label += " `" + root + "`";
            if (!p.empty())    label += " `" + p + "`";
            return label;
        }
        case sp::Kind::FindDefinition: {
            std::string s = str("symbol");
            const std::string label = "Find definition";
            return s.empty() ? label : label + " `" + s + "`";
        }
        case sp::Kind::SearchDocs:
        case sp::Kind::SearchCode: {
            std::string q = str("query");
            std::string label = (kind == sp::Kind::SearchDocs) ? "Search docs" : "Search code";
            return q.empty() ? label : label + ": \"" + clip(q, 60) + "\"";
        }
        case sp::Kind::RepoMap: {
            std::string f = str("focus");
            return f.empty() ? "Repo map" : "Repo map: " + clip(f, 50);
        }
        case sp::Kind::ListDir: {
            std::string p = display_path(str("path"), cwd);
            return p.empty() ? "List directory" : "List `" + p + "`";
        }
        case sp::Kind::GitStatus: return "git status";
        case sp::Kind::GitDiff:   return "git diff";
        case sp::Kind::GitLog:    return "git log";
        case sp::Kind::GitShow:   return "git show";
        case sp::Kind::GitBlame:  return "git blame";
        case sp::Kind::GitBranch: {
            std::string action = str("action");
            if (action.empty()) action = "list";
            std::string name = str("name");
            std::string t = "git branch " + action;
            return name.empty() ? t : t + " " + name;
        }
        case sp::Kind::GitStash: {
            std::string action = str("action");
            if (action.empty()) action = "list";
            return "git stash " + action;
        }
        case sp::Kind::GitRebase: {
            std::string action = str("action");
            std::string up = str("upstream");
            if (action == "onto" && !up.empty()) return "git rebase onto " + up;
            return action.empty() ? "git rebase" : "git rebase " + action;
        }
        case sp::Kind::GitCherryPick: {
            std::string action = str("action");
            if (action.empty()) action = "pick";
            return "git cherry-pick " + action;
        }
        case sp::Kind::WebFetch: {
            std::string u = str("url");
            return u.empty() ? "Fetch" : "Fetch " + u;
        }
        case sp::Kind::WebSearch: {
            std::string q = str("query");
            return q.empty() ? "Web search" : "\"" + clip(q, 60) + "\"";
        }
        case sp::Kind::Todo: {
            if (args.contains("todos") && args["todos"].is_array()) {
                std::string joined;
                for (const auto& t : args["todos"]) {
                    if (t.is_object() && t.contains("content") && t["content"].is_string()) {
                        if (!joined.empty()) joined += ", ";
                        joined += t["content"].get<std::string>();
                    }
                }
                if (!joined.empty()) return "Update TODOs: " + clip(joined, 80);
            }
            return "Update TODOs";
        }
        case sp::Kind::Task: {
            std::string d = str("display_description");
            if (d.empty()) d = clip(str("prompt"), 60);
            return d.empty() ? "Task" : d;
        }
        case sp::Kind::Skill: {
            std::string s = str("name");
            return s.empty() ? "Skill" : "Skill: " + s;
        }
        case sp::Kind::Remember: return "Remember";
        case sp::Kind::Forget:   return "Forget";
        case sp::Kind::Wipe:     return "Wipe memory";
    }
    return n;   // unreachable; switch is exhaustive over Kind
}

// File locations a tool call touches, for Zed's follow-along. Emits the
// primary path with a line anchor when the call names one (Read offset,
// grep/find line hints), so Zed can jump the editor to the right spot.
a::List<a::ToolCallLocation> tool_locations(const ToolUse& tc, std::string_view cwd = {}) {
    a::List<a::ToolCallLocation> locs;
    const auto& args = tc.args;
    auto num = [&](const char* k) -> std::optional<std::int64_t> {
        if (args.contains(k) && args[k].is_number_integer())
            return args[k].get<std::int64_t>();
        return std::nullopt;
    };
    // claude-code-acp emits the RAW (absolute) path in locations — only the
    // card *title* is display-pathed. Zed uses locations to open the file in
    // the editor, which needs the real on-disk path, not a project-relative
    // one.
    //
    // And it must be ABSOLUTE, not merely raw. Zed's resolve_location
    // (acp_thread.rs) tries project_path_for_absolute_path, then falls back
    // to open_local_buffer only `if is_absolute(path)` — a relative path
    // matches neither and returns None, so the location is silently dropped
    // and follow-along stops moving. Nothing errors; the feature just
    // quietly stops working.
    //
    // A tool arg is whatever the model typed, which for `read` is very often
    // "src/main.cpp". So resolve against the session cwd here rather than
    // trusting it.
    auto absolutise = [&](std::string p) -> std::string {
        if (p.empty() || cwd.empty()) return p;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path fp{p};
        if (fp.is_absolute()) return p;
        auto joined = fs::path{cwd} / fp;
        // lexically_normal, not canonical: the file may not exist yet (a
        // `write` to a new path still wants a location), and canonical would
        // throw or return empty for it.
        return joined.lexically_normal().string();
    };
    auto add = [&](std::initializer_list<const char*> keys, bool default_line_one = false) {
        for (const char* key : keys) {
            if (args.contains(key) && args[key].is_string()) {
                std::string p = args[key].get<std::string>();
                if (p.empty()) continue;
                a::ToolCallLocation loc;
                loc.path = absolutise(std::move(p));
                auto ln = num("line"); if (!ln) ln = num("offset"); if (!ln) ln = num("start_line");
                // claude-code-acp's Read emits `line: input.offset ?? 1`, so a
                // Read with no offset still anchors the editor at line 1.
                if (!ln && default_line_one) ln = 1;
                if (ln) loc.line = a::Just<std::int64_t>(*ln);
                locs.push_back(std::move(loc));
                return;
            }
        }
    };
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    if (!spec) return locs;
    switch (spec->kind) {
        case sp::Kind::Read:
            add({"path", "file_path", "filepath", "filename"}, /*default_line_one=*/true);
            break;
        case sp::Kind::Edit:
        case sp::Kind::Write:
        case sp::Kind::ListDir:
        case sp::Kind::Diagnostics:
        case sp::Kind::Grep:
        case sp::Kind::Glob:
        case sp::Kind::FindDefinition:
            add({"path", "file_path", "filepath", "filename"});
            break;
        default: break;
    }
    return locs;
}

// Build the prompt text from acp ContentBlock[].
// One prompt's content blocks, split into what the model takes.
//
// Images used to be dropped on the floor here — the visitor had an empty
// arm for ImageContent and the function returned only text. So pasting a
// screenshot into Zed's agent panel silently sent the words and discarded
// the picture, on a build whose TUI has had vision for months. Nothing
// reported it, because a dropped block looks exactly like a model that
// ignored the image.
struct PromptParts {
    std::string               text;
    std::vector<ImageContent> images;
};

[[nodiscard]] PromptParts prompt_parts_from_blocks(const a::List<a::ContentBlock>& blocks) {
    PromptParts out;
    for (const auto& b : blocks) {
        a::match(b,
            [&](const a::TextContent& t) { out.text += t.text; out.text += "\n"; },
            [&](const a::ResourceLinkContent& l) {
                out.text += "[resource: " + (l.name.empty() ? l.uri : l.name)
                          + " (" + l.uri + ")]\n";
            },
            [&](const a::ResourceContent& r) {
                a::match(r.resource,
                    [&](const a::TextResource& tr) { out.text += tr.text; out.text += "\n"; },
                    [&](const a::BlobResource&)    {});
            },
            [&](const a::ImageContent& img) {
                // ACP sends base64 + mimeType; our ImageContent holds raw
                // bytes + media_type. Decode once here so every provider's
                // image path sees the same shape it does from the TUI.
                //
                // A block that fails to decode is SKIPPED rather than
                // aborting the turn: one bad attachment must not lose the
                // prompt it arrived with.
                auto bytes = util::base64_decode(img.data);
                if (bytes.empty()) return;
                out.images.emplace_back(
                    img.mimeType.empty() ? std::string{"image/png"} : img.mimeType,
                    std::move(bytes));
            },
            // Audio has no path into a prompt yet — no provider we ship
            // accepts it, and silently transcoding is worse than declining
            // the capability (which we do, so a client should not send it).
            [&](const a::AudioContent&) {});
    }
    return out;
}

// agentty StopReason → acp::StopReason.
a::StopReason acp_stop_reason(StopReason r, bool cancelled, bool errored) {
    if (cancelled) return a::StopReason::Cancelled;
    if (errored)   return a::StopReason::Refusal;
    switch (r) {
        // ACP has a native Refusal, so a decline keeps its identity across the
        // bridge instead of arriving in Zed as a normal end_turn.
        case StopReason::Refusal:      return a::StopReason::Refusal;
        // Truncation is truncation to a client: both leave the answer cut
        // off mid-thought, and MaxTokens is the signal ACP has for it. The
        // CAUSE differs (request cap vs model window) and that distinction
        // matters for what the user should do about it, but ACP has no
        // separate value, and silently calling a truncated turn EndTurn
        // would tell the client the answer is complete.
        case StopReason::ContextExceeded:
        case StopReason::MaxTokens:    return a::StopReason::MaxTokens;
        case StopReason::EndTurn:
        case StopReason::ToolUse:
        case StopReason::StopSequence:
        case StopReason::Unspecified:  return a::StopReason::EndTurn;
    }
    return a::StopReason::EndTurn;
}

// A speculative diff-content block from an edit/write call's own args, shown
// the moment the call is announced so Zed renders the diff card before the
// tool runs (claude-code-acp does the same for FileEdit/FileWrite). The
// authoritative diff — computed from the real on-disk before/after — replaces
// this in the completion update.
std::optional<a::ToolCallContent> announce_diff(const ToolUse& tc, std::string_view cwd) {
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    if (!spec) return std::nullopt;
    const auto& args = tc.args;
    auto s = [&](const char* k) -> std::string {
        return (args.contains(k) && args[k].is_string()) ? args[k].get<std::string>() : std::string{};
    };
    std::string path = s("path"); if (path.empty()) path = s("file_path");
    if (path.empty()) return std::nullopt;
    a::TCC_Diff diff;
    // Raw absolute path: Zed applies the diff to this exact on-disk file.
    // claude-code-acp passes input.file_path unmodified here. (void)cwd.
    (void)cwd;
    diff.path = path;
    if (spec->kind == sp::Kind::Write) {
        // A Write replaces the whole file. Announce oldText as an empty string
        // (not null) so Zed renders it as a normal diff card — the same widget
        // shape as Edit — rather than a special "new file" affordance that
        // looks different from an edit. The completion update carries the
        // authoritative on-disk before/after (which fills in real prior text
        // when the file already existed).
        diff.oldText = a::Just<std::string>(std::string{});
        diff.newText = s("content");
        return a::ToolCallContent{std::move(diff)};
    }
    if (spec->kind == sp::Kind::Edit) {
        std::string oldt = s("old_text"), newt = s("new_text");
        if (oldt.empty() && newt.empty()) return std::nullopt;
        diff.oldText = a::Just<std::string>(oldt);
        diff.newText = newt;
        return a::ToolCallContent{std::move(diff)};
    }
    return std::nullopt;
}

// The FULL command text for a terminal-kind tool call, wrapped as a fenced
// code block so Zed renders it verbatim in the (expandable) card body. The
// card *title* is deliberately clipped to one short line (tool_title →
// clip(...96)), and Zed further single-line-clamps long titles — so without
// this the full `command` only lived in rawInput, which Zed doesn't surface
// in the card. Emitting it as content means the untruncated command is always
// visible. Only for command-carrying kinds; nullopt otherwise.
std::optional<a::ToolCallContent> command_content(const ToolUse& tc) {
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    const auto& args = tc.args;

    // Unknown / MCP tools: claude-code-acp's `Other` case pretty-prints the raw
    // input as a ```json block so EVERY tool card has a legible body instead of
    // an empty one. Do the same so an MCP tool surfaced to Zed shows its
    // arguments rather than a bare title.
    if (!spec) {
        if (args.is_null()) return std::nullopt;
        std::string dump = args.dump(2);
        if (dump == "null" || dump.empty()) return std::nullopt;
        std::string md = "```json\n" + dump + "\n```";
        return a::ToolCallContent{a::TCC_Content{text_block(std::move(md)), json::object()}};
    }

    switch (spec->kind) {
        case sp::Kind::Bash:
        case sp::Kind::Diagnostics:
        case sp::Kind::Test:
        case sp::Kind::ProcessStart: {
            if (!args.contains("command") || !args["command"].is_string())
                return std::nullopt;
            std::string cmd = args["command"].get<std::string>();
            if (cmd.empty()) return std::nullopt;
            std::string md = "```sh\n" + cmd + "\n```";
            return a::ToolCallContent{a::TCC_Content{text_block(std::move(md)), json::object()}};
        }
        case sp::Kind::WebFetch: {
            // claude-code-acp attaches WebFetch's `prompt` as body content (the
            // url is already in the title). Mirror it so the fetch intent shows
            // in the card.
            if (!args.contains("prompt") || !args["prompt"].is_string())
                return std::nullopt;
            std::string body = args["prompt"].get<std::string>();
            if (body.empty()) return std::nullopt;
            return a::ToolCallContent{a::TCC_Content{text_block(std::move(body)), json::object()}};
        }
        default:
            return std::nullopt;
    }
}

// A plain-text content block for the Task tool, whose defining input is the
// sub-agent prompt (prose, not a command or diff). claude-code-acp attaches
// the sub-agent prompt as a content block so the full text is visible in the
// card body even though the title is clipped. (WebFetch's input is a bare
// `url` already shown in the title, so it needs no extra block.)
std::optional<a::ToolCallContent> prompt_content(const ToolUse& tc) {
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    if (!spec || spec->kind != sp::Kind::Task) return std::nullopt;
    const auto& args = tc.args;
    if (!args.contains("prompt") || !args["prompt"].is_string()) return std::nullopt;
    std::string body = args["prompt"].get<std::string>();
    if (body.empty()) return std::nullopt;
    return a::ToolCallContent{a::TCC_Content{text_block(std::move(body)), json::object()}};
}

// Escape the markdown metacharacters Zed's card renderer would otherwise
// interpret, so raw file/grep text with `*`, backticks, `#`, `_`, `[`, `<`
// renders literally instead of turning into bold/headers/links/HTML. Mirrors
// claude-code-acp's markdownEscape() applied to tool-result text.
std::string markdown_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char c : s) {
        switch (c) {
            case '\\': case '`': case '*': case '_': case '{': case '}':
            case '[':  case ']': case '<': case '>': case '#': case '+':
            case '-':  case '.': case '!': case '|':
                out.push_back('\\');
                [[fallthrough]];
            default:
                out.push_back(c);
        }
    }
    return out;
}

// Render a completed tool's result text into the ACP content block Zed shows
// in the card body, matching claude-code-acp's per-tool result shaping:
//
//   • Read → rebuild a line-numbered view (`<n>\t<line>`) starting at the
//     read's offset, so Zed renders a clean gutter-numbered excerpt instead of
//     a raw blob. The body is fenced as a plain code block so the tab-numbered
//     lines and any markdown metacharacters in the file render verbatim.
//   • Everything else → the text as-is, but markdown-escaped so stray `*`/`#`/
//     backticks in output don't get parsed as formatting.
//
// Returns a TCC_Content ready to push onto the completion update. `text` is the
// tool's model-facing output; `tc` supplies the tool identity + args (offset).
a::ToolCallContent result_content_block(const ToolUse& tc, const std::string& text) {
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    const sp::Kind kind = spec ? spec->kind : sp::Kind::Read;

    if (kind == sp::Kind::Read && !text.empty()) {
        // Starting line = the read's offset (1-based), matching claude-code-acp
        // (`structuredRead.file.startLine ?? input.offset ?? 1`). agentty's
        // read tool prepends an optional `display_description` line and appends
        // a trailing `[showing lines X-Y of N]` hint; both are model-facing
        // scaffolding, not file bytes, so strip a leading description line and
        // a trailing bracket hint before numbering so the gutter stays aligned
        // with real file lines.
        std::int64_t start = 1;
        const auto& args = tc.args;
        for (const char* k : {"offset", "start_line"}) {
            if (args.contains(k) && args[k].is_number_integer()) {
                start = args[k].get<std::int64_t>();
                break;
            }
        }
        if (start < 1) start = 1;

        std::string_view body{text};
        // Drop the trailing "\n[showing lines ...]" hint if present.
        if (auto hint = body.rfind("\n[showing lines ");
            hint != std::string_view::npos)
            body = body.substr(0, hint);

        std::string numbered;
        numbered.reserve(body.size() + body.size() / 8);
        // read numbers its own lines now ("%6d\t"); only number old,
        // unnumbered results here.
        auto already_numbered = [](std::string_view s) {
            std::size_t k = 0;
            while (k < s.size() && s[k] == ' ') ++k;
            const std::size_t d = k;
            while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
            return k > d && k < s.size() && s[k] == '\t';
        };
        if (already_numbered(body)) {
            std::string md = "```\n" + std::string{body} + "\n```";
            return a::ToolCallContent{a::TCC_Content{text_block(std::move(md)), json::object()}};
        }
        std::int64_t ln = start;
        std::size_t pos = 0;
        while (pos <= body.size()) {
            std::size_t nl = body.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? body.substr(pos)
                : body.substr(pos, nl - pos);
            numbered += std::to_string(ln);
            numbered += '\t';
            numbered.append(line);
            numbered += '\n';
            ++ln;
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        // Fence so the tab layout and any markdown chars render verbatim.
        std::string md = "```\n" + numbered + "```";
        return a::ToolCallContent{a::TCC_Content{text_block(std::move(md)), json::object()}};
    }

    // Command output (bash/test/diagnostics/process/git): fence as a code
    // block so terminal output renders as a monospace log — exactly what a Zed
    // user expects from a shell card — instead of markdown-escaped prose. This
    // is the static fallback for when the live-terminal path isn't taken.
    if ((kind == sp::Kind::Bash || kind == sp::Kind::Test
         || kind == sp::Kind::Diagnostics || kind == sp::Kind::ProcessStart
         || kind == sp::Kind::ProcessPoll || kind == sp::Kind::ProcessStop
         || kind == sp::Kind::GitCommit)
        && !text.empty()) {
        std::string body{text};
        std::string md = body.find("```") == std::string::npos
                       ? "```\n" + body + "\n```"
                       : std::move(body);
        return a::ToolCallContent{a::TCC_Content{text_block(std::move(md)), json::object()}};
    }

    // Non-Read results: escape markdown so raw output renders literally.
    return a::ToolCallContent{
        a::TCC_Content{text_block(markdown_escape(text)), json::object()}};
}

// An ACP plan (Zed's checklist widget) built from the `todo` tool's args.
// the native plan UI instead of an opaque "Update TODOs" tool card. Returns
// nullopt when the call isn't a todo write or carries no usable list.
std::optional<a::SU_Plan> plan_from_todos(const ToolUse& tc) {
    namespace sp = tools::spec;
    const auto* spec = sp::lookup(tc.name.value);
    if (!spec || spec->kind != sp::Kind::Todo) return std::nullopt;
    const auto& args = tc.args;
    if (!args.contains("todos") || !args["todos"].is_array()) return std::nullopt;

    auto status_of = [](std::string_view s) {
        if (s == "in_progress") return a::PlanEntryStatus::InProgress;
        if (s == "completed")   return a::PlanEntryStatus::Completed;
        return a::PlanEntryStatus::Pending;
    };

    a::SU_Plan plan;
    for (const auto& t : args["todos"]) {
        if (!t.is_object()) continue;
        std::string content = t.value("content", std::string{});
        if (content.empty()) continue;
        a::PlanEntry e;
        e.content  = std::move(content);
        e.priority = a::PlanEntryPriority::Medium;   // agentty todos carry no priority
        e.status   = status_of(t.value("status", std::string{"pending"}));
        plan.entries.push_back(std::move(e));
    }
    if (plan.entries.empty()) return std::nullopt;
    return plan;
}

// Helper: a ToolCall (announcement) from a pending ToolUse.
a::ToolCall make_tool_call(const ToolUse& tc, a::ToolCallStatus status,
                           std::string_view cwd = {}) {
    a::ToolCall out;
    out.toolCallId = a::ToolCallId{tc.id.value};
    out.title      = tool_title(tc, cwd);
    // The PROGRAMMATIC name, alongside the human title. Stabilised in spec
    // 1.8.0 and optional, but a client that wants to group, filter or icon
    // by tool identity cannot parse it back out of "Reading src/main.cpp" —
    // so omitting it just makes that impossible.
    out.name       = a::Just<std::string>(tc.name.value);
    out.kind       = acp_tool_kind(tc.name.value);
    out.status     = status;
    if (!tc.args.is_null()) out.rawInput = a::Just<json>(tc.args);
    out.locations  = tool_locations(tc, cwd);
    a::List<a::ToolCallContent> content;
    if (auto d = announce_diff(tc, cwd)) content.push_back(std::move(*d));
    if (auto c = command_content(tc))    content.push_back(std::move(*c));
    if (auto pc = prompt_content(tc))    content.push_back(std::move(*pc));
    if (!content.empty()) out.content = std::move(content);
    return out;
}

// ── Shell parity with Zed's native agent ────────────────────────────────────
// The slash-command menu Zed renders in its composer. claude-code-acp exposes
// its own set here; agentty exposes the ones that map to real turn actions plus
// every installed, model-invocable skill as `/<name>`. Sent on session new/load
// so Zed's `/` menu is populated the same way it is for the native agent.
a::List<a::AvailableCommand> available_commands(Io io, bool air = false) {
    a::List<a::AvailableCommand> cmds;
    auto add = [&](std::string name, std::string desc, const char* hint) {
        a::AvailableCommand c;
        c.name        = std::move(name);
        c.description = std::move(desc);
        if (hint)
            c.input = a::Just<a::AvailableCommandInput>(
                a::AvailableCommandInput{a::UnstructuredCommandInput{hint}});
        cmds.push_back(std::move(c));
    };
    add("compact", "Summarize the conversation to reclaim context window", nullptr);
    add("new",     "Start a fresh conversation, clearing prior context", nullptr);
    // Zed groups and runs these as agent actions, not prompts it echoes.
    for (auto& c : cmds) c.meta = json{{"command_category", "native"}};
    std::set<std::string> seen{"compact", "new"};
    // The user's own slash commands (.agentty/.claude/commands/*.md), with
    // their argument hint, the same set the TUI's `/` picker shows.
    for (const auto& c : tools::commands::all(io)) {
        if (c.name.empty() || !seen.insert(c.name).second) continue;
        add(c.name, c.description.empty() ? "Run /" + c.name : c.description,
            c.argument_hint.empty() ? nullptr : c.argument_hint.c_str());
    }
    // Every installed skill. user_only ones are exactly the ones meant to be
    // run by hand, so they are listed too; the model just never sees them.
    for (const auto& sk : tools::skills::all(io)) {
        if (sk.name.empty() || !seen.insert(sk.name).second) continue;
        add(sk.name,
            sk.description.empty() ? std::string{"Run the "} + sk.name + " skill"
                                   : sk.description,
            "optional arguments");
        // AIR shows skills as skills, with the file behind them.
        if (air)
            cmds.back().meta = json{{"jetbrains", {{"air", {{"version", 1}, {"kind", "skill"},
                {"skillPath", (sk.dir / "SKILL.md").string()}}}}}};
    }
#if AGENTTY_MCP
    // MCP server prompts. Zed groups commands tagged "mcp".
    try {
        for (const auto& pr : ::agentty::mcp::mcp_prompts()) {
            if (pr.name.empty() || !seen.insert(pr.name).second) continue;
            std::string hint;
            for (const auto& arg : pr.arguments) {
                if (!hint.empty()) hint += ' ';
                hint += arg.required ? arg.name : "[" + arg.name + "]";
            }
            const std::string desc = !pr.description.empty() ? pr.description
                                   : !pr.title.empty()       ? pr.title
                                                             : "Prompt from " + pr.server;
            add(pr.name, desc, hint.empty() ? nullptr : hint.c_str());
            cmds.back().meta = json{{"command_category", "mcp"}};
        }
    } catch (...) {}
#endif
    return cmds;
}

// The `model` config option Zed renders as a dropdown, populated with the
// provider's model catalog. `current` is the session's active model id. When
// `auth` has no credentials we still emit the static seed list so the dropdown
// isn't empty before sign-in. Mirrors how the native agent advertises its
// model picker. Kept off the hot path (built on session new/load only).
// A model choice's wire value: "<provider>\t<model>", the same composite key
// the TUI's recent-models list uses, so two providers serving the same model
// id stay distinct.
std::string choice_value(std::string_view provider, std::string_view model) {
    return std::string{provider} + '\t' + std::string{model};
}
std::pair<std::string, std::string> split_choice(std::string_view v) {
    const auto tab = v.find('\t');
    if (tab == std::string_view::npos) return {"", std::string{v}};   // bare model id
    return {std::string{v.substr(0, tab)}, std::string{v.substr(tab + 1)}};
}

// The effort levels a model accepts, low to high, as wire strings.
std::vector<Effort> effort_ladder(const std::string& model, const std::string& provider) {
    const auto caps = resolved_caps(model, provider);
    const std::uint8_t set = effort_set_of(caps);
    std::vector<Effort> out;
    for (Effort e : {Effort::Minimal, Effort::Low, Effort::Medium, Effort::High,
                     Effort::Xhigh, Effort::Max})
        if (set & effort_bit(e)) out.push_back(e);
    return out;
}

// "" (default) | "none" | a wire level, clamped to what the model takes.
// Returns the value to show in the dropdown.
std::string effective_effort(const std::string& want, const std::vector<Effort>& ladder) {
    if (ladder.empty()) return "none";
    if (want == "none") return "none";
    if (want.empty() || want == "auto") return "auto";
    for (Effort e : ladder)
        if (effort_wire(e) == want) return want;
    return "auto";
}

// The permission profile (Ask / Write / Minimal) as a config option,
// category "mode". Sent on every connection next to the legacy `modes`
// field: Zed's profile picker is the first `select` option with category
// "mode", and once configOptions are present Zed ignores `modes`. Same
// three ids as mode_state()/mode_id_for().
[[nodiscard]] a::ConfigOption mode_config_option(const char* current_mode_id) {
    auto mk = [](const char* value, const char* name, const char* desc) {
        a::ConfigSelectOption o;
        o.value       = value;
        o.name        = name;
        o.description = a::Just<std::string>(desc);
        return o;
    };
    a::List<a::ConfigSelectOption> opts;
    opts.push_back(mk("ask", "Ask",
        "Prompt before edits, commands, and network access"));
    opts.push_back(mk("write", "Write",
        "Edit files and run commands without prompting; still prompt for risky ops"));
    opts.push_back(mk("minimal", "Minimal",
        "Prompt for everything, including file reads"));

    a::ConfigOption opt;
    opt.id           = "mode";
    opt.name         = "Permission Mode";
    opt.description  = a::Just<std::string>(
        "How agentty asks before acting on your workspace");
    opt.category     = a::Just<std::string>("mode");
    opt.type         = "select";
    opt.currentValue = current_mode_id ? current_mode_id : "ask";
    opt.options      = a::ConfigSelectOptions{a::CSO_Ungrouped{std::move(opts)}};
    return opt;
}

} // namespace

AgentServer::AgentServer(rpc::Channel      channel,
                         StreamFn          stream,
                         auth::AuthHeader  auth,
                         std::string       model_id,
                         Profile           profile)
    : router_(make_router()),
      peer_(std::make_unique<rpc::Peer>(std::move(channel), make_dispatch(), "acp")),
      stream_(std::move(stream)),
      model_id_(std::move(model_id)),
      profile_(profile),
      auth_(std::move(auth)) {
    // A dead client must not wedge a worker forever: our calls to it
    // (permission, terminal/*) fail with Timeout after this. 5 min is
    // generous for a human reading a permission dialog.
    peer_->set_default_timeout(std::chrono::minutes(5));
}

AgentServer::~AgentServer() {
    if (client_elicit_url_.load(std::memory_order_relaxed))
        ::agentty::mcp::set_url_elicitation({});
    // peer_ is destroyed with the members: ~Peer stops and joins its
    // reader/writer (Peer is joins_on_destroy, so no lock can own it, and
    // this destructor never runs inside a lock body).
}

AgentServer::Router AgentServer::make_router() {
    namespace m = a::to_agent;
    Router r;
    // Every handler gets the reader thread's capability through Ctx.
    r.on<m::Initialize>([](Ctx& c, const a::InitializeParams& p) { return c.self.on_initialize(c.cap, p); });
    r.on<m::SessionNew>([](Ctx& c, const a::NewSessionParams& p) { return c.self.on_new_session(c.cap, p); });
    r.on<m::SessionCancel>([](Ctx& c, const a::CancelParams& p) { c.self.on_cancel(c.cap, p); });
    r.on<a::CancelRequest>([](Ctx& c, const a::CancelRequestParams& p) { c.self.on_cancel_request(c.cap, p.id); });
    // session/prompt is answered in on_call, before the router.
    r.defer<m::SessionPrompt>();
    r.on<m::SessionLoad>([](Ctx& c, const a::LoadSessionParams& p) {
        return c.self.on_load_session(c.cap, p);
    });
    r.on<m::SessionResume>([](Ctx& c, const a::ResumeSessionParams& p) { return c.self.on_resume_session(c.cap, p); });
    r.on<m::SessionFork>([](Ctx& c, const a::ForkSessionParams& p) { return c.self.on_fork_session(c.cap, p); });
    r.on<m::SessionList>([](Ctx& c, const a::ListSessionsParams& p) { return c.self.on_list_sessions(c.cap, p); });
    r.on<m::SessionClose>([](Ctx& c, const a::CloseSessionParams& p) {
        c.self.on_close_session(c.cap, p); return a::Unit{};
    });
    r.on<m::SessionDelete>([](Ctx& c, const a::DeleteSessionParams& p) {
        c.self.on_delete_session(c.cap, p); return a::Unit{};
    });
    r.on<m::SessionSetMode>([](Ctx& c, const a::SetModeParams& p) {
        c.self.on_set_mode(c.cap, p); return a::Unit{};
    });
    r.on<m::SessionSetConfig>([](Ctx& c, const a::SetConfigOptionParams& p) {
        return c.self.on_set_config_option(c.cap, p);
    });
    r.on<m::Authenticate>([](Ctx& c, const a::AuthenticateParams& p) {
        c.self.on_authenticate(p);
        return a::Unit{};
    });
    r.on<m::Logout>([](Ctx& c, const a::Unit&) { c.self.on_logout(c.cap); return a::Unit{}; });
    return r;
}

rpc::Dispatch AgentServer::make_dispatch() {
    rpc::Dispatch d;
    d.on_call         = [this](const maya::unlocked& cap, const a::Call& c) { return on_call(cap, c); };
    d.on_notification = [this](const maya::unlocked& cap, const a::Notification& n) {
        on_notification(cap, n);
    };
    // Every frame lands on the `acp` channel of the structured log, captured
    // by default in a non-release build or via AGENTTY_LOG=acp=trace.
    if (logx::enabled(logx::Channel::Acp, logx::Level::Trace))
        d.trace = [](bool inbound, std::string_view line) {
            AGT_LOG(Acp, Trace, "acp.frame", "dir={} raw={}", inbound ? "in" : "out", line);
        };
    return d;
}

std::optional<std::string> AgentServer::on_call(const Cap& cap, const a::Call& c) {
    if (c.method == a::to_agent::SessionPrompt::name) {
        Responder resp(*peer_, c.id);
        try {
            on_prompt(cap, jsonrpc::from_json<a::PromptParams>(c.params), std::move(resp));
        } catch (const std::exception& e) {
            return a::reply(c.id, std::unexpected(a::RpcError(a::errc::InvalidParams, e.what())));
        }
        return std::nullopt;
    }
    Ctx ctx{*this, cap};
    return router_.handle(ctx, c);
}

void AgentServer::on_notification(const Cap& cap, const a::Notification& n) {
    Ctx ctx{*this, cap};
    router_.handle(ctx, n);
}

int AgentServer::serve(const Cap& cap) {
    peer_->start();
    peer_->wait_closed(cap);   // EOF on stdin
    return 0;
}

void AgentServer::persist(const Cap& cap, const std::string& id) {
    // Copy the thread out under the lock, write it outside.
    if (auto snapshot = with_session(cap, id, [](Session& s) { return s.thread; }))
        persistence::save_thread(IoAccess::grant(), cap, *snapshot);
}

// The editor's MCP servers (session/new, load, resume, fork), turned into
// mcp.json entries and handed to the MCP bridge. The bridge reconnects only
// when the set changed, so sending the same list each session is free.
void AgentServer::adopt_client_mcp(const Cap&, const a::List<a::McpServer>& servers) {
#if AGENTTY_MCP
    json out = json::object();
    for (const auto& s : servers) {
        a::match(s,
            [&](const a::StdioMcpServer& v) {
                json e{{"command", v.command}, {"args", v.args}};
                json env = json::object();
                for (const auto& kv : v.env) env[kv.name] = kv.value;
                if (!env.empty()) e["env"] = std::move(env);
                out[v.name] = std::move(e);
            },
            [&](const a::HttpMcpServer& v) {
                json h = json::object();
                for (const auto& kv : v.headers) h[kv.name] = kv.value;
                out[v.name] = json{{"type", "http"}, {"url", v.url}, {"headers", std::move(h)}};
            },
            [&](const a::SseMcpServer& v) {
                json h = json::object();
                for (const auto& kv : v.headers) h[kv.name] = kv.value;
                out[v.name] = json{{"type", "sse"}, {"url", v.url}, {"headers", std::move(h)}};
            });
    }
    // Connecting can take seconds; don't hold up session/new for it. When it
    // lands, re-send every live session's / menu so the servers' prompts show.
    util::run_isolated_detached("acp.mcp_servers",
        [](const maya::unlocked& cap, std::stop_token, json servers, maya::co_owned<AgentServer> self) {
            ::agentty::mcp::set_client_servers(servers);
            self->refresh_command_menus(cap);
        },
        std::move(out), maya::co_owned<AgentServer>::of(*this));
#else
    (void)servers;
#endif
}

// The / menu changed under every session (an MCP server connected and
// brought prompts): send each live session the new list.
void AgentServer::refresh_command_menus(const Cap& cap) {
    const auto ids = sessions_.read(cap, [](const Sessions& ss) {
        std::vector<std::string> out;
        for (const auto& [id, s] : ss.by_id) out.push_back(id);
        return out;
    });
    if (ids.empty()) return;
    a::SU_AvailableCommands ac;
    ac.availableCommands = available_commands(IoAccess::grant(), air_.load(std::memory_order_relaxed));
    for (const auto& id : ids) send_update(cap, id, ac);
}

void AgentServer::send_update(const maya::below<maya::leaf>& cap, const std::string& session_id,
                              a::SessionUpdate update) {
    a::SessionUpdateMsg msg;
    msg.sessionId = a::SessionId{session_id};
    msg.update    = std::move(update);
    peer_->notify<a::to_client::SessionUpdate>(cap, msg);
}

void AgentServer::notice(const maya::below<maya::leaf>& cap, const std::string& session_id,
                         std::string severity,
                         std::string title, std::string description) {
    if (!client_notices_.load(std::memory_order_relaxed) || title.empty()) return;
    a::SU_Notice n;
    n.severity = std::move(severity);
    n.title    = std::move(title);
    if (!description.empty()) n.description = a::Just(std::move(description));
    send_update(cap, session_id, std::move(n));
}

bool AgentServer::terminal_channel() const noexcept {
    return client_terminal_meta_.load(std::memory_order_relaxed);
}

json AgentServer::air_meta(json payload) const {
    if (!air_.load(std::memory_order_relaxed)) return json::object();
    payload["version"] = 1;
    return json{{"jetbrains", {{"air", std::move(payload)}}}};
}

// AIR's per-tool hints: a command's own description as the card title, the
// task tool marked as a subagent, a skill load named. Empty for other tools.
json AgentServer::air_tool_meta(const ToolUse& tc) const {
    if (!air_.load(std::memory_order_relaxed) || !tc.args.is_object()) return json::object();
    json p = json::object();
    const std::string& n = tc.name.value;
    if (is_command_tool(n)) {
        for (const char* k : {"display_description", "description"})
            if (auto it = tc.args.find(k); it != tc.args.end() && it->is_string()
                && !it->get<std::string>().empty()) {
                p["commandTitle"] = *it;
                break;
            }
    } else if (n == "task") {
        p["subagent"] = true;
    } else if (n == "skill") {
        if (auto it = tc.args.find("name"); it != tc.args.end() && it->is_string())
            p["skill"] = json{{"name", *it}};
    }
    return p.empty() ? json::object() : air_meta(std::move(p));
}

// Merge `extra` into `meta`, keeping other namespaces.
static void merge_meta(json& meta, const json& extra) {
    if (!extra.is_object() || extra.empty()) return;
    if (!meta.is_object()) meta = json::object();
    meta.merge_patch(extra);
}

// Relay an MCP server's elicitation/create (form mode) to the editor. The two
// share the restricted JSON Schema, so the form passes through; ACP adds the
// session and tool call scope. Anything we can't relay is declined.
json AgentServer::elicit_via_client(const Cap& cap, const std::string& session_id,
                                    const std::string& tool_id, const json& mcp,
                                    std::stop_token cancel) {
    const json decline{{"action", "decline"}};
    if (!mcp.is_object()) return decline;
    const std::string mode = mcp.value("mode", std::string{"form"});
    json req;
    if (mode == "url" && client_elicit_url_.load(std::memory_order_relaxed)) {
        // The editor shows the URL and opens it only with the user's consent.
        if (!mcp.contains("url") || !mcp["url"].is_string()
            || !mcp.contains("elicitationId") || !mcp["elicitationId"].is_string())
            return decline;
        req = json{{"mode", "url"},
                   {"message", mcp.value("message", std::string{"The tool needs you to open a link."})},
                   {"url", mcp["url"]},
                   {"elicitationId", mcp["elicitationId"]},
                   {"sessionId", session_id},
                   {"toolCallId", tool_id}};
    } else if (mode == "form" && client_elicit_form_.load(std::memory_order_relaxed)
               && mcp.contains("requestedSchema") && mcp["requestedSchema"].is_object()) {
        req = json{{"mode", "form"},
                   {"message", mcp.value("message", std::string{"The tool needs some input."})},
                   {"requestedSchema", mcp["requestedSchema"]},
                   {"sessionId", session_id},
                   {"toolCallId", tool_id}};
    } else {
        return decline;
    }
    // A cancelled turn withdraws the form ($/cancel_request) and declines.
    rpc::CallOptions o;
    o.cancel      = cancel;
    o.notify_peer = true;
    auto got = peer_->call<a::to_client::ElicitationCreate>(cap, req, std::move(o));
    if (!got || !got->is_object() || !got->contains("action")) return decline;
    json out{{"action", (*got)["action"]}};
    if ((*got)["action"] == "accept" && got->contains("content")) out["content"] = (*got)["content"];
    return out;
}

bool AgentServer::is_command_tool(std::string_view name) {
    const auto* spec = tools::spec::lookup(name);
    return spec && spec->kind == tools::spec::Kind::Bash;
}

void AgentServer::send_terminal_output(const maya::below<maya::leaf>& cap,
                                       const std::string& session_id,
                                       const std::string& tool_id, std::string_view data) {
    a::ToolCallUpdate u;
    u.toolCallId = a::ToolCallId{tool_id};
    // Zed replaces with terminal_output chunks; clients that ask for
    // terminal_output_delta append. Same payload either way.
    const char* key = client_terminal_delta_.load(std::memory_order_relaxed)
                    ? "terminal_output_delta" : "terminal_output";
    // progress text is already valid UTF-8 (exec converts it).
    u.meta = json{{key, {{"terminal_id", tool_id}, {"data", std::string{data}}}}};
    send_update(cap, session_id, a::SU_ToolCallUpdate{std::move(u)});
}

// _meta.terminal_exit for a finished command. The shell tool's result says
// "failed with exit code N" on a failure and nothing on success.
json AgentServer::terminal_exit_meta(const std::string& tool_id, std::string_view text,
                                     bool failed) {
    json code = failed ? json(1) : json(0);
    constexpr std::string_view kTag = "failed with exit code ";
    if (const auto at = text.find(kTag); at != std::string_view::npos) {
        int n = 0;
        const char* p = text.data() + at + kTag.size();
        const char* end = text.data() + text.size();
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') { n = ::agentty::util::push_digit(n, *p); ++p; any = true; }
        if (any) code = n;
    } else if (text.find("timed out after") != std::string_view::npos) {
        code = nullptr;   // killed, no exit status
    }
    return json{{"terminal_exit", {{"terminal_id", tool_id}, {"exit_code", code},
                                    {"signal", nullptr}}}};
}

void AgentServer::emit_session_config(const Cap& cap, const std::string& session_id,
                                      const std::string& model_id) {
    // Slash-command menu (Zed's `/` composer menu).
    {
        a::SU_AvailableCommands ac;
        ac.availableCommands = available_commands(IoAccess::grant(),   // ACP handler: not a reducer
                                                  air_.load(std::memory_order_relaxed));
        if (!ac.availableCommands.empty())
            send_update(cap, session_id, std::move(ac));
    }
    // Session config options, emitted as ONE complete-state notification (the
    // v2 contract: a SU_ConfigOptions carries the FULL set, not a delta). We
    // advertise the permission MODE and, when a catalog is available, the
    // MODEL. A config-option-aware client renders these as native selectors
    // (Zed's per-session dropdowns); it can flip them live via
    // session/set_config_option with no restart. The permission mode is ALSO
    // carried by the legacy `modes` field on the session result for v1
    // clients — dual-surface, no gap either way.
    emit_config_state(cap, session_id, model_id);
}

// The current, COMPLETE config-option state for a session as a single
// SU_ConfigOptions notification. Single source of truth for both the initial
// advertisement (emit_session_config) and every post-change echo
// (on_set_config_option) — the v2 spec requires each SU_ConfigOptions to carry
// the full set so dependent options stay consistent. Reads live session state
// (profile + model) so the echoed `currentValue`s always match reality.
void AgentServer::emit_config_state(const Cap& cap, const std::string& session_id,
                                    const std::string& fallback_model) {
    a::SU_ConfigOptions co;
    co.configOptions = config_options(cap, session_id, fallback_model);
    if (co.configOptions.empty()) return;   // nothing to advertise
    send_update(cap, session_id, std::move(co));
}

// The complete config state for a session. Returned in every session/new,
// load, resume and set_config_option result (clients that only read results,
// like JetBrains, need it there) and echoed as config_option_update.
a::List<a::ConfigOption> AgentServer::config_options(const Cap& cap,
                                                     const std::string& session_id,
                                                     const std::string& fallback_model) {
    struct Cfg { Profile prof; std::string provider, model, effort; bool reasoning; };
    Cfg c{profile_, "", fallback_model, "", true};
    if (auto got = with_session(cap, session_id, [](Session& s) {
            return Cfg{s.profile, s.provider, s.model, s.effort, s.show_reasoning};
        })) {
        c = *got;
        if (c.model.empty()) c.model = fallback_model;
    }
    a::List<a::ConfigOption> out;
    // The permission mode is always a config option. A client that gets
    // configOptions builds its selectors from them alone (Zed drops `modes`
    // as soon as configOptions is present, on v1 too), so leaving it out
    // meant no profile picker at all.
    {
        auto opt = mode_config_option(mode_id_for(c.prof));
        if (auto* u = std::get_if<a::CSO_Ungrouped>(&opt.options))
            for (auto& o : u->items)
                o.meta = air_meta(json{{"kind", o.value == "write" ? "auto_review" : "standard"}});
        out.push_back(std::move(opt));
    }
    for (auto& o : model_options(cap, c.provider, c.model, c.effort, c.reasoning))
        out.push_back(std::move(o));
    return out;
}

// Every model on every provider the user is signed into, grouped by
// provider, the same set the TUI's fused picker (ctrl+/) shows. Without a
// stream_to_ the server can only reach the active provider, so only its
// models are offered.
std::vector<AgentServer::ModelChoice> AgentServer::model_choices(const Cap& cap) {
    if (auto have = model_choices_.current(); have && !have->empty())
        return *have;

    const Io io = IoAccess::grant();
    std::vector<ModelChoice> out;
    const auto active = provider::active();
    const std::string active_id = active.catalog_id();
    auto add_provider = [&](const std::string& pid, const std::string& label,
                            const provider::Selection& sel, const auth::AuthHeader& auth) {
        std::vector<ModelInfo> models;
        try { models = provider::list_models_for(io, cap, sel, auth); } catch (...) {}
        for (const auto& m : models)
            out.push_back({choice_value(pid, m.id.value), pid, m.id.value,
                           m.display_name.empty() ? m.id.value : m.display_name,
                           pid, label});
    };

    // The active provider first: its models are what most users pick from.
    add_provider(active_id, provider::provider_display_name(active), active, auth());
    if (stream_to_) {
        try {
            const auto settings = persistence::load_settings(io);
            const auto view = provider::load_auth_view(io, settings);
            for (const auto& src : provider::catalog_sources(settings, view)) {
                if (src.needs_signin || src.id == active_id) continue;
                const auto sel = provider::parse_selection(src.id);
                if (sel.kind == provider::Kind::ExternalAcp) continue;
                add_provider(src.id, src.label, sel,
                             provider::credentials::resolve(io, src.id));
            }
        } catch (...) { /* the active provider's models still stand */ }
    }
    model_choices_.publish(std::make_shared<const std::vector<ModelChoice>>(out));
    return out;
}

// Model, then the knobs that depend on it: reasoning effort (only for a
// model that has one) and whether to show its thinking. Rebuilt on every
// change, since the effort ladder depends on the model.
a::List<a::ConfigOption> AgentServer::model_options(const Cap& cap,
                                                    const std::string& provider,
                                                    const std::string& model,
                                                    const std::string& effort,
                                                    bool show_reasoning) {
    a::List<a::ConfigOption> out;
    const auto choices = model_choices(cap);
    const std::string active_id = provider::active().catalog_id();
    const std::string pid = provider.empty() ? active_id : provider;

    if (!choices.empty()) {
        const std::string current = choice_value(pid, model);
        bool present = false;
        // Group by provider when there is more than one, so the dropdown
        // reads "Anthropic › Claude Opus" the way the TUI picker does.
        std::vector<std::string> order;
        std::map<std::string, a::ConfigSelectGroup> groups;
        for (const auto& c : choices) {
            if (c.value == current) present = true;
            auto [it, fresh] = groups.try_emplace(c.group);
            if (fresh) {
                order.push_back(c.group);
                it->second.group = c.group;
                it->second.name  = c.group_name;
            }
            it->second.options.push_back(a::ConfigSelectOption{c.value, c.name, a::Nothing, json::object()});
        }
        a::ConfigOption opt;
        opt.id       = "model";
        opt.name     = "Model";
        opt.category = a::Just<std::string>("model");
        opt.type     = "select";
        if (!present) {
            // A model not in any catalog (-m, a fine-tune): list it so the
            // dropdown shows what is actually running.
            a::ConfigSelectGroup& g = groups[pid];
            if (g.group.empty()) { g.group = pid; g.name = pid; order.push_back(pid); }
            g.options.insert(g.options.begin(),
                             a::ConfigSelectOption{current, model, a::Nothing, json::object()});
        }
        opt.currentValue = current;
        if (order.size() == 1) {
            opt.options = a::ConfigSelectOptions{a::CSO_Ungrouped{std::move(groups[order[0]].options)}};
        } else {
            a::List<a::ConfigSelectGroup> gs;
            for (const auto& g : order) gs.push_back(std::move(groups[g]));
            opt.options = a::ConfigSelectOptions{a::CSO_Grouped{std::move(gs)}};
        }
        out.push_back(std::move(opt));
    }

    // Reasoning effort, the model's own ladder plus auto and off.
    if (const auto ladder = effort_ladder(model, pid); !ladder.empty()) {
        a::List<a::ConfigSelectOption> opts;
        opts.push_back({"auto", "Auto", a::Just<std::string>("agentty picks per turn"), json::object()});
        opts.push_back({"none", "Off", a::Nothing, json::object()});
        for (Effort e : ladder) {
            std::string w{effort_wire(e)};
            std::string label = w;
            if (!label.empty()) label[0] = static_cast<char>(std::toupper(label[0]));
            opts.push_back({w, label, a::Nothing, json::object()});
        }
        a::ConfigOption opt;
        opt.id           = "effort";
        opt.name         = "Thinking";
        opt.description  = a::Just<std::string>("How hard the model reasons before answering");
        opt.category     = a::Just<std::string>("thought_level");
        opt.type         = "select";
        opt.currentValue = effective_effort(effort, ladder);
        opt.options      = a::ConfigSelectOptions{a::CSO_Ungrouped{std::move(opts)}};
        out.push_back(std::move(opt));

        // Show thinking: a boolean, so only for clients that render them.
        if (client_bool_config_.load(std::memory_order_relaxed)) {
            a::ConfigOption b;
            b.id          = "show_thinking";
            b.name        = "Show thinking";
            b.description = a::Just<std::string>("Stream the model's reasoning into the chat");
            b.category    = a::Just<std::string>("model_config");
            b.type        = "boolean";
            b.currentBool = show_reasoning;
            out.push_back(std::move(b));
        }
    }
    return out;
}

void AgentServer::replay_history(const Cap& cap, const std::string& session_id,
                                 const Thread& thread) {
    // Project-relative display paths in replayed tool cards, matching a live
    // turn. The session cwd is already set by the time replay runs.
    const std::string scwd =
        with_session(cap, session_id, [](Session& s) { return s.cwd; }).value_or(std::string{});

    // Send the whole replay as one write: replay emits many session/update
    // notifications back-to-back on the reader thread.
    peer_->batch(cap, [&] {
        replay_history_body(cap, session_id, thread, scwd);
    });
}

void AgentServer::replay_history_body(const Cap& cap, const std::string& session_id,
                                      const Thread& thread, const std::string& scwd) {

    // Replay is a synchronous fan-out to the wire on the reader thread (one
    // blocking write per message + per tool call), so an arbitrarily large
    // on-disk thread would block every other request for the whole replay.
    // Bound it: restore only the most recent kReplayMax messages (the rest
    // stay in thread.messages and are still sent to the model on the next
    // prompt — this only limits what's re-rendered in the client), and cap
    // the tool calls emitted per assistant message.
    constexpr std::size_t kReplayMax          = 200;
    constexpr std::size_t kReplayToolsPerMsg  = 100;
    const std::size_t total = thread.messages.size();
    const std::size_t start = total > kReplayMax ? total - kReplayMax : 0;
    const bool entity = client_compaction_.load(std::memory_order_relaxed);
    // Each compaction is shown where it happened: just before the first
    // message after its boundary.
    std::size_t next_cmp = 0;
    auto replay_compactions_before = [&](std::size_t mi) {
        while (entity && next_cmp < thread.compactions.size() &&
               thread.compactions[next_cmp].up_to_index <= mi) {
            const auto& rec = thread.compactions[next_cmp];
            a::List<a::ContentBlock> sum;
            sum.push_back(text_block(rec.summary));
            send_update(cap, session_id, a::SU_Compaction{
                "cmp_" + session_id + "_" + std::to_string(next_cmp + 1), "completed",
                a::Just(std::move(sum)), a::Nothing, json::object()});
            ++next_cmp;
        }
    };
    for (std::size_t mi = start; mi < total; ++mi) {
        replay_compactions_before(mi);
        const auto& m = thread.messages[mi];
        if (m.role == Role::User) {
            if (m.text.empty()) continue;
            send_update(cap, session_id, a::SU_UserMessageChunk{
                text_block(m.text), a::Just(a::MessageId{m.id.value})});
        } else if (m.role == Role::Assistant) {
            if (!m.thinking.empty())
                send_update(cap, session_id, a::SU_AgentThoughtChunk{
                    text_block(m.thinking), a::Just(a::MessageId{m.id.value})});
            const std::string body = m.text.empty() ? m.streaming_text : m.text;
            if (!body.empty())
                send_update(cap, session_id, a::SU_AgentMessageChunk{
                    text_block(body), a::Just(a::MessageId{m.id.value})});

            std::size_t emitted = 0;
            for (const auto& tc : m.tool_calls) {
                if (emitted++ >= kReplayToolsPerMsg) break;
                {
                    auto call = make_tool_call(tc, a::ToolCallStatus::Pending, scwd);
                    merge_meta(call.meta, air_tool_meta(tc));
                    send_update(cap, session_id, a::SU_ToolCall{std::move(call)});
                }

                const auto status = (tc.is_failed() || tc.is_rejected())
                                  ? a::ToolCallStatus::Failed : a::ToolCallStatus::Completed;
                a::ToolCallUpdate upd;
                upd.toolCallId = a::ToolCallId{tc.id.value};
                upd.status     = a::Just(status);
                const std::string out = tc.output();
                if (!out.empty()) {
                    a::List<a::ToolCallContent> content;
                    // Match the live path's per-tool result shaping (Read →
                    // line-numbered; others markdown-escaped) so a replayed
                    // session's cards look identical to when they first ran.
                    if (status == a::ToolCallStatus::Failed) {
                        std::string shown = out.find("```") == std::string::npos
                                          ? "```\n" + out + "\n```" : out;
                        content.push_back(a::ToolCallContent{
                            a::TCC_Content{text_block(std::move(shown)), json::object()}});
                    } else {
                        content.push_back(result_content_block(tc, out));
                    }
                    upd.content   = a::Just(std::move(content));
                    upd.rawOutput = a::Just<json>(json{{"text", out}});
                }
                send_update(cap, session_id, a::SU_ToolCallUpdate{std::move(upd)});
            }
        }
    }
    replay_compactions_before(total);
}

a::InitializeResult AgentServer::on_initialize(const Cap&, const a::InitializeParams& p) {
    a::InitializeResult r;
    // Remember whether the client can host terminals for us. Zed advertises
    // this; a bare/test client may not. Gates the live-terminal execution of
    // `bash` in run_tools (see client_supports_terminal_).
    client_supports_terminal_.store(p.clientCapabilities.terminal,
                                    std::memory_order_relaxed);
    // v1.11 session surfaces: notices, compaction entities and boolean config
    // options may only be sent to a client that said it renders them.
    client_notices_.store(p.clientCapabilities.session.notices, std::memory_order_relaxed);
    client_compaction_.store(p.clientCapabilities.session.compaction,
                             std::memory_order_relaxed);
    client_bool_config_.store(p.clientCapabilities.session.booleanConfig,
                              std::memory_order_relaxed);
    client_elicit_form_.store(p.clientCapabilities.elicitForm, std::memory_order_relaxed);
    client_elicit_url_.store(p.clientCapabilities.elicitUrl, std::memory_order_relaxed);
    // URL mode: MCP servers may ask for it, and their "done" goes to the editor.
    if (p.clientCapabilities.elicitUrl)
        // Weak: the callback is a process-wide slot, and a server reader can
        // take a copy of it just before ~AgentServer clears it.
        ::agentty::mcp::set_url_elicitation([weak = weak_from_this()](const std::string& id) {
            // Called by the MCP bridge from a server's reader thread when a
            // URL elicitation completes. That thread holds no agentty lock
            // (it is mcp-cpp's transport loop), so the notify re-states it
            // here -- a reviewed root (tests/lint/allowlist.txt).
            auto self = weak.lock();
            if (!self) return;
            try {
                maya::thread_root([&](const maya::unlocked& c) {
                    self->peer_->notify_raw(c, "elicitation/complete", json{{"elicitationId", id}});
                });
            } catch (...) {}
        });
    // Zed's terminal channel: a command tool call carries _meta.terminal_info,
    // its output streams as _meta.terminal_output and ends with terminal_exit,
    // and the editor renders a real terminal in the card. Works with our own
    // sandboxed execution, unlike client-hosted terminal/create.
    {
        const json& cm = p.clientCapabilities.meta;
        const auto flag = [&](const char* k) {
            return cm.is_object() && cm.contains(k) && cm[k].is_boolean() && cm[k].get<bool>();
        };
        client_terminal_meta_.store(flag("terminal_output") || flag("terminal_output_delta"),
                                    std::memory_order_relaxed);
        client_terminal_delta_.store(flag("terminal_output_delta"), std::memory_order_relaxed);
        client_terminal_auth_.store(flag("terminal-auth"), std::memory_order_relaxed);
        // JetBrains AIR declares {jetbrains:{air:{version>=1, capabilities:[...]}}}.
        // A malformed declaration enables nothing.
        if (cm.is_object() && cm.contains("jetbrains") && cm["jetbrains"].is_object()) {
            const json& air = cm["jetbrains"].value("air", json{});
            const bool ok = air.is_object() && air.contains("version")
                         && air["version"].is_number_integer() && air["version"].get<int>() >= 1
                         && air.contains("capabilities") && air["capabilities"].is_array();
            if (ok) {
                air_.store(true, std::memory_order_relaxed);
                for (const auto& c : air["capabilities"])
                    if (c == "diffPatch") air_diff_patch_.store(true, std::memory_order_relaxed);
            }
        }
    }
    // Negotiate to min(what we support, what the client offered). We advertise
    // up to kMaxProtocolVersion (v1 by default; v2 only if the acp-cpp build
    // opted into the Draft via -DACP_ENABLE_V2_DRAFT). A v1-only client still
    // lands on v1; a v2 client lands on v2 only when we were built for it.
    r.protocolVersion = a::negotiate_version(a::kMaxProtocolVersion, p.protocolVersion);
    // Latch the agreed version for the clean-cut config-surface gate (v2_config).
    negotiated_version_.store(r.protocolVersion, std::memory_order_relaxed);

    r.agentInfo = a::Just<a::ImplementationInfo>(
        {"agentty", a::Nothing, a::Just<std::string>(AGENTTY_VERSION)});

    auto& caps = r.agentCapabilities;
    caps.loadSession = true;
    caps.promptCapabilities.embeddedContext = true;
    // Images in prompts. Declared because the blocks are now DECODED into
    // the user message (prompt_parts_from_blocks) rather than dropped —
    // claiming this while discarding them would be worse than not claiming
    // it, since the client would show the attachment as sent.
    //
    // Whether the selected MODEL can see them is a separate question the
    // provider layer already answers (supports_vision / the org-policy
    // entitlement), and it degrades by withholding the image rather than
    // failing the turn. Audio stays undeclared: no provider we ship takes
    // it, and transcoding silently would be a lie about what was sent.
    caps.promptCapabilities.image = true;
    caps.auth.logout = a::Just(a::Unit{});
    caps.sessionCapabilities.list      = a::Just(a::Unit{});
    caps.sessionCapabilities.resume    = a::Just(a::Unit{});
    caps.sessionCapabilities.close     = a::Just(a::Unit{});
    caps.sessionCapabilities.fork      = a::Just(a::Unit{});   // unstable, used by Zed
    caps.sessionCapabilities.additionalDirectories = a::Just(a::Unit{});
    // JetBrains AIR: say which of its extensions we speak. Only to AIR.
    if (air_.load(std::memory_order_relaxed))
        r.meta = json{{"jetbrains", {{"air", {{"version", 1},
                       {"capabilities", json::array({"diffPatch"})}}}}}};
#if AGENTTY_MCP
    // The MCP bridge speaks stdio, Streamable HTTP and SSE, so the editor's
    // servers of every kind can be handed over.
    caps.mcpCapabilities.http = true;
    caps.mcpCapabilities.sse  = true;
#endif
    caps.sessionCapabilities.deleteCap = a::Just(a::Unit{});
    // Advertise generic JSON-RPC request cancellation ($/cancel_request,
    // stabilized 2026-06-29) via _meta — the spec carries extension
    // capabilities there. A client that sees this may cancel an in-flight
    // request by its id; we honour it for session/prompt requests.
    caps.meta["cancelRequest"] = true;

    // Advertise how to sign in when we have no credentials, so Zed renders a
    // proper "Sign in" affordance in the panel instead of erroring on the
    // first prompt. The method is informational — agentty's real login runs
    // out-of-band (`agentty login`) — but surfacing it matches the native
    // agent's sign-in UX. When already authenticated we advertise nothing, so
    // Zed shows the agent as ready.
    if (auth::is_empty(auth())) {
        // A terminal method lets the editor run `agentty login` in its own
        // terminal and then call authenticate, which re-reads what it saved.
        // Zed advertises it as auth.terminal (standard) or _meta.terminal-auth.
        if (p.clientCapabilities.authTerminal) {
            a::AuthMethod m;
            m.id          = "agentty-login";
            m.name        = "Sign in to agentty";
            m.description = a::Just<std::string>(
                "Opens a terminal to sign in with Claude, ChatGPT, Copilot or an API key.");
            m.type        = "terminal";
            m.args        = {"login"};
            r.authMethods.push_back(std::move(m));
        } else if (client_terminal_auth_.load(std::memory_order_relaxed)) {
            a::AuthMethod m;
            m.id          = "agentty-login";
            m.name        = "Sign in to agentty";
            m.description = a::Just<std::string>(
                "Opens a terminal to sign in with Claude, ChatGPT, Copilot or an API key.");
            m.meta = json{{"terminal-auth", {{"command", util::self_exe_path().string()},
                                             {"args", json::array({"login"})},
                                             {"label", "agentty login"}}}};
            r.authMethods.push_back(std::move(m));
        } else {
            a::AuthMethod m;
            m.id          = "agentty-login";
            m.name        = "Sign in to agentty";
            m.description = a::Just<std::string>(
                "Run `agentty login` in a terminal to authenticate your provider, "
                "then reconnect.");
            r.authMethods.push_back(std::move(m));
        }
    }
    return r;
}

a::NewSessionResult AgentServer::on_new_session(const Cap& cap, const a::NewSessionParams& p) {
    auto extra = extra_roots_of(p.additionalDirectories);   // throws on a bad path
    adopt_client_mcp(cap, p.mcpServers);
    ThreadId tid = persistence::new_id();
    const std::string sid = tid.value;
    const Profile session_profile = profile_;
    {
        Session s;
        s.id  = sid;
        s.cwd = p.cwd;
        s.extra_roots = std::move(extra);
        s.profile = profile_;
        s.thread.id = tid;
        s.thread.workspace = p.cwd;
        s.thread.title = p.cwd.empty() ? std::string{"ACP session"}
                                       : std::string{"ACP "} + p.cwd;
        sessions_.with(cap, [](Sessions& ss, Session v) {
            std::string k = v.id;
            ss.by_id.emplace(std::move(k), std::move(v));
        }, std::move(s));
    }
    persist(cap, sid);
    index_session(cap, sid);

    a::NewSessionResult r;
    r.sessionId = a::SessionId{sid};
    // v1 clients also get the mode as SessionModeState. The `mode` config
    // option below is sent on every connection, since Zed only reads modes
    // when no configOptions are present.
    if (!v2_config())
        r.modes = a::Just(modes_for_client(session_profile));
    // The selectors ride on the result too: JetBrains builds its model
    // dropdown from it and never looks at the update that follows.
    if (auto opts = config_options(cap, sid, model_id_); !opts.empty())
        r.configOptions = a::Just(std::move(opts));
    // Slash-command menu, and the same config as an update for clients that
    // only watch notifications.
    emit_session_config(cap, sid, model_id_);
    return r;
}

// session/fork: a new session whose history is a copy of another's, the
// editor's "fork from here". The source can be live or only on disk; the
// copy keeps its model, thinking and mode, and gets a fresh id and title.
a::NewSessionResult AgentServer::on_fork_session(const Cap& cap, const a::ForkSessionParams& p) {
    auto extra = extra_roots_of(p.additionalDirectories);
    adopt_client_mcp(cap, p.mcpServers);
    const std::string src = p.sessionId.value;
    if (src.empty()) throw a::RpcError(a::errc::InvalidParams, "session/fork: missing sessionId");

    struct Copy { Thread thread; Profile profile; std::string model, provider, effort; bool reasoning; };
    std::optional<Copy> copy = with_session(cap, src, [](Session& s) {
        return Copy{s.thread, s.profile, s.model, s.provider, s.effort, s.show_reasoning};
    });
    if (!copy) {
        auto loaded = persistence::load_thread_by_id(IoAccess::grant(), ThreadId{src});
        if (!loaded)
            throw a::RpcError(a::errc::InvalidParams, "session/fork: no such session: " + src,
                              json{{"sessionId", src}});
        copy = Copy{std::move(*loaded), profile_, "", "", "", true};
    }

    ThreadId tid = persistence::new_id();
    const std::string sid = tid.value;
    {
        Session s;
        s.id       = sid;
        s.cwd      = p.cwd;
        s.extra_roots = std::move(extra);
        s.profile  = copy->profile;
        s.model    = std::move(copy->model);
        s.provider = std::move(copy->provider);
        s.effort   = std::move(copy->effort);
        s.show_reasoning = copy->reasoning;
        s.thread   = std::move(copy->thread);
        s.thread.id = tid;
        s.thread.forked_from = src;
        if (!p.cwd.empty()) s.thread.workspace = p.cwd;
        s.thread.title = s.thread.title.empty() ? std::string{"Fork"}
                                                : "Fork of " + s.thread.title;
        sessions_.with(cap, [](Sessions& ss, Session v) {
            std::string k = v.id;
            ss.by_id.emplace(std::move(k), std::move(v));
        }, std::move(s));
    }
    persist(cap, sid);
    index_session(cap, sid);

    a::NewSessionResult r;
    r.sessionId = a::SessionId{sid};
    if (!v2_config())
        r.modes = a::Just(modes_for_client(copy->profile));
    if (auto opts = config_options(cap, sid, model_id_); !opts.empty())
        r.configOptions = a::Just(std::move(opts));
    emit_session_config(cap, sid, model_id_);
    const std::string title = with_session(cap, sid, [](Session& s) { return s.thread.title; })
                                  .value_or(std::string{});
    if (!title.empty())
        send_update(cap, sid, a::SU_SessionInfo{a::Just(title), a::Nothing, json::object()});
    return r;
}

a::LoadSessionResult AgentServer::on_load_session(const Cap& cap, const a::LoadSessionParams& p) {
    adopt_client_mcp(cap, p.mcpServers);
    std::string sid = p.sessionId.value;
    std::string cwd = p.cwd;
    // InvalidParams, not InternalError. Zed's client branches on the code
    // (agent_servers/src/acp.rs): AuthRequired opens the sign-in flow, any
    // other non-InternalError is surfaced to the user as-is, and
    // InternalError is treated as "the agent broke" — so reporting a client
    // mistake as InternalError blames us for their bad request and loses
    // the actionable message.
    if (sid.empty())
        throw a::RpcError(a::errc::InvalidParams, "session/load: missing sessionId");

    Thread thread;
    bool   from_memory = false;
    Profile profile = profile_;

    auto extra = extra_roots_of(p.additionalDirectories);
    // Live session: adopt the new cwd and roots, and copy the thread out.
    if (auto live = with_session(cap, sid, [](Session& s, std::string new_cwd,
                                         std::vector<std::string> roots) {
            if (!new_cwd.empty()) s.cwd = std::move(new_cwd);
            s.extra_roots = std::move(roots);
            return std::pair{s.thread, s.profile};
        }, cwd, extra)) {
        thread      = std::move(live->first);
        profile     = live->second;
        from_memory = true;
    }

    if (!from_memory) {
        // load_thread_by_id reads either on-disk format (the append log or
        // the legacy single document); a bare <id>.json read misses a thread
        // saved as a log.
        auto loaded = persistence::load_thread_by_id(IoAccess::grant(), ThreadId{sid});
        // A subagent run, opened from its parent's card.
        if (!loaded) loaded = persistence::load_subagent_thread(IoAccess::grant(), ThreadId{sid});
        if (!loaded)
            throw a::RpcError(a::errc::InvalidParams,
                              "session/load: no such session: " + sid,
                              json{{"sessionId", sid}});
        thread = std::move(*loaded);

        Session s;
        s.id = sid; s.cwd = cwd; s.profile = profile; s.thread = thread;
        s.extra_roots = std::move(extra);
        // try_emplace: a concurrent load of the same id may have made it live
        // since the check above (and queued a turn on it). Keep that one.
        sessions_.with(cap, [](Sessions& ss, Session v) {
            std::string k = v.id;
            ss.by_id.try_emplace(std::move(k), std::move(v));
        }, std::move(s));
    }

    replay_history(cap, sid, thread);
    // A fork made in the TUI points at its parent's transcript outside the
    // workspace; let the read tools reach exactly that file (#83).
    {
        std::vector<std::string> transcripts;
        for (const auto& msg : thread.messages)
            if (msg.fork_note && !msg.fork_transcript.empty())
                transcripts.push_back(msg.fork_transcript);
        if (!transcripts.empty())
            tools::util::allow_fork_transcripts(IoAccess::grant(), persistence::threads_dir(),
                                                transcripts);
    }
    // Re-advertise the slash-command menu + model picker for the restored
    // session, and push its title so Zed's sidebar entry is populated the same
    // as the native agent.
    emit_session_config(cap, sid, model_id_);
    if (!thread.title.empty())
        send_update(cap, sid, a::SU_SessionInfo{
            a::Just<std::string>(thread.title), a::Nothing, json::object()});

    a::LoadSessionResult r;
    if (!v2_config())
        r.modes = a::Just(modes_for_client(profile));
    if (auto opts = config_options(cap, sid, model_id_); !opts.empty())
        r.configOptions = a::Just(std::move(opts));
    return r;
}

void AgentServer::on_cancel(const Cap& cap, const a::CancelParams& p) {
    // Cancels the running turn and every prompt queued behind it. A prompt
    // sent after this notification is not affected.
    auto toks = with_session(cap, p.sessionId.value, [](Session& s) {
        std::vector<http::CancelTokenPtr> out;
        for (const auto& [rid, tok] : s.queued) out.push_back(tok);
        return out;
    });
    if (toks) cancel_tokens(cap, std::move(*toks));
}

void AgentServer::cancel_tokens(const maya::below<maya::leaf>& cap, std::vector<http::CancelTokenPtr> toks) {
    for (auto& t : toks) if (t) t->cancel();
    // Wake prompts waiting in acquire_turn so a cancelled one leaves the line.
    sessions_.with(cap, [](Sessions&) {});
}

void AgentServer::register_prompt(const maya::below<maya::leaf>& cap, const std::string& sid, const std::string& rid) {
    sessions_.with(cap, [](Sessions& ss, std::string id, std::string r) {
        auto it = ss.by_id.find(id);
        if (it == ss.by_id.end()) return;
        it->second.queued.emplace_back(r, std::make_shared<http::CancelToken>());
        ss.reqid_to_session[std::move(r)] = std::move(id);
    }, sid, rid);
}

std::optional<AgentServer::Acquired>
AgentServer::acquire_turn(const Cap& cap, const std::string& sid, const std::string& rid,
                          std::optional<Message> msg, std::string typed) {
    return sessions_.wait_with(cap,
        [](const Sessions& ss, const std::string& id, const std::string& r,
           const std::optional<Message>&, const std::string&) {
            auto it = ss.by_id.find(id);
            if (it == ss.by_id.end()) return true;
            const auto& q = it->second.queued;
            auto me = std::ranges::find(q, r, &std::pair<std::string, http::CancelTokenPtr>::first);
            if (me == q.end() || me->second->is_cancelled()) return true;
            return me == q.begin();
        },
        [](Sessions& ss, std::string id, std::string r, std::optional<Message> m,
           std::string typed_text) -> std::optional<Acquired> {
            auto it = ss.by_id.find(id);
            if (it == ss.by_id.end()) return std::nullopt;
            Session& s = it->second;
            auto me = std::ranges::find(s.queued, r,
                                        &std::pair<std::string, http::CancelTokenPtr>::first);
            if (me == s.queued.end()) return std::nullopt;
            if (me->second->is_cancelled()) {   // gave up while waiting
                s.queued.erase(me);
                ss.reqid_to_session.erase(r);
                return std::nullopt;
            }
            Acquired out{me->second, {}};
            s.cancel = me->second;
            if (m) {
                if (s.thread.messages.empty() && !typed_text.empty()) {
                    s.thread.title = persistence::title_from_first_message(typed_text);
                    out.title = s.thread.title;
                }
                s.thread.messages.push_back(std::move(*m));
            }
            return out;
        }, sid, rid, std::move(msg), std::move(typed));
}

void AgentServer::release_turn(const maya::below<maya::leaf>& cap, const std::string& sid, const std::string& rid) {
    sessions_.with(cap, [](Sessions& ss, std::string id, std::string r) {
        ss.reqid_to_session.erase(r);
        auto it = ss.by_id.find(id);
        if (it == ss.by_id.end()) return;
        Session& s = it->second;
        auto me = std::ranges::find(s.queued, r,
                                    &std::pair<std::string, http::CancelTokenPtr>::first);
        if (me == s.queued.end()) return;
        if (s.cancel == me->second) s.cancel.reset();
        s.queued.erase(me);
    }, sid, rid);
}

void AgentServer::on_cancel_request(const Cap& cap, const a::RpcId& id) {
    // Generic JSON-RPC $/cancel_request: the client wants to cancel the
    // request with this id. We only track in-flight session/prompt requests
    // (the only long-running ones), so map the id back to its session and
    // cancel that turn — identical effect to a session/cancel, just addressed
    // by request id instead of sessionId. Unknown ids (already-settled turns,
    // or non-prompt requests) are a no-op, which is spec-legal.
    auto tok = sessions_.with(cap, [](Sessions& ss, std::string rid) -> http::CancelTokenPtr {
        auto mit = ss.reqid_to_session.find(rid);
        if (mit == ss.reqid_to_session.end()) return nullptr;
        auto sit = ss.by_id.find(mit->second);
        if (sit == ss.by_id.end()) return nullptr;
        for (const auto& [r, t] : sit->second.queued) if (r == rid) return t;
        return nullptr;
    }, id.dump());
    if (tok) cancel_tokens(cap, std::vector<std::shared_ptr<http::CancelToken>>{tok});
}

// ── Session modes ───────────────────────────────────────────────────────────
const char* AgentServer::mode_id_for(Profile p) {
    switch (p) {
        case Profile::Ask:     return "ask";
        case Profile::Write:   return "write";
        case Profile::Minimal: return "minimal";
    }
    return "ask";
}

Profile AgentServer::profile_from_mode_id(const std::string& mode_id, Profile fallback) {
    if (mode_id == "ask")     return Profile::Ask;
    if (mode_id == "write")   return Profile::Write;
    if (mode_id == "minimal") return Profile::Minimal;
    return fallback;
}

a::SessionModeState AgentServer::mode_state(Profile current) {
    a::SessionModeState st;
    st.currentModeId = a::SessionModeId{mode_id_for(current)};
    st.availableModes = {
        a::SessionMode{a::SessionModeId{"ask"}, "Ask",
            a::Just<std::string>("Prompt before edits, commands, and network access")},
        a::SessionMode{a::SessionModeId{"write"}, "Write",
            a::Just<std::string>("Edit files and run commands without prompting; still prompt for risky ops")},
        a::SessionMode{a::SessionModeId{"minimal"}, "Minimal",
            a::Just<std::string>("Prompt for everything, including file reads")},
    };
    return st;
}

// mode_state plus AIR's mode kind: Write still asks for risky ops, so it is
// auto_review; the others ask first.
a::SessionModeState AgentServer::modes_for_client(Profile current) const {
    auto st = mode_state(current);
    for (auto& m : st.availableModes)
        m.meta = air_meta(json{{"kind", m.id.value == "write" ? "auto_review" : "standard"}});
    return st;
}

void AgentServer::on_set_mode(const Cap& cap, const a::SetModeParams& p) {
    const auto applied = with_session(cap, p.sessionId.value, [](Session& s, std::string mode) {
        s.profile = profile_from_mode_id(mode, s.profile);
        return s.profile;
    }, p.modeId.value);
    if (!applied)
        throw a::RpcError(a::errc::InvalidParams,
                          "session/set_mode: unknown sessionId: " + p.sessionId.value,
                          json{{"sessionId", p.sessionId.value}});
    send_update(cap, p.sessionId.value,
        a::SU_CurrentMode{a::SessionModeId{mode_id_for(*applied)}, json::object()});
    // Keep the `mode` config option in step for clients that render that.
    emit_config_state(cap, p.sessionId.value, model_id_);
}

a::SetConfigOptionResult AgentServer::on_set_config_option(const Cap& cap, const a::SetConfigOptionParams& p) {
    const auto unknown = [&p] {
        return a::RpcError(a::errc::InvalidParams,
                           "session/set_config_option: unknown sessionId: "
                               + p.sessionId.value,
                           json{{"sessionId", p.sessionId.value}});
    };
    if (!with_session(cap, p.sessionId.value, [](Session&) { return true; })) throw unknown();

    const auto bad = [&p](std::string why, json supported = nullptr) {
        json data{{"configId", p.configId}, {"value", p.value}};
        if (!supported.is_null()) data["supported"] = std::move(supported);
        return a::RpcError(a::errc::InvalidParams,
                           "session/set_config_option: " + why, std::move(data));
    };
    if (p.configId != "show_thinking" && !p.value.is_string())
        throw bad(p.configId + " takes a string value");
    const std::string value = p.value_string();

    if (p.configId == "model") {
        // A value from our list is "<provider>\t<model>"; a bare id (an older
        // client, or a hand-written one) means a model on the active provider.
        auto [prov, model] = split_choice(value);
        if (model.empty()) throw bad("empty model");
        const auto choices = model_choices(cap);
        const bool listed = std::ranges::any_of(choices, [&](const ModelChoice& c) {
            return c.value == value || (prov.empty() && c.model == model);
        });
        if (!listed && !choices.empty())
            throw bad("model '" + model + "' is not on any signed-in provider");
        if (!prov.empty() && prov != provider::active().catalog_id() && !stream_to_)
            throw bad("this agent can only use the active provider's models");
        if (prov == provider::active().catalog_id()) prov.clear();
        if (!with_session(cap, p.sessionId.value,
                          [](Session& s, std::string pv, std::string m) {
                              s.provider = std::move(pv);
                              s.model    = std::move(m);
                              return true;
                          }, std::move(prov), std::move(model)))
            throw unknown();
    } else if (p.configId == "effort") {
        const auto cur = with_session(cap, p.sessionId.value, [](Session& s) {
            return std::pair{s.provider, s.model};
        });
        if (!cur) throw unknown();
        const std::string model = cur->second.empty() ? model_id_ : cur->second;
        const auto ladder = effort_ladder(model, cur->first.empty()
                                                    ? provider::active().catalog_id() : cur->first);
        json ok = json::array({"auto", "none"});
        for (Effort e : ladder) ok.push_back(std::string{effort_wire(e)});
        if (std::ranges::find(ok, json(value)) == ok.end())
            throw bad("this model doesn't take effort '" + value + "'", ok);
        with_session(cap, p.sessionId.value, [](Session& s, std::string v) {
            s.effort = v == "auto" ? std::string{} : std::move(v);
            return true;
        }, value);
    } else if (p.configId == "show_thinking") {
        if (!p.is_bool()) throw bad("show_thinking takes a boolean");
        with_session(cap, p.sessionId.value, [](Session& s, bool on) {
            s.show_reasoning = on;
            return true;
        }, p.value_bool());
    } else if (p.configId == "mode") {
        // Permission profile, shared with the legacy session/set_mode path.
        // Validate against the known ids so a bad value is an error, not a
        // silent Ask fallback that looks like success.
        if (value != "ask" && value != "write" && value != "minimal")
            throw bad("mode must be ask|write|minimal",
                      json::array({"ask", "write", "minimal"}));
        const auto got = with_session(cap, p.sessionId.value, [](Session& s, std::string mode) {
            s.profile = profile_from_mode_id(mode, s.profile);
            return s.profile;
        }, value);
        if (!got) throw unknown();
        const Profile applied = *got;
        // v2 clients see the change in the config echo below; a v1 client
        // also keeps a SessionModeState, so update that too.
        if (!v2_config())
            send_update(cap, p.sessionId.value,
                a::SU_CurrentMode{a::SessionModeId{mode_id_for(applied)}, json::object()});
    } else {
        // An unknown config id is an error, not a no-op that looks like success.
        throw bad("unknown configId '" + p.configId + "'",
                  json::array({"mode", "model", "effort", "show_thinking"}));
    }

    // Echo the COMPLETE new config state, in the result (the spec requires
    // it; Zed and JetBrains reconcile their selectors from it) and as an
    // update for clients that only watch notifications.
    a::SetConfigOptionResult r{config_options(cap, p.sessionId.value, model_id_), json::object()};
    emit_config_state(cap, p.sessionId.value, model_id_);
    return r;
}

// The editor calls this after running our terminal login method (or any time
// it wants to check). Re-read the credential store: the login just wrote it
// from another process, and our copy is from startup.
void AgentServer::on_authenticate(const a::AuthenticateParams& p) {
    if (p.methodId != "agentty-login")
        throw a::RpcError(a::errc::InvalidParams,
                          "authenticate: unknown methodId '" + p.methodId + "'");
    if (auth::is_empty(auth()) && reauth_) {
        auth::AuthHeader fresh = reauth_();
        auth_.store(fresh);
        // The model list depends on which providers are signed in.
        model_choices_.publish(nullptr);
    }
    if (auth::is_empty(auth()))
        throw a::RpcError(a::AuthRequired,
                          "still not signed in. Run `agentty login` and try again.");
}

void AgentServer::on_logout(const Cap&) {
    auth::clear_credentials(IoAccess::grant());   // ACP handler, not a reducer
    auth_.store(auth::AuthHeader{auth::ApiKeyHeader{""}});
}

// ── Session lifecycle: list / resume / close / delete ────────────────────────
// acp_sessions.json is read-modify-written by every ACP instance (one per
// editor window), so it goes through with_shared_file like the thread index:
// the in-process lane plus the cross-process file lock.
namespace {
std::filesystem::path session_index_path() {
    return persistence::threads_dir() / "acp_sessions.json";
}
persistence::Lane& session_index_lane() {
    static persistence::Lane l;
    return l;
}
json read_session_index(const char* where) {
    std::ifstream ifs(session_index_path());
    if (!ifs) return json::object();
    try { json j; ifs >> j; if (j.is_object()) return j; }
    catch (const std::exception& e) { util::dbglog(where, e.what()); }
    catch (...) { util::dbglog(where, "non-std exception"); }
    return json::object();
}
void write_session_index(const json& j) {
    // A session title/cwd derived from an untrusted ACP `cwd` can contain
    // invalid UTF-8; the default dump() throws type_error(316) on that.
    // Substitute U+FFFD instead so indexing can never crash the worker.
    (void)persistence::write_json_atomic(IoAccess::grant(), 
        session_index_path(), j.dump(-1, ' ', false, json::error_handler_t::replace));
}
}  // namespace

json AgentServer::load_session_index(const Cap& cap) {
    return persistence::with_shared_file(cap, session_index_lane(), session_index_path(),
        [](const maya::held<lock_levels::file_lane>&, persistence::Held) {
            return read_session_index("acp.load_session_index");
        });
}

void AgentServer::index_session(const Cap& cap, const std::string& id) {
    auto meta = with_session(cap, id, [](Session& s) { return std::pair{s.cwd, s.thread.title}; });
    if (!meta) return;
    persistence::with_shared_file(cap, session_index_lane(), session_index_path(),
        [](const maya::held<lock_levels::file_lane>&, persistence::Held, std::string key,
           json entry) {
            json j = read_session_index("acp.index_session");
            j[key] = std::move(entry);
            write_session_index(j);
        }, id, json{
            {"cwd", meta->first},
            {"title", meta->second},
            {"updatedAt", std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
        });
}

void AgentServer::unindex_session(const Cap& cap, const std::string& id) {
    persistence::with_shared_file(cap, session_index_lane(), session_index_path(),
        [](const maya::held<lock_levels::file_lane>&, persistence::Held, std::string key) {
            json j = read_session_index("acp.unindex_session");
            if (!j.contains(key)) return;
            j.erase(key);
            write_session_index(j);
        }, id);
}

a::ListSessionsResult AgentServer::on_list_sessions(const Cap& cap, const a::ListSessionsParams& p) {
    std::string filter_cwd = p.cwd.value_or("");
    json index = load_session_index(cap);

    // Live sessions not yet in the on-disk index (cwd, title).
    const auto live = sessions_.read(cap, [](const Sessions& ss) {
        std::vector<std::array<std::string, 3>> out;
        for (const auto& [id, s] : ss.by_id) out.push_back({id, s.cwd, s.thread.title});
        return out;
    });
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& [id, cwd, title] : live)
        if (!index.contains(id))
            index[id] = json{{"cwd", cwd}, {"title", title}, {"updatedAt", now_s}};

    struct Row { std::string id, cwd, title; std::int64_t at; };
    std::vector<Row> rows;
    for (auto it = index.begin(); it != index.end(); ++it) {
        const json& meta = it.value();
        std::string cwd = meta.value("cwd", "");
        if (cwd.empty() || (!filter_cwd.empty() && cwd != filter_cwd)) continue;
        const auto t = meta.find("title");
        const auto u = meta.find("updatedAt");
        rows.push_back({it.key(), std::move(cwd),
                        t != meta.end() && t->is_string() ? t->get<std::string>() : "",
                        u != meta.end() && u->is_number_integer() ? u->get<std::int64_t>() : 0});
    }
    // Newest first: editors show this as "recent chats".
    std::ranges::sort(rows, [](const Row& x, const Row& y) {
        return x.at != y.at ? x.at > y.at : x.id < y.id;
    });

    // Opaque cursor = offset into that order. Pages of 100.
    constexpr std::size_t kPage = 100;
    std::size_t from = 0;
    if (p.cursor) {
        try { from = std::stoul(*p.cursor); } catch (...) {
            throw a::RpcError(a::errc::InvalidParams, "session/list: bad cursor");
        }
    }
    a::ListSessionsResult result;
    for (std::size_t i = from; i < rows.size() && i < from + kPage; ++i) {
        Row& r = rows[i];
        a::SessionInfo info;
        info.sessionId = a::SessionId{std::move(r.id)};
        info.cwd       = std::move(r.cwd);
        if (!r.title.empty()) info.title = a::Just<std::string>(std::move(r.title));
        if (r.at > 0) info.updatedAt = a::Just<std::string>(iso8601(r.at));
        result.sessions.push_back(std::move(info));
    }
    if (from + kPage < rows.size())
        result.nextCursor = a::Just<std::string>(std::to_string(from + kPage));
    return result;
}

a::ResumeSessionResult AgentServer::on_resume_session(const Cap& cap, const a::ResumeSessionParams& p) {
    return on_load_session(cap, p);   // restore + replay; same result shape
}

// Drop a session and cancel every turn it owns (the running one and any
// queued behind it). Erasing it alone left a running turn streaming and
// running tools until its next step boundary noticed the session was gone,
// and session/delete's file removal then raced that turn's saves.
void AgentServer::drop_session(const Cap& cap, const std::string& sid) {
    auto toks = sessions_.with(cap, [](Sessions& ss, std::string id) {
        std::vector<std::shared_ptr<http::CancelToken>> out;
        auto it = ss.by_id.find(id);
        if (it == ss.by_id.end()) return out;
        for (const auto& [rid, tok] : it->second.queued) out.push_back(tok);
        ss.by_id.erase(it);
        return out;
    }, sid);
    cancel_tokens(cap, std::move(toks));
}

void AgentServer::on_close_session(const Cap& cap, const a::CloseSessionParams& p) {
    drop_session(cap, p.sessionId.value);
}

void AgentServer::on_delete_session(const Cap& cap, const a::DeleteSessionParams& p) {
    drop_session(cap, p.sessionId.value);
    persistence::delete_thread(IoAccess::grant(), cap, ThreadId{p.sessionId.value});
    unindex_session(cap, p.sessionId.value);
}

// ── Prompt + turn loop ───────────────────────────────────────────────────────
void AgentServer::on_prompt(const Cap& cap, const a::PromptParams& p, Responder resp) {
    std::string sid  = p.sessionId.value;
    auto parts = prompt_parts_from_blocks(p.prompt);
    std::string text = std::move(parts.text);
    const std::string typed_raw = text;   // before any /command expansion

    // Local OpenAI-compatible backends (Ollama, llama.cpp) need no key, so
    // an empty auth is legitimate for them. Only demand credentials for a
    // hosted (TLS) endpoint or the Anthropic path.
    {
        const auto& sel = provider::active();
        const bool keyless_local =
            sel.kind == provider::Kind::OpenAI && !sel.openai_endpoint.use_tls;
        const bool known = with_session(cap, sid, [](Session&) { return true; }).has_value();
        if (!known) {
            resp.error(cap, a::errc::InvalidParams, "unknown sessionId: " + sid);
            return;
        }
        if (auth::is_empty(auth()) && !keyless_local) {
            resp.error(cap, a::AuthRequired,
                "agentty has no credentials — run `agentty login` first");
            return;
        }
    }

    // Slash commands from the menu we advertise. These are actions, not
    // prompts: sending "/compact" to the model would just get a chat reply.
    {
        std::string_view t = text;
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.remove_prefix(1);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))  t.remove_suffix(1);
        if (t == "/new") {
            // A fresh context in the same session: the client keeps its tab.
            with_session(cap, sid, [](Session& s) {
                s.thread.messages.clear();
                s.thread.compactions.clear();
                s.warned_context = false;
                return true;
            });
            persist(cap, sid);
            send_update(cap, sid, a::SU_AgentMessageChunk{
                text_block("Started a fresh conversation. Earlier messages are no longer "
                           "sent to the model."), a::Nothing});
            resp.ok(cap, a::PromptResult{a::StopReason::EndTurn, json::object()});
            return;
        }
        if (t == "/compact" || t.starts_with("/compact ")) {
            std::string rid = jsonrpc::to_json(resp.id()).dump();
            register_prompt(cap, sid, rid);
            util::run_isolated_detached("acp.compact",
                [](const maya::unlocked& cap, std::stop_token, maya::co_owned<AgentServer> self,
                   std::string sid, std::string rid, Responder r) {
                    self->run_compact(IoAccess::grant(), cap, std::move(sid), std::move(rid),
                                      std::move(r));
                }, maya::co_owned<AgentServer>::of(*this), std::move(sid), std::move(rid),
                std::move(resp));
            return;
        }
        // A user slash command expands into its prompt template; a skill
        // splices in its instructions; an MCP prompt renders. One resolver
        // (runtime/slash.hpp) decides which, the same one the TUI uses.
        if (t.starts_with('/')) {
            const Io io = IoAccess::grant();
            const slash::Catalog cat = slash::discover(io);
            bool active = false;
            if (const auto hit = slash::resolve(t, cat);
                hit.what == slash::Resolved::What::Skill) {
                const Thread snap = with_session(cap, sid, [](Session& s) {
                    return s.thread;
                }).value_or(Thread{});
                active = tools::skills::is_active_in(hit.entry->name, ::agentty::visible_text(snap));
            }
            if (auto out = slash::expand_checked(io, t, cat, active)) {
                // A prompt missing a required argument: tell the user what to
                // type and end the turn. Sent to the model, the note would be
                // answered as if it were a question.
                if (out->usage) {
                    send_update(cap, sid, a::SU_AgentMessageChunk{text_block(std::move(out->text)), a::Nothing});
                    resp.ok(cap, a::PromptResult{a::StopReason::EndTurn, json::object()});
                    return;
                }
                text = std::move(out->text);
            }
        }
    }
    // Title from what the user typed ("/review src"), not the expanded template.
    std::string typed = typed_raw;
    Message um;
    um.role   = Role::User;
    um.text   = std::move(text);
    um.images = std::move(parts.images);

    // Get in line now, so a session/cancel or $/cancel_request that arrives
    // before the worker starts still finds this prompt. The message joins the
    // thread when its turn comes, so a queued prompt never lands mid-turn.
    std::string req_id_dump = jsonrpc::to_json(resp.id()).dump();
    register_prompt(cap, sid, req_id_dump);

    // Run the whole turn off the reader thread; the engine stays free to
    // deliver our outbound permission responses. The Responder resolves the
    // deferred session/prompt when the turn settles. The job co-owns the
    // server, so a turn still running at exit can't outlive it.
    util::run_isolated_detached("acp.turn_worker",
        [](const maya::unlocked& cap, std::stop_token, maya::co_owned<AgentServer> self,
           std::string sid, std::string rid, Responder r, Message m, std::string typed) {
            self->run_turn(IoAccess::grant(), cap, std::move(sid), std::move(rid), std::move(r),
                           std::move(m), std::move(typed));
        }, maya::co_owned<AgentServer>::of(*this), std::move(sid),
        std::move(req_id_dump), std::move(resp), std::move(um), std::move(typed));
}

namespace {
// A call a weak local model leaked as text, salvaged into a tool call.
bool is_salvaged_id(const std::string& id) {
    return std::string_view{id}.starts_with("call_salvaged_");
}
constexpr std::string_view kMemoryToolNames[] = {"remember", "forget", "wipe_memory"};
}  // namespace

// /compact: summarise the thread so far and record it, so later turns send
// the summary instead of the full history. The transcript itself is kept
// (the client still shows it), same as the TUI. Reported as a v1.11
// compaction entity when the client renders those, as plain text otherwise.
void AgentServer::run_compact(Io, const Cap& cap, std::string sid, std::string rid, Responder resp) {
  struct Release {
      AgentServer& s; const Cap& c; const std::string& sid; const std::string& rid;
      ~Release() { s.release_turn(c, sid, rid); }
  } release{*this, cap, sid, rid};
  try {
    const auto settle = [&] { release_turn(cap, sid, rid); };
    if (!acquire_turn(cap, sid, rid)) {
        settle();
        if (with_session(cap, sid, [](Session&) { return true; }))
            resp.ok(cap, a::PromptResult{a::StopReason::Cancelled, json::object()});
        else
            resp.error(cap, a::errc::InvalidParams, "unknown sessionId: " + sid);
        return;
    }
    struct Snap {
        Thread thread;
        std::string model, provider;
        http::CancelTokenPtr cancel;
    };
    auto snap = with_session(cap, sid, [](Session& s) {
        return Snap{s.thread, s.model, s.provider, s.cancel};
    });
    if (!snap) { settle(); resp.error(cap, a::errc::InvalidParams, "unknown sessionId: " + sid); return; }
    const std::size_t up_to = snap->thread.messages.size();
    if (up_to < 2) {
        settle();
        send_update(cap, sid, a::SU_AgentMessageChunk{
            text_block("Nothing to compact yet."), a::Nothing});
        resp.ok(cap, a::PromptResult{a::StopReason::EndTurn, json::object()});
        return;
    }

    const bool entity = client_compaction_.load(std::memory_order_relaxed);
    const std::string cid = "cmp_" + sid + "_" + std::to_string(snap->thread.compactions.size() + 1);
    const auto started = std::chrono::steady_clock::now();
    // AIR's compaction facts, sent with each status.
    const auto facts = [&](json extra) {
        extra["trigger"] = "manual";
        extra["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        return air_meta(json{{"contextCompaction", std::move(extra)}});
    };
    if (entity) send_update(cap, sid, a::SU_Compaction{cid, "in_progress", a::Nothing, a::Nothing,
                                                  air_meta(json{{"contextCompaction", {{"version", 1}, {"trigger", "manual"}}}})});

    provider::Request req;
    req.model      = snap->model.empty() ? model_id_ : snap->model;
    req.max_tokens = max_output_tokens_for(req.model);
    req.cancel     = snap->cancel;
    req.auth       = auth();
    req.messages   = app::cmd::compaction_request_messages(
        snap->thread, static_cast<int>(ui::context_max_for_model(req.model)));

    std::string summary, error;
    provider::EventSink sink = [&](Msg m) {
        std::visit([&](auto&& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, msg::StreamMsg>) {
                std::visit([&](auto&& ev) {
                    using T = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<T, StreamTextDelta>) {
                        summary += ev.text;
                        if (entity)
                            send_update(cap, sid, a::SU_CompactionSummaryChunk{
                                cid, text_block(ev.text), json::object()});
                    } else if constexpr (std::is_same_v<T, StreamError>) {
                        error = ev.message;
                    }
                }, domain);
            }
        }, m);
    };
    try {
        if (!snap->provider.empty() && stream_to_) stream_to_(cap, snap->provider, std::move(req), sink);
        else                                        stream_(cap, std::move(req), sink);
    } catch (const std::exception& e) { error = e.what(); }

    const bool cancelled = snap->cancel && snap->cancel->is_cancelled();
    summary = app::cmd::summary_from_reply(summary);
    if (cancelled || !error.empty() || summary.empty()) {
        settle();
        if (entity)
            send_update(cap, sid, a::SU_Compaction{cid, cancelled ? "cancelled" : "failed", a::Nothing,
                        cancelled ? a::Nothing : a::Just<std::string>(error.empty() ? "empty summary" : error),
                        facts(cancelled ? json{{"version", 1}}
                                        : json{{"version", 1}, {"error", error.empty() ? "empty summary" : error}})});
        if (cancelled) { resp.ok(cap, a::PromptResult{a::StopReason::Cancelled, json::object()}); return; }
        resp.error(cap, a::errc::InternalError, "compaction failed: " + (error.empty() ? std::string{"empty summary"} : error));
        return;
    }

    // Record it. Messages that arrived while summarising are past up_to and
    // stay on the wire verbatim.
    const auto before_after = with_session(cap, sid, [](Session& s, std::size_t upto, std::string text) {
        Thread::CompactionRecord rec;
        rec.up_to_index = std::min(upto, s.thread.messages.size());
        rec.summary     = std::move(text);
        rec.tokens_before = app::cmd::estimate_wire_tokens(s.thread);
        s.thread.compactions.push_back(std::move(rec));
        s.thread.compactions.back().tokens_after = app::cmd::estimate_wire_tokens(s.thread);
        s.warned_context = false;
        return std::pair{s.thread.compactions.back().tokens_before,
                         s.thread.compactions.back().tokens_after};
    }, up_to, summary);
    settle();
    persist(cap, sid);
    index_session(cap, sid);

    if (entity) {
        // The streamed chunks were the raw reply; this replaces them with the
        // clean summary (the spec's authoritative replacement).
        a::List<a::ContentBlock> clean;
        clean.push_back(text_block(summary));
        json f{{"version", 1}};
        if (before_after) {
            f["preTokens"]  = before_after->first;
            f["postTokens"] = before_after->second;
        }
        send_update(cap, sid, a::SU_Compaction{cid, "completed", a::Just(std::move(clean)),
                                          a::Nothing, facts(std::move(f))});
    } else {
        std::string msg = "Compacted the conversation.";
        if (before_after && before_after->first > 0)
            msg += " About " + std::to_string(before_after->first / 1000) + "k tokens down to " +
                   std::to_string(before_after->second / 1000) + "k.";
        send_update(cap, sid, a::SU_AgentMessageChunk{text_block(std::move(msg)), a::Nothing});
    }
    resp.ok(cap, a::PromptResult{a::StopReason::EndTurn, json::object()});
  } catch (const std::exception& e) {
    resp.error(cap, a::errc::InternalError, std::string{"compaction failed: "} + e.what());
  } catch (...) {
    resp.error(cap, a::errc::InternalError, "compaction failed");
  }
}

void AgentServer::run_turn(Io io, const Cap& cap, std::string session_id, std::string req_id_dump,
                           Responder resp, Message user_msg, std::string typed) {
  // Hand the session to the next queued prompt however this one ends.
  struct Release {
      AgentServer& s; const Cap& c; const std::string& sid; const std::string& rid;
      ~Release() { s.release_turn(c, sid, rid); }
  } release{*this, cap, session_id, req_id_dump};
  // This runs on a DETACHED worker thread, entirely outside acp-cpp's
  // handle_request try/catch. An uncaught throw here would call
  // std::terminate() and kill every session in the process; and a return
  // without resolving `resp` leaves the client's session/prompt future
  // hanging forever (the Responder destructor does not auto-reply). So the
  // whole body is guarded: any escape resolves the deferred prompt with an
  // InternalError. The ok/error done-latch makes the trailing resp.ok(cap, ...)
  // a no-op if we already errored, so exactly one reply is sent.
  try {
    // Wait for any turn already running in this session, then add our
    // message. Cancelled while queued: answer cancelled, never run.
    auto got = acquire_turn(cap, session_id, req_id_dump, std::move(user_msg), std::move(typed));
    if (!got) {
        if (with_session(cap, session_id, [](Session&) { return true; }))
            resp.ok(cap, a::PromptResult{a::StopReason::Cancelled, json::object()});
        else
            resp.error(cap, a::errc::InvalidParams, "unknown sessionId: " + session_id);
        return;
    }
    if (!got->title.empty())
        send_update(cap, session_id, a::SU_SessionInfo{
            a::Just<std::string>(got->title), a::Nothing, json::object()});

    bool cancelled = false;
    bool errored = false;
    std::string error_msg;
    StopReason last_stop = StopReason::EndTurn;

    // Doom-loop guard for weak local models: count salvaged (leaked-JSON)
    // tool rounds across the turn. After kMaxSalvaged of them — or any
    // salvaged memory-tool call — force the turn to finish in plain text
    // instead of looping (the TUI's dedup_releaked_salvage_calls analog).
    constexpr int kMaxSalvaged = 3;
    int salvaged_rounds = 0;
    bool force_text_retry = false;   // next sub-turn: re-prompt with NO tools
    bool budget_done = false;        // the one final tool-free retry has fired

    for (int turn = 0; turn < 64; ++turn) {
        // A concurrent session/close|delete ends the turn at the next step.
        if (!with_session(cap, session_id, [](Session&) { return true; })) {
            errored = true; error_msg = "session vanished"; break;
        }

        StopReason stop = stream_completion(io, cap, session_id, cancelled, error_msg,
                                            force_text_retry);
        last_stop = stop;
        force_text_retry = false;
        if (!error_msg.empty()) { errored = true; break; }
        if (cancelled) break;
        if (stop != StopReason::ToolUse) break;

        // Inspect the just-streamed assistant message's tool calls. A leaked
        // call from a weak local model carries the call_salvaged_ id. Memory
        // tools (remember/forget/wipe) leaked on a non-explicit turn are junk,
        // and once the salvage budget is spent it's a doom loop. In both cases
        // we FAIL the leaked calls without running them, feed that failure
        // back as a tool result (so the wire tool_use↔result pairing stays
        // valid), and let the model take ONE more sub-turn to answer in plain
        // text — instead of returning a blank bubble.
        {
            // Fail leaked calls in the live session under the lock, and
            // report which ones so the client cards follow, unlocked.
            struct Salvage { bool any = false, mem_leak = false;
                             std::vector<std::string> failed; };
            const auto sv = with_session(cap, session_id,
                [](Session& cs, bool fail_now) -> Salvage {
                    Salvage out;
                    if (cs.thread.messages.empty()) return out;
                    Message& back = cs.thread.messages.back();
                    if (back.role != Role::Assistant) return out;
                    for (const auto& tc : back.tool_calls) {
                        if (!is_salvaged_id(tc.id.value)) continue;
                        out.any = true;
                        for (auto t : kMemoryToolNames)
                            if (t == tc.name.value) out.mem_leak = true;
                    }
                    if (!(fail_now || out.mem_leak)) return out;
                    for (auto& tc : back.tool_calls) {
                        if (!is_salvaged_id(tc.id.value) || tc.is_terminal()) continue;
                        tc.status = ToolUse::Failed{
                            {}, {}, "this call was not run — do NOT call tools, "
                            "answer the user in plain text now"};
                        out.failed.push_back(tc.id.value);
                    }
                    return out;
                }, salvaged_rounds + 1 > kMaxSalvaged);
            if (sv && sv->any) {
                ++salvaged_rounds;
                const bool budget_blown = salvaged_rounds > kMaxSalvaged;
                if (sv->mem_leak || budget_blown) {
                    for (const auto& id : sv->failed) {
                        a::ToolCallUpdate u;
                        u.toolCallId = a::ToolCallId{id};
                        u.status     = a::Just(a::ToolCallStatus::Failed);
                        send_update(cap, session_id, a::SU_ToolCallUpdate{std::move(u)});
                    }
                    // Over budget: one final tool-free retry so the model
                    // answers in plain text instead of an empty bubble. The
                    // budget_done latch makes it fire exactly once.
                    if (budget_blown) {
                        if (budget_done) { last_stop = StopReason::EndTurn; break; }
                        budget_done = true;
                        force_text_retry = true;
                        continue;
                    }
                    if (!sv->failed.empty()) { force_text_retry = true; continue; }
                }
            }
        }

        bool ran = run_tools(cap, session_id, cancelled);
        if (cancelled || !ran) break;
    }

    // Persist if the session still exists. The cancel handle and the reqid
    // mapping go when the turn is released.
    const bool alive = with_session(cap, session_id, [](Session&) { return true; }).value_or(false);
    if (alive) {
        persist(cap, session_id);
        index_session(cap, session_id);
        // Bump the sidebar's "last active" for this chat.
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        send_update(cap, session_id, a::SU_SessionInfo{
            a::Nothing, a::Just<std::string>(iso8601(now_s)), json::object()});
    }

    a::PromptResult result;
    result.stopReason = acp_stop_reason(last_stop, cancelled, errored);
    // A failed turn is an error with its reason, not a stop reason: `refusal`
    // would tell the client the model declined and hide why. Cancellation
    // still wins, as the spec requires.
    if (errored && !cancelled) {
        resp.error(cap, a::errc::InternalError,
                   error_msg.empty() ? std::string{"turn failed"} : error_msg);
        return;
    }
    resp.ok(cap, result);
  } catch (const std::exception& e) {
    util::dbglog("acp.run_turn", e.what());
    resp.error(cap, a::errc::InternalError,
               std::string{"turn failed: "} + e.what());
  } catch (...) {
    util::dbglog("acp.run_turn", "non-std exception");
    resp.error(cap, a::errc::InternalError, "turn failed (unknown error)");
  }
}

StopReason AgentServer::stream_completion(Io io, const Cap& cap, const std::string& session_id,
                                          bool& out_cancelled,
                                          std::string& out_error,
                                          bool suppress_tools) {
    provider::Request req;
    // Snapshot what the request needs ONCE. A model change mid-turn applies
    // to the NEXT turn; the stream itself runs with no lock held.
    struct Snap {
        std::string model, cwd, provider, effort;
        bool show_reasoning;
        http::CancelTokenPtr cancel;
        std::vector<Message> messages;
    };
    auto snap = with_session(cap, session_id, [](Session& s) {
        // The wire view: after /compact the summary replaces the prefix.
        return Snap{s.model, s.cwd, s.provider, s.effort, s.show_reasoning, s.cancel,
                    app::cmd::wire_messages_for(s.thread)};
    });
    if (!snap) { out_error = "session vanished"; return StopReason::Unspecified; }
    const std::string sess_model = snap->model;
    req.model         = sess_model.empty() ? model_id_ : sess_model;
    {
        // Reasoning effort: the session's choice, clamped to the model.
        // Unset means auto, the same per-model default the TUI uses.
        const auto caps = resolved_caps(req.model, snap->provider);
        const Effort e = snap->effort == "none" ? Effort::None
                       : snap->effort.empty()   ? Effort::Auto
                                                : effort_from_wire(snap->effort);
        req.effort         = std::string{effort_wire_for(e, caps)};
        req.show_reasoning = snap->show_reasoning && !req.effort.empty();
    }
    // Per-model output-token ceiling (mirrors cmd_factory::launch_stream).
    // Without this the default 16384 is shared across reasoning + tool JSON
    // and a large `edit` truncates mid-input_json. context_window is left 0
    // here (no per-session /api/show probe on the ACP path); the Ollama
    // transport then applies its safe agent-sized num_ctx default, still 4x
    // Ollama's truncating floor.
    req.max_tokens    = max_output_tokens_for(req.model);
    // Product-level prompt policy is shared with the TUI request builder:
    // hosted models get the complete agent/tool/RAG contract, while constrained
    // local endpoints get the compact profile.
    {
        const auto sel = provider::active();
        req.system_prompt = provider::system_prompt_for(io, sel);
        // First-class weak-model support (agent-zero style): weak models on
        // the Ollama native endpoint use the JSON-protocol path (inline tool
        // catalog, single {tool_name,tool_args} object) instead of the native
        // tools array. Mirrors cmd_factory's launch_stream.
        req.json_protocol = is_weak_model(req.model)
            && sel.kind == provider::Kind::OpenAI
            && sel.openai_endpoint.native_api;
    }
    req.cancel        = snap->cancel;
    req.auth          = auth();
    req.messages      = std::move(snap->messages);
    // suppress_tools: a weak local model that just leaked a junk tool call
    // gets a tool-free retry. With no tools advertised it cannot leak another
    // and answers the user in plain text (proven: qwen2.5-coder:7b greets
    // cleanly only when no tools are on the wire).
    if (!suppress_tools) {
        std::string_view newest_user;
        for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
            if (it->role == Role::User && !it->is_proactive_context()) {
                newest_user = it->text;
                break;
            }
        }
        auto selected = tools::select_wire_tools(newest_user);
        // Weak local models hallucinate `skill` and the memory tools on
        // greetings/small talk — don't advertise them (mirrors cmd_factory).
        const bool weak = is_weak_model(req.model);
        auto weak_hidden = [](std::string_view n) {
            return n == "skill" || n == "remember"
                || n == "forget" || n == "wipe_memory";
        };
        req.tools.reserve(selected.size());
        for (const auto* tool : selected) {
            if (weak && weak_hidden(tool->name.value)) continue;
            req.tools.push_back({tool->name.value, tool->description,
                                 tool->input_schema, tool->eager_input_streaming});
        }
    }

    Message assistant;
    assistant.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    // Argument JSON per tool call, keyed by id. A provider may interleave
    // several calls' deltas and send every End at the end of the turn
    // (OpenAI chat does), so one shared buffer gave every call but the last
    // empty args (#116).
    std::unordered_map<std::string, std::string> tool_json;
    auto find_call = [&](const ToolCallId& id) -> ToolUse* {
        for (auto it = assistant.tool_calls.rbegin(); it != assistant.tool_calls.rend(); ++it)
            if (it->id == id) return &*it;
        // An id-less event (a provider that never names calls) means the
        // latest call, which was the old behaviour.
        if (id.value.empty() && !assistant.tool_calls.empty())
            return &assistant.tool_calls.back();
        return nullptr;
    };
    StreamUsage last_usage;
    bool have_usage = false;
    int last_err_status = 0;
    std::optional<std::chrono::seconds> last_retry_after;
    bool last_err_replayable = true;
    const std::string sid = session_id;
    const std::string scwd = snap->cwd;
    const std::string msg_id = assistant.id.value;

    auto sink = [&](agentty::Msg m) {
        std::visit([&](auto&& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, msg::StreamMsg>) {
                std::visit([&](auto&& ev) {
                    using E = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<E, StreamTextDelta>) {
                        assistant.text += ev.text;
                        send_update(cap, sid, a::SU_AgentMessageChunk{
                            text_block(ev.text), a::Just(a::MessageId{msg_id})});
                    } else if constexpr (std::is_same_v<E, StreamThinkingDelta>) {
                        // Adaptive/extended thinking (effort on): forward the
                        // visible summary text as an ACP thought chunk so Zed
                        // renders its collapsible "Thinking" section, matching
                        // claude-code-acp. The opaque signature stays internal
                        // (replayed to the provider, never shown to the client).
                        // Signed blocks are kept verbatim per block, the same
                        // as the TUI: Anthropic rejects a tool turn whose
                        // thinking block was dropped or merged.
                        auto open_block = [&]() -> Message::ThinkingBlock& {
                            if (assistant.thinking_blocks.empty())
                                assistant.thinking_blocks.emplace_back();
                            return assistant.thinking_blocks.back();
                        };
                        if (ev.block_boundary && !assistant.thinking_blocks.empty()
                            && (!assistant.thinking_blocks.back().text.empty()
                                || !assistant.thinking_blocks.back().signature.empty()
                                || !assistant.thinking_blocks.back().redacted_data.empty())) {
                            assistant.thinking_blocks.emplace_back();
                            if (!assistant.thinking.empty()) assistant.thinking += "\n\n";
                        }
                        if (!ev.redacted_data.empty())
                            open_block().redacted_data = std::move(ev.redacted_data);
                        if (!ev.signature.empty()) {
                            open_block().signature += ev.signature;
                            assistant.thinking_signature = open_block().signature;
                        }
                        if (!ev.text.empty()) {
                            open_block().text += ev.text;
                            assistant.thinking += ev.text;   // replayed on load
                            send_update(cap, sid, a::SU_AgentThoughtChunk{
                                text_block(ev.text), a::Just(a::MessageId{msg_id})});
                        }
                    } else if constexpr (std::is_same_v<E, StreamReasoning>) {
                        // Codex/Responses encrypted reasoning: replayed on the
                        // next request, tagged with the site that minted it.
                        if (!ev.encrypted.empty()) {
                            assistant.reasoning_site = ev.site;
                            if (!assistant.reasoning_encrypted.empty())
                                assistant.reasoning_encrypted.push_back('\n');
                            assistant.reasoning_encrypted += ev.encrypted;
                        }
                    } else if constexpr (std::is_same_v<E, StreamToolUseStart>) {
                        ToolUse tc;
                        tc.id = ev.id; tc.name = ev.name;
                        assistant.tool_calls.push_back(std::move(tc));
                        tool_json[ev.id.value].clear();
                        a::ToolCall call;
                        call.toolCallId = a::ToolCallId{ev.id.value};
                        // Args haven't streamed yet, so this is the kind-based
                        // fallback ("Read", "Write", "Terminal", …) — never the
                        // bare lowercase tool name. The SU_ToolCallUpdate on
                        // StreamToolUseEnd refines it with the arg detail.
                        call.title      = tool_title(assistant.tool_calls.back(), scwd);
                        call.name       = a::Just<std::string>(ev.name.value);
                        call.kind       = acp_tool_kind(ev.name.value);
                        call.status     = a::ToolCallStatus::Pending;
                        // Zed creates the terminal from the FIRST tool_call;
                        // output and exit then stream on later updates.
                        if (is_command_tool(ev.name.value) && terminal_channel())
                            call.meta = json{{"terminal_info",
                                              {{"terminal_id", ev.id.value}, {"cwd", scwd}}}};
                        send_update(cap, sid, a::SU_ToolCall{std::move(call)});
                    } else if constexpr (std::is_same_v<E, StreamToolUseDelta>) {
                        if (ToolUse* tcp = find_call(ev.id)) tool_json[tcp->id.value] += ev.partial_json;
                    } else if constexpr (std::is_same_v<E, StreamToolUseSnapshot>) {
                        if (ToolUse* tcp = find_call(ev.id)) tool_json[tcp->id.value] = ev.json;   // replaces
                    } else if constexpr (std::is_same_v<E, StreamObservedToolResult>) {
                        // An external agent already ran it; run_tools skips
                        // terminal calls so it isn't run twice.
                        if (ToolUse* tcp = find_call(ev.id)) {
                            const auto now = std::chrono::steady_clock::now();
                            std::string out = ev.output;
                            if (out.empty())
                                out = ev.failed ? "External agent tool failed."
                                                : "Executed by external agent.";
                            if (ev.failed) tcp->settle_failed(now, std::move(out));
                            else           tcp->settle_done(now, std::move(out));
                        }
                    } else if constexpr (std::is_same_v<E, StreamToolUseEnd>) {
                        if (ToolUse* tcp = find_call(ev.id)) {
                            auto& tc = *tcp;
                            const std::string& raw = tool_json[tc.id.value];
                            try {
                                tc.args = raw.empty()
                                    ? json::object() : json::parse(raw);
                                // `cmd` → `command` etc. so the ACP card
                                // title/content (tool_title, command_content)
                                // read the canonical key — the client only
                                // sees tc.args, never the raw stream JSON.
                                (void)::agentty::app::detail::canonify_tool_args(
                                    tc.name.value, tc.args);
                            } catch (const std::exception& e) {
                                util::dbglog("acp.tool_args.parse", e.what());
                                tc.args = json::object();
                            } catch (...) {
                                util::dbglog("acp.tool_args.parse", "non-std exception");
                                tc.args = json::object();
                            }
                            a::ToolCallUpdate upd;
                            upd.toolCallId = a::ToolCallId{tc.id.value};
                            upd.title      = a::Just(tool_title(tc, scwd));
                            upd.rawInput   = a::Just<json>(tc.args);
                            upd.locations  = a::Just(tool_locations(tc, scwd));
                            // Now that args have fully streamed in, an
                            // edit/write can show a speculative diff card
                            // immediately (before permission/execution), and a
                            // terminal call can show its FULL command (the
                            // title is clipped to one line).
                            a::List<a::ToolCallContent> c;
                            if (auto d = announce_diff(tc, scwd)) c.push_back(std::move(*d));
                            if (auto cc = command_content(tc))    c.push_back(std::move(*cc));
                            if (auto pc = prompt_content(tc))     c.push_back(std::move(*pc));
                            if (!c.empty()) upd.content = a::Just(std::move(c));
                            merge_meta(upd.meta, air_tool_meta(tc));
                            send_update(cap, sid, a::SU_ToolCallUpdate{std::move(upd)});

                            // A `todo` write drives Zed's native plan/checklist
                            // widget (claude-code-acp parity) — emit the plan the
                            // moment the list is known, in addition to the tool
                            // card above.
                            if (auto plan = plan_from_todos(tc))
                                send_update(cap, sid, std::move(*plan));
                        }
                    } else if constexpr (std::is_same_v<E, StreamFinished>) {
                        stop = ev.stop_reason;
                    } else if constexpr (std::is_same_v<E, StreamError>) {
                        out_error = ev.message;
                        last_err_status = ev.http_status;
                        last_retry_after = ev.retry_after;
                        last_err_replayable = !ev.non_replayable;
                    } else if constexpr (std::is_same_v<E, StreamUsage>) {
                        last_usage = ev; have_usage = true;
                    }
                }, domain);
            }
        }, m);
    };

    // Retry a request that failed before anything reached the editor, when
    // the failure is the kind that passes (overload, rate limit, a dropped
    // connection) and resending can't duplicate work. Same rule as the TUI:
    // once text or a tool call streamed, the turn is the user's to retry.
    constexpr int kMaxAttempts = 4;
    for (int attempt = 1;; ++attempt) {
        provider::Request this_req = req;
        try {
            if (!snap->provider.empty() && stream_to_)
                stream_to_(cap, snap->provider, std::move(this_req), sink);
            else
                stream_(cap, std::move(this_req), sink);
        } catch (const std::exception& e) {
            out_error = std::string{"stream backend: "} + e.what();
        }
        if (out_error.empty() || attempt >= kMaxAttempts) break;
        if (snap->cancel && snap->cancel->is_cancelled()) break;
        const bool streamed = !assistant.text.empty() || !assistant.thinking.empty()
                           || !assistant.tool_calls.empty();
        const auto cls = provider::classify_stream_error(out_error, last_err_status);
        if (streamed || !last_err_replayable
            || (cls != provider::ErrorClass::Transient && cls != provider::ErrorClass::RateLimit))
            break;
        auto wait = std::chrono::seconds{1 << (attempt - 1)};   // 1, 2, 4 s
        if (last_retry_after) wait = std::clamp(*last_retry_after, std::chrono::seconds{1},
                                                std::chrono::seconds{60});
        notice(cap, sid, "warning",
               cls == provider::ErrorClass::RateLimit ? "Rate limited, retrying"
                                                      : "Provider error, retrying",
               out_error + " (attempt " + std::to_string(attempt + 1) + " of " +
                   std::to_string(kMaxAttempts) + " in " + std::to_string(wait.count()) + "s)");
        // Wait on the cancel itself: a stop ends the wait at once.
        if (maya::delay_for(cap, snap->cancel ? snap->cancel->token() : std::stop_token{}, wait))
            break;
        out_error.clear();
        last_err_status = 0;
        last_retry_after.reset();
        stop = StopReason::Unspecified;
    }

    if (snap->cancel && snap->cancel->is_cancelled()) out_cancelled = true;

    if (have_usage) {
        const std::string model = sess_model.empty() ? model_id_ : sess_model;
        long long used = static_cast<long long>(last_usage.input_tokens) +
                         last_usage.cache_creation_input_tokens +
                         last_usage.cache_read_input_tokens +
                         last_usage.output_tokens;
        a::SU_Usage u;
        u.used = used < 0 ? 0 : used;
        u.size = static_cast<std::int64_t>(ui::context_max_for_model(model));
        // Warn once per session as the window passes 85%: the next turns are
        // where answers get cut off.
        if (u.size > 0 && u.used * 100 >= u.size * 85) {
            const bool first = with_session(cap, session_id, [](Session& s) {
                return !std::exchange(s.warned_context, true);
            }).value_or(false);
            if (first)
                notice(cap, sid, "warning", "Context window is nearly full",
                       std::to_string(u.used * 100 / u.size) +
                       "% used. Run /compact, or start a new thread.");
        }
        send_update(cap, sid, std::move(u));
    }
    if (!out_error.empty() && !out_cancelled)
        notice(cap, sid, "error", "The model request failed", out_error);

    // Structural mutation of the shared messages vector — lock the
    // per-session thread mutex so a concurrent reader-thread snapshot
    // (session/load|resume replay, persist) can't observe a half-grown
    // vector or race the reallocation. Held only for the push, not the
    // stream above.
    with_session(cap, session_id,
                 [](Session& s, Message m) { s.thread.messages.push_back(std::move(m)); return true; },
                 std::move(assistant));
    return stop;
}

void AgentServer::settle_tool(const Cap& cap, const TurnCtx& ctx, ToolUse& tc, ToolUse::Status st,
                              std::chrono::steady_clock::time_point executing_since) {
    if (executing_since != std::chrono::steady_clock::time_point{}) {
        if (auto* d = std::get_if<ToolUse::Done>(&st))   d->executing_since = executing_since;
        if (auto* f = std::get_if<ToolUse::Failed>(&st)) f->executing_since = executing_since;
    }
    tc.status = st;
    // Write it back by (message id, tool id): a session closed mid-turn, or a
    // message that moved, just means nothing to update.
    with_session(cap, ctx.id, [](Session& s, std::string mid, std::string tid, ToolUse::Status v) {
        for (auto it = s.thread.messages.rbegin(); it != s.thread.messages.rend(); ++it) {
            if (it->id.value != mid) continue;
            for (auto& t : it->tool_calls)
                if (t.id.value == tid) { t.status = std::move(v); return true; }
            return false;
        }
        return false;
    }, ctx.msg_id, tc.id.value, std::move(st));
}

bool AgentServer::run_tools(const Cap& cap, const std::string& session_id, bool& out_cancelled) {
    // Copy out the batch and what the gate needs, once. The profile is one
    // stable value for the whole batch; a mode change mid-turn takes effect
    // next turn. Tools run on the copies with no lock held, and each settled
    // status is written back through settle_tool.
    struct Batch {
        TurnCtx ctx;
        Profile profile = Profile::Ask;
        std::vector<ToolUse> calls;
        std::set<std::string> grants;
    };
    auto batch = with_session(cap, session_id, [](Session& s) -> std::optional<Batch> {
        if (s.thread.messages.empty()) return std::nullopt;
        const Message& last = s.thread.messages.back();
        if (last.role != Role::Assistant || last.tool_calls.empty()) return std::nullopt;
        return Batch{TurnCtx{s.id, s.cwd, last.id.value, s.cancel, s.extra_roots},
                     s.profile, last.tool_calls, s.grants};
    });
    if (!batch || !*batch) return false;
    const TurnCtx ctx = (*batch)->ctx;
    const Profile profile = (*batch)->profile;
    auto& grants = (*batch)->grants;

    // Tool-scheduling note: unlike the TUI path (cmd_factory), which uses
    // effects::is_parallel_safe(active, want) to fan out compose-safe tools
    // (Pure/ReadFs/Net) and serialise exclusive ones (WriteFs/Exec), the ACP
    // path runs every tool call STRICTLY SERIALLY in emission order. Serial
    // execution is a subset of the parallel-safety rule, so it can never
    // violate the exclusivity the effect set demands; it just forgoes the
    // latency win of overlapping independent reads. Results land in emission
    // order for the wire tool_use/result pairing, and Zed renders the cards
    // sequentially anyway.
    auto set_status = [&](ToolUse& tc, ToolUse::Status st) { settle_tool(cap, ctx, tc, std::move(st)); };

    for (auto& tc : (*batch)->calls) {
        if (ctx.cancelled()) { out_cancelled = true; return false; }
        if (tc.is_terminal()) continue;   // settled by an external agent

        // Resolve the tool ONCE so the permission gate and the execution run
        // against the same definition (an mcp/tools/list_changed mid-turn can
        // otherwise re-resolve the name to a ToolDef with different effects).
        const tools::ToolDef* td =
            tool::DynamicDispatch::resolve(tc.name.value);

        bool needs_perm =
            tool::DynamicDispatch::needs_permission_with(td, profile)
            && !grants.contains(tc.name.value);
        // "Always reject" from earlier this session: refused without asking.
        const bool denied_always = needs_perm && with_session(cap, ctx.id, [](Session& s, std::string n) {
            return s.denials.contains(n);
        }, tc.name.value).value_or(false);
        if (needs_perm) {
            PermissionOutcome outcome = PermissionOutcome::Deny;
            if (!denied_always) {
                a::ToolCallUpdate u;
                u.toolCallId = a::ToolCallId{tc.id.value};
                u.status     = a::Just(a::ToolCallStatus::Pending);
                send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(u)});
                outcome = ask_permission(cap, ctx.id, tc,
                                         ctx.cancel ? ctx.cancel->token() : std::stop_token{});
            }
            if (ctx.cancelled()) { out_cancelled = true; return false; }
            if (outcome == PermissionOutcome::AllowAlways) {
                grants.insert(tc.name.value);
                with_session(cap, ctx.id, [](Session& s, std::string n) { s.grants.insert(std::move(n)); return true; },
                             tc.name.value);
            }
            if (outcome == PermissionOutcome::DenyAlways)
                with_session(cap, ctx.id, [](Session& s, std::string n) { s.denials.insert(std::move(n)); return true; },
                             tc.name.value);
            if (outcome == PermissionOutcome::Deny || outcome == PermissionOutcome::DenyAlways) {
                set_status(tc, ToolUse::Rejected{});
                a::ToolCallUpdate r;
                r.toolCallId = a::ToolCallId{tc.id.value};
                r.status     = a::Just(a::ToolCallStatus::Failed);
                send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(r)});
                continue;
            }
        }

        a::ToolCallUpdate running;
        running.toolCallId = a::ToolCallId{tc.id.value};
        running.status     = a::Just(a::ToolCallStatus::InProgress);
        send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(running)});

        // Live-terminal fast path: when the client hosts terminals and our
        // sandbox isn't wrapping commands, run `bash` through the client's ACP
        // terminal so Zed streams stdout into its native terminal widget (the
        // claude-code-acp UX) instead of us capturing static text. Emits its
        // own completion update + sets tc.status; nullopt means "not eligible,
        // use internal execution below".
        if (auto tr = run_bash_via_terminal(cap, ctx, tc)) {
            if (tr->cancelled) { out_cancelled = true; return false; }
            continue;
        }

        // Run the tool on a worker so a session/cancel that lands mid-tool
        // (on the engine's reader thread) makes THIS turn loop return
        // promptly instead of blocking until a long bash/web_fetch finishes.
        // The tool itself isn't interruptible (its interface is synchronous),
        // so on cancel we drop its result — the same "discard the late
        // output" contract the TUI path uses via its idempotent tool-output
        // guard.
        //
        // pool.submit_isolated, not the standard async helper: that helper's
        // future joins in its destructor, so abandoning a cancelled tool used
        // to need a detached "tool_reaper" thread just to hold the future
        // somewhere it could afford to block. The pool's future drops
        // cleanly, and the job stays owned (waited for at teardown).
        //
        // `wake` is the one thing this loop waits on. Two sources stop it:
        // the tool finishing (the job requests it on the way out) and the
        // session's cancel (a stop_callback bridges it). Both are events, so
        // the wait below wakes the instant either happens — it used to poll
        // the future every 50 ms.
        std::stop_source wake;
        // The skills the model can already see, derived from this session's
        // visible transcript (copied out under the sessions lock),
        // then handed to the job — the `skill` tool runs there and has no
        // Thread. Only the skill tool reads it, so only it pays to derive.
        std::vector<std::string> active_skills;
        if (tc.name.value == "skill") {
            active_skills = with_session(cap, ctx.id, [](Session& s) {
                return tools::skills::active_in(::agentty::visible_text(s.thread));
            }).value_or(std::vector<std::string>{});
        }
        tools::CallContext call_ctx;
        call_ctx.active_skills = std::move(active_skills);
        // The session's cancel reaches the running tool itself (a shell is
        // stopped), not only this wait.
        if (ctx.cancel) call_ctx.cancel.push_back(ctx.cancel->token());
        // This session's folders: relative paths and shells start in its cwd,
        // and it may reach its cwd plus the editor's extra roots, even when
        // the agent process was started somewhere else.
        if (!ctx.cwd.empty()) {
            call_ctx.project = ctx.cwd;
            if (!tools::util::is_within_workspace(ctx.cwd))
                call_ctx.extra_roots.push_back(ctx.cwd);
        }
        for (const auto& r : ctx.extra_roots) call_ctx.extra_roots.push_back(r);
        // The callbacks below co-own the server, not borrow it: a tool that is
        // cancelled or times out is abandoned on the pool and can still call
        // them after this turn (and its handle) are gone.
        const auto self = maya::co_owned<AgentServer>::of(*this);
        // An MCP server asking the user something mid-call goes to the
        // editor's form UI, tied to this session and tool card.
        if (client_elicit_form_.load(std::memory_order_relaxed)
            || client_elicit_url_.load(std::memory_order_relaxed))
            call_ctx.elicit = [self, sid = ctx.id, tid = tc.id.value,
                               tok = ctx.cancel ? ctx.cancel->token() : std::stop_token{}](const json& mcp) {
                // Called by the MCP bridge on the tool's call thread (an MCP
                // server's request, relayed synchronously), which holds no
                // agentty lock: a reviewed root (tests/lint/allowlist.txt).
                return maya::thread_root([&](const maya::unlocked& c) {
                    return self->elicit_via_client(c, sid, tid, mcp, tok);
                });
            };
        // A subagent run is kept as its own session so Zed can show it
        // nested under this card (_meta.subagent_session_info). Set once by
        // the subagent's completion, read after the tool returns: a snapshot.
        auto sub_sid = std::make_shared<maya::snapshot<std::string>>();
        if (tc.name.value == "task")
            call_ctx.on_subagent_done = [sub_sid, parent = ctx.id](const Thread& t) {
                Thread copy = t;
                copy.id = persistence::new_id();
                copy.forked_from = parent;
                if (copy.title.empty()) copy.title = "Subagent";
                if (persistence::save_subagent_thread(IoAccess::grant(), copy))
                    sub_sid->store(copy.id.value);
            };
        // Zed's terminal channel: a shell command's output streams into a real
        // terminal in the card while it runs. progress hands us the whole
        // buffer each time; we send what's new.
        const bool term = is_command_tool(tc.name.value) && terminal_channel();
        if (term) {
            auto sent = std::make_shared<std::size_t>(0);
            // Progress is reported from the tool's worker thread, which holds
            // no agentty lock; the notify (a leaf: the peer's outbox) re-states
            // it at this one callback (tests/lint/allowlist.txt).
            call_ctx.progress = [self, sid = ctx.id, tid = tc.id.value, sent](std::string_view all) {
                if (all.size() <= *sent) return;
                maya::thread_root([&](const maya::unlocked& c) {
                    self->send_terminal_output(c, sid, tid, all.substr(*sent));
                });
                *sent = all.size();
            };
        } else {
            // Any other tool that reports progress (a subagent's live log, a
            // test run, a shell in an editor without terminals): show the
            // latest snapshot in the card while it runs. At most ~6 updates a
            // second; the final result replaces it when the tool settles.
            struct Throttle { std::chrono::steady_clock::time_point last{}; std::size_t sent = 0; };
            auto th = std::make_shared<Throttle>();
            call_ctx.progress = [self, sid = ctx.id, tid = tc.id.value, th](std::string_view all) {
                const auto now = std::chrono::steady_clock::now();
                if (all.size() == th->sent || now - th->last < std::chrono::milliseconds(150)) return;
                th->last = now;
                th->sent = all.size();
                // Tail only: a card is not a log viewer, and a 1 MB snapshot
                // per update would swamp the pipe.
                constexpr std::size_t kTail = 8 * 1024;
                std::string_view tail = all.size() > kTail ? all.substr(all.size() - kTail) : all;
                a::ToolCallUpdate u;
                u.toolCallId = a::ToolCallId{tid};
                u.status = a::Just(a::ToolCallStatus::InProgress);
                a::List<a::ToolCallContent> c;
                c.push_back(a::ToolCallContent{a::TCC_Content{
                    text_block("```\n" + tools::util::sanitize_utf8(tail) + "\n```"), json::object()}});
                u.content = a::Just(std::move(c));
                maya::thread_root([&](const maya::unlocked& rc) {   // tool worker: no lock held
                    self->send_update(rc, sid, a::SU_ToolCallUpdate{std::move(u)});
                });
            };
        }
        // The job gets its own copy of the definition it was gated against.
        // `abandon` stops the tool itself when this loop gives up on it at
        // the deadline. Without it a timed-out shell/process kept running
        // after its result was thrown away, and since ACP runs tools in
        // order, the NEXT tool (an edit, another shell) ran concurrently with
        // it on the same files, against the strictly-serial contract below.
        std::stop_source abandon;
        call_ctx.cancel.push_back(abandon.get_token());
        auto fut = util::background_pool().submit_isolated(
            [](const maya::unlocked& job_cap, std::stop_token pool_stop,
               std::optional<tools::ToolDef> def,
               std::string name, nlohmann::json args, std::stop_source wake,
               tools::CallContext call_ctx) {
                struct Signal {
                    std::stop_source& s;
                    ~Signal() { s.request_stop(); }
                } on_exit{wake};
                // Process teardown stops the pool: the tool must see it too.
                call_ctx.cancel.push_back(std::move(pool_stop));
                return tool::DynamicDispatch::execute_with(job_cap, def ? &*def : nullptr,
                                                           name, args, call_ctx);
            },
            td ? std::optional<tools::ToolDef>{*td} : std::nullopt,
            tc.name.value, tc.args, wake, std::move(call_ctx));
        std::optional<std::stop_callback<std::function<void()>>> on_cancel;
        if (ctx.cancel)
            on_cancel.emplace(ctx.cancel->token(),
                              std::function<void()>{[wake]() mutable {
                                  wake.request_stop();
                              }});

        // Belt-and-suspenders wall-clock ceiling: a tool that hangs forever
        // (e.g. a stalled network read with no internal timeout) plus a
        // client that never sends session/cancel would otherwise pin this
        // worker — and the session/prompt future — permanently. On the
        // deadline we fail the tool and move on.
        constexpr auto kToolDeadline = std::chrono::minutes(10);
        const auto tool_start = std::chrono::steady_clock::now();
        const bool woke = maya::delay_for(cap, wake.get_token(), kToolDeadline);
        on_cancel.reset();

        // Finished beats cancelled: a tool that completed in the same instant
        // the user cancelled has a real result, and reporting it is honest.
        const bool finished =
            fut.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
        const bool tool_cancelled =
            !finished && woke && ctx.cancelled();
        const bool tool_timed_out = !finished && !woke;

        if (tool_cancelled || tool_timed_out) {
            // Drop the future, but stop the tool first: the job keeps running
            // on the pool until it notices (a shell is killed, a fetch
            // aborted), is waited for at teardown, and its late result is
            // discarded. A cancel already reached it via ctx.cancel.
            abandon.request_stop();
            const char* why = tool_timed_out ? "timed out" : "cancelled";
            if (tool_timed_out) util::dbglog("acp.run_tools.timeout", tc.name.value);
            settle_tool(cap, ctx, tc, ToolUse::Failed{{}, {}, why}, tool_start);
            a::ToolCallUpdate c;
            c.toolCallId = a::ToolCallId{tc.id.value};
            c.status     = a::Just(a::ToolCallStatus::Failed);
            send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(c)});
            if (tool_cancelled) { out_cancelled = true; return false; }
            // A timeout fails just this tool; continue the turn so the model
            // can react to the failure result.
            continue;
        }

        // A pool that is already stopping (process teardown) never runs the
        // job and breaks the promise. Fail this tool rather than the turn.
        std::optional<tool::ExecResult> got;
        try {
            got = fut.get();
        } catch (const std::exception& e) {
            util::dbglog("acp.run_tools.submit", e.what());
            set_status(tc, ToolUse::Failed{{}, {}, std::string{"could not run tool: "} + e.what()});
            a::ToolCallUpdate f;
            f.toolCallId = a::ToolCallId{tc.id.value};
            f.status     = a::Just(a::ToolCallStatus::Failed);
            send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(f)});
            continue;
        }
        auto result = std::move(*got);

        a::ToolCallUpdate upd;
        upd.toolCallId = a::ToolCallId{tc.id.value};

        if (result) {
            a::List<a::ToolCallContent> content;
            if (result->change) {
                const auto& ch = *result->change;
                a::TCC_Diff diff;
                // Raw absolute path — must match the announce diff's path so Zed
                // associates the completion with the same card (claude-code-acp
                // keys diffs by the raw file_path in both phases). The
                // authoritative diff is the real on-disk before/after.
                diff.path    = ch.path;
                diff.newText = ch.new_contents;
                if (!ch.original_contents.empty())
                    diff.oldText = a::Just<std::string>(ch.original_contents);
                // AIR clients that asked for it get one exact git patch; the
                // text fields become the placeholders its contract specifies.
                if (air_diff_patch_.load(std::memory_order_relaxed)) {
                    if (auto patch = air_git_patch(ch)) {
                        diff.oldText = a::Nothing;
                        diff.newText.clear();
                        diff.meta = air_meta(json{{"diffPatch", {{"version", 1},
                                                    {"format", "git_patch"},
                                                    {"text", *patch}}}});
                    }
                }
                content.push_back(a::ToolCallContent{std::move(diff)});

                // Anchor the card at the first changed line so Zed can jump to
                // it (per-hunk line numbers, like claude-code-acp's
                // structuredPatch locations).
                a::List<a::ToolCallLocation> locs;
                for (const auto& h : ch.hunks) {
                    a::ToolCallLocation loc;
                    loc.path = ch.path;
                    if (h.new_start > 0)
                        loc.line = a::Just<std::int64_t>(static_cast<std::int64_t>(h.new_start));
                    locs.push_back(std::move(loc));
                }
                if (locs.empty()) {
                    a::ToolCallLocation loc; loc.path = ch.path;
                    locs.push_back(std::move(loc));
                }
                upd.locations = a::Just(std::move(locs));
            }
            // When a diff card is shown, the diff IS the result — don't also
            // repeat the "Edited (3+ 1-)" summary as a text block (that's noise
            // in Zed; claude-code-acp returns no extra content for Edit/Write).
            // The summary still rides in rawOutput for the model.
            if (!result->change && !result->text.empty())
                content.push_back(result_content_block(tc, result->text));

            upd.status    = a::Just(a::ToolCallStatus::Completed);
            upd.content   = a::Just(std::move(content));
            upd.rawOutput = a::Just<json>(json{{"text", result->text}});
            if (term) upd.meta = terminal_exit_meta(tc.id.value, result->text, false);
            if (auto s = sub_sid->get(); !s.empty())
                merge_meta(upd.meta, json{{"subagent_session_info",
                                           {{"session_id", s}, {"message_start_index", 0}}}});
            // tool_start, NOT tc.executing_since(). ACP never moves a tool
            // into Running, so executing_since() has nothing recorded to read
            // and ALWAYS returns its liveness fallback, card birth -- which
            // here includes the session/request_permission round trip to the
            // client. tool_start is stamped above at the pool submit that is
            // this path's actual dispatch, and the timeout branch already
            // uses it. The two terminal paths now agree.
            settle_tool(cap, ctx, tc, ToolUse::Done{{}, {}, result->text}, tool_start);
        } else {
            std::string detail = result.error().render();
            a::List<a::ToolCallContent> content;
            // Fence the error so Zed renders it as a distinct code block rather
            // than blending it into prose (claude-code-acp does the same for
            // error results). Skip the fence if the detail already looks fenced.
            std::string shown = detail.find("```") == std::string::npos
                              ? "```\n" + detail + "\n```"
                              : detail;
            content.push_back(a::ToolCallContent{
                a::TCC_Content{text_block(std::move(shown)), json::object()}});
            upd.status    = a::Just(a::ToolCallStatus::Failed);
            upd.content   = a::Just(std::move(content));
            upd.rawOutput = a::Just<json>(json{{"text", detail}, {"error", true}});
            if (term) upd.meta = terminal_exit_meta(tc.id.value, detail, true);
            // Same as the Done branch: tool_start is the dispatch stamp.
            settle_tool(cap, ctx, tc, ToolUse::Failed{{}, {}, detail}, tool_start);
        }

        send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(upd)});
    }

    return true;
}

// Live-terminal execution of a `bash` call. See header for the contract.
std::optional<AgentServer::TerminalRun>
AgentServer::run_bash_via_terminal(const Cap& cap, const TurnCtx& ctx, ToolUse& tc) {
    namespace sp = tools::spec;

    // Eligibility: the `bash` tool only; the client must host terminals; and
    // our sandbox must NOT be wrapping commands (a wrapped command must run
    // through agentty's own bwrap/sandbox-exec, which the client terminal
    // knows nothing about). When any of these fail we return nullopt so the
    // caller falls back to internal execution.
    const auto* spec = sp::lookup(tc.name.value);
    if (!spec || spec->kind != sp::Kind::Bash) return std::nullopt;
    if (!client_supports_terminal_.load(std::memory_order_relaxed)) return std::nullopt;
    if (tools::util::sandbox::is_active()) return std::nullopt;

    const auto& args = tc.args;
    auto str = [&](const char* k) -> std::string {
        return (args.contains(k) && args[k].is_string()) ? args[k].get<std::string>() : std::string{};
    };
    std::string command = str("command");
    if (command.empty()) command = str("cmd");
    if (command.empty()) command = str("shell_command");
    if (command.empty()) return std::nullopt;   // nothing to run; let internal path report the error

    // Resolve the working directory the same way the bash tool would: an
    // explicit `cd`/alias, else the session cwd. Passed to the client so its
    // terminal starts where the user expects.
    std::string cd = str("cd");
    if (cd.empty()) cd = str("cwd");
    if (cd.empty()) cd = str("workdir");
    const std::string& run_cwd = cd.empty() ? ctx.cwd : cd;

    auto is_cancelled = [&] { return ctx.cancelled(); };
    auto set_status = [&](ToolUse::Status st,
                          std::chrono::steady_clock::time_point since = {}) {
        settle_tool(cap, ctx, tc, std::move(st), since);
    };
    // Execution starts HERE (the terminal create is the dispatch), not at
    // the card's birth — mirror the local bash tool's convention so the
    // stats panel's tool-time slice measures running, not queueing.
    const auto exec_started = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& detail) -> TerminalRun {
        set_status(ToolUse::Failed{{}, {}, detail}, exec_started);
        a::ToolCallUpdate f;
        f.toolCallId = a::ToolCallId{tc.id.value};
        f.status     = a::Just(a::ToolCallStatus::Failed);
        a::List<a::ToolCallContent> c;
        c.push_back(a::ToolCallContent{a::TCC_Content{text_block(detail), json::object()}});
        f.content = a::Just(std::move(c));
        send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(f)});
        return TerminalRun{false, false, detail};
    };

    // 1) Ask the client to spawn the process in its own terminal.
    a::CreateTerminalParams cp;
    cp.sessionId = a::SessionId{ctx.id};
    // Run through a shell so pipes/redirects/globs behave like the bash tool.
#ifdef _WIN32
    cp.command = "cmd.exe";
    cp.args    = {"/S", "/C", command};
#else
    cp.command = "/bin/sh";
    cp.args    = {"-c", command};
#endif
    if (!run_cwd.empty()) cp.cwd = a::Just<std::string>(run_cwd);
    // Cap captured output to the bash tool's wire budget so the model sees a
    // comparable amount of text regardless of execution path.
    cp.outputByteLimit = a::Just<std::int64_t>(static_cast<std::int64_t>(spec->max_output_chars));

    std::string terminal_id;
    try {
        terminal_id = call<a::to_client::TerminalCreate>(cap, *peer_, cp).terminalId;
    } catch (const std::exception& e) {
        // Client couldn't create the terminal after advertising the cap: fall
        // back to internal execution rather than failing the tool.
        util::dbglog("acp.terminal.create", e.what());
        return std::nullopt;
    }
    if (terminal_id.empty()) return std::nullopt;

    // Ensure the terminal is always released, even on an early return/throw.
    struct Releaser {
        AgentServer* self; const Cap& cap; std::string sid, tid; bool armed = true;
        void disarm() { armed = false; }
        ~Releaser() {
            if (!armed) return;
            try {
                a::TerminalRef r; r.sessionId = a::SessionId{sid}; r.terminalId = tid;
                (void)call<a::to_client::TerminalRelease>(cap, *self->peer_, r);
            } catch (...) { /* client gone; nothing to clean up */ }
        }
    } releaser{this, cap, ctx.id, terminal_id};

    a::TerminalRef ref; ref.sessionId = a::SessionId{ctx.id}; ref.terminalId = terminal_id;

    // 2) Show the live terminal in the tool card. Zed renders the streaming
    //    output itself from this content block keyed to the terminal id.
    {
        a::ToolCallUpdate u;
        u.toolCallId = a::ToolCallId{tc.id.value};
        a::List<a::ToolCallContent> c;
        c.push_back(a::ToolCallContent{a::TCC_Terminal{terminal_id, json::object()}});
        u.content = a::Just(std::move(c));
        send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(u)});
    }

    // 3) Wait for the process to exit. A session cancel cuts the wait short,
    //    then terminal/kill stops the process.
    a::TerminalExitStatus exit{};
    {
        rpc::CallOptions o;
        o.timeout = std::chrono::milliseconds{0};   // a build can run for an hour; cancel is the way out
        if (ctx.cancel) o.cancel = ctx.cancel->token();
        auto waited = peer_->call<a::to_client::TerminalWaitForExit>(cap, ref, std::move(o));
        if (waited) {
            exit = std::move(*waited);
        } else if (!is_cancelled()) {
            return fail(std::string{"terminal wait failed: "} + waited.error().what());
        } else {
            try { a::TerminalKillParams k; k.sessionId = ref.sessionId; k.terminalId = ref.terminalId;
                  (void)call<a::to_client::TerminalKill>(cap, *peer_, k); }
            catch (const std::exception& e) { util::dbglog("acp.terminal_kill", e.what()); }
            catch (...) { util::dbglog("acp.terminal_kill", "non-std exception"); }
            set_status(ToolUse::Failed{{}, {}, "cancelled"});
            a::ToolCallUpdate c;
            c.toolCallId = a::ToolCallId{tc.id.value};
            c.status     = a::Just(a::ToolCallStatus::Failed);
            // Replace the live terminal block with static text: the releaser
            // frees the terminal as we return, and Zed renders a released
            // terminal's block as empty or broken.
            a::List<a::ToolCallContent> cc;
            cc.push_back(a::ToolCallContent{
                a::TCC_Content{text_block("(cancelled)"), json::object()}});
            c.content = a::Just(std::move(cc));
            send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(c)});
            return TerminalRun{false, true, {}};
        }
    }

    // 4) Pull the final captured output.
    std::string output;
    bool truncated = false;
    try {
        a::TerminalOutputParams op; op.sessionId = ref.sessionId; op.terminalId = ref.terminalId;
        auto out = call<a::to_client::TerminalOutput>(cap, *peer_, op);
        output    = std::move(out.output);
        truncated = out.truncated;
    } catch (const std::exception& e) {
        return fail(std::string{"terminal output failed: "} + e.what());
    }

    const std::int64_t code = exit.exitCode.has_value() ? *exit.exitCode : 0;
    const bool ok = !exit.signal.has_value() && code == 0;

    // Build the model-visible result text, mirroring the bash tool's shape:
    // the captured output, plus an exit annotation on non-zero/ signal.
    std::string model_text = output;
    if (truncated) model_text += "\n[output truncated]";
    if (exit.signal.has_value())
        model_text += "\n[terminated by signal " + *exit.signal + "]";
    else if (code != 0)
        model_text += "\n[exit code " + std::to_string(code) + "]";

    // 5) Final card update: keep the terminal content block (Zed keeps the
    //    scrollback), attach rawOutput for the model, set terminal status.
    a::ToolCallUpdate upd;
    upd.toolCallId = a::ToolCallId{tc.id.value};
    upd.status     = a::Just(ok ? a::ToolCallStatus::Completed : a::ToolCallStatus::Failed);
    a::List<a::ToolCallContent> content;
    content.push_back(a::ToolCallContent{a::TCC_Terminal{terminal_id, json::object()}});
    // ALSO attach the captured output as a fenced text block. The terminal is
    // released the moment this function returns (the Releaser), and Zed drops a
    // released terminal's live widget — so without a static text copy the
    // completed card would show NOTHING. The fenced block is the durable
    // scrollback the user (and a reopened session) always sees. Empty output
    // (e.g. a silent successful command) still gets a minimal note so the card
    // isn't blank.
    {
        std::string shown = model_text.empty() ? std::string{"(no output)"} : model_text;
        std::string md = shown.find("```") == std::string::npos
                       ? "```\n" + shown + "\n```"
                       : std::move(shown);
        content.push_back(a::ToolCallContent{
            a::TCC_Content{text_block(std::move(md)), json::object()}});
    }
    upd.content   = a::Just(std::move(content));
    upd.rawOutput = a::Just<json>(json{{"text", model_text},
                                       {"exitCode", code},
                                       {"truncated", truncated}});
    send_update(cap, ctx.id, a::SU_ToolCallUpdate{std::move(upd)});

    // Execution began at the terminal create (the dispatch), not the card's
    // birth — stamp that onto the settled state so the stats fold measures
    // running time, not queueing.
    if (ok) set_status(ToolUse::Done{{}, {}, model_text}, exec_started);
    else    set_status(ToolUse::Failed{{}, {}, model_text}, exec_started);

    return TerminalRun{ok, false, std::move(model_text)};
}

AgentServer::PermissionOutcome
AgentServer::ask_permission(const Cap& cap, const std::string& session_id, const ToolUse& tc,
                            std::stop_token cancel) {
    const std::string cwd =
        with_session(cap, session_id, [](Session& s) { return s.cwd; }).value_or(std::string{});
    a::RequestPermissionParams req;
    req.sessionId = a::SessionId{session_id};
    // ToolCallUpdate carries the tool card for the permission dialog.
    req.toolCall.toolCallId = a::ToolCallId{tc.id.value};
    req.toolCall.title      = a::Just(tool_title(tc, cwd));
    req.toolCall.kind       = a::Just(acp_tool_kind(tc.name.value));
    if (!tc.args.is_null()) req.toolCall.rawInput = a::Just<json>(tc.args);
    req.toolCall.locations  = a::Just(tool_locations(tc, cwd));
    a::List<a::ToolCallContent> c;
    if (auto d = announce_diff(tc, cwd))  c.push_back(std::move(*d));
    if (auto cc = command_content(tc))    c.push_back(std::move(*cc));
    if (auto pc = prompt_content(tc))     c.push_back(std::move(*pc));
    if (!c.empty()) req.toolCall.content = a::Just(std::move(c));
    // Labels name the tool, so "always" says what it covers: this tool, for
    // the rest of this session.
    const std::string tool = tc.name.value;
    req.options = {
        a::PermissionOption{"allow_once",    "Allow",                        a::PermissionOptionKind::AllowOnce,    json::object()},
        a::PermissionOption{"allow_always",  "Always allow " + tool,         a::PermissionOptionKind::AllowAlways,  json::object()},
        a::PermissionOption{"reject_once",   "Reject",                       a::PermissionOptionKind::RejectOnce,   json::object()},
        a::PermissionOption{"reject_always", "Always reject " + tool,        a::PermissionOptionKind::RejectAlways, json::object()},
    };
    // AIR's approval heading: the card title, plus the command's own summary.
    if (air_.load(std::memory_order_relaxed)) {
        json perm{{"version", 1}, {"title", tool_title(tc, cwd)}};
        if (tc.args.is_object())
            if (auto it = tc.args.find("display_description");
                it != tc.args.end() && it->is_string() && !it->get<std::string>().empty())
                perm["description"] = *it;
        req.meta = air_meta(json{{"permission", std::move(perm)}});
        merge_meta(req.toolCall.meta, air_tool_meta(tc));
    }

    // session/cancel withdraws the open dialog with $/cancel_request, as the
    // spec's cancel cascade asks; the call then settles as Deny.
    rpc::CallOptions o;
    o.cancel      = cancel;
    o.notify_peer = true;
    try {
        auto r = peer_->call<a::to_client::RequestPermission>(cap, req, std::move(o));   // blocks worker, not reader
        if (!r) return PermissionOutcome::Deny;
        auto outcome = std::move(*r);
        return a::match(outcome.outcome,
            [](const a::PO_Cancelled&) { return PermissionOutcome::Deny; },
            [](const a::PO_Selected& s) {
                if (s.optionId == "allow_always")  return PermissionOutcome::AllowAlways;
                if (s.optionId == "allow_once")    return PermissionOutcome::AllowOnce;
                if (s.optionId == "reject_always") return PermissionOutcome::DenyAlways;
                return PermissionOutcome::Deny;
            });
    } catch (const std::exception&) {
        return PermissionOutcome::Deny;   // client disconnected/errored
    }
}

} // namespace agentty::acp
