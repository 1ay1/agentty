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
// https://agents.md. Unlike CLAUDE.md (a personal memory hierarchy with
// user/project/local tiers), AGENTS.md is intentionally PROJECT-SCOPED ONLY
// per the published spec: a single file at <project_root>/AGENTS.md, no
// user tier, no local tier. (The spec also describes nested monorepo files
// for subpackages; agentty's workspace model is single-tier, so we read
// only the project-root file — the user explicitly chose this scope.)
//
// `project_root` is passed in (rather than resolved here) so this helper
// stays self-contained: msg_shared.hpp does NOT pull in the util/fs_helpers
// machinery, leaving the wire layer free of the ToolError/registry surface.
// Callers (anthropic/prompt.cpp, openai/transport.cpp, ollama/transport.cpp)
// already link util and resolve project_root() from there.
//
// Returns "" when the file is missing/empty so callers elide the block
// without emitting an empty wrapper tag. Same 64 KiB cap + trailing-
// whitespace trim as CLAUDE.md, via the shared wire::read_capped_file.
//
// Wire shape: a SEPARATE top-level <agents-md>…</agents-md> block, injected
// BEFORE the existing <memory> block. Keeping the standardized public
// project guidance visually distinct from personal CLAUDE.md notes lets the
// model tell them apart and apply precedence correctly.
[[nodiscard]] inline std::string agents_md_block(
    std::string_view               intro,
    const std::filesystem::path&   project_root) {
    const std::string content = read_capped_file(project_root / "AGENTS.md");
    if (content.empty()) return {};

    std::string m = "\n\n<agents-md>\n";
    m += intro;
    m += "\n";
    m += content;
    m += "\n</agents-md>";
    return m;
}

} // namespace agentty::provider::wire
