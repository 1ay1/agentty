#pragma once
// agentty::acp::AgentServer — the ACP agent that lets agentty run as a
// subprocess Zed (or any ACP client) drives over stdio.
//
// Lifecycle (JSON-RPC methods the client calls on us). We implement the full
// ACP v1 agent surface; every optional method is advertised via the matching
// capability in `initialize`:
//   initialize          → negotiate protocol version + advertise capabilities
//   authenticate        → no-op success (we authenticate ourselves from
//                          ~/.config/agentty); reports auth-required if creds
//                          are missing
//   logout              → drop the in-process credential (auth.logout cap)
//   session/new         → mint a session id, record cwd, start an empty Thread;
//                          return the available session modes
//   session/load        → restore a persisted Thread, replay history, return modes
//   session/resume      → like load but resolves with the mode/config state
//   session/list        → enumerate persisted sessions (sessionCapabilities.list)
//   session/close       → drop a session from memory (sessionCapabilities.close)
//   session/delete      → delete a persisted session (sessionCapabilities.delete)
//   session/set_mode    → switch permission tier (Ask/Write/Minimal as ACP modes)
//   session/set_config_option → set a per-session config option (e.g. model)
//   session/prompt      → run ONE agent turn against the model; stream
//                         session/update notifications as text/tools land;
//                         resolve with a StopReason when the turn settles
//   session/cancel      → (notification) trip the in-flight turn's cancel token
//
// While a turn runs we call BACK to the client:
//   session/update              (notification) — message chunks, tool calls,
//                              and a usage_update (token accounting) per
//                              completion so the client's context meter tracks
//   session/request_permission  (request)      — gate WriteFs/Exec tools
//
// The turn loop mirrors the TUI's launch_stream + kick_pending_tools
// (src/runtime/app/cmd_factory.cpp) but without maya: same provider, same
// tools::registry, same permission policy, same wire-message shaping.
//
// The provider is type-erased behind a std::function so the turn loop is
// testable against a scripted in-memory provider, exactly like the TUI.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/acp/jsonrpc.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::acp {

// The provider call, type-erased: (Request, EventSink) → void. Matches
// provider::Provider::stream. Lets the server hold any provider without
// templating the whole class.
using StreamFn =
    std::function<void(provider::Request, provider::EventSink)>;

// Per-session state: one agentty Thread + the workspace cwd the client
// opened the session against, plus the in-flight turn's cancel handle.
// `profile` is the session's live permission tier, switchable at runtime via
// session/set_mode (it shadows the server-wide default). `model` overrides
// the server default model for this session (session/set_config_option).
struct Session {
    std::string id;
    std::string cwd;
    Thread      thread;
    Profile     profile = Profile::Ask;
    std::string model;   // empty = use server default
    std::shared_ptr<http::CancelToken> cancel;
    // Session-scoped "always allow" grants keyed by tool name, set when
    // the client picks the allow_always option. Consulted in run_tools
    // before prompting so a granted tool never re-asks this session.
    std::set<std::string> grants;
};

class AgentServer {
public:
    // `stream` is the provider entrypoint (bind AnthropicProvider::stream or
    // a test double). `auth` is the resolved wire credential; may be empty,
    // in which case prompts report authentication-required. `model_id` is the
    // default model for new sessions.
    AgentServer(rpc::Peer&        peer,
                StreamFn          stream,
                auth::AuthHeader  auth,
                std::string       model_id,
                Profile           profile = Profile::Ask);

    // Install handlers and block on peer.run() until the client disconnects.
    // Returns a process exit code.
    int serve();

private:
    // ── JSON-RPC method handlers ─────────────────────────────────────────
    rpc::Outcome handle_request(const std::string& method,
                                const nlohmann::json& params,
                                const nlohmann::json& id);
    void         handle_notification(const std::string& method,
                                     const nlohmann::json& params);

    nlohmann::json on_initialize(const nlohmann::json& params);
    nlohmann::json on_new_session(const nlohmann::json& params);
    nlohmann::json on_list_sessions(const nlohmann::json& params);
    nlohmann::json on_resume_session(const nlohmann::json& params);
    nlohmann::json on_close_session(const nlohmann::json& params);
    nlohmann::json on_delete_session(const nlohmann::json& params);
    nlohmann::json on_set_mode(const nlohmann::json& params);
    nlohmann::json on_set_config_option(const nlohmann::json& params);
    nlohmann::json on_logout(const nlohmann::json& params);
    // session/load: restore a persisted Thread from disk, replay its history
    // as session/update notifications, then resolve. Returns the load result
    // (null body). Throws on unknown / unreadable session id.
    nlohmann::json on_load_session(const nlohmann::json& params);
    void           on_prompt(const nlohmann::json& id, const nlohmann::json& params);
    void           on_cancel(const nlohmann::json& params);

    // ── The headless turn loop ───────────────────────────────────────────
    // Runs to completion on a worker thread. Streams session/update
    // notifications and resolves the deferred session/prompt response with a
    // stopReason. `session_id` keys the session under session_mtx_.
    void run_turn(nlohmann::json prompt_id, std::string session_id);

    // Drive ONE model completion (one SSE stream): build the wire request,
    // call stream_, translate Msg events to ACP session/update. Returns the
    // anthropic StopReason of the completion and appends the assistant
    // Message (with any tool_calls) to the session thread. `out_cancelled`
    // is set if the turn was cancelled mid-stream.
    StopReason stream_completion(Session& sess, bool& out_cancelled,
                                 std::string& out_error);

    // Execute every pending tool call on the last assistant message,
    // requesting permission where the policy demands it. Appends the
    // tool_result-bearing User message. Returns false if the turn was
    // cancelled while awaiting permission.
    bool run_tools(Session& sess, bool& out_cancelled);

    // ── Helpers ──────────────────────────────────────────────────────────
    void send_update(const std::string& session_id, nlohmann::json update);
    // session/request_permission round-trip. Returns the user's choice so
    // run_tools can record a session-scoped grant on allow_always.
    enum class PermissionOutcome { Deny, AllowOnce, AllowAlways };
    PermissionOutcome ask_permission(const std::string& session_id, const ToolUse& tc);

    Session* find_session(const std::string& id);

    // ── Session modes ────────────────────────────────────────────────────
    // agentty's three permission tiers surface as ACP session modes so Zed
    // can switch them from its mode picker. Built once; shared by every
    // new/load/resume response.
    static nlohmann::json mode_state(Profile current);
    static Profile        profile_from_mode_id(const std::string& mode_id,
                                               Profile fallback);
    static const char*    mode_id_for(Profile p);

    // ── Persisted session index (cwd + title sidecar) ────────────────────
    // The core Thread store has no cwd field, but ACP SessionInfo requires
    // one. We keep a tiny sidecar index (threads_dir()/acp_sessions.json)
    // mapping sessionId → {cwd, title, updatedAt} so session/list can report
    // accurate metadata across a subprocess restart.
    void                  index_session(const Session& sess);
    void                  unindex_session(const std::string& id);
    nlohmann::json        load_session_index();   // { id: {cwd,title,updatedAt} }

    // Persist a session's Thread to the on-disk store (same format the TUI
    // uses, so ACP sessions show up in the thread picker and survive a
    // subprocess restart). Called after every turn and on session creation.
    void persist(const Session& sess);

    // Replay a thread's full conversation history to the client as
    // session/update notifications (user_message_chunk / agent_message_chunk
    // + tool_call cards), per the session/load contract.
    void replay_history(const std::string& session_id, const Thread& thread);

    // Wire tool list, built once from the static tools::registry(). The
    // registry never changes at runtime, so we snapshot it on first use
    // instead of rebuilding the vector every completion.
    const std::vector<provider::ToolSpec>& wire_tools();

    rpc::Peer&       peer_;
    StreamFn         stream_;
    auth::AuthHeader auth_;
    std::string      model_id_;
    Profile          profile_;

    std::once_flag                  tools_once_;
    std::vector<provider::ToolSpec> wire_tools_;

    std::mutex                                   session_mtx_;
    std::unordered_map<std::string, Session>     sessions_;
    std::mutex                                   index_mtx_;  // guards the cwd sidecar
    std::uint64_t                                next_tool_uid_ = 1;
};

} // namespace agentty::acp
