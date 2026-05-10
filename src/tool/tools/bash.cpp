#include "agentty/domain/refined.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/bash_validate.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

// Strip ANSI / OSC escape sequences from captured subprocess output.
// Programs like `python3 crazy.py`, `ls --color`, `git log` etc. emit
// CSI (`\x1b[...m`), OSC (`\x1b]...BEL`), and bare-ESC sequences when
// stdout looks like a TTY — and a sandboxed subprocess can be tricked
// into thinking it does.  Without stripping, two things break:
//
//   1. The ESC bytes get written to maya's canvas; canvas drops them
//      (canvas.cpp:222 skips `< 0x20`) but the *parameter tail* of each
//      sequence (`[?25l`, `[2J`, `[38;2;255;180;26m`, …) becomes giant
//      blobs of literal text — a 10×80 RGB burst is ~12 KB *post*-strip.
//   2. That blob lands in agentty's tool body preview as a TextElement and
//      blows up the canvas height in a single frame.  compose_inline_frame's
//      partial-rewrite path can't handle frame-to-frame growth larger
//      than `term_h - composer_height` cleanly — the symptom is whole
//      rows of an earlier frame staying visible above the rewrite span,
//      mixed in with the new content.
//
// Stripping at capture time keeps the body preview a faithful rendering
// of what the user *meant* to see (the program's text content), and
// keeps the canvas growth proportional to that text rather than to its
// SGR-escape weight.  Cost: one linear pass over the output buffer.
std::string strip_ansi_escapes(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        if (b != 0x1b) {              // not an escape — keep verbatim
            out.push_back(in[i]);
            ++i;
            continue;
        }
        if (i + 1 >= in.size()) { ++i; continue; }   // dangling ESC
        unsigned char next = static_cast<unsigned char>(in[i + 1]);
        if (next == '[') {
            // CSI: ESC [ params final.  Final byte is in 0x40..0x7e
            // (`@A..Z[\]^_a..z{|}~`).  Params are any of `\x30..\x3f`
            // (digits, `:;<=>?`).  Intermediate bytes `\x20..\x2f` rare
            // in the wild but allowed; we just skip until a final byte.
            i += 2;
            while (i < in.size()) {
                unsigned char c = static_cast<unsigned char>(in[i++]);
                if (c >= 0x40 && c <= 0x7e) break;
            }
        } else if (next == ']') {
            // OSC: ESC ] params terminator.  Terminator is BEL (0x07)
            // or ST (`ESC \`).  Some malformed sources never terminate;
            // cap the scan at a sane budget so a runaway OSC can't
            // swallow the rest of the buffer.
            i += 2;
            std::size_t cap = std::min(in.size(), i + 4096);
            while (i < cap) {
                unsigned char c = static_cast<unsigned char>(in[i]);
                if (c == 0x07) { ++i; break; }
                if (c == 0x1b && i + 1 < in.size()
                    && in[i + 1] == '\\') { i += 2; break; }
                ++i;
            }
        } else {
            // Other ESC sequences: ESC + one byte (charset selection,
            // ESC P / X / ^ / _ … followed by ST, etc).  Conservative:
            // just drop ESC + the next byte.  A misinterpreted DCS/SOS
            // tail will print as harmless ASCII.
            i += 2;
        }
    }
    return out;
}

// Bash command bounds, encoded in the type so the runner doesn't
// re-validate. `timeout` is `Bounded<int, 1, 300>` — 1 s minimum (a 0 s
// timeout fires before the subprocess can fork), 300 s (5 min) ceiling
// is a sane cap on a single agent step (anything truly long-running
// belongs in a backgrounded job, not a synchronous tool call).  Default
// is 60 s — most shell commands the model invokes are fast (file
// metadata, `git status`, small builds); the long tail of legitimate
// 60-300s commands can opt in via the `timeout` arg.  `command` is
// `NonEmpty<string>` — the subprocess runner can dereference without
// checking. The parser is the *only* place that can fail-construct
// these; once a `BashArgs` exists, all of its invariants have been
// proven.
using Command = domain::NonEmpty<std::string>;
using TimeoutSecs = domain::Bounded<int, 1, 300>;

struct BashArgs {
    Command     command;
    TimeoutSecs timeout;
    std::string cd;        // optional; empty = inherit cwd
    std::string display_description;
};

std::expected<BashArgs, ToolError> parse_bash_args(const json& j) {
    util::ArgReader ar(j);
    auto cmd_opt = ar.require_str("command");
    if (!cmd_opt)
        return std::unexpected(ToolError::invalid_args("command required"));
    std::string cmd = *std::move(cmd_opt);
    if (auto why = util::validate_bash_command(cmd); !why.empty())
        return std::unexpected(ToolError::invalid_args(std::move(why)));
    auto cmd_refined = Command::try_make(std::move(cmd));
    if (!cmd_refined)
        return std::unexpected(ToolError::invalid_args(
            std::string{cmd_refined.error().what}));

    int timeout_int = ar.integer("timeout", 60);
    // Also accept `timeout_ms` (Zed's convention). Convert to seconds.
    if (ar.has("timeout_ms")) {
        int ms = ar.integer("timeout_ms", 0);
        if (ms > 0) timeout_int = (ms + 999) / 1000;
    }
    // Coerce out-of-range values to the documented default — anything we
    // could meaningfully reject we silently fix instead, since the
    // primary cause is the model passing nonsense (e.g. -1 to disable).
    if (timeout_int <= 0 || timeout_int > 300) timeout_int = 60;
    auto timeout = TimeoutSecs::try_make(timeout_int);
    if (!timeout)
        return std::unexpected(ToolError::invalid_args(
            "timeout must be in [1, 300]"));

    std::string cd = ar.str("cd", "");
    if (!cd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(cd, ec))
            return std::unexpected(ToolError::invalid_args(
                "cd '" + cd + "' is not a directory"));
        // Workspace boundary applies to the `cd` arg too — otherwise the
        // model could route around the gate by `cd /etc && cat passwd`.
        // The body of the command can still escape via its own `cd` /
        // absolute paths; that's why bash has Exec effects and prompts
        // under Ask. The cd arg is the explicit, declared one we *can*
        // gate cleanly.
        if (auto wp = util::make_workspace_path(cd, "bash"); !wp)
            return std::unexpected(std::move(wp.error()));
    }
    return BashArgs{
        std::move(*cmd_refined),
        std::move(*timeout),
        std::move(cd),
        ar.str("display_description", ""),
    };
}

ExecResult run_bash(const BashArgs& a) {
    auto t0 = std::chrono::steady_clock::now();
    // When `cd` is set, prefix `cd <dir> && …`. Quoting differs per shell:
    // POSIX sh uses single quotes (literal, '\'' for embedded quotes);
    // Windows cmd.exe does not understand single quotes — it requires
    // double-quoted paths, and `cd /d` is needed to cross drive letters.
    // cmd.exe has no general escape for `"` inside `"..."`; bail on such
    // paths rather than emit a broken command.
    // Pull the raw command + timeout out of their refinement wrappers
    // once, here at the use site — downstream string concatenation and
    // the subprocess runner take primitive types.
    const std::string& cmd_str = a.command.value();
    const int           tmo_s   = a.timeout.value();

    std::string effective = cmd_str;
    if (!a.cd.empty()) {
#ifdef _WIN32
        if (a.cd.find('"') != std::string::npos)
            return std::unexpected(ToolError::invalid_args(
                "cd path contains '\"', which cmd.exe cannot quote"));
        effective = "cd /d \"" + a.cd + "\" && " + cmd_str;
#else
        std::string q;
        q.reserve(a.cd.size() + 4);
        q.push_back('\'');
        for (char c : a.cd) { if (c == '\'') q += "'\\''"; else q.push_back(c); }
        q.push_back('\'');
        effective = "cd " + q + " && " + cmd_str;
#endif
    }
    // sandbox::run_shell_command is a drop-in for run_command_s when
    // sandbox is off; when active (bwrap on Linux / sandbox-exec on
    // macOS) it wraps the shell call so the model can't escape the
    // workspace via `cd` / absolute paths / shell tricks. The user's
    // declared `cd` arg is already workspace-checked above; this is
    // the runtime layer that catches what the body does.
    auto r = util::sandbox::run_shell_command(effective, 30000, std::chrono::seconds{tmo_s});
    // Sanitize before any of the formatting branches below see r.output —
    // see strip_ansi_escapes' rationale.  We do this here (not deeper in
    // the subprocess layer) because bash output is the only path that
    // routinely carries TTY escapes; other tools that capture subprocess
    // output produce structured data we wouldn't want to munge.
    r.output = strip_ansi_escapes(r.output);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Zed-style per-state output: success+empty is affirmative,
    // failure names its exit code, timeout surfaces partial output.
    if (!r.started)
        return std::unexpected(ToolError::spawn(
            "failed to spawn command: " + r.start_error));

    auto fence = [](const std::string& body) {
        return std::string{"```\n"} + body + (body.empty() || body.back() == '\n'
                                              ? "" : "\n") + "```";
    };

    std::ostringstream out;
    if (r.timed_out) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. No output was captured.";
        } else {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. Output captured before timeout:\n\n"
                << fence(r.output);
        }
    } else if (r.exit_code != 0) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".";
        } else {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".\n\n" << fence(r.output);
        }
    } else if (r.output.empty()) {
        out << "Command executed successfully.";
    } else {
        out << fence(r.output);
    }
    if (r.truncated)
        out << "\n\n[output truncated at 30000 bytes]";
    // Elapsed is useful for planning follow-ups; omit for anything
    // under 500 ms to keep output tidy.
    if (elapsed_ms >= 500)
        out << "\n\n[elapsed: "
            << (elapsed_ms < 10000
                ? (std::to_string(elapsed_ms) + " ms")
                : (std::to_string(elapsed_ms / 1000) + "."
                   + std::to_string((elapsed_ms % 1000) / 100) + " s"))
            << "]";

    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_bash() {
    // Compile-time bind to the spec catalog. A typo here ("bsh") fails
    // to compile via the static_assert inside spec::require — there is
    // no way to register a tool whose name isn't in the catalog.
    constexpr const auto& kSpec = spec::require<"bash">();
    ToolDef t;
    t.name        = ToolName{std::string{kSpec.name}};
    t.description =
#ifdef _WIN32
        "Run a shell command via Windows cmd.exe and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "This runs under cmd.exe on Windows — use native equivalents like "
        "`dir`, `where`, `systeminfo`, `type`, `findstr`, or `powershell -c`. "
        "Do NOT use POSIX-only commands (`uname`, `cat /etc/os-release`, "
        "`sw_vers`, `ls`, `grep`, `sed`, `awk`, heredocs) — they will fail. "
        "Do NOT use for file IO — use the write/edit/read tools instead."
#else
        "Run a shell command and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "Do NOT use for file IO — use the write/edit/read tools instead "
        "(no cat/echo/sed/heredoc to create or modify files)."
#endif
    ;
    t.input_schema = json{
        {"type","object"},
        {"required", {"command"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI — e.g. "
                               "'Run the test suite'. Optional but strongly "
                               "recommended."}}},
            {"command", {{"type","string"}, {"description","The shell command to execute"}}},
            {"cd",      {{"type","string"}, {"description",
                "Working directory for the command. If set, runs as `cd <dir> && <command>`."}}},
            {"timeout", {{"type","integer"}, {"description","Timeout in seconds (default 60, max 300)"}}},
            {"timeout_ms", {{"type","integer"}, {"description",
                "Alternative timeout in milliseconds (rounded up to seconds)."}}},
        }},
    };
    // Both effects + eager-streaming come from the spec catalog, which
    // is also where the static_asserts live that prove `bash` is the
    // only Exec tool besides `diagnostics`. Editing the catalog is
    // the only way to change either; this factory just consumes it.
    t.effects               = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<BashArgs>(parse_bash_args, run_bash);
    return t;
}

} // namespace agentty::tools
