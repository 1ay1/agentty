#pragma once
// agentty::provider — the SHARED system-prompt policy: the agent/tool/RAG
// instruction text plus CLAUDE.md-tier and learned-memory assembly, used by
// EVERY provider (Anthropic, native ChatGPT/Codex, hosted OpenAI-compatible).
//
// This used to live under provider::anthropic, which was a layering bug: a
// Claude-namespaced function was the single source of truth for all wires. It
// now sits at the provider:: root so the SSOT isn't misfiled, and per-provider
// divergence goes through prompt_overlay() (a small appended DELTA) rather than
// forking the whole base. Pure string building — no wire, no SSE, no network.
// The prompt is composed in C++ and baked into the binary; nothing is read
// from disk at runtime, so there is no prompt-injection surface. See prompt.cpp.

#include <string>
#include <string_view>
#include <vector>

#include "agentty/provider/provider.hpp"   // provider::ToolSpec

namespace agentty::provider {

using ToolSpec = provider::ToolSpec;

// The shared base system prompt with live env info (OS/shell stanza, CLAUDE.md
// tiers, learned-memory blocks, skills catalog).
//
// `lean` builds a trimmed variant for SUBAGENTS: it keeps the operational
// discipline (file-editing, shell, context-economy, big-codebases,
// in-house-languages, environment) but OMITS the memory-tools protocol and the
// on-demand skills catalog — a subagent never calls remember/forget/wipe (not
// in its allowlist) and doesn't persist facts, so that large block is pure
// billed dead-weight on its prefix. Default (false) is the full parent prompt.
[[nodiscard]] std::string default_system_prompt(bool lean = false);

// Per-provider prompt OVERLAY — a small delta appended to the shared base so a
// specific (usually "pedantic") model can get extra rules or tone tweaks
// WITHOUT forking the whole prompt. `provider_id` is the canonical provider id
// (e.g. "anthropic", "chatgpt", "openai", "ollama"); an unknown or overlay-less
// provider returns an empty string. Baked into the binary like the base.
[[nodiscard]] std::string prompt_overlay(std::string_view provider_id);

// The base prompt with the provider's overlay applied. Callers that know the
// provider id should prefer this over default_system_prompt() so overlays take
// effect; the policy layer (prompt_policy.cpp) uses it.
[[nodiscard]] std::string system_prompt_with_overlay(std::string_view provider_id,
                                                     bool lean = false);

// Tool specs corresponding to our local tool implementations.
[[nodiscard]] std::vector<ToolSpec> default_tools();

} // namespace agentty::provider
