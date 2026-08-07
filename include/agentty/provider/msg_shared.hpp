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

} // namespace agentty::provider::wire
