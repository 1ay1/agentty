#pragma once
// agentty::acp::AgentServer — the ACP agent that lets agentty run as a
// subprocess Zed (or any ACP client) drives over stdio.
//
// Built on the acp-cpp library (acp-cpp/ submodule): acp-cpp owns the wire
// algebra, JSON-RPC engine, codecs, and stdio transport. This class is the
// agentty-specific glue: it implements the AgentHandlers surface, drives a
// headless turn loop against the same provider + tools + permission policy
// the TUI uses, and translates each step into acp-cpp SessionUpdate values.
//
// session/prompt is registered as an ASYNC handler (on_session_prompt_async):
// a prompt drives a whole turn that streams session/update notifications and
// calls BACK to the client (session/request_permission) before it can
// resolve. Those callbacks need the engine's reader thread free to deliver
// their responses, so the handler hands an acp::RpcEngine::Responder to a
// worker thread and returns immediately; the worker resolves it when the turn
// settles. (See acp-cpp's deferred-response support.)
//
// Lifecycle (ACP v1 agent surface; every optional method is advertised via
// the matching capability in `initialize`):
//   initialize / authenticate / logout
//   session/new · load · resume · list · close · delete
//   session/set_mode · set_config_option
//   session/prompt (async) · session/cancel (notification)

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <acp/acp.hpp>

#include <maya/runtime.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::acp {

// The provider call, type-erased: (Request, EventSink) → void. Matches
// provider::Provider::stream.
using StreamFn =
    std::function<void(provider::Request, provider::EventSink)>;

// Per-session state: one agentty Thread + the workspace cwd the client opened
// the session against, plus the in-flight turn's cancel handle. `profile` is
// the live permission tier (session/set_mode). `model` overrides the server
// default for this session (session/set_config_option).
struct Session {
    std::string id;
    std::string cwd;
    Thread      thread;
    Profile     profile = Profile::Ask;
    std::string model;   // empty = use server default
    std::shared_ptr<http::CancelToken> cancel;
    std::set<std::string> grants;   // session-scoped "always allow", by tool
};

// Every live session plus the in-flight prompt map, behind ONE lock. The turn
// loop runs on a worker while the reader thread serves load/list/set_mode/
// cancel, so all of it is shared. Nothing holds the lock across a network
// stream or a tool run: the turn works on snapshots and writes results back
// by id, so a session closed mid-turn just makes the write-back a no-op.
struct Sessions {
    std::unordered_map<std::string, Session> by_id;
    // JSON-RPC id (id.dump()) of an in-flight session/prompt → its session,
    // so a generic $/cancel_request can cancel the right turn. Set in
    // on_prompt, erased when the turn settles.
    std::unordered_map<std::string, std::string> reqid_to_session;
};

class AgentServer {
public:
    // `stream` is the provider entrypoint (bind AnthropicProvider::stream or a
    // test double). `auth` is the resolved wire credential; may be empty (then
    // prompts report authentication-required). `model_id` is the default model
    // for new sessions.
    AgentServer(::acp::FdTransport& transport,
                StreamFn          stream,
                auth::AuthHeader  auth,
                std::string       model_id,
                Profile           profile = Profile::Ask);

    // Install handlers, start the transport's read pump, and block until the
    // client disconnects (EOF on stdin). Returns a process exit code.
    int serve();

private:
    using Responder = ::acp::RpcEngine::Responder<::acp::PromptResult>;

    // ── AgentHandlers method handlers (typed via acp-cpp) ─────────────────
    ::acp::InitializeResult     on_initialize(const ::acp::InitializeParams&);
    ::acp::NewSessionResult     on_new_session(const ::acp::NewSessionParams&);
    ::acp::ListSessionsResult   on_list_sessions(const ::acp::ListSessionsParams&);
    void                        on_load_session(const ::acp::LoadSessionParams&);
    ::acp::ResumeSessionResult  on_resume_session(const ::acp::ResumeSessionParams&);
    void                        on_close_session(const ::acp::CloseSessionParams&);
    void                        on_delete_session(const ::acp::DeleteSessionParams&);
    void                        on_set_mode(const ::acp::SetModeParams&);
    ::acp::SetConfigOptionResult on_set_config_option(const ::acp::SetConfigOptionParams&);
    void                        on_logout();
    void                        on_prompt(const ::acp::PromptParams&, Responder);
    void                        on_cancel(const ::acp::CancelParams&);
    // Generic $/cancel_request: cancel the turn whose session/prompt request
    // has this JSON-RPC id (falls back to a no-op for an unknown id).
    void                        on_cancel_request(const ::acp::RpcId& id);

    // Build the AgentHandlers bundle (captures this). Called in the init list
    // to construct conn_; safe because member storage exists at that point.
    ::acp::AgentHandlers make_handlers();

    // ── The headless turn loop ───────────────────────────────────────────
    void run_turn(std::string session_id, std::string req_id_dump, Responder resp);

    StopReason stream_completion(const std::string& session_id, bool& out_cancelled,
                                 std::string& out_error,
                                 bool suppress_tools = false);
    bool       run_tools(const std::string& session_id, bool& out_cancelled);

    // ── Helpers ──────────────────────────────────────────────────────────
    void send_update(const std::string& session_id, ::acp::SessionUpdate update);

    // Emit the slash-command menu + model-picker config option for a session,
    // so Zed's composer `/` menu and model dropdown match the native agent.
    // Sent as session/update notifications; call outside the session lock.
    void emit_session_config(const std::string& session_id,
                             const std::string& model_id);
    // Complete config-option state (SSOT for the initial advertise + every
    // post-change echo). Reads live session profile/model.
    void emit_config_state(const std::string& session_id,
                           const std::string& fallback_model);

    enum class PermissionOutcome { Deny, AllowOnce, AllowAlways };
    PermissionOutcome ask_permission(const std::string& session_id, const ToolUse& tc);

    // Live-terminal execution of a `bash` tool call via the client's ACP
    // terminal (terminal/create → wait_for_exit → output → release), so the
    // command streams into Zed's native terminal widget instead of running
    // internally and returning static text. Only used when the client
    // advertised `terminal` AND agentty's sandbox is NOT wrapping commands
    // (otherwise internal execution keeps the sandbox contract). Emits the
    // InProgress/Completed/Failed SU_ToolCallUpdate itself (with a terminal
    // content block) and sets tc.status. Returns the outcome so run_tools can
    // honour cancellation; std::nullopt means "not eligible — fall back to
    // internal execution".
    struct TerminalRun { bool ok; bool cancelled; std::string output; };
    // What a tool run needs from its session, copied out once per batch.
    struct TurnCtx {
        std::string id, cwd, msg_id;
        std::shared_ptr<http::CancelToken> cancel;
        [[nodiscard]] bool cancelled() const { return cancel && cancel->is_cancelled(); }
    };
    std::optional<TerminalRun> run_bash_via_terminal(const TurnCtx& ctx, ToolUse& tc);

    // Settle a tool call: set it on the local copy AND write it back into
    // the live session (matched by message id + tool id), in one step.
    void settle_tool(const TurnCtx& ctx, ToolUse& tc, ToolUse::Status st,
                     std::chrono::steady_clock::time_point executing_since = {});

    // Run f(Session&, args...) on the live session `id` under the sessions
    // lock; nullopt if there is no such session. f is captureless and its
    // args/result are copied in and out, the same rules as guarded<T>::with,
    // because that is what this is. Nothing escapes that points into a
    // session, so a concurrent close can never free one under a reader.
    template <class F, class... Args>
    auto with_session(const std::string& id, F f, Args... args) {
        (void)f;
        using R = std::invoke_result_t<F&, Session&, Args&&...>;
        return sessions_.with([](Sessions& ss, std::string k, Args... a) -> std::optional<R> {
            auto it = ss.by_id.find(k);
            if (it == ss.by_id.end()) return std::nullopt;
            return F{}(it->second, std::move(a)...);
        }, id, std::move(args)...);
    }

    // ── Session modes ────────────────────────────────────────────────────
    static ::acp::SessionModeState mode_state(Profile current);
    static Profile        profile_from_mode_id(const std::string& mode_id,
                                               Profile fallback);
    static const char*    mode_id_for(Profile p);

    // ── Persisted session index (cwd + title sidecar) ────────────────────
    void                  index_session(const std::string& id);
    void                  unindex_session(const std::string& id);
    nlohmann::json        load_session_index();

    void                  persist(const std::string& id);
    void                  replay_history(const std::string& session_id,
                                         const Thread& thread);

    ::acp::FdTransport&       transport_;
    ::acp::ClientConnection   conn_;
    StreamFn                  stream_;
    auth::AuthHeader          auth_;
    std::string               model_id_;
    Profile                   profile_;

    // Did the client advertise `terminal` support at initialize? When true
    // (and agentty's sandbox is NOT wrapping commands), a `bash` tool call is
    // executed via the client's ACP terminal (terminal/create → wait → output)
    // instead of internally, so Zed renders its native streaming terminal
    // widget — the same UX as claude-code-acp. Set once in on_initialize
    // (reader thread, before any prompt), read-only thereafter.
    std::atomic<bool>         client_supports_terminal_{false};

    // The ACP protocol version agreed at initialize (min of ours + theirs).
    // The clean-cut seam for v1-vs-v2 emission: a session's config surface is
    // rendered ONE way per connection — v1 clients get SessionModeState, v2+
    // clients get the `mode` config option — never both. Set once in
    // on_initialize (reader thread, before any session), read-only thereafter.
    std::atomic<int>          negotiated_version_{::acp::kProtocolVersion};
    [[nodiscard]] bool v2_config() const noexcept {
        return negotiated_version_.load(std::memory_order_relaxed) >= 2;
    }


    maya::guarded<Sessions>   sessions_;
};

} // namespace agentty::acp
