#pragma once

// ── ExternalAcpBackend — an out-of-process ACP agent as an AcpBackend ─────────
//
// Implements the `AcpBackend` contract (provider/acp_backend.hpp) by driving a
// *remote* ACP agent — `claude-agent-acp`, `codex-acp`, or any conforming
// `agentclientprotocol` agent — over an rpc::Peer.
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
// It drives an already-connected peer rather than spawning one: the
// subprocess and `initialize` are a separate lifecycle. That keeps it
// testable against an in-memory channel wired to a fake agent, and usable
// over any byte channel. `spawn_acp_agent()` (see the .cpp) produces a
// connected handle for production.
//
// The Claude reference adapter (claude-agent-acp/src/acp-agent.ts) is the model
// this mirrors: a thin protocol translator that owns exactly one quirk surface
// (its `SDKMessage` → `session/update` mapping) and nothing structural.
//
// See docs/internal-acp-backends.md — this header is §6, step 5 of that plan.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <maya/runtime.hpp>

#include <acp/methods.hpp>        // acp::StopReason, PromptParams, NewSessionParams
#include <acp/protocol.hpp>       // the method table, jsonrpc::Router
#include <acp/updates.hpp>        // acp::SessionUpdate, SessionUpdateMsg

#include "agentty/io/http.hpp"                 // http::CancelTokenPtr
#include "agentty/provider/acp_backend.hpp"    // AcpBackend, TurnSink, TurnResult
#include "agentty/provider/provider.hpp"       // provider::Request
#include "agentty/rpc/peer.hpp"                // rpc::Peer, rpc::Dispatch

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

    // MCP servers delegated to the external agent through session/new. The
    // delegated agent owns execution; agentty only observes its tool updates.
    std::vector<acp::McpServer> mcp_servers;

    // Optional client-side capabilities the agent may call back into.
    AcpClientDelegate delegate;
};

class ExternalAcpBackend final : public AcpBackend {
public:
    // Construct the backend BEFORE its peer exists: the peer's dispatch comes
    // from make_dispatch() below, which closes over this backend. Call
    // connect() once the peer is started and initialized. prompt() before
    // connect() fails cleanly.
    explicit ExternalAcpBackend(ExternalAcpOptions opts);

    // Bind the (already-initialized) peer. It MUST outlive this backend's
    // use of it; ownership stays with the caller (usually SpawnedAcpAgent).
    void connect(rpc::Peer& peer) noexcept;

    // Non-copyable, non-movable — the dispatch closes over `this`.
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

    // How the agent's calls reach us (session/update → active sink; fs/*,
    // terminal/*, request_permission → delegate). The spawn factory builds
    // the peer with it. It closes over `this`, so this backend MUST outlive
    // the peer.
    [[nodiscard]] rpc::Dispatch make_dispatch();

private:
    using Router = ::jsonrpc::Router<ExternalAcpBackend>;
    Router make_router() const;

    // Ensure a live session exists; opens one via `session/new` if needed.
    // Returns the session id, or std::nullopt on failure (fills `err`).
    std::optional<acp::SessionId> ensure_session_(const Request& req,
                                                  std::optional<TurnError>& err);

    rpc::Peer*         peer_ = nullptr;   // set once by connect()
    ExternalAcpOptions opts_;
    Router             router_;

    // The reused session id (reuse_session mode), empty until the first round.
    maya::guarded<std::optional<std::string>> session_;

    // The round in flight. session/update arrives on the peer's reader,
    // which joins `inflight` before calling `sink`. prompt() takes the
    // round on exit and waits for in-flight calls, so the sink (which points at
    // prompt's stack) is never called after it returns. No round: updates drop.
    struct Round {
        std::function<void(acp::SessionUpdate)> sink;
        maya::stop_group                        inflight;
    };
    maya::published<Round> round_;

    // terminal/create runs the command synchronously (via the delegate) and
    // keeps its result here under a fresh id until terminal/release.
    struct TerminalState { std::string output; int exit_code = 0; bool truncated = false; };
    struct Terminals {
        std::unordered_map<std::string, TerminalState> by_id;
        std::uint64_t                                  next_id = 1;
    };
    maya::guarded<Terminals> terminals_;
};

// ── Turn-level StopReason mapping (pure, testable) ───────────────────────────
// ACP's turn-level StopReason → agentty's round-level StopReason. `cancelled`
// and `refusal` are NOT stop reasons in agentty's ladder — the caller turns
// them into a TurnError instead — so this maps only the "clean finish" arms;
// the caller special-cases cancelled/refusal before calling.
[[nodiscard]] StopReason map_acp_stop_reason(acp::StopReason r) noexcept;

// ── Subprocess factory ───────────────────────────────────────────────────────
// A connected, initialized agent and the process behind it. Destroy it to
// tear the agent down (closes pipes, waits/kills the child).
struct SpawnedAcpAgent {
    std::unique_ptr<rpc::Peer> connection;
    // Opaque owner of the subprocess. Reset after the peer: the peer's
    // reader reads the child's stdout stream.
    std::shared_ptr<void> process;

    SpawnedAcpAgent() = default;
    SpawnedAcpAgent(SpawnedAcpAgent&&) noexcept = default;
    SpawnedAcpAgent& operator=(SpawnedAcpAgent&&) noexcept = default;
    SpawnedAcpAgent(const SpawnedAcpAgent&) = delete;
    SpawnedAcpAgent& operator=(const SpawnedAcpAgent&) = delete;
    ~SpawnedAcpAgent() { reset(); }

    void reset() noexcept;

    [[nodiscard]] bool ok() const noexcept { return connection != nullptr; }
};

// Spawn `argv[0]` (e.g. "claude-agent-acp" or "codex-acp") with the remaining
// args as an ACP agent subprocess (mcp::cap::ChildProcess: fork/exec/pipe on
// POSIX, CreateProcess on Windows), run an rpc::Peer over its stdio with
// `dispatch`, run `initialize`, and return the connected handle. On failure
// returns a SpawnedAcpAgent with a null connection and `err` filled.
//
// PRODUCTION FLOW (resolves the backend/dispatch/peer cycle):
//     ExternalAcpBackend backend{opts};
//     auto agent = spawn_acp_agent(argv, init, backend.make_dispatch(), err);
//     if (!agent.ok()) { /* handle err */ }
//     backend.connect(*agent.connection);
//     // ... backend.prompt(...) ...  (keep `agent` alive for the backend's life)
[[nodiscard]] SpawnedAcpAgent spawn_acp_agent(const std::vector<std::string>& argv,
                                              const acp::InitializeParams&    init,
                                              rpc::Dispatch                   dispatch,
                                              std::string&                    err);

} // namespace agentty::provider
