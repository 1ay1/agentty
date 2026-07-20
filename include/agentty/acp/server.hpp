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
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <acp/acp.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/util/ranked_lock.hpp"

namespace agentty::acp {

// ── Lock hierarchy ────────────────────────────────────────────────────────
// The two mutexes below form a STRICT ordering enforced by the type system
// (util::ranked_lock): the outer lock has the LOWER rank and is always taken
// first. Nesting them in the wrong order is a compile error (assert_lock_order)
// or a debug abort (the thread-local held-rank tripwire) — never a silent
// production deadlock. See RUST-CRITIQUE.md #2.
//   SessionRank (10)  session map + per-session config (profile/model/cwd)
//   ThreadRank  (20)  per-session thread.messages
constexpr unsigned kSessionRank = 10;
constexpr unsigned kThreadRank  = 20;
using SessionMutex = util::RankedMutex<kSessionRank>;
using ThreadMutex  = util::RankedMutex<kThreadRank>;

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
    // Guards ALL access to `thread.messages` (and the tool_calls / status
    // fields nested in its elements). The turn loop runs on an OWNED,
    // exception-isolated worker (util::isolated_thread) that appends assistant
    // messages and mutates tool-call statuses, while the reader thread can
    // concurrently snapshot the thread for session/load|resume (replay) or
    // persist. session_mtx_ only protects the session MAP + config, not the
    // thread contents, so without this a load-during-turn is a data race on
    // the messages vector (a read racing a push_back). Held briefly around
    // each mutation / snapshot — NEVER across network streaming or tool
    // execution. shared_ptr so it stays valid for a worker that captured the
    // Session by shared_ptr even after a concurrent session/close erases the
    // map entry. RANK 20 (kThreadRank): the INNER lock — only ever taken while
    // holding, or not holding, session_mtx_ (rank 10), never the reverse.
    std::shared_ptr<ThreadMutex> thread_mtx = std::make_shared<ThreadMutex>();
};

class AgentServer {
public:
    // `stream` is the provider entrypoint (bind AnthropicProvider::stream or a
    // test double). `auth` is the resolved wire credential; may be empty (then
    // prompts report authentication-required). `model_id` is the default model
    // for new sessions.
    AgentServer(::acp::StdioTransport& transport,
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

    // Build the AgentHandlers bundle (captures this). Called in the init list
    // to construct conn_; safe because member storage exists at that point.
    ::acp::AgentHandlers make_handlers();

    // ── The headless turn loop ───────────────────────────────────────────
    void run_turn(std::string session_id, Responder resp);

    StopReason stream_completion(Session& sess, bool& out_cancelled,
                                 std::string& out_error,
                                 bool suppress_tools = false);
    bool       run_tools(Session& sess, bool& out_cancelled);

    // ── Helpers ──────────────────────────────────────────────────────────
    void send_update(const std::string& session_id, ::acp::SessionUpdate update);

    enum class PermissionOutcome { Deny, AllowOnce, AllowAlways };
    PermissionOutcome ask_permission(const std::string& session_id, const ToolUse& tc);

    // Returns a shared_ptr so a worker thread that captured the session can
    // keep it alive even if the reader thread erases the map entry
    // (session/close|delete) mid-turn. Returning a raw Session* here was a
    // use-after-free: the pointer escaped session_mtx_ and a concurrent
    // erase() destroyed the node under it.
    std::shared_ptr<Session> find_session(const std::string& id);

    // ── Session modes ────────────────────────────────────────────────────
    static ::acp::SessionModeState mode_state(Profile current);
    static Profile        profile_from_mode_id(const std::string& mode_id,
                                               Profile fallback);
    static const char*    mode_id_for(Profile p);

    // ── Persisted session index (cwd + title sidecar) ────────────────────
    void                  index_session(const Session& sess);
    void                  unindex_session(const std::string& id);
    nlohmann::json        load_session_index();

    void                  persist(const Session& sess);
    void                  replay_history(const std::string& session_id,
                                         const Thread& thread);

    const std::vector<provider::ToolSpec>& wire_tools();

    ::acp::StdioTransport&    transport_;
    ::acp::ClientConnection   conn_;
    StreamFn                  stream_;
    auth::AuthHeader          auth_;
    std::string               model_id_;
    Profile                   profile_;

    std::once_flag                  tools_once_;   // unused (kept for ABI sanity)
    bool                            wire_tools_built_ = false;
    unsigned long                   wire_tools_gen_   = 0;
    std::vector<provider::ToolSpec> wire_tools_;

    std::mutex                                              index_mtx_;
    // Rank 10 (kSessionRank): the OUTER lock of the session/thread hierarchy.
    SessionMutex                                           session_mtx_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

} // namespace agentty::acp
