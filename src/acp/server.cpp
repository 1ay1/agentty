// agentty::acp::AgentServer — implementation. See server.hpp for the design.
//
// This is the headless analogue of the TUI turn loop in
// src/runtime/app/cmd_factory.cpp. It reuses the SAME provider, tools, wire
// shaping, and permission policy, but drives them synchronously on a worker
// thread and translates every step into ACP session/update notifications.

#include "agentty/acp/server.hpp"

#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentty/diff/diff.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/policy.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::acp {
namespace {

using nlohmann::json;

// agentty spec::Kind → ACP ToolKind string. ACP kinds drive icon/UI choice
// on the client; "other" is the safe default.
const char* acp_tool_kind(std::string_view tool_name) {
    namespace sp = tools::spec;
    const auto* s = sp::lookup(tool_name);
    if (!s) return "other";
    switch (s->kind) {
        case sp::Kind::Read:           return "read";
        case sp::Kind::Edit:           return "edit";
        case sp::Kind::Write:          return "edit";
        case sp::Kind::Bash:           return "execute";
        case sp::Kind::Diagnostics:    return "execute";
        case sp::Kind::GitCommit:      return "execute";
        case sp::Kind::Grep:           return "search";
        case sp::Kind::Glob:           return "search";
        case sp::Kind::FindDefinition: return "search";
        case sp::Kind::ListDir:        return "read";
        case sp::Kind::GitStatus:      return "read";
        case sp::Kind::GitDiff:        return "read";
        case sp::Kind::GitLog:         return "read";
        case sp::Kind::WebFetch:       return "fetch";
        case sp::Kind::WebSearch:      return "fetch";
        case sp::Kind::Todo:           return "think";
        case sp::Kind::Remember:       return "other";
        case sp::Kind::Forget:         return "other";
        case sp::Kind::Wipe:           return "other";
    }
    return "other";
}

// One-line human title for a tool call card. Keeps the model's raw args out
// of the headline; the rawInput field carries the full args.
std::string tool_title(const ToolUse& tc) {
    const auto& a = tc.args;
    auto str = [&](const char* k) -> std::string {
        if (a.contains(k) && a[k].is_string()) return a[k].get<std::string>();
        return {};
    };
    if (tc.name.value == "read"  || tc.name.value == "edit"
     || tc.name.value == "write") {
        std::string p = str("path");
        if (!p.empty()) return tc.name.value + " " + p;
    }
    if (tc.name.value == "bash") {
        std::string c = str("command");
        if (!c.empty()) return "bash: " + c.substr(0, 80);
    }
    if (tc.name.value == "grep" || tc.name.value == "glob") {
        std::string p = str("pattern");
        if (!p.empty()) return tc.name.value + " " + p;
    }
    return tc.name.value;
}

// File locations a tool call touches, for Zed's "follow-along" (it can open /
// highlight the file as the agent works). Empty array when the tool has no
// obvious file argument. Returns an ACP ToolCallLocation[] ({path, line?}).
json tool_locations(const ToolUse& tc) {
    json locs = json::array();
    const auto& a = tc.args;
    auto add = [&](const char* key) {
        if (a.contains(key) && a[key].is_string()) {
            const std::string p = a[key].get<std::string>();
            if (!p.empty()) {
                json loc{{"path", p}};
                if (a.contains("line") && a["line"].is_number_integer())
                    loc["line"] = a["line"].get<int>();
                locs.push_back(std::move(loc));
            }
        }
    };
    const std::string& n = tc.name.value;
    if (n == "read" || n == "edit" || n == "write"
     || n == "list_dir" || n == "git_diff" || n == "diagnostics")
        add("path");
    return locs;
}

// Build the prompt text from the ACP ContentBlock[] of a session/prompt. We
// support text + resource_link (the ACP baseline). Embedded resources and
// images are flattened to their text where present, else a short marker.
std::string prompt_text_from_blocks(const json& blocks) {
    std::string out;
    if (!blocks.is_array()) return out;
    for (const auto& b : blocks) {
        const std::string type = b.value("type", "");
        if (type == "text") {
            out += b.value("text", "");
            out += "\n";
        } else if (type == "resource_link") {
            // A pointer to a workspace file. Surface the uri so the model
            // can read it with the `read` tool.
            std::string uri = b.value("uri", "");
            std::string name = b.value("name", "");
            out += "[resource: " + (name.empty() ? uri : name) + " (" + uri + ")]\n";
        } else if (type == "resource") {
            // Embedded resource: inline its text contents if present.
            if (b.contains("resource") && b["resource"].is_object()) {
                const auto& r = b["resource"];
                if (r.contains("text") && r["text"].is_string()) {
                    out += r["text"].get<std::string>();
                    out += "\n";
                }
            }
        }
    }
    return out;
}

// agentty StopReason → ACP stopReason string.
const char* acp_stop_reason(StopReason r, bool cancelled, bool errored) {
    if (cancelled) return "cancelled";
    if (errored)   return "refusal";  // surfaced as a non-clean end
    switch (r) {
        case StopReason::EndTurn:      return "end_turn";
        case StopReason::MaxTokens:    return "max_tokens";
        case StopReason::ToolUse:      return "end_turn";  // shouldn't surface here
        case StopReason::StopSequence: return "end_turn";
        case StopReason::Unspecified:  return "end_turn";
    }
    return "end_turn";
}

} // namespace

AgentServer::AgentServer(rpc::Peer&       peer,
                         StreamFn         stream,
                         auth::AuthHeader auth,
                         std::string      model_id,
                         Profile          profile)
    : peer_(peer),
      stream_(std::move(stream)),
      auth_(std::move(auth)),
      model_id_(std::move(model_id)),
      profile_(profile) {}

int AgentServer::serve() {
    peer_.on_request([this](const std::string& m, const json& p, const json& id) {
        return handle_request(m, p, id);
    });
    peer_.on_notification([this](const std::string& m, const json& p) {
        handle_notification(m, p);
    });
    peer_.run();   // blocks until the client disconnects
    return 0;
}

Session* AgentServer::find_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(session_mtx_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

const std::vector<provider::ToolSpec>& AgentServer::wire_tools() {
    std::call_once(tools_once_, [this] {
        const auto& reg = tools::registry();
        wire_tools_.reserve(reg.size());
        for (const auto& t : reg)
            wire_tools_.push_back({t.name.value, t.description, t.input_schema,
                                   t.eager_input_streaming});
    });
    return wire_tools_;
}

void AgentServer::persist(const Session& sess) {
    // Same on-disk format the TUI writes (threads_dir()/<id>.json), so the
    // session survives a subprocess restart, is loadable via session/load,
    // and shows up in the TUI's thread picker. Cheap even for a 0-message
    // stub written right after session/new.
    persistence::save_thread(sess.thread);
}

void AgentServer::replay_history(const std::string& session_id,
                                 const Thread& thread) {
    // Reconstruct the conversation as session/update notifications, in order,
    // exactly as the client would have seen them live. User turns replay as
    // user_message_chunk; assistant text as agent_message_chunk; each tool
    // call as a completed tool_call card (announce + final state in one).
    for (const auto& m : thread.messages) {
        if (m.role == Role::User) {
            if (m.text.empty()) continue;
            send_update(session_id, json{
                {"sessionUpdate", "user_message_chunk"},
                {"messageId", m.id.value},
                {"content", {{"type", "text"}, {"text", m.text}}},
            });
        } else if (m.role == Role::Assistant) {
            const std::string body = m.text.empty() ? m.streaming_text : m.text;
            if (!body.empty()) {
                send_update(session_id, json{
                    {"sessionUpdate", "agent_message_chunk"},
                    {"messageId", m.id.value},
                    {"content", {{"type", "text"}, {"text", body}}},
                });
            }
            for (const auto& tc : m.tool_calls) {
                // Announce the call with its final input.
                send_update(session_id, json{
                    {"sessionUpdate", "tool_call"},
                    {"toolCallId", tc.id.value},
                    {"title", tool_title(tc)},
                    {"kind", acp_tool_kind(tc.name.value)},
                    {"status", "pending"},
                    {"rawInput", tc.args},
                    {"locations", tool_locations(tc)},
                });
                // Then its terminal status + output, so the card renders
                // complete on reload.
                const char* status = tc.is_failed()   ? "failed"
                                   : tc.is_rejected()  ? "failed"
                                   :                     "completed";
                const std::string out = tc.output();
                json update{
                    {"sessionUpdate", "tool_call_update"},
                    {"toolCallId", tc.id.value},
                    {"status", status},
                };
                if (!out.empty()) {
                    update["content"] = json::array({
                        json{{"type", "content"},
                             {"content", {{"type", "text"}, {"text", out}}}},
                    });
                    update["rawOutput"] = json{{"text", out}};
                }
                send_update(session_id, update);
            }
        }
    }
}

rpc::Outcome AgentServer::handle_request(const std::string& method,
                                         const json& params,
                                         const json& id) {
    try {
        if (method == "initialize")
            return rpc::Outcome::ok(on_initialize(params));
        if (method == "authenticate") {
            // We authenticate ourselves from ~/.config/agentty. If we have no
            // usable credential, tell the client auth is required.
            if (auth::is_empty(auth_))
                return rpc::Outcome::fail(rpc::code::kAuthRequired,
                    "agentty has no credentials — run `agentty login` first");
            return rpc::Outcome::ok(json::object());
        }
        if (method == "logout")
            return rpc::Outcome::ok(on_logout(params));
        if (method == "session/new")
            return rpc::Outcome::ok(on_new_session(params));
        if (method == "session/load")
            return rpc::Outcome::ok(on_load_session(params));
        if (method == "session/resume")
            return rpc::Outcome::ok(on_resume_session(params));
        if (method == "session/list")
            return rpc::Outcome::ok(on_list_sessions(params));
        if (method == "session/close")
            return rpc::Outcome::ok(on_close_session(params));
        if (method == "session/delete")
            return rpc::Outcome::ok(on_delete_session(params));
        if (method == "session/set_mode")
            return rpc::Outcome::ok(on_set_mode(params));
        if (method == "session/set_config_option")
            return rpc::Outcome::ok(on_set_config_option(params));
        if (method == "session/prompt") {
            // Long-running: kick off the turn on a worker and reply later via
            // Peer::respond(id, ...). Tell the peer not to reply synchronously.
            on_prompt(id, params);
            return rpc::deferred();
        }
        if (method == "session/cancel") {  // some clients send it as a request
            on_cancel(params);
            return rpc::Outcome::ok(json::object());
        }
        return rpc::Outcome::fail(rpc::code::kMethodNotFound,
                                  "unknown method: " + method);
    } catch (const std::exception& e) {
        return rpc::Outcome::fail(rpc::code::kInternalError, e.what());
    }
}

void AgentServer::handle_notification(const std::string& method,
                                      const json& params) {
    if (method == "session/cancel") on_cancel(params);
    // Other notifications (e.g. initialized) are ignored.
}

json AgentServer::on_initialize(const json& params) {
    // Echo the client's protocol version if we support it (we support v1).
    int client_version = params.value("protocolVersion", 1);
    int negotiated = client_version >= 1 ? 1 : client_version;

    return json{
        {"protocolVersion", negotiated},
        {"agentInfo", {
            {"name", "agentty"},
            {"version", AGENTTY_VERSION},
        }},
        {"agentCapabilities", {
            // session/load is gated by this top-level flag (v1 quirk).
            {"loadSession", true},
            {"promptCapabilities", {
                {"image", false},
                {"audio", false},
                {"embeddedContext", true},
            }},
            // We don't connect to MCP servers ourselves.
            {"mcpCapabilities", {
                {"http", false},
                {"sse", false},
            }},
            // We authenticate ourselves from ~/.config/agentty, and support
            // dropping that credential via the `logout` method.
            {"auth", {
                {"logout", json::object()},
            }},
            // The full optional session surface. `{}` for each = "supported".
            {"sessionCapabilities", {
                {"list",   json::object()},
                {"resume", json::object()},
                {"close",  json::object()},
                {"delete", json::object()},
            }},
        }},
        // We authenticate ourselves; advertise no client-driven auth methods.
        {"authMethods", json::array()},
    };
}

json AgentServer::on_new_session(const json& params) {
    std::string cwd = params.value("cwd", "");
    std::lock_guard<std::mutex> lk(session_mtx_);
    // Use a real ThreadId as the session id so the session is loadable from
    // the on-disk thread store (threads_dir()/<id>.json) after a restart and
    // shows up in the TUI's thread picker.
    ThreadId tid = persistence::new_id();
    std::string sid = tid.value;
    Session s;
    s.id  = sid;
    s.cwd = cwd;
    s.profile = profile_;   // inherit the server-wide default tier
    s.thread.id = tid;
    s.thread.title = cwd.empty() ? std::string{"ACP session"}
                                 : std::string{"ACP "} + cwd;
    const Session& stored = sessions_.emplace(sid, std::move(s)).first->second;
    persist(stored);
    index_session(stored);
    return json{
        {"sessionId", sid},
        {"modes", mode_state(stored.profile)},
    };
}

json AgentServer::on_load_session(const json& params) {
    std::string sid = params.value("sessionId", "");
    std::string cwd = params.value("cwd", "");
    if (sid.empty())
        throw std::runtime_error("session/load: missing sessionId");

    Thread thread;
    bool   from_memory = false;
    Profile profile = profile_;

    // If this subprocess already has the session live in memory, use that —
    // it's authoritative and sidesteps any not-yet-flushed async disk write.
    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        if (auto it = sessions_.find(sid); it != sessions_.end()) {
            if (!cwd.empty()) it->second.cwd = cwd;
            thread      = it->second.thread;   // copy under lock
            profile     = it->second.profile;
            from_memory = true;
        }
    }

    if (!from_memory) {
        // Cross-restart path: restore the persisted Thread from disk (same
        // store the TUI writes), then register it as a live session.
        auto path = persistence::threads_dir() / (sid + ".json");
        auto loaded = persistence::load_thread_file(path);
        if (!loaded)
            throw std::runtime_error("session/load: no such session: " + sid);
        thread = std::move(*loaded);

        std::lock_guard<std::mutex> lk(session_mtx_);
        Session s;
        s.id      = sid;
        s.cwd     = cwd;
        s.profile = profile;
        s.thread  = thread;
        sessions_.insert_or_assign(sid, std::move(s));
    }

    // Replay the full conversation so the client rebuilds the transcript,
    // THEN resolve with the current mode state.
    replay_history(sid, thread);
    return json{{"modes", mode_state(profile)}};
}

void AgentServer::on_cancel(const json& params) {
    std::string sid = params.value("sessionId", "");
    if (Session* s = find_session(sid); s && s->cancel)
        s->cancel->cancel();
}

// ── Session modes ─────────────────────────────────────────────────────────
const char* AgentServer::mode_id_for(Profile p) {
    switch (p) {
        case Profile::Ask:     return "ask";
        case Profile::Write:   return "write";
        case Profile::Minimal: return "minimal";
    }
    return "ask";
}

Profile AgentServer::profile_from_mode_id(const std::string& mode_id,
                                          Profile fallback) {
    if (mode_id == "ask")     return Profile::Ask;
    if (mode_id == "write")   return Profile::Write;
    if (mode_id == "minimal") return Profile::Minimal;
    return fallback;
}

json AgentServer::mode_state(Profile current) {
    // agentty's three permission tiers as ACP SessionMode[]. The client
    // (Zed) renders these in its mode picker and calls session/set_mode with
    // the chosen id.
    return json{
        {"currentModeId", mode_id_for(current)},
        {"availableModes", json::array({
            json{{"id", "ask"},     {"name", "Ask"},
                 {"description", "Prompt before edits, commands, and network access"}},
            json{{"id", "write"},   {"name", "Write"},
                 {"description", "Edit files and run commands without prompting; still prompt for risky ops"}},
            json{{"id", "minimal"}, {"name", "Minimal"},
                 {"description", "Prompt for everything, including file reads"}},
        })},
    };
}

json AgentServer::on_set_mode(const json& params) {
    std::string sid    = params.value("sessionId", "");
    std::string modeId = params.value("modeId", "");
    Session* s = find_session(sid);
    if (!s) throw std::runtime_error("session/set_mode: unknown sessionId: " + sid);
    s->profile = profile_from_mode_id(modeId, s->profile);
    // Echo the change back so the client's mode indicator stays in sync even
    // if it didn't initiate the switch (per the current_mode_update contract).
    send_update(sid, json{
        {"sessionUpdate", "current_mode_update"},
        {"currentModeId", mode_id_for(s->profile)},
    });
    return json::object();
}

// ── Session config options ───────────────────────────────────────────
json AgentServer::on_set_config_option(const json& params) {
    std::string sid      = params.value("sessionId", "");
    std::string configId = params.value("configId", "");
    std::string value    = params.value("value", "");
    Session* s = find_session(sid);
    if (!s) throw std::runtime_error("session/set_config_option: unknown sessionId: " + sid);
    // The only config option we expose is the model selector; the value is a
    // model id string. Empty value resets to the server default.
    if (configId == "model")
        s->model = value;
    return json{{"configOptions", json::array()}};
}

// ── Authentication ───────────────────────────────────────────────
json AgentServer::on_logout(const json& /*params*/) {
    // Drop the in-process credential and wipe the on-disk store so a
    // subsequent prompt reports authentication-required.
    auth::clear_credentials();
    auth_ = auth::ApiKeyHeader{""};   // empty arm → auth::is_empty() == true
    return json::object();
}

// ── Session lifecycle: list / resume / close / delete ───────────────────
json AgentServer::load_session_index() {
    std::lock_guard<std::mutex> lk(index_mtx_);
    std::ifstream ifs(persistence::threads_dir() / "acp_sessions.json");
    if (!ifs) return json::object();
    try {
        json j; ifs >> j;
        if (j.is_object()) return j;
    } catch (...) {}
    return json::object();
}

void AgentServer::index_session(const Session& sess) {
    std::lock_guard<std::mutex> lk(index_mtx_);
    auto path = persistence::threads_dir() / "acp_sessions.json";
    json j = json::object();
    { std::ifstream ifs(path); if (ifs) { try { ifs >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j[sess.id] = json{
        {"cwd", sess.cwd},
        {"title", sess.thread.title},
        {"updatedAt", std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
    };
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs) ofs << j.dump();
}

void AgentServer::unindex_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(index_mtx_);
    auto path = persistence::threads_dir() / "acp_sessions.json";
    json j = json::object();
    { std::ifstream ifs(path); if (ifs) { try { ifs >> j; } catch (...) { return; } } }
    if (!j.is_object() || !j.contains(id)) return;
    j.erase(id);
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs) ofs << j.dump();
}

json AgentServer::on_list_sessions(const json& params) {
    // Filter by cwd if the client asks. Pagination (cursor/nextCursor) is
    // unnecessary at our scale — we return the whole set in one page.
    std::string filter_cwd = params.value("cwd", "");
    json index = load_session_index();

    // Fold in any live, not-yet-indexed sessions (defensive: index_session
    // runs on creation, but keep the two views consistent).
    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        for (const auto& [id, s] : sessions_) {
            if (!index.contains(id))
                index[id] = json{{"cwd", s.cwd}, {"title", s.thread.title}};
        }
    }

    json sessions = json::array();
    for (auto it = index.begin(); it != index.end(); ++it) {
        const std::string& id = it.key();
        const json& meta = it.value();
        std::string cwd = meta.value("cwd", "");
        if (!filter_cwd.empty() && cwd != filter_cwd) continue;
        // SessionInfo requires a cwd; if we never recorded one, skip rather
        // than emit an invalid entry.
        if (cwd.empty()) continue;
        json info{{"sessionId", id}, {"cwd", cwd}};
        if (meta.contains("title") && meta["title"].is_string())
            info["title"] = meta["title"];
        sessions.push_back(std::move(info));
    }
    return json{{"sessions", std::move(sessions)}};
}

json AgentServer::on_resume_session(const json& params) {
    // Resume is load + a richer response (mode/config state). Reuse the load
    // path to restore + replay, then resolve with the mode state.
    std::string sid = params.value("sessionId", "");
    (void)on_load_session(params);   // restores, registers, replays history
    Profile profile = profile_;
    if (Session* s = find_session(sid)) profile = s->profile;
    return json{
        {"modes", mode_state(profile)},
        {"configOptions", json::array()},
    };
}

json AgentServer::on_close_session(const json& params) {
    // Drop the session from memory (free its Thread + cancel token) but leave
    // it on disk so it can be loaded/resumed/listed again later.
    std::string sid = params.value("sessionId", "");
    std::lock_guard<std::mutex> lk(session_mtx_);
    sessions_.erase(sid);
    return json::object();
}

json AgentServer::on_delete_session(const json& params) {
    // Permanent removal: drop from memory, delete the on-disk thread, and
    // remove the sidecar index entry.
    std::string sid = params.value("sessionId", "");
    { std::lock_guard<std::mutex> lk(session_mtx_); sessions_.erase(sid); }
    persistence::delete_thread(ThreadId{sid});
    unindex_session(sid);
    return json::object();
}

void AgentServer::send_update(const std::string& session_id, json update) {
    peer_.notify("session/update", json{
        {"sessionId", session_id},
        {"update", std::move(update)},
    });
}

void AgentServer::on_prompt(const json& id, const json& params) {
    std::string sid = params.value("sessionId", "");
    std::string text = prompt_text_from_blocks(params.value("prompt", json::array()));

    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        auto it = sessions_.find(sid);
        if (it == sessions_.end()) {
            peer_.respond(id, rpc::Outcome::fail(rpc::code::kInvalidParams,
                "unknown sessionId: " + sid));
            return;
        }
        if (auth::is_empty(auth_)) {
            peer_.respond(id, rpc::Outcome::fail(rpc::code::kAuthRequired,
                "agentty has no credentials — run `agentty login` first"));
            return;
        }
        Message um;
        um.role = Role::User;
        um.text = std::move(text);
        // Give the session a meaningful title from its first user message
        // (the "ACP <cwd>" placeholder is only useful before any prompt).
        if (it->second.thread.messages.empty() && !um.text.empty())
            it->second.thread.title =
                persistence::title_from_first_message(um.text);
        it->second.thread.messages.push_back(std::move(um));
        it->second.cancel = std::make_shared<http::CancelToken>();
    }

    // Run the whole turn off the reader thread so the peer keeps servicing
    // inbound traffic (session/cancel, our own permission responses).
    std::thread([this, id, sid]() mutable {
        run_turn(std::move(id), std::move(sid));
    }).detach();
}

void AgentServer::run_turn(json prompt_id, std::string session_id) {
    bool cancelled = false;
    bool errored = false;
    std::string error_msg;
    StopReason last_stop = StopReason::EndTurn;

    // The agent loop: stream a completion, run any tools it requested, repeat
    // until the model stops asking for tools (or we cancel / error).
    //
    // We do NOT hold session_mtx_ across the blocking stream / tool execution:
    // session/cancel must be able to trip the cancel token concurrently, and
    // only one turn ever runs per session, so the Session* stays valid for the
    // turn's lifetime (sessions are never erased mid-turn). The lock only
    // guards the sessions_ map structure, taken briefly inside find_session.
    for (int turn = 0; turn < 64; ++turn) {
        Session* s = find_session(session_id);
        if (!s) { errored = true; error_msg = "session vanished"; break; }

        StopReason stop = stream_completion(*s, cancelled, error_msg);
        last_stop = stop;
        if (!error_msg.empty()) { errored = true; break; }
        if (cancelled) break;

        if (stop != StopReason::ToolUse) break;  // model is done

        bool ran = run_tools(*s, cancelled);
        if (cancelled || !ran) break;
        // Loop: feed tool results back to the model.
    }

    if (Session* s = find_session(session_id)) {
        s->cancel.reset();
        // Persist the updated transcript so the session survives a restart
        // and can be reloaded via session/load.
        persist(*s);
        index_session(*s);   // refresh title/updatedAt in the cwd sidecar
    }

    json result{{"stopReason", acp_stop_reason(last_stop, cancelled, errored)}};
    if (errored && !error_msg.empty()) result["_meta"] = json{{"error", error_msg}};
    peer_.respond(prompt_id, rpc::Outcome::ok(std::move(result)));
}

StopReason AgentServer::stream_completion(Session& sess, bool& out_cancelled,
                                          std::string& out_error) {
    // ── Build the wire request, mirroring launch_stream ──────────────────
    provider::Request req;
    req.model         = sess.model.empty() ? model_id_ : sess.model;
    req.system_prompt = provider::anthropic::default_system_prompt();
    req.cancel        = sess.cancel;
    req.auth          = auth_;
    req.messages      = sess.thread.messages;
    req.tools         = wire_tools();

    // A fresh assistant message accumulates this completion's text + tools.
    Message assistant;
    assistant.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;   // accumulates input_json_delta for the open tool
    bool any_text_streamed = false;
    StreamUsage last_usage;      // most recent usage frame this completion
    bool have_usage = false;
    const std::string sid = sess.id;
    const std::string msg_id = assistant.id.value;

    // Translate each provider Msg event into ACP session/update.
    auto sink = [&](agentty::Msg m) {
        std::visit([&](auto&& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, msg::StreamMsg>) {
                std::visit([&](auto&& ev) {
                    using E = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<E, StreamTextDelta>) {
                        assistant.text += ev.text;
                        any_text_streamed = true;
                        send_update(sid, json{
                            {"sessionUpdate", "agent_message_chunk"},
                            {"messageId", msg_id},
                            {"content", {{"type", "text"}, {"text", ev.text}}},
                        });
                    } else if constexpr (std::is_same_v<E, StreamToolUseStart>) {
                        ToolUse tc;
                        tc.id   = ev.id;
                        tc.name = ev.name;
                        assistant.tool_calls.push_back(std::move(tc));
                        cur_tool_json.clear();
                        // Announce the pending tool call so the client shows a
                        // card immediately (args fill in on completion).
                        send_update(sid, json{
                            {"sessionUpdate", "tool_call"},
                            {"toolCallId", ev.id.value},
                            {"title", ev.name.value},
                            {"kind", acp_tool_kind(ev.name.value)},
                            {"status", "pending"},
                        });
                    } else if constexpr (std::is_same_v<E, StreamToolUseDelta>) {
                        cur_tool_json += ev.partial_json;
                    } else if constexpr (std::is_same_v<E, StreamToolUseEnd>) {
                        if (!assistant.tool_calls.empty()) {
                            auto& tc = assistant.tool_calls.back();
                            try {
                                tc.args = cur_tool_json.empty()
                                    ? json::object()
                                    : json::parse(cur_tool_json);
                            } catch (...) { tc.args = json::object(); }
                            send_update(sid, json{
                                {"sessionUpdate", "tool_call_update"},
                                {"toolCallId", tc.id.value},
                                {"title", tool_title(tc)},
                                {"rawInput", tc.args},
                                {"locations", tool_locations(tc)},
                            });
                        }
                    } else if constexpr (std::is_same_v<E, StreamFinished>) {
                        stop = ev.stop_reason;
                    } else if constexpr (std::is_same_v<E, StreamError>) {
                        out_error = ev.message;
                    } else if constexpr (std::is_same_v<E, StreamUsage>) {
                        last_usage = ev;
                        have_usage = true;
                    }
                    // StreamHeartbeat / StreamStarted: ignored.
                }, domain);
            }
        }, m);
    };

    try {
        stream_(std::move(req), sink);
    } catch (const std::exception& e) {
        out_error = std::string{"stream backend: "} + e.what();
    }

    if (sess.cancel && sess.cancel->is_cancelled()) {
        out_cancelled = true;
    }

    // Surface token accounting to the client as a usage_update so Zed's
    // context meter tracks the session. `used` = tokens currently in context
    // (this completion's input + the output it just produced); `size` = the
    // model's context window. Anthropic reports input_tokens at message_start
    // and the running output_tokens at message_delta, so last_usage holds the
    // final tally once the stream ends.
    if (have_usage) {
        const std::string model = sess.model.empty() ? model_id_ : sess.model;
        long long used = static_cast<long long>(last_usage.input_tokens) +
                         last_usage.cache_creation_input_tokens +
                         last_usage.cache_read_input_tokens +
                         last_usage.output_tokens;
        send_update(sid, json{
            {"sessionUpdate", "usage_update"},
            {"used", used < 0 ? 0 : used},
            {"size", ui::context_max_for_model(model)},
        });
    }

    sess.thread.messages.push_back(std::move(assistant));
    (void)any_text_streamed;
    return stop;
}

bool AgentServer::run_tools(Session& sess, bool& out_cancelled) {
    if (sess.thread.messages.empty()) return false;
    Message& last = sess.thread.messages.back();
    if (last.role != Role::Assistant || last.tool_calls.empty()) return false;

    // We mutate each ToolUse on `last` in place to its terminal status
    // (Done/Failed/Rejected). The transport's build_messages synthesises the
    // paired tool_result follow-up message from these same tool_calls
    // (reading tc.output() / tc.is_failed() / tc.is_rejected()), so there is
    // nothing to append separately — the wire pairing is automatic.
    //
    // In ACP mode the CLIENT (Zed) owns the approval UI. The profile decides
    // which tools we gate behind a session/request_permission round-trip:
    //   Ask (default) — prompt for Exec / WriteFs / Net; auto-run reads so an
    //                    agent loop's read/grep/glob don't spam dialogs.
    //   Minimal       — prompt for everything that touches the outside world,
    //                    reads included.
    //   Write         — same side-effect prompts as Ask (Exec/WriteFs/Net
    //                    still prompt) but never prompts for reads.
    const Profile profile = sess.profile;

    for (auto& tc : last.tool_calls) {
        if (sess.cancel && sess.cancel->is_cancelled()) {
            out_cancelled = true;
            return false;
        }

        // Permission gate. In Write profile everything auto-allows; we still
        // ask the client for Exec/WriteFs so Zed can show its own approval UI
        // when it wants to (the policy decides whether we MUST ask).
        bool needs_perm =
            tool::DynamicDispatch::needs_permission(tc.name.value, profile)
            && !sess.grants.contains(tc.name.value);
        if (needs_perm) {
            send_update(sess.id, json{
                {"sessionUpdate", "tool_call_update"},
                {"toolCallId", tc.id.value},
                {"status", "pending"},
            });
            auto outcome = ask_permission(sess.id, tc);
            if (sess.cancel && sess.cancel->is_cancelled()) {
                out_cancelled = true;
                return false;
            }
            if (outcome == PermissionOutcome::AllowAlways)
                sess.grants.insert(tc.name.value);
            const bool ok = outcome != PermissionOutcome::Deny;
            if (!ok) {
                tc.status = ToolUse::Rejected{};
                send_update(sess.id, json{
                    {"sessionUpdate", "tool_call_update"},
                    {"toolCallId", tc.id.value},
                    {"status", "failed"},
                });
                // tc stays Rejected; build_messages emits an is_error
                // tool_result so the model can adapt.
                continue;
            }
        }

        // Mark running, then execute synchronously.
        send_update(sess.id, json{
            {"sessionUpdate", "tool_call_update"},
            {"toolCallId", tc.id.value},
            {"status", "in_progress"},
        });

        auto result = tool::DynamicDispatch::execute(tc.name.value, tc.args);

        json update{
            {"sessionUpdate", "tool_call_update"},
            {"toolCallId", tc.id.value},
        };

        if (result) {
            // A diff-producing tool (edit/write) carries a FileChange; render
            // it as ACP diff content so Zed shows the change inline.
            json content = json::array();
            if (result->change) {
                const auto& ch = *result->change;
                json diff{
                    {"type", "diff"},
                    {"path", ch.path},
                    {"newText", ch.new_contents},
                };
                if (!ch.original_contents.empty())
                    diff["oldText"] = ch.original_contents;
                content.push_back(std::move(diff));
            }
            if (!result->text.empty()) {
                content.push_back(json{
                    {"type", "content"},
                    {"content", {{"type", "text"}, {"text", result->text}}},
                });
            }
            update["status"] = "completed";
            update["content"] = std::move(content);
            update["rawOutput"] = json{{"text", result->text}};

            // Mutate the stored tc so the wire payload sees the output.
            tc.status = ToolUse::Done{{}, {}, result->text};
        } else {
            std::string detail = result.error().render();
            update["status"] = "failed";
            update["content"] = json::array({
                json{{"type", "content"},
                     {"content", {{"type", "text"}, {"text", detail}}}},
            });
            tc.status = ToolUse::Failed{{}, {}, detail};
        }

        send_update(sess.id, update);
    }

    return true;
}

AgentServer::PermissionOutcome
AgentServer::ask_permission(const std::string& session_id, const ToolUse& tc) {
    json options = json::array({
        json{{"optionId", "allow_once"},  {"name", "Allow"},          {"kind", "allow_once"}},
        json{{"optionId", "allow_always"},{"name", "Always allow"},   {"kind", "allow_always"}},
        json{{"optionId", "reject_once"}, {"name", "Reject"},         {"kind", "reject_once"}},
    });
    json req{
        {"sessionId", session_id},
        {"toolCall", {
            {"toolCallId", tc.id.value},
            {"title", tool_title(tc)},
            {"kind", acp_tool_kind(tc.name.value)},
            {"rawInput", tc.args},
            {"locations", tool_locations(tc)},
        }},
        {"options", std::move(options)},
    };
    try {
        json resp = peer_.request("session/request_permission", req);
        // resp = { outcome: { outcome: "selected", optionId: "..." } | { outcome: "cancelled" } }
        if (!resp.contains("outcome")) return PermissionOutcome::Deny;
        const auto& oc = resp["outcome"];
        std::string kind = oc.value("outcome", "");
        if (kind == "cancelled") return PermissionOutcome::Deny;
        if (kind == "selected") {
            std::string opt = oc.value("optionId", "");
            if (opt == "allow_always") return PermissionOutcome::AllowAlways;
            if (opt == "allow_once")   return PermissionOutcome::AllowOnce;
            return PermissionOutcome::Deny;
        }
        return PermissionOutcome::Deny;
    } catch (const std::exception&) {
        // Client disconnected or errored — treat as denied.
        return PermissionOutcome::Deny;
    }
}

} // namespace agentty::acp
