#pragma once
// Canonical agent prompt policy.
//
// Prompt selection is runtime policy, not a wire-transport concern. Every
// entry point (TUI, ACP server, tests) calls this function so a provider switch
// cannot silently change tool/RAG behavior through a duplicated if/else tree.

#include <string>

#include "agentty/util/io.hpp"

namespace agentty::provider {

struct Selection;

// Build the product-level system prompt for a selected backend. Hosted capable
// models share the full agent prompt; constrained local endpoints receive the
// compact local profile. External ACP agents own their own prompt/session.
[[nodiscard]] std::string system_prompt_for(Io, const Selection& selection);

// True when `selection` gets the compact local-model prompt: an OpenAI-compat
// server you run (loopback / LAN, plain HTTP). Shared so the main turn and
// subagents / `agentty run` can't disagree about which prompt a model gets.
[[nodiscard]] bool uses_local_prompt(const Selection& selection);

// The compact prompt for a selection uses_local_prompt() accepts: Ollama's
// own for its native API, the OpenAI-compat local one otherwise. Both carry
// the same action rules.
[[nodiscard]] std::string local_system_prompt_for(const Selection& selection);

} // namespace agentty::provider
