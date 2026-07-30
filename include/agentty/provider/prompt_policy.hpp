#pragma once
// Canonical agent prompt policy.
//
// Prompt selection is runtime policy, not a wire-transport concern. Every
// entry point (TUI, ACP server, tests) calls this function so a provider switch
// cannot silently change tool/RAG behavior through a duplicated if/else tree.

#include <string>

namespace agentty::provider {

struct Selection;

// Build the product-level system prompt for a selected backend. Hosted capable
// models share the full agent prompt; constrained local endpoints receive the
// compact local profile. External ACP agents own their own prompt/session.
[[nodiscard]] std::string system_prompt_for(const Selection& selection);

} // namespace agentty::provider
