#pragma once
// agentty::tools::web_search_policy — the web_search policy the tool layer obeys.
//
// The policy is persisted with the rest of the settings and edited in the
// Web Search pane, but it is READ off the UI thread: by the request builder
// that decides what goes on the wire, and by the dispatch closure that runs
// the tool on a worker. So it lives in one process-wide slot.
//
// Writers: in the TUI, ONLY the host's handle(PublishWebSearchPolicy), which the
// dispatch seam (publish_derived) returns on the first fold and whenever the
// Model's record changes. A reducer never calls install(). Elsewhere
// (`agentty run`, ACP, `mcp serve`, which have no Model) the entry point calls
// seed(io) once at startup, which reads settings.json (env overrides applied).

#include "agentty/domain/web_search_config.hpp"
#include "agentty/util/io.hpp"

namespace agentty::tools::web_search_policy {

// Load the policy from settings.json unless one is already installed.
void seed(Io io);

// The policy in force; defaults until seed() or install() runs.
[[nodiscard]] web_search_cfg::Config current();

// Replace the policy in force. Takes effect for the next request built and
// the next call dispatched; a call already running is not affected.
void install(web_search_cfg::Config c);

} // namespace agentty::tools::web_search_policy
