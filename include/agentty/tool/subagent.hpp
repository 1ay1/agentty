#pragma once
// agentty::tools::subagent — injection seam for the `task` tool.
//
// The subagent loop needs the wire credential + default model, which
// live in the runtime layer (deps()). The tool layer must not depend on
// the runtime, so startup installs a small config blob here; the `task`
// tool reads it at execute time. If nothing is installed (tests, ACP
// without a default model), the tool returns a clear "unavailable"
// error instead of crashing.

#include <functional>
#include <string>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::tools::subagent {

// Runtime-installed config for the subagent loop. Set once at startup.
struct Config {
    auth::AuthHeader auth;       // wire credential for the sub-stream
    std::string      model;      // model id for sub-agent turns
    bool             installed = false;

    // Provider-agnostic stream seam — the SAME dispatch main.cpp installs
    // into Deps::stream (routes on provider::active() at call time:
    // Anthropic / OpenAI-compat / Ollama native). Installed alongside auth
    // so a subagent talks to whatever backend the USER selected instead of
    // hardcoding the Anthropic transport — previously `task` failed on
    // every non-Anthropic provider (wrong wire, wrong auth). Null ⇒ the
    // runner falls back to the Anthropic transport (old behaviour, keeps
    // tests that install only auth+model working).
    std::function<void(provider::Request, provider::EventSink)> stream;
};

// Install the subagent config (call once at startup, after auth resolves).
void install(Config cfg);

// Update just the model the subagent loop uses, without disturbing auth
// or the installed flag. Called when the user switches models mid-session
// (model picker) so subagents track the live model instead of the stale
// startup default. No-op if the config was never installed.
void set_model(std::string model);

// Snapshot the installed config. `installed == false` until install() runs.
[[nodiscard]] Config current();

// Maximum nesting depth. A subagent may itself spawn subagents, but only
// down to this depth — beyond it the `task` tool refuses, preventing a
// runaway fork bomb / unbounded token spend. Depth 0 is the top-level
// agent; the first subagent runs at depth 1.
inline constexpr int kMaxDepth = 2;

// Maximum sub-agent turns (model completions) before the loop force-stops
// and returns whatever it has. Bounds token spend + wall-clock so a
// looping subagent can't wedge the parent indefinitely.
inline constexpr int kMaxTurns = 24;

// Process-wide current nesting depth, incremented while a subagent runs.
// Read by the `task` tool to enforce kMaxDepth. Thread-safe via atomic;
// subagents run on the parent tool's worker thread (run_tool is
// task_isolated), so each nesting level is on its own thread.
[[nodiscard]] int current_depth() noexcept;
void push_depth() noexcept;
void pop_depth() noexcept;

} // namespace agentty::tools::subagent
