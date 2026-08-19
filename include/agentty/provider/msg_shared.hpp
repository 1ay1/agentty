#pragma once
// agentty::provider::wire — cross-transport message/prompt helpers.
//
// Small, stateless building blocks every transport (Anthropic, OpenAI-compat,
// Ollama, ChatGPT/Codex) used to re-implement verbatim: the "assistant turn
// carries tool results" predicate, the home-dir resolver, the capped file
// read, and the CLAUDE.md user/project/local memory-block wrapper. Hoisted
// here so a fix (a new memory tier, a Windows path quirk, the 64 KiB cap)
// lands once. Byte output is guarded by the per-transport wire golden tests.
//
// Distinct from wire.hpp (byte-level SSE framing, deliberately domain-light):
// this one legitimately needs the domain Message + filesystem.

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "agentty/domain/conversation.hpp"

namespace agentty::provider::wire {

// True whenever an assistant message carries ANY tool_calls. Anthropic (and
// the OpenAI-shaped wires) require every tool_use/tool_call be paired with a
// tool_result in the following message, terminal or not — sending the call
// without its pair 400s and wedges the replayed transcript. So this drives the
// "emit the follow-up tool-result turn" decision on every transport.
[[nodiscard]] inline bool is_assistant_with_results(const Message& m) noexcept {
    return m.role == Role::Assistant && !m.tool_calls.empty();
}

// User home directory, portably. HOME (POSIX) first, USERPROFILE (Windows)
// second; empty path when neither is set.
[[nodiscard]] inline std::filesystem::path home_dir() noexcept {
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path{h};
#if defined(_WIN32)
    if (const char* h = std::getenv("USERPROFILE"); h && *h)
        return std::filesystem::path{h};
#endif
    return {};
}

// Read a text file, swallowing any I/O error (missing / unreadable → empty).
// Truncated to `cap` bytes so one rogue multi-MB CLAUDE.md can't poison the
// system prompt on every turn. Trailing whitespace is trimmed so a wrapper tag
// never gets a blank line jammed against it.
[[nodiscard]] inline std::string read_capped_file(
    const std::filesystem::path& p, std::size_t cap = 64u * 1024u) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::is_regular_file(p, ec) || ec) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'
                          || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

// Resolve the global AGENTS.md — a user-level file that applies across
// all projects, analogous to Codex's ~/.codex/AGENTS.md and OpenCode's
// ~/.config/opencode/AGENTS.md. Not part of the published agents.md spec
// (which is project-scoped only), but a pragmatic extension that major
// tools implement.
//
// Two candidates are checked in priority order; the first non-empty file
// wins:
//   1. ~/.agentty/AGENTS.md  — agentty-specific global guidance
//   2. ~/.agents/AGENTS.md   — shared global guidance for any agent tool
//
// Returns the path to the first existing non-empty file, or an empty path
// when neither candidate exists.
[[nodiscard]] inline std::filesystem::path resolve_global_agents_md() noexcept {
    const auto home = home_dir();
    if (home.empty()) return {};

    const std::filesystem::path candidates[] = {
        home / ".agentty" / "AGENTS.md",
        home / ".agents"  / "AGENTS.md",
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec) && !ec) {
            // Non-empty check via read_capped_file (cheaper than stat+open twice).
            if (!read_capped_file(p).empty())
                return p;
        }
    }
    return {};
}

// The CLAUDE.md memory hierarchy — the "lite" wrapper (user/project/local
// tiers only, no learned-memory / skills) that the local-model prompts share.
//   User    ~/CLAUDE.md            personal, all projects
//   Project <cwd>/CLAUDE.md        committed, project-specific
//   Local   <cwd>/CLAUDE.local.md  gitignored, personal-to-this-project
// Empty/missing tiers are elided; all-empty returns "". The intro line is a
// parameter so callers can keep their exact wording (byte-identity matters —
// this feeds the system prompt behind a cache breakpoint).
[[nodiscard]] inline std::string claude_md_blocks(std::string_view intro) {
    const std::string user    = read_capped_file(home_dir() / "CLAUDE.md");
    const std::string project = read_capped_file(std::filesystem::path{"CLAUDE.md"});
    const std::string local   = read_capped_file(std::filesystem::path{"CLAUDE.local.md"});
    if (user.empty() && project.empty() && local.empty()) return {};

    std::string m = "\n\n<memory>\n";
    m += intro;
    m += "\n";
    if (!user.empty())    m += "<user-memory>\n"    + user    + "\n</user-memory>\n";
    if (!project.empty()) m += "<project-memory>\n" + project + "\n</project-memory>\n";
    if (!local.empty())   m += "<local-memory>\n"   + local   + "\n</local-memory>\n";
    m += "</memory>";
    return m;
}

// AGENTS.md — the open standard for project-scoped agent guidance, stewarded
// by the Agentic AI Foundation (AAIF) under the Linux Foundation. See
// https://agents.md. The published spec is project-scoped only (no user
// tier, no local tier). However, major tools like Codex (~/.codex/AGENTS.md)
// and OpenCode (~/.config/opencode/AGENTS.md) implement a global/user scope
// as a pragmatic extension. agentty follows suit: when `global_path` is
// non-empty (resolved by the caller via wire::resolve_global_agents_md()),
// its content is emitted in a separate <agents-md-global> block BEFORE the
// project-level <agents-md> block, so project guidance overrides global.
//
// The spec also describes nested monorepo files for subpackages: "Place
// another AGENTS.md inside each package. Agents automatically read the
// nearest file in the directory tree, so the closest one takes precedence."
// When `search_from` is a subdirectory of `workspace_root` (i.e. the agent's
// cwd is inside a package), we walk upward looking for a second AGENTS.md
// that is *not* the root file. If found, its content is emitted in a
// separate <agents-md-package> block so the model can distinguish root-level
// guidance from package-specific guidance and apply precedence correctly
// (nearest wins). The walk stops at workspace_root — never escapes the
// workspace boundary.
//
// `workspace_root` is passed in (rather than resolved here) so this helper
// stays self-contained: msg_shared.hpp does NOT pull in the util/fs_helpers
// machinery, leaving the wire layer free of the ToolError/registry surface.
// Callers (anthropic/prompt.cpp, openai/transport.cpp, ollama/transport.cpp)
// already link util and resolve workspace_root() / project_root() from there.
//
// Returns "" when ALL tiers (global + root) are missing/empty so callers
// elide the block without emitting an empty wrapper tag. Same 64 KiB cap +
// trailing-whitespace trim as CLAUDE.md, via the shared wire::read_capped_file.
//
// Wire shape (precedence low → high):
//   <agents-md-global>…</agents-md-global>      (optional, ~/.agentty/AGENTS.md)
//   <agents-md>…</agents-md>                      (root, <workspace_root>/AGENTS.md)
//   <agents-md-package>…</agents-md-package>      (optional, nearest nested)
// The nested block is skipped when the nearest AGENTS.md is the root file
// (same canonical path) to avoid duplication. Keeping the standardized public
// project guidance visually distinct from personal CLAUDE.md notes lets the
// model tell them apart and apply precedence correctly.
[[nodiscard]] inline std::string agents_md_block(
    std::string_view               intro,
    const std::filesystem::path&   workspace_root,
    const std::filesystem::path&   search_from = {},
    const std::filesystem::path&   global_path = {}) {
    // ── Global scope (user-level, applies across all projects) ──
    const std::string global_content =
        global_path.empty() ? std::string{} : read_capped_file(global_path);

    const std::string content = read_capped_file(workspace_root / "AGENTS.md");
    if (content.empty() && global_content.empty()) return {};

    std::string m;

    // Global block first (lowest precedence).
    if (!global_content.empty()) {
        m += "\n\n<agents-md-global>\n";
        m += "Global guidance from ~/.agentty/AGENTS.md (or ~/.agents/AGENTS.md). "
             "Applies across all projects; overridden by project-level guidance "
             "below.\n";
        m += global_content;
        m += "\n</agents-md-global>";
    }

    // Root project block.
    if (!content.empty()) {
        m += "\n\n<agents-md>\n";
        m += intro;
        m += "\n";
        m += content;
        m += "\n</agents-md>";
    }

    // ── Nested monorepo walk: find nearest AGENTS.md below workspace_root ──
    // Walk upward from `search_from` (typically the agent's cwd clamped
    // inside the workspace via project_root()) looking for an AGENTS.md that
    // is NOT the root file. Stops at workspace_root — never escapes the
    // workspace boundary.
    if (!search_from.empty()) {
        std::error_code ec;
        auto root_canon = std::filesystem::weakly_canonical(workspace_root, ec);
        if (ec) root_canon = workspace_root;
        auto root_agents = root_canon / "AGENTS.md";

        auto dir = std::filesystem::weakly_canonical(search_from, ec);
        if (ec) dir = search_from;

        while (dir.has_parent_path()
           && dir != root_canon
           && std::filesystem::is_directory(dir, ec) && !ec) {
            auto candidate = dir / "AGENTS.md";
            auto candidate_canon = std::filesystem::weakly_canonical(candidate, ec);
            if (!ec && std::filesystem::is_regular_file(candidate, ec) && !ec
                && candidate_canon != root_agents) {
                const std::string nested = read_capped_file(candidate);
                if (!nested.empty()) {
                    m += "\n\n<agents-md-package>\n";
                    m += "Package-specific guidance from the nearest AGENTS.md "
                         "(https://agents.md). The closest AGENTS.md to the "
                         "edited file wins; treat this as overriding the "
                         "root-level guidance above where they conflict.\n";
                    m += nested;
                    m += "\n</agents-md-package>";
                }
                break;  // nearest found — stop walking
            }
            dir = dir.parent_path();
        }
    }

    return m;
}

} // namespace agentty::provider::wire
