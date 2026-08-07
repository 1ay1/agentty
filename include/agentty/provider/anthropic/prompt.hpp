#pragma once
// agentty::provider::anthropic — system-prompt + CLAUDE.md/learned-memory
// assembly. Split out of transport.cpp so the prompt policy (the large,
// slowly-changing instruction text + the memory-tier collection) lives in its
// own translation unit, the way the ChatGPT provider keeps responses/oauth/
// provider in separate modules. Pure string building — no wire, no SSE, no
// network. See prompt.cpp.

#include <string>
#include <vector>

#include "agentty/provider/provider.hpp"   // provider::ToolSpec

namespace agentty::provider::anthropic {

using ToolSpec = provider::ToolSpec;

// Standard system prompt with env info.
//
// `lean` builds a trimmed variant for SUBAGENTS: it keeps the operational
// discipline (file-editing, shell, context-economy, big-codebases,
// in-house-languages, environment) but OMITS the memory-tools protocol and the
// on-demand skills catalog — a subagent never calls remember/forget/wipe (not
// in its allowlist) and doesn't persist facts, so that large block is pure
// billed dead-weight on its prefix. Default (false) is the full parent prompt.
[[nodiscard]] std::string default_system_prompt(bool lean = false);

// Tool specs corresponding to our local tool implementations.
[[nodiscard]] std::vector<ToolSpec> default_tools();

} // namespace agentty::provider::anthropic
