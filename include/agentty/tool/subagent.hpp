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
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/smart_mode.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::tools::subagent {

// Runtime-installed config for the subagent loop. Auth/model are refreshed as
// the user switches providers, accounts, and models during the session.
struct Config {
    auth::AuthHeader auth;       // wire credential for the sub-stream
    std::string      model;      // model id for sub-agent turns
    bool             installed = false;

    // The active provider's available models, refreshed alongside `model`.
    // The subagent runner routes READ-ONLY roles (explorer/reviewer) to the
    // cheapest capable model in this list (catalog::cheapest_capable_model),
    // since those roles do grunt work a small model handles as well as a
    // flagship. Empty (or nothing cheaper) ⇒ the role runs on `model`, so a
    // single-model provider sees no change. Never routes cross-provider.
    std::vector<ModelInfo> candidates;

    // Provider-agnostic stream seam — the SAME dispatch main.cpp installs
    // into Deps::stream (routes on provider::active() at call time:
    // Anthropic / OpenAI-compat / Ollama native). Installed alongside auth
    // so a subagent talks to whatever backend the USER selected instead of
    // hardcoding the Anthropic transport — previously `task` failed on
    // every non-Anthropic provider (wrong wire, wrong auth). Null ⇒ the
    // runner falls back to the Anthropic transport (old behaviour, keeps
    // tests that install only auth+model working).
    std::function<provider::StreamResult(provider::Request,
                                         provider::EventSink)> stream;

    // Smart Mode config, mirrored from the Model so the subagent runner can
    // resolve a worker's model by its ROLE (Layer 3b) when
    // smart.subagent_routing() is on. Off/unconfigured ⇒ the existing
    // read-only tier auto-router stands. Refreshed via set_smart().
    smart::RoleConfig smart;
    // The provider the parent turn is currently dispatched on. A pinned slot
    // model is only meaningful to the endpoint that serves it, so the role
    // resolver replays a pin ONLY when its recorded provider matches this
    // (see smart::SlotOverride::provider). Mirrored down alongside `smart`
    // because `task` runs on a worker thread with no access to the Model.
    // Empty ⇒ unknown, and every pin is honoured (pre-existing behaviour).
    std::string provider;
};

// Install the subagent config (call once at startup, after auth resolves).
void install(Config cfg);

// Update just the auth header the subagent loop uses. Called whenever login,
// logout, account switching, or provider switching changes the runtime auth.
// Empty is valid for local and native-OAuth providers whose transports resolve
// credentials themselves.
void set_auth(auth::AuthHeader auth);

// Update just the model the subagent loop uses, without disturbing auth
// or the installed flag. Called when the user switches models mid-session
// (model picker) so subagents track the live model instead of the stale
// startup default. No-op if the config was never installed.
void set_model(std::string model);

// Update the provider's available-models list the router picks cheap roles
// from. Called alongside set_model whenever the model list is (re)loaded or
// the provider changes, so routing always reflects the live provider.
void set_candidates(std::vector<ModelInfo> candidates);

// Update the Smart Mode role config the subagent router honours (Layer 3b).
// Called alongside set_candidates whenever Smart Mode or the model list
// changes. No-op if the config was never installed.
void set_smart(smart::RoleConfig smart);

// Update the provider the parent turn runs on, so pinned slots stay scoped to
// the endpoint that can actually serve them. Pushed alongside set_smart /
// set_candidates whenever the active provider changes.
void set_provider(std::string provider);

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
//
// This is the CEILING across all roles — the bound that exists to stop a
// runaway, not a target. Per-role budgets below are what actually apply;
// keep this >= the largest of them.
inline constexpr int kMaxTurns = 80;

// ── Per-role turn budgets ──────────────────────────────────────
// One global cap could not fit every role, because the roles do
// structurally different work:
//
//   READ-ONLY roles (explorer, reviewer) are a bounded sweep: read, grep,
//   map, summarise. They converge in a handful of turns and a big budget
//   buys nothing but token spend — an explorer still on turn 40 is lost,
//   not thorough.
//
//   WRITE roles (coder, tester, general) are ITERATIVE: read the code,
//   make an edit, build, read the errors, fix, re-build, run the tests,
//   fix again. One honest implementation cycle is easily 6-10 turns, and
//   a real task is several cycles. At 24 turns a coder routinely spent
//   its whole budget on orientation and reported "I ran out of turns" —
//   the failure this sizing exists to prevent. Compilation-heavy work
//   (C++ here) is the worst case: every build is a turn, every error list
//   is another read.
//
// These are CAPS, not quotas: a subagent that finishes in 5 turns stops in
// 5. Nothing is spent by raising a ceiling that isn't reached, so the cost
// of being generous is only paid by runs that would otherwise have FAILED
// at the cap — which is exactly the trade we want.
inline constexpr int kMaxTurnsReadOnly = 32;
inline constexpr int kMaxTurnsWrite    = 80;

// The budget for a role, given whether it may modify the workspace.
[[nodiscard]] inline constexpr int max_turns_for(bool read_only) noexcept {
    return read_only ? kMaxTurnsReadOnly : kMaxTurnsWrite;
}

static_assert(kMaxTurnsReadOnly <= kMaxTurns);
static_assert(kMaxTurnsWrite    <= kMaxTurns);

// Process-wide current nesting depth, incremented while a subagent runs.
// Read by the `task` tool to enforce kMaxDepth. Thread-safe via atomic;
// subagents run on the parent tool's worker thread (run_tool is
// task_isolated), so each nesting level is on its own thread.
[[nodiscard]] int current_depth() noexcept;
void push_depth() noexcept;
void pop_depth() noexcept;

// Provenance of an agent persona by name, for the task card's transparency
// tag: "builtin" (explorer/reviewer/…), "user" (~/.agentty/agents), or
// "project" (a workspace-local .agentty/agents that rode in on the repo).
// Returns "builtin" for an unknown name (the safe default — no tag shown).
// A PROJECT agent's role prompt is attacker-controllable via a clone, so the
// UI surfaces it; it is NOT blocked (its tools stay gated like any other).
[[nodiscard]] std::string_view agent_origin(std::string_view name) noexcept;

} // namespace agentty::tools::subagent
