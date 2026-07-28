#pragma once

// ── agentty::provider — ExternalAcpBackend → Provider adapter ────────────────
//
// Presents an external ACP agent subprocess as a plain `stream(Request,
// EventSink)` Provider, so the runtime's ONE dispatch seam (main.cpp
// `stream_fn`) can route to it with a single branch — exactly like the native
// Anthropic / ChatGPT providers. No new `Kind` switch fans out across the
// codebase; the agent-specific ACP quirks stay inside ExternalAcpBackend.
//
// Responsibilities:
//   • Lazily SPAWN (and cache) the agent subprocess keyed by its spec id, using
//     the launch spec from acp_agents.hpp. One long-lived subprocess per agent
//     serves every request — its ACP session is reused across rounds.
//   • Translate one `prompt()` round: acp::SessionUpdate → agentty Msg
//     (StreamTextDelta / StreamThinkingDelta / StreamToolUse* / StreamUsage),
//     then settle with StreamFinished or StreamError from the TurnResult.
//   • Cancellation: the Request carries no cancel token, so this owns one and
//     wires it through so Esc mid-stream reaches the agent.
//
// Thread-safety: the spawn cache is guarded; stream() may be called from the
// provider worker thread. The cached subprocess is torn down at process exit
// (or when release_acp_agents() is called).

#include <string>

#include "agentty/provider/provider.hpp"   // Request, EventSink, StreamResult

namespace agentty::provider {

// Drive the external ACP agent named by `agent_id` (a registry spec id such as
// "claude-agent-acp" / "codex-acp", or a config-defined id) for one request,
// streaming normalized Msgs into `sink`. Spawns + caches the subprocess on
// first use. On spawn/launch failure emits StreamStarted + StreamError so the
// UI shows a clean error instead of a silent hang. Returns a StreamResult
// naming how the round ended (mapped from the ACP backend's own TurnResult) so
// the ACP arm reports its outcome through the same value as the native ones.
StreamResult stream_external_acp(const std::string&   agent_id,
                                 Request               req,
                                 EventSink             sink);

// Tear down every cached agent subprocess (idempotent). Called at shutdown; the
// per-agent teardown uses ExternalAcpBackend's hardened watchdog so a wedged
// agent can't hang the exit path.
void release_acp_agents() noexcept;

} // namespace agentty::provider
