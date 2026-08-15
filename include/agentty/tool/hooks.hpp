#pragma once
// agentty::tools::hooks — user-authored lifecycle hooks with an explicit
// consent gate.
//
// A hook is a shell command the user configures to run at a lifecycle
// event. v1 supports the two highest-value events:
//
//   pre_tool    before a tool executes.  Receives the tool name + JSON args
//               on stdin; a NON-ZERO exit BLOCKS the tool call (its stdout
//               becomes the error the model sees). Use: policy gates
//               ("never rm -rf", "block edits to prod.config").
//   post_tool   after a tool executes.   Receives name + args + result on
//               stdin; exit code is logged but never blocks. Use: auto-
//               format after edit, notifications, audit logs.
//
// Config file (first found wins): .agentty/hooks.json, then
// ~/.agentty/hooks.json:
//
//   {
//     "pre_tool":  [ {"match": "bash",        "run": "./scripts/guard.sh"} ],
//     "post_tool": [ {"match": "edit|write",  "run": "clang-format -i $FILE"} ]
//   }
//
// `match` is an ERE matched against the tool name; `run` is executed via
// sh -c with the event payload on stdin and these env vars:
//   AGENTTY_HOOK_EVENT   pre_tool | post_tool
//   AGENTTY_HOOK_TOOL    tool name
//
// ── SECURITY: the consent gate ─────────────────────────────────────────
// Hooks are arbitrary shell that runs AUTOMATICALLY on every matching tool
// call — the single most dangerous extension surface an agent host can
// offer (a malicious repo could ship .agentty/hooks.json and exfiltrate on
// the first `read`). agentty therefore NEVER runs a hooks file that has
// not been explicitly approved:
//
//   • The FIRST time a hooks file (or a changed version of one) is seen,
//     its hooks DO NOT RUN. agentty surfaces a one-line notice telling the
//     user to run `agentty hooks approve` to inspect + approve it.
//   • `agentty hooks approve` prints the full file, asks for explicit
//     y/N confirmation on the terminal, and stores the file's SHA-256 in
//     ~/.agentty/hooks_approved.json.
//   • Any byte change to the file invalidates the approval (hash mismatch
//     → back to not-running + notice). There is no "approve forever".
//   • AGENTTY_NO_HOOKS=1 disables the whole subsystem.
//
// Hook commands run through the SAME sandbox wrapper as the bash tool
// (bwrap / sandbox-exec when enabled), so an approved-but-compromised hook
// is still workspace-confined.

#include <optional>
#include <string>
#include <string_view>

namespace agentty::tools::hooks {

struct PreToolDecision {
    bool        blocked = false;  // non-zero hook exit → tool call blocked
    std::string reason;           // hook stdout (the error the model sees)
};

// Run every approved+matching pre_tool hook for `tool`. Returns the first
// blocking decision, or an empty (allowed) one. Never throws; a hooks file
// that is missing / unapproved / unparseable yields "allowed".
[[nodiscard]] PreToolDecision
run_pre_tool(std::string_view tool, const std::string& args_json);

// Run every approved+matching post_tool hook. Fire-and-forget semantics:
// failures are logged to the hook log, never propagated.
void run_post_tool(std::string_view tool, const std::string& args_json,
                   const std::string& result_text);

// True when a hooks file exists whose hash is NOT approved (drives the
// one-line TUI notice). Cheap: stat + memoised hash.
[[nodiscard]] bool pending_approval();

// Path of the active hooks file (project first, then user), empty if none.
[[nodiscard]] std::string active_file();

// The `agentty hooks` subcommand: `list` shows configured hooks + approval
// state; `approve` prints the file and interactively approves it. Returns
// a process exit code.
int cli(const std::string& verb);

} // namespace agentty::tools::hooks
