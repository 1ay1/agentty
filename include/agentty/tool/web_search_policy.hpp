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
// Model's record changes — a reducer never calls install(). Elsewhere
// (`agentty run`, ACP, `mcp serve`, which have no Model) nothing installs, and
// the slot seeds itself from settings.json (env overrides applied) the first
// time anything asks.

#include "agentty/domain/web_search_config.hpp"

namespace agentty::tools::web_search_policy {

// The policy in force. Seeded from settings.json (env overrides applied) the
// first time anything asks; defaults if the file is unreadable.
[[nodiscard]] web_search_cfg::Config current();

// Replace the policy in force. Takes effect for the next request built and
// the next call dispatched; a call already running is not affected.
void install(web_search_cfg::Config c);

} // namespace agentty::tools::web_search_policy
