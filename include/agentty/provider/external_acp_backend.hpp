#pragma once

// ── ExternalAcpBackend — an out-of-process ACP agent as an AcpBackend ─────────
//
// Implements the `AcpBackend` contract (provider/acp_backend.hpp) by driving a
// *remote* ACP agent — `claude-agent-acp`, `codex-acp`, or any conforming
// `agentclientprotocol` agent — over acp-cpp's editor-side `AgentConnection`.
// agentty is the CLIENT (editor); the subprocess is the AGENT (LLM side).
//
// This is the INBOUND direction of the same `acp::SessionUpdate` vocabulary
// agentty already emits OUTBOUND from src/acp/server.cpp. One `prompt()` call:
//
//   1. lazily opens a session (`session/new` once, then reused across rounds);
//   2. sends the tail user message as `session/prompt`;
//   3. forwards every inbound `session/update` — agent message / thought /
//      tool_call / tool_call_update / usage — straight to the `TurnSink`;
//   4. settles the round with a `TurnResult` whose `StopReason` is decoded from
//      the agent's turn-level ACP `StopReason` (end_turn / max_tokens /
//      refusal / cancelled), applying the terminal-event discipline exactly
//      once — no spurious "cancelled" on a clean turn.
//
// WHY it drives an already-connected `AgentConnection` rather than spawning:
// the connection (transport + subprocess + `initialize`) is a separate
// lifecycle concern. Injecting it keeps this class (a) unit-testable against an
// in-memory `FdTransport` pair wired to a fake agent, with zero subprocess or
// sandbox dependency, and (b) reusable for a native-pipe agent, a socket
// agent, or a test double. `spawn_acp_agent()` (see the .cpp) is the
// convenience factory that produces a connected handle for production.
//
// The Claude reference adapter (claude-agent-acp/src/acp-agent.ts) is the model
// this mirrors: a thin protocol translator that owns exactly one quirk surface
// (its `SDKMessage` → `session/update` mapping) and nothing structural.
//
// See docs/internal-acp-backends.md — this header is §6, step 5 of that plan.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <acp/agent.hpp>          // acp::AgentConnection, acp::ClientHandlers
#include <acp/methods.hpp>        // acp::StopReason, PromptParams, NewSessionParams
#include <acp/updates.hpp>        // acp::SessionUpdate, SessionUpdateMsg

#include "agentty/io/http.hpp"                 // http::CancelTokenPtr
#include "agentty/provider/acp_backend.hpp"    // AcpBackend, TurnSink, TurnResult
#include "agentty/provider/provider.hpp"       // provider::Request

namespace agentty::provider {

// ── Sandbox delegate ─────────────────────────────────────────────────────────
// An external agent may ask the CLIENT (us) to read/write files and run
// terminals on its behalf (ACP `fs/*` and `terminal/*`). agentty already owns a
// sandbox + permission gate for exactly this; rather than reach into it from
// here, the backend takes a small delegate so the wiring stays in one place and
// tests can supply an in-memory stub. All hooks are optional — an unset hook
// declines the capability (the agent is told the method is unsupported), which
// is the safe default: a mis-declared agent can't silently escape the sandbox.
struct AcpClientDelegate {
    // fs/read_text_file → return file contents (optionally a line window).
    std::function<std::optional<std::string>(
        const std::string& path, std::optional<int> line, std::optional<int> limit)>
        read_text_file;

    // fs/write_text_file → persist; return false to refuse (permission denied).
    std::function<bool(const std::string& path, const std::string& content)>
        write_text_file;

    // request_permission → true = allow the tool the agent is about to run.
    // Unset ⇒ allow (the agent's own gate governs); return false to deny.
    std::function<bool(const acp::RequestPermissionParams&)> request_permission;

    // terminal/create → run a command and return its result. The ACP terminal
    // protocol is a stateful lifecycle (create → output/wait_for_exit →
    // kill/release), but a synchronous executor satisfies it by running the
    // command to completion inside create() and caching the result for the
    // follow-up calls — which is exactly what run_terminal does here. Return
    // value: (merged stdout+stderr, exit code). Unset ⇒ terminals declined.
    struct TerminalResult {
        std::string output;
        int         exit_code = 0;
        bool        truncated = false;
    };
    std::function<TerminalResult(const std::string& command,
                                 const std::vector<std::string>& args,
                                 const std::optional<std::string>& cwd)>
        run_terminal;
};

// A delegate wired to agentty's REAL boundary: fs reads go through the read
// gate (workspace + skill read-allowlist, symlink-escape blocked), writes
// through the write gate (workspace only), and terminal/permission through the
// OS-native sandbox. This is the delegate production callers want; tests supply
// their own in-memory stub. Defined in the .cpp so the header stays free of the
// tool/sandbox headers.
[[nodiscard]] AcpClientDelegate default_sandbox_delegate();

// ── Options ──────────────────────────────────────────────────────────────────
struct ExternalAcpOptions {
    // Working directory handed to `session/new` (the agent's project root).
    std::string cwd;

    // If true, one session is opened on the first `prompt()` and reused for
    // every subsequent round (the agent keeps its own transcript — this is how
    // claude-agent-acp / codex-acp work, and it's what lets the agent replay
    // its own reasoning between tool rounds without us re-sending it). If
    // false, a fresh session is opened per round (stateless; for agents that
    // don't persist, or for strict per-round isolation in tests).
    bool reuse_session = true;

    // Optional client-side capabilities the agent may call back into.
    AcpClientDelegate delegate;
};

class ExternalAcpBackend final : public AcpBackend {
public:
    // Construct the backend BEFORE its connection exists. This is required by
    // the spawn flow: acp::AgentConnection installs handlers at construction,
    // and those handlers are produced by make_handlers() below (which closes
    // over this backend) — so the backend must exist first. Call connect() once
    // the connection is built and initialized. prompt() before connect() fails
    // cleanly.
    explicit ExternalAcpBackend(ExternalAcpOptions opts);

    // Bind the (already-initialized) connection. `conn` MUST outlive this
    // backend — ownership stays with the caller (usually SpawnedAcpAgent).
    void connect(acp::AgentConnection& conn) noexcept;

    // Non-copyable, non-movable — handlers close over `this`.
    ExternalAcpBackend(const ExternalAcpBackend&)            = delete;
    ExternalAcpBackend& operator=(const ExternalAcpBackend&) = delete;

    ~ExternalAcpBackend() override = default;

    // AcpBackend contract. Drives one round; streams SessionUpdates to `sink`;
    // returns the round's TurnResult. Thread-compatible: one round at a time.
    TurnResult prompt(const Request&              req,
                      const TurnSink&             sink,
                      const http::CancelTokenPtr& cancel) override;

    // Test/inspection hook: the session id opened by the first prompt (empty
    // until then, or in per-round mode).
    [[nodiscard]] std::string session_id() const;

    // Build the acp::ClientHandlers that route the agent's inbound callbacks
    // (session/update → active sink; fs/* + terminal/* + request_permission →
    // delegate). acp::AgentConnection installs handlers at CONSTRUCTION only, so
    // the spawn factory calls this to obtain them BEFORE the connection is
    // built, and hands the same connection back to the backend. The handlers
    // close over `this`, so this backend MUST outlive the connection.
    [[nodiscard]] acp::ClientHandlers make_handlers();

private:
    // Ensure a live session exists; opens one via `session/new` if needed.
    // Returns the session id, or std::nullopt on failure (fills `err`).
    std::optional<acp::SessionId> ensure_session_(const Request& req,
                                                  std::optional<TurnError>& err);

    acp::AgentConnection* conn_ = nullptr;   // set by connect(); non-owning
    ExternalAcpOptions    opts_;

    mutable std::mutex          mu_;
    std::optional<acp::SessionId> session_;

    // The sink for the round currently in flight. session/update arrives on the
    // transport's reader thread, so `prompt()` publishes the active sink here
    // for the duration of the round and clears it on exit. A null sink drops
    // updates (e.g. a late notification after the round settled) rather than
    // crashing — matching the "emit terminal exactly once" invariant.
    std::mutex                              sink_mu_;
    std::function<void(acp::SessionUpdate)> active_sink_;

    // Terminal lifecycle state. terminal/create runs the command synchronously
    // (via the delegate) and caches its result here under a generated id;
    // terminal/output + terminal/wait_for_exit read it back; terminal/release
    // drops it. Guarded by its own mutex — terminal callbacks arrive on the
    // transport reader thread, independent of the round's sink.
    struct TerminalState { std::string output; int exit_code = 0; bool truncated = false; };
    std::mutex                                       term_mu_;
    std::unordered_map<std::string, TerminalState>   terminals_;
    std::uint64_t                                    next_terminal_id_ = 1;
};

// ── Turn-level StopReason mapping (pure, testable) ───────────────────────────
// ACP's turn-level StopReason → agentty's round-level StopReason. `cancelled`
// and `refusal` are NOT stop reasons in agentty's ladder — the caller turns
// them into a TurnError instead — so this maps only the "clean finish" arms;
// the caller special-cases cancelled/refusal before calling.
[[nodiscard]] StopReason map_acp_stop_reason(acp::StopReason r) noexcept;

// ── Subprocess factory ───────────────────────────────────────────────────────
// A connected, initialized agent + the process/transport that backs it. Destroy
// it to tear the agent down (closes pipes, waits/kills the child).
struct SpawnedAcpAgent {
    std::unique_ptr<acp::AgentConnection> connection;
    // Opaque lifetime holder for the subprocess + transport threads. Kept type-
    // erased so this header doesn't drag in the OS process headers.
    std::shared_ptr<void> process;

    [[nodiscard]] bool ok() const noexcept { return connection != nullptr; }
};

// Spawn `argv[0]` (e.g. "claude-agent-acp" or "codex-acp") with the remaining
// args as an ACP agent subprocess, wire its stdio to an acp::AgentConnection
// over acp's StdioTransport (portable: POSIX fork/exec/pipe, Windows
// CreateProcess/CreatePipe via mcp::cap::ChildProcess), install `handlers`, run
// `initialize`, and return the connected handle. On failure returns a
// SpawnedAcpAgent with a null connection and `err` filled.
//
// PRODUCTION FLOW (resolves the ctor/handlers/connection cycle):
//     ExternalAcpBackend backend{opts};
//     auto agent = spawn_acp_agent(argv, init, backend.make_handlers(), err);
//     if (!agent.ok()) { /* handle err */ }
//     backend.connect(*agent.connection);
//     // ... backend.prompt(...) ...  (keep `agent` alive for the backend's life)
[[nodiscard]] SpawnedAcpAgent spawn_acp_agent(const std::vector<std::string>& argv,
                                              const acp::InitializeParams&    init,
                                              acp::ClientHandlers             handlers,
                                              std::string&                    err);

} // namespace agentty::provider
