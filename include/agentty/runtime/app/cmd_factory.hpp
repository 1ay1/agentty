#pragma once
// agentty::app::cmd — factories for the side-effecting commands the runtime issues.
//
// These wrap maya's Cmd<Msg> with agentty-specific glue: kicking off a streaming
// turn, executing a tool, advancing pending tool execution after a turn ends.

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app::cmd {

// Mutates `m` to install a fresh cancel token in m.stream.cancel, then
// dispatches the streaming task on a worker. Esc (CancelStream) flips the
// token to abort the in-flight stream.
[[nodiscard]] maya::Cmd<Msg> launch_stream(Model& m);

// What the model actually sees on the next request: applies any
// Thread::CompactionRecord substitution (latest record's summary
// replaces messages[0..up_to_index) on the wire). Mirrors what
// launch_stream's normal-turn branch ships. Callers use this when
// they need to reason about the wire payload size or shape — the
// auto-compaction triggers in particular need to estimate the
// COMPACTED prefix, not the raw transcript, otherwise they re-fire
// immediately after every compaction.
[[nodiscard]] std::vector<Message> wire_messages_for(const Thread& t);

// Bytes-based prefix token estimate computed against the wire view
// (i.e. with compaction substitution applied). Same approximation as
// `estimate_prefix_tokens(Thread)` but the right denominator for
// auto-compaction logic and the context-gauge.
[[nodiscard]] int estimate_wire_tokens(const Thread& t);

[[nodiscard]] maya::Cmd<Msg> run_tool(ToolCallId id,
                                      ToolName tool_name,
                                      nlohmann::json args);

// Inspect the latest assistant turn and either fire off pending tool calls,
// request permission, or kick the follow-up stream once tool results are in.
// Mutates `m` (sets phase, may push a placeholder assistant message).
[[nodiscard]] maya::Cmd<Msg> kick_pending_tools(Model& m);

[[nodiscard]] maya::Cmd<Msg> fetch_models();

// ── In-app login modal ──────────────────────────────────────────────────
// Fire-and-forget: shells out to the platform browser opener. Wrapped in
// Cmd::task so a wedged xdg-open / open / ShellExecute can never block
// the reducer tick.
[[nodiscard]] maya::Cmd<Msg> open_browser_async(std::string url);

// Run the OAuth code-exchange HTTP POST off the UI thread. Dispatches
// LoginExchanged{result} on completion regardless of success/failure —
// the reducer matches on `expected<OAuthToken, OAuthError>` to decide
// whether to install creds or transition to Failed.
[[nodiscard]] maya::Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                                            auth::PkceVerifier verifier,
                                            auth::OAuthState   state);

// Run the OAuth refresh HTTP POST off the UI thread. Dispatched from
// `AgenttyApp::init()` when `auth::take_pending_refresh()` returned a
// stashed token (i.e. on-disk creds were expired but had a refresh
// token). The TUI is already drawn by the time this runs, so the user
// sees a sticky "refreshing OAuth token…" toast in the bottom row
// instead of the old pre-TUI stderr line, and startup is no longer
// gated on the network round trip.
[[nodiscard]] maya::Cmd<Msg> refresh_oauth(std::string refresh_token);

// Walk ~/.agentty/threads/ and parse every thread JSON off the UI thread.
// Dispatches `ThreadsLoaded{vec}` on completion. The directory walk +
// parse can take seconds with hundreds of multi-MB files in real-world
// use, so it runs as a background task instead of blocking startup;
// `init()` returns immediately with an empty thread list.
[[nodiscard]] maya::Cmd<Msg> load_threads_async();

} // namespace agentty::app::cmd
