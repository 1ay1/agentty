#pragma once
// agentty::provider::dbg — raw wire dump, now a thin shim over logx.
//
// This used to be its own logger with its own env vars (AGENTTY_DEBUG_API,
// AGENTTY_DEBUG_FILE) and its own file. That was one of EIGHT logging knobs,
// and the cost was not the code — it was that nobody could remember which one
// to set, so in practice none got set and bugs were diagnosed by guessing.
//
// Everything now lands on the `wire` channel of the single structured log:
//
//   • non-release builds  — captured by DEFAULT, no env var at all
//   • release builds      — AGENTTY_LOG=wire=trace (or `trace` for everything)
//
// One knob (AGENTTY_LOG), one file (AGENTTY_LOG_FILE, else
// ~/.agentty/logs/agentty.log). Wire bytes sit inline with the auth, tool and
// smart-mode events from the same turn, which is what you actually want when
// a turn misbehaves — the old separate file could not be correlated with
// anything.
//
// Level choice: chunks are Trace (high volume, only wanted when you are
// reading the protocol), request/response metadata is Debug.

#include <string_view>

#include "agentty/util/logx.hpp"

namespace agentty::provider {

// True iff wire-level dumping would be recorded. Call before building an
// expensive message; the macros already gate themselves.
[[nodiscard]] inline bool debug_log() noexcept {
    return logx::enabled(logx::Channel::Wire, logx::Level::Trace);
}

// Dump one raw stream chunk, VERBATIM and untruncated, tagged with the
// dialect that produced it ("openai-chat", "openai-responses",
// "anthropic-messages", "ollama-native").
//
// Verbatim matters: every wire bug this codebase has shipped was a parser
// disagreeing with what the server actually sent, and the parser is the thing
// under suspicion. A pretty-printed or clipped frame hides the evidence — the
// Copilot `response.function_call_arguments.done` event that carried the tool
// arguments (and whose omission made every tool call fail as "invalid args")
// sat past the old 2 KB truncation point in a real turn.
//
// Naming the dialect matters too: one account can serve some models on
// /chat/completions and others on /responses, so a trace without the label
// leaves you guessing which codec to blame.
inline void dbg_chunk(const char* dialect, std::string_view chunk) noexcept {
    AGT_LOG(Wire, Trace, "wire.chunk", "dialect={} bytes={} raw={}",
            dialect, chunk.size(), chunk);
}

} // namespace agentty::provider
