#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

// Categorize a non-zero git exit. git speaks in stderr sentences, not
// exit codes — the model recovers better when we name the failure mode
// than when we hand it `[exit code 128] fatal: not a git repository`.
ToolError classify_git_failure(const util::SubprocessResult& r,
                               std::string_view op) {
    if (!r.started)
        return ToolError::spawn(std::string{op} + ": " + r.start_error
            + " (is `git` installed and on PATH?)");

    std::string_view out = r.output;
    auto contains = [&](std::string_view needle) {
        return out.find(needle) != std::string_view::npos;
    };

    // Most common: not in a repo. git emits this from every subcommand.
    if (contains("not a git repository"))
        return ToolError::not_found(std::string{op}
            + " failed: not inside a git repository. Run `git init` first, "
              "or invoke from a directory under an existing repo.");

    // Missing identity (git ~2.42+). Surfaces from `git commit`.
    if (contains("Please tell me who you are") || contains("empty ident"))
        return ToolError::subprocess(std::string{op}
            + " failed: git identity not configured. Run "
              "`git config user.email \"you@example.com\"` and "
              "`git config user.name \"Your Name\"` (drop `--global` to "
              "scope to this repo only).");

    // Detached HEAD / unknown ref.
    if (contains("unknown revision") || contains("bad revision"))
        return ToolError::not_found(std::string{op}
            + " failed: unknown revision/ref. " + std::string{out});

    // Index lock — racy commit, leftover from a crashed git.
    if (contains(".git/index.lock"))
        return ToolError::subprocess(std::string{op}
            + " failed: another git process holds .git/index.lock. Wait for "
              "it to finish, or remove the stale lock if no git is running.");

    if (r.timed_out)
        return ToolError::subprocess(std::string{op}
            + " timed out. Output so far:\n" + r.output);

    // Generic fallthrough — hand back stdout/stderr and the exit code so
    // the model has the actual git error to read.
    return ToolError::subprocess(std::string{op}
        + " failed (exit " + std::to_string(r.exit_code) + "):\n"
        + r.output);
}

// Helper: run argv and either return its output (success) or a typed error.
std::expected<std::string, ToolError>
run_git(const std::vector<std::string>& argv, std::string_view op,
        std::size_t max_bytes = 30'000) {
    auto r = util::run_argv_s(argv, max_bytes);
    if (!r.started || r.timed_out || r.exit_code != 0)
        return std::unexpected(classify_git_failure(r, op));
    std::string out = std::move(r.output);
    if (r.truncated) out += "\n[output truncated]";
    return out;
}

} // namespace

// ── git_status ───────────────────────────────────────────────────────────

namespace {

struct GitStatusArgs {
    std::string root;
    std::string display_description;
};

std::expected<GitStatusArgs, ToolError> parse_git_status_args(const json& j) {
    util::ArgReader ar(j);
    return GitStatusArgs{
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_status(const GitStatusArgs& a) {
    auto wp = util::make_workspace_path(a.root, "git_status");
    if (!wp) return std::unexpected(std::move(wp.error()));
    auto out = run_git({"git", "-C", wp->string(), "status",
                        "--porcelain=v2", "--branch"}, "git_status");
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) output = "working tree clean";
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_status() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"git_status">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Show the current git status: branch, staged/unstaged changes, "
                    "untracked files, ahead/behind counts.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path", {{"type","string"}, {"description","Repository path (default: cwd)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<GitStatusArgs>(parse_git_status_args, run_git_status);
    return t;
}

// ── git_diff ─────────────────────────────────────────────────────────────

namespace {

struct GitDiffArgs {
    std::string path;
    bool staged;
    std::string ref;
    std::string display_description;
};

std::expected<GitDiffArgs, ToolError> parse_git_diff_args(const json& j) {
    util::ArgReader ar(j);
    return GitDiffArgs{
        ar.str("path", ""),
        ar.boolean("staged", false),
        ar.str("ref", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_diff(const GitDiffArgs& a) {
    std::vector<std::string> argv = {"git", "diff", "--stat", "-p"};
    if (a.staged) argv.push_back("--cached");
    if (!a.ref.empty()) argv.push_back(a.ref);
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path(a.path, "git_diff");
        if (!wp) return std::unexpected(std::move(wp.error()));
        argv.push_back("--");
        argv.push_back(wp->string());
    }
    auto out = run_git(argv, "git_diff", 50'000);
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) return ToolOutput{"no changes", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_diff() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"git_diff">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Show git diff. By default shows unstaged changes. Use staged=true "
                    "for staged changes, or specify a ref/range.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",    {{"type","string"}, {"description","File or directory to diff"}}},
            {"staged",  {{"type","boolean"}, {"description","Show staged changes (default: false)"}}},
            {"ref",     {{"type","string"}, {"description","Git ref or range (e.g. HEAD~3, main..HEAD)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<GitDiffArgs>(parse_git_diff_args, run_git_diff);
    return t;
}

// ── git_log ──────────────────────────────────────────────────────────────

namespace {

struct GitLogArgs {
    int count;
    std::string path;
    std::string ref;
    bool oneline;
    std::string display_description;
};

std::expected<GitLogArgs, ToolError> parse_git_log_args(const json& j) {
    util::ArgReader ar(j);
    return GitLogArgs{
        ar.integer("count", 20),
        ar.str("path", ""),
        ar.str("ref", "HEAD"),
        ar.boolean("oneline", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_log(const GitLogArgs& a) {
    // Defensive: a negative or zero count would either error in git or
    // return the entire history. Clamp to [1, 1000] — 1000 is generous
    // for a chat-window readout, anything larger likely meant `git log
    // | head`.
    int n = a.count;
    if (n <= 0) n = 20;
    if (n > 1000) n = 1000;

    std::vector<std::string> argv = {"git", "log"};
    if (a.oneline) {
        argv.push_back("--oneline");
    } else {
        argv.push_back("--format=%h %ad %an%n  %s");
        argv.push_back("--date=short");
    }
    argv.push_back("-" + std::to_string(n));
    argv.push_back(a.ref.empty() ? std::string{"HEAD"} : a.ref);
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path(a.path, "git_log");
        if (!wp) return std::unexpected(std::move(wp.error()));
        argv.push_back("--");
        argv.push_back(wp->string());
    }
    auto out = run_git(argv, "git_log");
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) return ToolOutput{"no commits", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_log() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"git_log">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Show git commit history. Returns commit hash, author, date, and message.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"count",   {{"type","integer"}, {"description","Number of commits (default: 20)"}}},
            {"path",    {{"type","string"}, {"description","Filter by file path"}}},
            {"ref",     {{"type","string"}, {"description","Branch or ref (default: HEAD)"}}},
            {"oneline", {{"type","boolean"}, {"description","One-line format (default: false)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<GitLogArgs>(parse_git_log_args, run_git_log);
    return t;
}

// ── git_commit ───────────────────────────────────────────────────────────

namespace {

struct GitCommitArgs {
    std::string message;
    std::vector<std::string> files;
    bool stage_all;
    std::string display_description;
};

std::expected<GitCommitArgs, ToolError> parse_git_commit_args(const json& j) {
    util::ArgReader ar(j);
    auto msg_opt = ar.require_str("message");
    if (!msg_opt)
        return std::unexpected(ToolError::invalid_args("commit message required"));

    // Strip surrounding whitespace; reject if nothing left. git itself
    // rejects empty messages, but we want a typed InvalidArgs (so the
    // permission UI doesn't surface it as a generic subprocess fail)
    // and a clearer hint than git's terse "Aborting commit due to empty
    // commit message".
    std::string msg = std::move(*msg_opt);
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(msg.begin(), msg.end(), not_space);
    auto last  = std::find_if(msg.rbegin(), msg.rend(), not_space).base();
    if (first >= last)
        return std::unexpected(ToolError::invalid_args(
            "commit message is empty / whitespace only"));
    msg.assign(first, last);

    std::vector<std::string> files;
    if (const json* f = ar.raw("files"); f && f->is_array()) {
        files.reserve(f->size());
        for (const auto& el : *f) {
            if (el.is_string()) {
                auto s = el.get<std::string>();
                if (!s.empty()) files.push_back(std::move(s));
            }
            // Non-string array entries are silently dropped — dumping a
            // raw object as a pathspec would either no-op or stage
            // something unintended.
        }
    }

    return GitCommitArgs{
        std::move(msg),
        std::move(files),
        ar.boolean("stage_all", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_commit(const GitCommitArgs& a) {
    // Validate the staging plan up front — better to refuse with a clear
    // message than to commit nothing and surface git's "nothing to commit"
    // as a subprocess failure.
    if (!a.stage_all && a.files.empty()) {
        // No files + no stage_all: rely on whatever is already staged.
        // If nothing is staged, git commit fails with a friendly message
        // — we'll catch and re-classify it below.
    }

    if (a.stage_all) {
        if (auto r = run_git({"git", "add", "-A"}, "git_commit (add -A)"); !r)
            return std::unexpected(std::move(r.error()));
    }
    for (const auto& f : a.files) {
        auto wp = util::make_workspace_path(f, "git_commit");
        if (!wp) return std::unexpected(std::move(wp.error()));
        if (auto r = run_git({"git", "add", "--", wp->string()},
                             "git_commit (add)"); !r)
            return std::unexpected(std::move(r.error()));
    }

    // Commit. Special-case "nothing to commit" — it's a very common
    // model mistake (asked to commit when the working tree is clean) and
    // benefits from a distinct, recoverable error category.
    auto r = util::run_argv_s({"git", "commit", "-m", a.message});
    if (!r.started || r.timed_out || r.exit_code != 0) {
        std::string_view out = r.output;
        if (out.find("nothing to commit") != std::string_view::npos
         || out.find("no changes added to commit") != std::string_view::npos)
            return std::unexpected(ToolError::invalid_args(
                "nothing to commit — working tree clean, or no files staged. "
                "Pass `stage_all: true`, or list files in `files: [...]`."));
        return std::unexpected(classify_git_failure(r, "git_commit"));
    }
    std::string output = std::move(r.output);
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_commit() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"git_commit">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Stage files and create a git commit. Specify files to stage, "
                    "or use stage_all to stage everything.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"message"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"message",   {{"type","string"}, {"description","Commit message"}}},
            {"files",     {{"type","array"}, {"items",{{"type","string"}}},
                           {"description","Files to stage before committing"}}},
            {"stage_all", {{"type","boolean"}, {"description","Stage all changes (default: false)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<GitCommitArgs>(parse_git_commit_args, run_git_commit);
    return t;
}

} // namespace agentty::tools
