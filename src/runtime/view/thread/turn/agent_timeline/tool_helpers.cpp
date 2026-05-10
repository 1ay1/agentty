#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui {

// ── Display ─────────────────────────────────────────────────────────────

// Pretty title-case for the tool name shown as the timeline event label.
// Maps agentty's lowercase canonical names to brand TitleCase forms.
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

// Tool category — semantic grouping for color + stats badge.
//   inspect (read/grep/glob/list/find/diag/web)  → info
//   mutate  (edit/write)                          → accent
//   execute (bash)                                → success
//   plan    (todo)                                → warn
//   vcs     (git_*)                               → highlight
maya::Color tool_category_color(const std::string& n) {
    if (n == "edit" || n == "write")  return accent;
    if (n == "bash")                  return success;
    if (n == "todo")                  return warn;
    if (n.rfind("git_", 0) == 0)      return highlight;
    return info;
}

std::string_view tool_category_label(const std::string& n) {
    if (n == "edit" || n == "write")  return "mutate";
    if (n == "bash")                  return "execute";
    if (n == "todo")                  return "plan";
    if (n.rfind("git_", 0) == 0)      return "vcs";
    return "inspect";
}

// ── Status mapping ──────────────────────────────────────────────────────

// `Approved` folds into Running because both render with the same
// in-flight spinner — the widget doesn't need a separate stage.
maya::AgentEventStatus tool_event_status(const ToolUse& tc) {
    if (tc.is_running() || tc.is_approved()) return maya::AgentEventStatus::Running;
    if (tc.is_pending())                     return maya::AgentEventStatus::Pending;
    if (tc.is_done())                        return maya::AgentEventStatus::Done;
    if (tc.is_failed())                      return maya::AgentEventStatus::Failed;
    return maya::AgentEventStatus::Rejected;
}

// ── Detail line ─────────────────────────────────────────────────────────

namespace {

std::string pretty_path(std::string p) {
    if (p.empty()) return p;
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec).string();
    if (!ec && !cwd.empty() && p.size() > cwd.size()
        && p.compare(0, cwd.size(), cwd) == 0 && p[cwd.size()] == '/')
        return p.substr(cwd.size() + 1);
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::string h{home};
        if (p.size() > h.size() && p.compare(0, h.size(), h) == 0
            && p[h.size()] == '/')
            return std::string{"~/"} + p.substr(h.size() + 1);
    }
    return p;
}

} // namespace

// One-line "what this tool is doing" for the timeline. Tool-specific
// so the user can read the sequence at a glance: paths for fs ops, the
// actual command for bash, the pattern for grep, etc. When the tool
// has settled, folds in post-completion stats so the timeline doubles
// as a compact result log without expanding individual cards.
std::string tool_timeline_detail(const ToolUse& tc) {
    auto safe = [&](const char* k) -> std::string { return safe_arg(tc.args, k); };
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    const auto& n = tc.name.value;
    const auto path_pp = pretty_path(path);

    if (n == "read") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (auto off = safe_int_arg(tc.args, "offset", 0); off > 0)
            detail += " @" + std::to_string(off);
        if (tc.is_done()) {
            int lines = count_lines(tc.output());
            if (lines > 1) detail += "  \xc2\xb7  " + std::to_string(lines) + " lines";
        }
        return detail;
    }
    if (n == "write") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (tc.is_done()) {
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
        if (tc.args.is_object()) {
            auto it = tc.args.find("edits");
            if (it != tc.args.end() && it->is_array() && !it->empty())
                detail += "  \xc2\xb7  " + std::to_string(it->size()) + " edits";
        }
        if (tc.is_done()) {
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
    if (n == "find_definition") {
        std::string detail = safe("symbol");
        if (tc.is_done()) {
            int hits = 0;
            const auto& out = tc.output();
            for (std::size_t p = 0; (p = out.find("## Matches in ", p)) != std::string::npos; p += 14)
                ++hits;
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits)
                                 + (hits == 1 ? " file" : " files");
        }
        return detail;
    }
    if (n == "web_fetch") {
        std::string detail = safe("url");
        if (tc.is_done()) {
            const auto& out = tc.output();
            auto nl = out.find('\n');
            if (nl != std::string::npos && out.starts_with("HTTP ")) {
                auto sp = out.find(' ', 5);
                if (sp != std::string::npos)
                    detail += "  \xc2\xb7  " + out.substr(5, sp - 5);
            }
        }
        return detail;
    }
    if (n == "web_search") {
        std::string detail = safe("query");
        if (tc.is_done()) {
            int hits = 0;
            const auto& out = tc.output();
            for (std::size_t p = 0; p + 1 < out.size(); ++p) {
                if ((p == 0 || out[p - 1] == '\n')
                    && std::isdigit(static_cast<unsigned char>(out[p]))
                    && (out[p+1] == '.' || (p + 2 < out.size() && out[p+2] == '.')))
                    ++hits;
            }
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits)
                                 + (hits == 1 ? " result" : " results");
        }
        return detail;
    }
    if (n == "git_commit") {
        auto m = safe("message");
        if (auto nl = m.find('\n'); nl != std::string::npos) m = m.substr(0, nl);
        if (tc.is_done()) {
            const auto& out = tc.output();
            auto open = out.find('[');
            if (open != std::string::npos) {
                auto close = out.find(']', open);
                auto sp = out.find(' ', open + 1);
                if (sp != std::string::npos && sp < close) {
                    auto hash = out.substr(sp + 1, close - sp - 1);
                    if (!hash.empty() && hash.size() <= 12)
                        m += "  \xc2\xb7  " + hash;
                }
            }
        }
        return m;
    }
    if (n == "git_status" && tc.is_done()) {
        const auto& out = tc.output();
        std::string branch;
        int modified = 0, staged = 0, untracked = 0;
        std::size_t lo = 0;
        while (lo < out.size()) {
            auto eol = out.find('\n', lo);
            std::string_view line{out.data() + lo,
                (eol == std::string::npos ? out.size() : eol) - lo};
            if (line.starts_with("# branch.head ")) branch = std::string{line.substr(14)};
            else if (line.size() >= 4 && line[0] == '?')             ++untracked;
            else if (line.size() >= 4 && (line[0] == '1' || line[0] == '2')) {
                if (line[2] != '.') ++staged;
                if (line[3] == 'M' || line[3] == 'D') ++modified;
            }
            if (eol == std::string::npos) break;
            lo = eol + 1;
        }
        std::string detail = branch.empty() ? std::string{"(detached)"} : branch;
        if (modified || staged || untracked) {
            detail += "  \xc2\xb7  ";
            bool first = true;
            auto add = [&](int n_, const char* suffix) {
                if (n_ <= 0) return;
                if (!first) detail += " ";
                detail += std::to_string(n_) + suffix;
                first = false;
            };
            add(modified, "M"); add(staged, "S"); add(untracked, "?");
        } else {
            detail += "  \xc2\xb7  clean";
        }
        return detail;
    }
    if (n == "git_diff" || n == "git_log" || n == "git_status")
        return path_pp.empty() ? std::string{"."} : path_pp;
    if (n == "todo") {
        if (tc.args.is_object()) {
            auto it = tc.args.find("todos");
            if (it != tc.args.end() && it->is_array() && !it->empty()) {
                int total = 0, done = 0, in_progress = 0;
                for (const auto& td : *it) {
                    if (!td.is_object()) continue;
                    ++total;
                    auto st = td.value("status", std::string{"pending"});
                    if (st == "completed")        ++done;
                    else if (st == "in_progress") ++in_progress;
                }
                std::string detail = std::to_string(done) + "/" + std::to_string(total);
                if (in_progress > 0)
                    detail += "  \xc2\xb7  " + std::to_string(in_progress) + " in progress";
                return detail;
            }
        }
        return "\xe2\x80\xa6";
    }
    return safe_arg(tc.args, "display_description");
}

} // namespace agentty::ui
