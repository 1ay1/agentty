#pragma once
// agentty::sandbox_cfg::provenance — which files the AGENT wrote, and what the
// host trusts.
//
// ── The threat this exists for ───────────────────────────────────────────
//
// Pillar Security's "Week of Sandbox Escapes" (July 2026) reproduced escapes
// across Cursor, Codex, Gemini CLI and Antigravity. In almost none of them did
// the agent break the sandbox. It wrote a file that a trusted component
// OUTSIDE the sandbox later ran:
//
//   * a .claude hook config              -> CVE-2026-48124, Cursor, CVSS 8.5
//   * a virtualenv interpreter           -> GHSA-p9g2-cr55-cw9c, run by the
//                                           Python extension during discovery
//   * a .git directory under another name-> GHSA (Cursor 3.0.0), fsmonitor
//   * a .vscode task config              -> Antigravity, run by the host later
//   * a `git show` invocation            -> GitPwned, Codex CLI 0.95.0
//
// Their conclusion, which I think is exactly right: "an agent's blast radius
// is not the agent process; it includes everything the agent can write that
// the host later trusts." A sandbox that confines the process and lets it
// write executable configuration has moved the boundary, not built one.
//
// ── Why agentty is already better here, and where the gap still is ──────
//
// agentty does not have the Cursor bug. Hooks run THROUGH the sandbox (same
// bwrap/claybin wrapper as the bash tool) and are content-hash gated, so any
// byte change re-prompts. Plugins use the same Approvals primitive.
//
// But the hash gate cannot answer the question Pillar says matters most:
//
//     "Can the product distinguish user-created project state from
//      agent-created project state?"
//
// Today it cannot. A hooks.json the USER wrote and a hooks.json the AGENT
// wrote produce the same prompt, so the prompt carries no signal -- and a
// prompt that always looks the same is a prompt people click through. The
// approval says "this file changed", when what a user needs to hear is "the
// agent just wrote the file that runs commands on your machine".
//
// ── What this module does ───────────────────────────────────────────────
//
// claybin's sandbox is the only place that can answer it cheaply, because the
// agent's writes all pass through one mount namespace we built. So:
//
//   1. Every path the sandbox writes is recorded, per turn, with the command
//      that wrote it. That is the provenance record.
//   2. Paths the HOST later trusts (hooks.json, .vscode/tasks.json,
//      .git/config, venv interpreters, .envrc ...) are a known set.
//   3. The intersection is the alarm: an agent-authored file that a host
//      component will execute. That escalates from "approve this hash" to a
//      named warning -- and by default, refuses.
//
// This is the layer Pillar builds a product to sell. It belongs in the agent,
// because the agent is the only thing that knows which writes were its own.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::sandbox_cfg {

// A file the sandbox wrote, and who asked.
struct WriteRecord {
    std::string path;     // absolute, as resolved inside the sandbox
    std::string command;  // the shell command that produced it
    std::uint64_t at_ms = 0;
    bool created = false;  // true = the file did not exist before
};

// Why a path is dangerous for an agent to author. Naming the mechanism
// matters: "the agent wrote .vscode/tasks.json" means nothing to most people,
// "the agent wrote a file VS Code will execute when you open this folder"
// means everything.
enum class TrustKind : std::uint8_t {
    HookConfig,     // agentty hooks.json, .claude/*, lifecycle automation
    EditorTask,     // .vscode/tasks.json, launch.json -- host runs these
    GitConfig,      // .git/config, hooks/, fsmonitor -- git runs these
    Interpreter,    // venv/bin/python, node_modules/.bin -- discovery runs these
    ShellInit,      // .envrc, .bashrc, activate scripts
    BuildScript,    // Makefile, package.json scripts, build.rs
    DaemonSocket,   // docker.sock and friends -- a second execution env
};

[[nodiscard]] constexpr std::string_view explain(TrustKind k) noexcept {
    switch (k) {
        case TrustKind::HookConfig:
            return "lifecycle hooks -- agentty runs these around every tool call";
        case TrustKind::EditorTask:
            return "an editor task -- VS Code can run this when the folder opens";
        case TrustKind::GitConfig:
            return "git configuration -- git runs hooks and helpers from here";
        case TrustKind::Interpreter:
            return "an interpreter or entry point -- tool discovery executes these";
        case TrustKind::ShellInit:
            return "shell init -- runs on your next shell in this directory";
        case TrustKind::BuildScript:
            return "a build script -- runs on the next build";
        case TrustKind::DaemonSocket:
            return "a privileged daemon -- it can act outside the sandbox entirely";
    }
    return "";
}

// An agent-authored write to something the host trusts. The thing worth
// interrupting a user for.
struct TrustHandoff {
    WriteRecord write;
    TrustKind kind;
};

// What to do when one is detected.
//
// Refuse is the default, and that is a deliberate break from how every other
// control here works. Approval prompts are the right shape for "this might be
// risky"; a trust handoff is the shape of every escape in the Pillar series,
// so the default is no, and the user opts into being asked.
enum class HandoffPolicy : std::uint8_t {
    Refuse,  // default: the write fails, and the pane says why
    Warn,    // allow, but surface it loudly and record it
    Allow,   // off. for a workspace whose config the agent is MEANT to edit
};

[[nodiscard]] constexpr const char* to_string(HandoffPolicy p) noexcept {
    switch (p) {
        case HandoffPolicy::Refuse: return "refuse";
        case HandoffPolicy::Warn:   return "warn";
        case HandoffPolicy::Allow:  return "allow";
    }
    return "refuse";
}

// Is this path something the host will later trust? Pure and table-driven, so
// the set is reviewable in one place rather than spread across call sites.
//
// Matching is on the path SHAPE, not on an exact name, because
// "Git directories do not have to be called .git" (Pillar, Cursor 3.0.0) is
// precisely the bug that comes from exact-name matching.
[[nodiscard]] bool is_host_trusted(std::string_view path, TrustKind* out_kind);

// The per-turn record. Cleared at the start of each turn so "what did the
// agent just write" is answerable without a full audit log.
struct Provenance {
    std::vector<WriteRecord> writes;
    std::vector<TrustHandoff> handoffs;

    void clear() {
        writes.clear();
        handoffs.clear();
    }
};

}  // namespace agentty::sandbox_cfg
