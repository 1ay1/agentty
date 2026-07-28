#pragma once

// ── Internal ACP backend boundary ────────────────────────────────────────────
//
// The unification layer. Every model backend — a native in-process HTTP+SSE
// adapter (Anthropic / OpenAI / Ollama / Codex-Responses) OR an external ACP
// agent subprocess (codex-acp, claude-agent-acp) — implements ONE contract:
// drive a round, stream normalized `acp::SessionUpdate` events, settle with a
// `TurnResult`. The runtime consumes only that, so a provider's quirks
// (reasoning replay, thinking-signature replay, SSE-vs-NDJSON framing, the
// terminal-event "cancelled" trap) live inside exactly one adapter and never
// leak into the core.
//
// This is the SAME `acp::SessionUpdate` vocabulary agentty already emits
// OUTBOUND from `src/acp/server.cpp` (agentty-as-ACP-agent for Zed). Adopting
// it INBOUND makes the engine speak ACP on both seams.
//
// See docs/internal-acp-backends.md for the full design, mapping tables, and
// migration plan. This header is §5 of that doc.

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include <acp/updates.hpp>   // acp::SessionUpdate

#include "agentty/io/http.hpp"          // http::CancelTokenPtr
#include "agentty/provider/provider.hpp" // provider::Request
#include "agentty/runtime/msg.hpp"       // agentty::StopReason

namespace agentty::provider {

// ── The single streaming sink ────────────────────────────────────────────────
// One normalized event, in ACP's `session/update` vocabulary. Callers pass a
// closure; the adapter fires it once per streamed delta (text, thought, tool
// call, tool-call update, usage). It never carries the terminal event — that is
// the return value of `prompt`, so "emit the terminal exactly once" is
// structural, not a convention four transports must each remember.
struct TurnSink {
    std::function<void(acp::SessionUpdate)> update;

    void operator()(acp::SessionUpdate su) const {
        if (update) update(std::move(su));
    }
};

// ── Terminal failure detail ──────────────────────────────────────────────────
// A round can fail. `user_cancel` distinguishes an Esc (not a fault — the HTTP
// layer reports the intentional abort as "cancelled", and only the adapter
// knows whether that abort was user-driven or its own early read-stop).
// `auth_expired` routes a 401/403 into the OAuth-refresh retry path.
struct TurnError {
    std::string                          message;
    std::optional<std::chrono::seconds>  retry_after;   // server Retry-After hint
    bool                                 auth_expired = false;
    bool                                 user_cancel  = false;
    bool                                 from_stall   = false;
};

// ── How a round ended ────────────────────────────────────────────────────────
// `stop` is agentty's ROUND-level StopReason (keeps `ToolUse`) — the agent loop
// reads it to decide continue-vs-finish. ACP's own turn-level StopReason is
// applied only at the OUTBOUND server seam, where N rounds have already been
// folded into one user-visible turn.
struct TurnResult {
    StopReason               stop = StopReason::EndTurn;
    std::optional<TurnError> error;   // engaged → the round failed

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }

    static TurnResult finished(StopReason s) noexcept { return {s, std::nullopt}; }
    static TurnResult failed(TurnError e) noexcept { return {StopReason::EndTurn, std::move(e)}; }
    static TurnResult cancelled() noexcept {
        TurnError e; e.message = "cancelled"; e.user_cancel = true;
        return failed(std::move(e));
    }
};

// ── The contract every backend satisfies ─────────────────────────────────────
// Type-erased (not a concept) because backends are pluggable and heterogeneous:
// an in-process HTTP adapter and an out-of-process ACP subprocess sit behind the
// same handle. One virtual call per round is free next to a network round-trip.
class AcpBackend {
public:
    virtual ~AcpBackend() = default;

    // Drive ONE provider round-trip (one model completion). Streams
    // `acp::SessionUpdate`s through `sink`; returns the round's `TurnResult`.
    // agentty's agent loop (kick_pending_tools) turns N rounds into one turn and
    // executes any requested tools itself — a `SU_ToolCall` is only the model
    // ASKING for a tool; execution, sandboxing, and permission stay agentty's.
    virtual TurnResult prompt(const Request&              req,
                              const TurnSink&             sink,
                              const http::CancelTokenPtr& cancel) = 0;
};

} // namespace agentty::provider
