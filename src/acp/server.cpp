// agentty::acp::AgentServer — implementation. See server.hpp for the design.
//
// Headless analogue of the TUI turn loop (src/runtime/app/cmd_factory.cpp).
// Reuses the SAME provider, tools, wire shaping, and permission policy, driven
// synchronously on a worker thread and translated into acp-cpp SessionUpdate
// notifications via the acp-cpp ClientConnection.

#include "agentty/acp/server.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentty/diff/diff.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/policy.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/util/dbglog.hpp"

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::acp {
namespace {

using nlohmann::json;
namespace a = ::acp;

// Construct a text ContentBlock.
a::ContentBlock text_block(std::string s) {
    return a::ContentBlock{a::TextContent{std::move(s), a::Nothing, json::object()}};
}

// agentty spec::Kind → acp::ToolKind.
a::ToolKind acp_tool_kind(std::string_view tool_name) {
    namespace sp = tools::spec;
    const auto* s = sp::lookup(tool_name);
    if (!s) return a::ToolKind::Other;
    switch (s->kind) {
        case sp::Kind::Read:           return a::ToolKind::Read;
        case sp::Kind::Edit:           return a::ToolKind::Edit;
        case sp::Kind::Write:          return a::ToolKind::Edit;
        case sp::Kind::Bash:           return a::ToolKind::Execute;
        case sp::Kind::Diagnostics:    return a::ToolKind::Execute;
        case sp::Kind::GitCommit:      return a::ToolKind::Execute;
        case sp::Kind::Grep:           return a::ToolKind::Search;
        case sp::Kind::Glob:           return a::ToolKind::Search;
        case sp::Kind::FindDefinition: return a::ToolKind::Search;
        case sp::Kind::SearchDocs:     return a::ToolKind::Search;
        case sp::Kind::RepoMap:        return a::ToolKind::Search;
        case sp::Kind::ListDir:        return a::ToolKind::Read;
        case sp::Kind::GitStatus:      return a::ToolKind::Read;
        case sp::Kind::GitDiff:        return a::ToolKind::Read;
        case sp::Kind::GitLog:         return a::ToolKind::Read;
        case sp::Kind::WebFetch:       return a::ToolKind::Fetch;
        case sp::Kind::WebSearch:      return a::ToolKind::Fetch;
        case sp::Kind::Todo:           return a::ToolKind::Think;
        case sp::Kind::Remember:       return a::ToolKind::Other;
        case sp::Kind::Forget:         return a::ToolKind::Other;
        case sp::Kind::Wipe:           return a::ToolKind::Other;
        case sp::Kind::Task:           return a::ToolKind::Think;
        case sp::Kind::Skill:          return a::ToolKind::Read;
    }
    return a::ToolKind::Other;
}

// One-line human title for a tool call card.
std::string tool_title(const ToolUse& tc) {
    const auto& args = tc.args;
    auto str = [&](const char* k) -> std::string {
        if (args.contains(k) && args[k].is_string()) return args[k].get<std::string>();
        return {};
    };
    if (tc.name.value == "read" || tc.name.value == "edit" || tc.name.value == "write") {
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

// File locations a tool call touches, for Zed's follow-along.
a::List<a::ToolCallLocation> tool_locations(const ToolUse& tc) {
    a::List<a::ToolCallLocation> locs;
    const auto& args = tc.args;
    auto add = [&](const char* key) {
        if (args.contains(key) && args[key].is_string()) {
            const std::string p = args[key].get<std::string>();
            if (!p.empty()) {
                a::ToolCallLocation loc;
                loc.path = p;
                if (args.contains("line") && args["line"].is_number_integer())
                    loc.line = a::Just<std::int64_t>(args["line"].get<std::int64_t>());
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

// Build the prompt text from acp ContentBlock[].
std::string prompt_text_from_blocks(const a::List<a::ContentBlock>& blocks) {
    std::string out;
    for (const auto& b : blocks) {
        a::match(b,
            [&](const a::TextContent& t) { out += t.text; out += "\n"; },
            [&](const a::ResourceLinkContent& l) {
                out += "[resource: " + (l.name.empty() ? l.uri : l.name)
                     + " (" + l.uri + ")]\n";
            },
            [&](const a::ResourceContent& r) {
                a::match(r.resource,
                    [&](const a::TextResource& tr) { out += tr.text; out += "\n"; },
                    [&](const a::BlobResource&)    {});
            },
            [&](const a::ImageContent&) {},
            [&](const a::AudioContent&) {});
    }
    return out;
}

// agentty StopReason → acp::StopReason.
a::StopReason acp_stop_reason(StopReason r, bool cancelled, bool errored) {
    if (cancelled) return a::StopReason::Cancelled;
    if (errored)   return a::StopReason::Refusal;
    switch (r) {
        case StopReason::MaxTokens:    return a::StopReason::MaxTokens;
        case StopReason::EndTurn:
        case StopReason::ToolUse:
        case StopReason::StopSequence:
        case StopReason::Unspecified:  return a::StopReason::EndTurn;
    }
    return a::StopReason::EndTurn;
}

// Helper: a ToolCall (announcement) from a pending ToolUse.
a::ToolCall make_tool_call(const ToolUse& tc, a::ToolCallStatus status) {
    a::ToolCall out;
    out.toolCallId = a::ToolCallId{tc.id.value};
    out.title      = tool_title(tc);
    out.kind       = acp_tool_kind(tc.name.value);
    out.status     = status;
    if (!tc.args.is_null()) out.rawInput = a::Just<json>(tc.args);
    out.locations  = tool_locations(tc);
    return out;
}

} // namespace

AgentServer::AgentServer(a::StdioTransport& transport,
                         StreamFn          stream,
                         auth::AuthHeader  auth,
                         std::string       model_id,
                         Profile           profile)
    : transport_(transport),
      conn_(transport.sink(), make_handlers()),
      stream_(std::move(stream)),
      auth_(std::move(auth)),
      model_id_(std::move(model_id)),
      profile_(profile) {}

a::AgentHandlers AgentServer::make_handlers() {
    a::AgentHandlers h;
    h.on_initialize  = [this](const a::InitializeParams& p)  { return on_initialize(p); };
    h.on_session_new = [this](const a::NewSessionParams& p)  { return on_new_session(p); };
    h.on_session_cancel = [this](const a::CancelParams& p)   { on_cancel(p); };
    h.on_session_prompt_async =
        [this](const a::PromptParams& p, Responder r) { on_prompt(p, std::move(r)); };

    h.on_session_load = [this](const a::LoadSessionParams& p) -> a::Unit {
        on_load_session(p); return a::Unit{};
    };
    h.on_session_resume = [this](const a::ResumeSessionParams& p) { return on_resume_session(p); };
    h.on_session_list   = [this](const a::ListSessionsParams& p)  { return on_list_sessions(p); };
    h.on_session_close  = [this](const a::CloseSessionParams& p) -> a::Unit {
        on_close_session(p); return a::Unit{};
    };
    h.on_session_delete = [this](const a::DeleteSessionParams& p) -> a::Unit {
        on_delete_session(p); return a::Unit{};
    };
    h.on_session_set_mode = [this](const a::SetModeParams& p) -> a::Unit {
        on_set_mode(p); return a::Unit{};
    };
    h.on_session_set_config_option =
        [this](const a::SetConfigOptionParams& p) { return on_set_config_option(p); };

    h.on_authenticate = [this](const a::AuthenticateParams&) -> a::Unit {
        if (auth::is_empty(auth_))
            throw a::RpcError(a::errc::AuthRequired,
                "agentty has no credentials — run `agentty login` first");
        return a::Unit{};
    };
    h.on_logout = [this]() -> a::Unit { on_logout(); return a::Unit{}; };
    return h;
}

int AgentServer::serve() {
    // ── observability ──────────────────────────────────────────────────────
    // AGENTTY_ACP_TRACE=1 → every JSON-RPC frame to stderr (debug Zed/glue).
    // Stderr is reserved by the ACP stdio framing for free-form logging.
    if (const char* t = std::getenv("AGENTTY_ACP_TRACE"); t && *t && std::strcmp(t, "0") != 0) {
        static std::mutex trace_mu;
        conn_.set_wire_trace([](a::WireDir dir, std::string_view line) {
            std::lock_guard lk(trace_mu);
            std::cerr << (dir == a::WireDir::Inbound ? "acp ← " : "acp → ")
                      << line << '\n';
            std::cerr.flush();
        });
    }
    // Surface transport-level faults (peer EOF, reader exception) — silent
    // failure here is the worst Zed-integration UX. errc::ConnectionLost is
    // expected at session end; everything else is worth seeing.
    conn_.set_error_callback([](int code, std::string_view msg) {
        if (code == a::errc::ConnectionLost) return;  // normal end-of-session
        std::cerr << "[acp] transport error (" << code << "): " << msg << '\n';
    });
    // Default deadline on outbound requests we issue to the client
    // (request_permission, read_text_file, write_text_file, terminal_*).
    // A dead client no longer wedges a worker thread forever — the future
    // fails with errc::Timeout and ask_permission falls through to Deny.
    // 5 min is generous for a human reading a permission dialog.
    conn_.set_default_timeout(std::chrono::minutes(5));

    transport_.start(conn_.engine());
    transport_.join();   // blocks until EOF on stdin
    return 0;
}

std::shared_ptr<Session> AgentServer::find_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(session_mtx_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

const std::vector<provider::ToolSpec>& AgentServer::wire_tools() {
    // Rebuild from tools::wire_tools() when the MCP tool set changed (a
    // server emitted tools/list_changed). tools::mcp_generation() is O(1),
    // so an unchanged set re-uses the cached vector; the rebuild only runs
    // when MCP actually re-listed. The first call always builds.
    const unsigned long g = tools::mcp_generation();
    if (wire_tools_built_ && wire_tools_gen_ == g) return wire_tools_;  // unchanged
    wire_tools_gen_   = g;
    wire_tools_built_ = true;
    wire_tools_.clear();
    const auto& reg = tools::wire_tools();
    wire_tools_.reserve(reg.size());
    for (const auto& t : reg)
        wire_tools_.push_back({t.name.value, t.description, t.input_schema,
                               t.eager_input_streaming});
    return wire_tools_;
}

void AgentServer::persist(const Session& sess) {
    // Snapshot the thread under the per-session mutex so a worker turn
    // mutating messages concurrently can't race save_thread's read. Copy
    // then write outside the lock to keep the hold short.
    Thread snapshot;
    {
        std::lock_guard<std::mutex> lk(*sess.thread_mtx);
        snapshot = sess.thread;
    }
    persistence::save_thread(snapshot);
}

void AgentServer::send_update(const std::string& session_id, a::SessionUpdate update) {
    a::SessionUpdateMsg msg;
    msg.sessionId = a::SessionId{session_id};
    msg.update    = std::move(update);
    conn_.session_update(msg);
}

void AgentServer::replay_history(const std::string& session_id, const Thread& thread) {
    // Replay is a synchronous fan-out to the wire on the reader thread (one
    // blocking write per message + per tool call), so an arbitrarily large
    // on-disk thread would block every other request for the whole replay.
    // Bound it: restore only the most recent kReplayMax messages (the rest
    // stay in thread.messages and are still sent to the model on the next
    // prompt — this only limits what's re-rendered in the client), and cap
    // the tool calls emitted per assistant message.
    constexpr std::size_t kReplayMax          = 200;
    constexpr std::size_t kReplayToolsPerMsg  = 100;
    const std::size_t total = thread.messages.size();
    const std::size_t start = total > kReplayMax ? total - kReplayMax : 0;
    for (std::size_t mi = start; mi < total; ++mi) {
        const auto& m = thread.messages[mi];
        if (m.role == Role::User) {
            if (m.text.empty()) continue;
            send_update(session_id, a::SU_UserMessageChunk{
                text_block(m.text), a::Just(a::MessageId{m.id.value})});
        } else if (m.role == Role::Assistant) {
            const std::string body = m.text.empty() ? m.streaming_text : m.text;
            if (!body.empty())
                send_update(session_id, a::SU_AgentMessageChunk{
                    text_block(body), a::Just(a::MessageId{m.id.value})});

            std::size_t emitted = 0;
            for (const auto& tc : m.tool_calls) {
                if (emitted++ >= kReplayToolsPerMsg) break;
                send_update(session_id, a::SU_ToolCall{
                    make_tool_call(tc, a::ToolCallStatus::Pending)});

                const auto status = (tc.is_failed() || tc.is_rejected())
                                  ? a::ToolCallStatus::Failed : a::ToolCallStatus::Completed;
                a::ToolCallUpdate upd;
                upd.toolCallId = a::ToolCallId{tc.id.value};
                upd.status     = a::Just(status);
                const std::string out = tc.output();
                if (!out.empty()) {
                    a::List<a::ToolCallContent> content;
                    content.push_back(a::ToolCallContent{a::TCC_Content{text_block(out), json::object()}});
                    upd.content   = a::Just(std::move(content));
                    upd.rawOutput = a::Just<json>(json{{"text", out}});
                }
                send_update(session_id, a::SU_ToolCallUpdate{std::move(upd)});
            }
        }
    }
}

a::InitializeResult AgentServer::on_initialize(const a::InitializeParams& p) {
    a::InitializeResult r;
    r.protocolVersion = (p.protocolVersion >= 1) ? a::kProtocolVersion : p.protocolVersion;

    r.agentInfo = a::Just<a::ImplementationInfo>(
        {"agentty", a::Nothing, a::Just<std::string>(AGENTTY_VERSION)});

    auto& caps = r.agentCapabilities;
    caps.loadSession = true;
    caps.promptCapabilities.embeddedContext = true;
    caps.auth.logout = a::Just(a::Unit{});
    caps.sessionCapabilities.list      = a::Just(a::Unit{});
    caps.sessionCapabilities.resume    = a::Just(a::Unit{});
    caps.sessionCapabilities.close     = a::Just(a::Unit{});
    caps.sessionCapabilities.deleteCap = a::Just(a::Unit{});
    return r;
}

a::NewSessionResult AgentServer::on_new_session(const a::NewSessionParams& p) {
    std::lock_guard<std::mutex> lk(session_mtx_);
    // Skill activations belong to the previous session's context. The
    // tracker is process-wide, so this is best-effort under concurrent
    // sessions — worst case a re-activation re-injects (token cost only).
    tools::skills::reset_activations();
    ThreadId tid = persistence::new_id();
    std::string sid = tid.value;
    Session s;
    s.id  = sid;
    s.cwd = p.cwd;
    s.profile = profile_;
    s.thread.id = tid;
    s.thread.title = p.cwd.empty() ? std::string{"ACP session"}
                                   : std::string{"ACP "} + p.cwd;
    auto ptr = std::make_shared<Session>(std::move(s));
    sessions_.emplace(sid, ptr);
    const Session& stored = *ptr;
    persist(stored);
    index_session(stored);

    a::NewSessionResult r;
    r.sessionId = a::SessionId{sid};
    r.modes     = a::Just(mode_state(stored.profile));
    return r;
}

void AgentServer::on_load_session(const a::LoadSessionParams& p) {
    std::string sid = p.sessionId.value;
    std::string cwd = p.cwd;
    if (sid.empty()) throw std::runtime_error("session/load: missing sessionId");
    // Loaded session = different context; allow skills to re-activate.
    tools::skills::reset_activations();

    Thread thread;
    bool   from_memory = false;
    Profile profile = profile_;

    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        if (auto it = sessions_.find(sid); it != sessions_.end()) {
            if (!cwd.empty()) it->second->cwd = cwd;
            // Snapshot the thread under the per-session thread mutex so a
            // worker turn appending an assistant message concurrently can't
            // race this copy (a read racing a push_back on the same vector).
            // Lock order: session_mtx_ then thread_mtx, as in on_prompt.
            std::lock_guard<std::mutex> tlk(*it->second->thread_mtx);
            thread      = it->second->thread;
            profile     = it->second->profile;
            from_memory = true;
        }
    }

    if (!from_memory) {
        auto path = persistence::threads_dir() / (sid + ".json");
        auto loaded = persistence::load_thread_file(path);
        if (!loaded) throw std::runtime_error("session/load: no such session: " + sid);
        thread = std::move(*loaded);

        std::lock_guard<std::mutex> lk(session_mtx_);
        Session s;
        s.id = sid; s.cwd = cwd; s.profile = profile; s.thread = thread;
        sessions_.insert_or_assign(sid, std::make_shared<Session>(std::move(s)));
    }

    replay_history(sid, thread);
}

void AgentServer::on_cancel(const a::CancelParams& p) {
    // Snapshot the cancel handle under the same lock find_session uses, then
    // act on the local copy — run_turn resets s->cancel concurrently, and a
    // bare read/reset of the same shared_ptr instance across threads is a
    // data race.
    std::shared_ptr<http::CancelToken> tok;
    if (auto s = find_session(p.sessionId.value); s) {
        std::lock_guard<std::mutex> lk(session_mtx_);
        tok = s->cancel;
    }
    if (tok) tok->cancel();
}

// ── Session modes ───────────────────────────────────────────────────────────
const char* AgentServer::mode_id_for(Profile p) {
    switch (p) {
        case Profile::Ask:     return "ask";
        case Profile::Write:   return "write";
        case Profile::Minimal: return "minimal";
    }
    return "ask";
}

Profile AgentServer::profile_from_mode_id(const std::string& mode_id, Profile fallback) {
    if (mode_id == "ask")     return Profile::Ask;
    if (mode_id == "write")   return Profile::Write;
    if (mode_id == "minimal") return Profile::Minimal;
    return fallback;
}

a::SessionModeState AgentServer::mode_state(Profile current) {
    a::SessionModeState st;
    st.currentModeId = a::SessionModeId{mode_id_for(current)};
    st.availableModes = {
        a::SessionMode{a::SessionModeId{"ask"}, "Ask",
            a::Just<std::string>("Prompt before edits, commands, and network access")},
        a::SessionMode{a::SessionModeId{"write"}, "Write",
            a::Just<std::string>("Edit files and run commands without prompting; still prompt for risky ops")},
        a::SessionMode{a::SessionModeId{"minimal"}, "Minimal",
            a::Just<std::string>("Prompt for everything, including file reads")},
    };
    return st;
}

void AgentServer::on_set_mode(const a::SetModeParams& p) {
    auto s = find_session(p.sessionId.value);
    if (!s) throw std::runtime_error("session/set_mode: unknown sessionId: " + p.sessionId.value);
    s->profile = profile_from_mode_id(p.modeId.value, s->profile);
    send_update(p.sessionId.value,
        a::SU_CurrentMode{a::SessionModeId{mode_id_for(s->profile)}, json::object()});
}

a::SetConfigOptionResult AgentServer::on_set_config_option(const a::SetConfigOptionParams& p) {
    auto s = find_session(p.sessionId.value);
    if (!s) throw std::runtime_error("session/set_config_option: unknown sessionId: " + p.sessionId.value);
    if (p.configId == "model") {
        s->model = p.value;
    } else {
        // Surface an unknown config id rather than silently accepting and
        // dropping it — a client setting e.g. "temperatuer" deserves an error,
        // not a no-op that looks like success.
        throw std::runtime_error(
            "session/set_config_option: unknown configId '" + p.configId
            + "' (supported: model)");
    }
    return a::SetConfigOptionResult{{}, json::object()};
}

void AgentServer::on_logout() {
    auth::clear_credentials();
    auth_ = auth::ApiKeyHeader{""};
}

// ── Session lifecycle: list / resume / close / delete ────────────────────────
json AgentServer::load_session_index() {
    std::lock_guard<std::mutex> lk(index_mtx_);
    std::ifstream ifs(persistence::threads_dir() / "acp_sessions.json");
    if (!ifs) return json::object();
    try { json j; ifs >> j; if (j.is_object()) return j; }
    catch (const std::exception& e) { util::dbglog("acp.load_session_index", e.what()); }
    catch (...) { util::dbglog("acp.load_session_index", "non-std exception"); }
    return json::object();
}

void AgentServer::index_session(const Session& sess) {
    std::lock_guard<std::mutex> lk(index_mtx_);
    auto path = persistence::threads_dir() / "acp_sessions.json";
    json j = json::object();
    { std::ifstream ifs(path); if (ifs) { try { ifs >> j; } catch (const std::exception& e) { util::dbglog("acp.index_session", e.what()); j = json::object(); } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j[sess.id] = json{
        {"cwd", sess.cwd},
        {"title", sess.thread.title},
        {"updatedAt", std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
    };
    // A session title/cwd derived from an untrusted ACP `cwd` can contain
    // invalid UTF-8; the default dump() throws type_error(316) on that.
    // Substitute U+FFFD instead so indexing can never crash the worker.
    (void)persistence::write_json_atomic(
        path, j.dump(-1, ' ', false, json::error_handler_t::replace));
}

void AgentServer::unindex_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(index_mtx_);
    auto path = persistence::threads_dir() / "acp_sessions.json";
    json j = json::object();
    { std::ifstream ifs(path); if (ifs) { try { ifs >> j; } catch (const std::exception& e) { util::dbglog("acp.unindex_session", e.what()); return; } catch (...) { return; } } }
    if (!j.is_object() || !j.contains(id)) return;
    j.erase(id);
    (void)persistence::write_json_atomic(
        path, j.dump(-1, ' ', false, json::error_handler_t::replace));
}

a::ListSessionsResult AgentServer::on_list_sessions(const a::ListSessionsParams& p) {
    std::string filter_cwd = p.cwd.value_or("");
    json index = load_session_index();

    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        for (const auto& [id, s] : sessions_)
            if (!index.contains(id))
                index[id] = json{{"cwd", s->cwd}, {"title", s->thread.title}};
    }

    a::ListSessionsResult result;
    for (auto it = index.begin(); it != index.end(); ++it) {
        const std::string& id = it.key();
        const json& meta = it.value();
        std::string cwd = meta.value("cwd", "");
        if (!filter_cwd.empty() && cwd != filter_cwd) continue;
        if (cwd.empty()) continue;
        a::SessionInfo info;
        info.sessionId = a::SessionId{id};
        info.cwd       = cwd;
        if (meta.contains("title") && meta["title"].is_string())
            info.title = a::Just<std::string>(meta["title"].get<std::string>());
        result.sessions.push_back(std::move(info));
    }
    return result;
}

a::ResumeSessionResult AgentServer::on_resume_session(const a::ResumeSessionParams& p) {
    on_load_session(p);   // restore + replay
    Profile profile = profile_;
    if (auto s = find_session(p.sessionId.value)) profile = s->profile;

    a::ResumeSessionResult r;
    r.modes = a::Just(mode_state(profile));
    return r;
}

void AgentServer::on_close_session(const a::CloseSessionParams& p) {
    std::lock_guard<std::mutex> lk(session_mtx_);
    sessions_.erase(p.sessionId.value);
}

void AgentServer::on_delete_session(const a::DeleteSessionParams& p) {
    { std::lock_guard<std::mutex> lk(session_mtx_); sessions_.erase(p.sessionId.value); }
    persistence::delete_thread(ThreadId{p.sessionId.value});
    unindex_session(p.sessionId.value);
}

// ── Prompt + turn loop ───────────────────────────────────────────────────────
void AgentServer::on_prompt(const a::PromptParams& p, Responder resp) {
    std::string sid  = p.sessionId.value;
    std::string text = prompt_text_from_blocks(p.prompt);

    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        auto it = sessions_.find(sid);
        if (it == sessions_.end()) {
            resp.error(a::errc::InvalidParams, "unknown sessionId: " + sid);
            return;
        }
        // Local OpenAI-compatible backends (Ollama, llama.cpp) need no key,
        // so an empty auth is legitimate for them. Only demand credentials
        // for a hosted (TLS) endpoint or the Anthropic path.
        const auto& sel = provider::active();
        const bool keyless_local =
            sel.kind == provider::Kind::OpenAI && !sel.openai_endpoint.use_tls;
        if (auth::is_empty(auth_) && !keyless_local) {
            resp.error(a::errc::AuthRequired,
                "agentty has no credentials — run `agentty login` first");
            return;
        }
        Message um;
        um.role = Role::User;
        um.text = std::move(text);
        if (it->second->thread.messages.empty() && !um.text.empty())
            it->second->thread.title = persistence::title_from_first_message(um.text);
        {
            // Structural mutation — also take the per-session thread mutex so
            // the worker's snapshot/read sites (which do NOT hold
            // session_mtx_) are excluded. Lock order: session_mtx_ then
            // thread_mtx, consistently everywhere.
            std::lock_guard<std::mutex> tlk(*it->second->thread_mtx);
            it->second->thread.messages.push_back(std::move(um));
        }
        it->second->cancel = std::make_shared<http::CancelToken>();
    }

    // Run the whole turn off the reader thread; the engine stays free to
    // deliver our outbound permission responses. The Responder resolves the
    // deferred session/prompt when the turn settles.
    std::thread([this, sid = std::move(sid), r = std::move(resp)]() mutable {
        run_turn(std::move(sid), std::move(r));
    }).detach();
}

void AgentServer::run_turn(std::string session_id, Responder resp) {
  // This runs on a DETACHED worker thread, entirely outside acp-cpp's
  // handle_request try/catch. An uncaught throw here would call
  // std::terminate() and kill every session in the process; and a return
  // without resolving `resp` leaves the client's session/prompt future
  // hanging forever (the Responder destructor does not auto-reply). So the
  // whole body is guarded: any escape resolves the deferred prompt with an
  // InternalError. The ok/error done-latch makes the trailing resp.ok(...)
  // a no-op if we already errored, so exactly one reply is sent.
  try {
    bool cancelled = false;
    bool errored = false;
    std::string error_msg;
    StopReason last_stop = StopReason::EndTurn;

    // Doom-loop guard for weak local models: count salvaged (leaked-JSON)
    // tool rounds across the turn. After kMaxSalvaged of them — or any
    // salvaged memory-tool call — force the turn to finish in plain text
    // instead of looping (the TUI's dedup_releaked_salvage_calls analog).
    constexpr int kMaxSalvaged = 3;
    int salvaged_rounds = 0;
    bool force_text_retry = false;   // next sub-turn: re-prompt with NO tools
    bool budget_done = false;        // the one final tool-free retry has fired
    auto is_salvaged = [](const std::string& id) {
        return std::string_view{id}.starts_with("call_salvaged_");
    };
    static constexpr std::string_view kMemoryTools[] = {
        "remember", "forget", "wipe_memory"};

    for (int turn = 0; turn < 64; ++turn) {
        // shared_ptr keeps the Session alive for the whole iteration even if
        // a concurrent session/close|delete erases the map entry mid-turn.
        auto s = find_session(session_id);
        if (!s) { errored = true; error_msg = "session vanished"; break; }

        StopReason stop = stream_completion(*s, cancelled, error_msg,
                                            force_text_retry);
        last_stop = stop;
        force_text_retry = false;
        if (!error_msg.empty()) { errored = true; break; }
        if (cancelled) break;
        if (stop != StopReason::ToolUse) break;

        // Inspect the just-streamed assistant message's tool calls. A leaked
        // call from a weak local model carries the call_salvaged_ id. Memory
        // tools (remember/forget/wipe) leaked on a non-explicit turn are junk,
        // and once the salvage budget is spent it's a doom loop. In both cases
        // we FAIL the leaked calls without running them, feed that failure
        // back as a tool result (so the wire tool_use↔result pairing stays
        // valid), and let the model take ONE more sub-turn to answer in plain
        // text — instead of returning a blank bubble.
        if (auto cs = find_session(session_id);
            cs && !cs->thread.messages.empty()) {
            // The worker mutates tc.status below; take the per-session thread
            // mutex for the whole inspect+fail block so a concurrent reader
            // snapshot (session/load, persist) sees a consistent tool-call
            // state. send_update does I/O but not on thread.messages, so
            // holding the lock across it is safe (no lock-order cycle: the
            // reader takes session_mtx_ then thread_mtx; we hold only
            // thread_mtx here).
            std::lock_guard<std::mutex> tlk(*cs->thread_mtx);
            Message& back = cs->thread.messages.back();
            if (back.role == Role::Assistant) {
                bool any_salvaged = false, mem_leak = false;
                for (auto& tc : back.tool_calls) {
                    if (!is_salvaged(tc.id.value)) continue;
                    any_salvaged = true;
                    for (auto t : kMemoryTools)
                        if (t == tc.name.value) mem_leak = true;
                }
                if (any_salvaged) ++salvaged_rounds;
                const bool budget_blown = salvaged_rounds > kMaxSalvaged;
                if (mem_leak || budget_blown) {
                    bool failed_any = false;
                    for (auto& tc : back.tool_calls) {
                        if (!is_salvaged(tc.id.value) || tc.is_terminal())
                            continue;
                        tc.status = ToolUse::Failed{
                            {}, {}, "this call was not run — do NOT call tools, "
                            "answer the user in plain text now"};
                        a::ToolCallUpdate u;
                        u.toolCallId = a::ToolCallId{tc.id.value};
                        u.status     = a::Just(a::ToolCallStatus::Failed);
                        send_update(session_id, a::SU_ToolCallUpdate{std::move(u)});
                        failed_any = true;
                    }
                    // Budget fully blown: one FINAL tool-free retry forces a
                    // plain-text answer (summarising any results already
                    // gathered) instead of returning an empty bubble. The
                    // budget_done latch makes it fire exactly once.
                    if (budget_blown) {
                        if (budget_done) { last_stop = StopReason::EndTurn; break; }
                        budget_done = true;
                        force_text_retry = true;
                        continue;
                    }
                    if (failed_any) { force_text_retry = true; continue; }
                }
            }
        }

        bool ran = run_tools(*s, cancelled);
        if (cancelled || !ran) break;
    }

    if (auto s = find_session(session_id)) {
        {
            // Reset the cancel handle under session_mtx_ so it doesn't race
            // an on_cancel reading the same shared_ptr instance.
            std::lock_guard<std::mutex> lk(session_mtx_);
            s->cancel.reset();
        }
        persist(*s);
        index_session(*s);
    }

    a::PromptResult result;
    result.stopReason = acp_stop_reason(last_stop, cancelled, errored);
    if (errored && !error_msg.empty()) result.meta = json{{"error", error_msg}};
    resp.ok(result);
  } catch (const std::exception& e) {
    util::dbglog("acp.run_turn", e.what());
    resp.error(a::errc::InternalError,
               std::string{"turn failed: "} + e.what());
  } catch (...) {
    util::dbglog("acp.run_turn", "non-std exception");
    resp.error(a::errc::InternalError, "turn failed (unknown error)");
  }
}

StopReason AgentServer::stream_completion(Session& sess, bool& out_cancelled,
                                          std::string& out_error,
                                          bool suppress_tools) {
    provider::Request req;
    req.model         = sess.model.empty() ? model_id_ : sess.model;
    // Per-model output-token ceiling (mirrors cmd_factory::launch_stream).
    // Without this the default 16384 is shared across reasoning + tool JSON
    // and a large `edit` truncates mid-input_json. context_window is left 0
    // here (no per-session /api/show probe on the ACP path); the Ollama
    // transport then applies its safe agent-sized num_ctx default, still 4x
    // Ollama's truncating floor.
    req.max_tokens    = max_output_tokens_for(req.model);
    // System prompt is chosen per provider (mirrors cmd_factory): Anthropic
    // gets the full Claude agentic prompt, Ollama (native /api/chat) its own
    // local-tuned prompt, other OpenAI-compatible backends the openai local
    // prompt (verbose Claude prose breaks small models).
    {
        const auto& sel = provider::active();
        if (sel.kind == provider::Kind::OpenAI && sel.openai_endpoint.native_api)
            req.system_prompt = provider::ollama::system_prompt();
        else if (sel.kind == provider::Kind::OpenAI)
            req.system_prompt = provider::openai::local_model_system_prompt();
        else
            req.system_prompt = provider::anthropic::default_system_prompt();
        // First-class weak-model support (agent-zero style): weak models on
        // the Ollama native endpoint use the JSON-protocol path (inline tool
        // catalog, single {tool_name,tool_args} object) instead of the native
        // tools array. Mirrors cmd_factory's launch_stream.
        req.json_protocol = is_weak_model(req.model)
            && sel.kind == provider::Kind::OpenAI
            && sel.openai_endpoint.native_api;
    }
    req.cancel        = sess.cancel;
    req.auth          = auth_;
    req.messages      = sess.thread.messages;
    // suppress_tools: a weak local model that just leaked a junk tool call
    // gets a tool-free retry. With no tools advertised it cannot leak another
    // and answers the user in plain text (proven: qwen2.5-coder:7b greets
    // cleanly only when no tools are on the wire).
    if (!suppress_tools) {
        // Weak local models hallucinate `skill` and the memory tools on
        // greetings/small talk — don't advertise them (mirrors cmd_factory).
        if (is_weak_model(req.model)) {
            auto weak_hidden = [](std::string_view n) {
                return n == "skill" || n == "remember"
                    || n == "forget" || n == "wipe_memory";
            };
            for (const auto& t : wire_tools())
                if (!weak_hidden(t.name)) req.tools.push_back(t);
        } else {
            req.tools = wire_tools();
        }
    }

    Message assistant;
    assistant.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;
    StreamUsage last_usage;
    bool have_usage = false;
    const std::string sid = sess.id;
    const std::string msg_id = assistant.id.value;

    auto sink = [&](agentty::Msg m) {
        std::visit([&](auto&& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, msg::StreamMsg>) {
                std::visit([&](auto&& ev) {
                    using E = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<E, StreamTextDelta>) {
                        assistant.text += ev.text;
                        send_update(sid, a::SU_AgentMessageChunk{
                            text_block(ev.text), a::Just(a::MessageId{msg_id})});
                    } else if constexpr (std::is_same_v<E, StreamToolUseStart>) {
                        ToolUse tc;
                        tc.id = ev.id; tc.name = ev.name;
                        assistant.tool_calls.push_back(std::move(tc));
                        cur_tool_json.clear();
                        a::ToolCall call;
                        call.toolCallId = a::ToolCallId{ev.id.value};
                        call.title      = ev.name.value;
                        call.kind       = acp_tool_kind(ev.name.value);
                        call.status     = a::ToolCallStatus::Pending;
                        send_update(sid, a::SU_ToolCall{std::move(call)});
                    } else if constexpr (std::is_same_v<E, StreamToolUseDelta>) {
                        cur_tool_json += ev.partial_json;
                    } else if constexpr (std::is_same_v<E, StreamToolUseEnd>) {
                        if (!assistant.tool_calls.empty()) {
                            auto& tc = assistant.tool_calls.back();
                            try {
                                tc.args = cur_tool_json.empty()
                                    ? json::object() : json::parse(cur_tool_json);
                            } catch (const std::exception& e) {
                                util::dbglog("acp.tool_args.parse", e.what());
                                tc.args = json::object();
                            } catch (...) {
                                util::dbglog("acp.tool_args.parse", "non-std exception");
                                tc.args = json::object();
                            }
                            a::ToolCallUpdate upd;
                            upd.toolCallId = a::ToolCallId{tc.id.value};
                            upd.title      = a::Just(tool_title(tc));
                            upd.rawInput   = a::Just<json>(tc.args);
                            upd.locations  = a::Just(tool_locations(tc));
                            send_update(sid, a::SU_ToolCallUpdate{std::move(upd)});
                        }
                    } else if constexpr (std::is_same_v<E, StreamFinished>) {
                        stop = ev.stop_reason;
                    } else if constexpr (std::is_same_v<E, StreamError>) {
                        out_error = ev.message;
                    } else if constexpr (std::is_same_v<E, StreamUsage>) {
                        last_usage = ev; have_usage = true;
                    }
                }, domain);
            }
        }, m);
    };

    try {
        stream_(std::move(req), sink);
    } catch (const std::exception& e) {
        out_error = std::string{"stream backend: "} + e.what();
    }

    if (sess.cancel && sess.cancel->is_cancelled()) out_cancelled = true;

    if (have_usage) {
        const std::string model = sess.model.empty() ? model_id_ : sess.model;
        long long used = static_cast<long long>(last_usage.input_tokens) +
                         last_usage.cache_creation_input_tokens +
                         last_usage.cache_read_input_tokens +
                         last_usage.output_tokens;
        a::SU_Usage u;
        u.used = used < 0 ? 0 : used;
        u.size = static_cast<std::int64_t>(ui::context_max_for_model(model));
        send_update(sid, std::move(u));
    }

    // Structural mutation of the shared messages vector — lock the
    // per-session thread mutex so a concurrent reader-thread snapshot
    // (session/load|resume replay, persist) can't observe a half-grown
    // vector or race the reallocation. Held only for the push, not the
    // stream above.
    {
        std::lock_guard<std::mutex> lk(*sess.thread_mtx);
        sess.thread.messages.push_back(std::move(assistant));
    }
    return stop;
}

bool AgentServer::run_tools(Session& sess, bool& out_cancelled) {
    if (sess.thread.messages.empty()) return false;
    Message& last = sess.thread.messages.back();
    if (last.role != Role::Assistant || last.tool_calls.empty()) return false;

    const Profile profile = sess.profile;

    // Tool-scheduling note: unlike the TUI path (cmd_factory), which uses
    // effects::is_parallel_safe(active, want) to fan out compose-safe tools
    // (Pure/ReadFs/Net) and serialise exclusive ones (WriteFs/Exec), the ACP
    // path runs every tool call STRICTLY SERIALLY in emission order. Serial
    // execution is a subset of the parallel-safety rule — it never starts a
    // second tool while another is in flight — so it can never violate the
    // exclusivity the effect set demands; it just forgoes the latency win of
    // overlapping independent reads. Kept serial deliberately: the ACP turn
    // loop is already off the reader thread, tool results must be appended to
    // `last.tool_calls` in a stable order for the wire tool_use↔result
    // pairing, and Zed renders the cards sequentially anyway.
    // The worker mutates each tc.status in place as tools resolve. Those
    // writes must be exclusive with a concurrent reader snapshot
    // (session/load, persist), which copies the whole messages vector under
    // thread_mtx. We CAN'T hold thread_mtx across a tool's execution (minutes
    // for a slow bash/web_fetch would stall load/persist), so each status
    // assignment is guarded individually via this helper. The reference
    // `last` stays valid across the loop because the worker never push_backs
    // during run_tools (no reallocation), and the reader only reads.
    auto set_status = [&](ToolUse& tc, ToolUse::Status st) {
        std::lock_guard<std::mutex> tlk(*sess.thread_mtx);
        tc.status = std::move(st);
    };

    for (auto& tc : last.tool_calls) {
        if (sess.cancel && sess.cancel->is_cancelled()) { out_cancelled = true; return false; }

        // Resolve the tool ONCE so the permission gate and the execution run
        // against the same definition (an mcp/tools/list_changed mid-turn can
        // otherwise re-resolve the name to a ToolDef with different effects).
        const tools::ToolDef* td =
            tool::DynamicDispatch::resolve(tc.name.value);

        bool needs_perm =
            tool::DynamicDispatch::needs_permission_with(td, profile)
            && !sess.grants.contains(tc.name.value);
        if (needs_perm) {
            a::ToolCallUpdate u;
            u.toolCallId = a::ToolCallId{tc.id.value};
            u.status     = a::Just(a::ToolCallStatus::Pending);
            send_update(sess.id, a::SU_ToolCallUpdate{std::move(u)});

            auto outcome = ask_permission(sess.id, tc);
            if (sess.cancel && sess.cancel->is_cancelled()) { out_cancelled = true; return false; }
            if (outcome == PermissionOutcome::AllowAlways) sess.grants.insert(tc.name.value);
            if (outcome == PermissionOutcome::Deny) {
                set_status(tc, ToolUse::Rejected{});
                a::ToolCallUpdate r;
                r.toolCallId = a::ToolCallId{tc.id.value};
                r.status     = a::Just(a::ToolCallStatus::Failed);
                send_update(sess.id, a::SU_ToolCallUpdate{std::move(r)});
                continue;
            }
        }

        a::ToolCallUpdate running;
        running.toolCallId = a::ToolCallId{tc.id.value};
        running.status     = a::Just(a::ToolCallStatus::InProgress);
        send_update(sess.id, a::SU_ToolCallUpdate{std::move(running)});

        // Run the tool on a worker so a session/cancel that lands mid-tool
        // (on the engine's reader thread) makes THIS turn loop return
        // promptly instead of blocking until a long bash/web_fetch finishes.
        // The tool itself isn't interruptible (its interface is synchronous),
        // so on cancel we detach the future and drop its result — the same
        // "discard the late output" contract the TUI path uses via its
        // idempotent tool-output guard. `shared_future` so the lambda below
        // can keep the running task alive after we stop waiting on it.
        //
        // std::async(launch::async) throws std::system_error when the OS
        // can't start a thread (fd/thread exhaustion). Fail only THIS tool
        // in that case rather than aborting the whole turn.
        std::shared_future<tool::ExecResult> fut;
        try {
            fut = std::async(std::launch::async,
                [td, name = tc.name.value, args = tc.args]() {
                    return tool::DynamicDispatch::execute_with(td, name, args);
                }).share();
        } catch (const std::exception& e) {
            util::dbglog("acp.run_tools.async", e.what());
            set_status(tc, ToolUse::Failed{{}, {}, std::string{"could not start tool: "} + e.what()});
            a::ToolCallUpdate f;
            f.toolCallId = a::ToolCallId{tc.id.value};
            f.status     = a::Just(a::ToolCallStatus::Failed);
            send_update(sess.id, a::SU_ToolCallUpdate{std::move(f)});
            continue;
        }

        bool tool_cancelled = false;
        bool tool_timed_out = false;
        // Belt-and-suspenders wall-clock ceiling: a tool that hangs forever
        // (e.g. a stalled network read with no internal timeout) plus a
        // client that never sends session/cancel would otherwise pin this
        // worker — and the session/prompt future — permanently. On the
        // deadline we fail the tool and move on; the future is detached below
        // just like the cancel path so the runaway task cleans itself up.
        constexpr auto kToolDeadline = std::chrono::minutes(10);
        const auto tool_start = std::chrono::steady_clock::now();
        for (;;) {
            if (fut.wait_for(std::chrono::milliseconds(50))
                    == std::future_status::ready)
                break;
            if (sess.cancel && sess.cancel->is_cancelled()) {
                tool_cancelled = true;
                break;
            }
            if (std::chrono::steady_clock::now() - tool_start > kToolDeadline) {
                tool_timed_out = true;
                break;
            }
        }

        if (tool_cancelled || tool_timed_out) {
            // Detach: keep the shared_future alive on a throwaway thread so
            // the worker can finish and clean up without us blocking on it.
            std::thread([fut]() mutable { fut.wait(); }).detach();
            const char* why = tool_timed_out ? "timed out" : "cancelled";
            if (tool_timed_out) util::dbglog("acp.run_tools.timeout", tc.name.value);
            set_status(tc, ToolUse::Failed{{}, {}, why});
            a::ToolCallUpdate c;
            c.toolCallId = a::ToolCallId{tc.id.value};
            c.status     = a::Just(a::ToolCallStatus::Failed);
            send_update(sess.id, a::SU_ToolCallUpdate{std::move(c)});
            if (tool_cancelled) { out_cancelled = true; return false; }
            // A timeout fails just this tool; continue the turn so the model
            // can react to the failure result.
            continue;
        }

        auto result = fut.get();

        a::ToolCallUpdate upd;
        upd.toolCallId = a::ToolCallId{tc.id.value};

        if (result) {
            a::List<a::ToolCallContent> content;
            if (result->change) {
                const auto& ch = *result->change;
                a::TCC_Diff diff;
                diff.path    = ch.path;
                diff.newText = ch.new_contents;
                if (!ch.original_contents.empty())
                    diff.oldText = a::Just<std::string>(ch.original_contents);
                content.push_back(a::ToolCallContent{std::move(diff)});
            }
            if (!result->text.empty())
                content.push_back(a::ToolCallContent{
                    a::TCC_Content{text_block(result->text), json::object()}});

            upd.status    = a::Just(a::ToolCallStatus::Completed);
            upd.content   = a::Just(std::move(content));
            upd.rawOutput = a::Just<json>(json{{"text", result->text}});
            set_status(tc, ToolUse::Done{{}, {}, result->text});
        } else {
            std::string detail = result.error().render();
            a::List<a::ToolCallContent> content;
            content.push_back(a::ToolCallContent{
                a::TCC_Content{text_block(detail), json::object()}});
            upd.status  = a::Just(a::ToolCallStatus::Failed);
            upd.content = a::Just(std::move(content));
            set_status(tc, ToolUse::Failed{{}, {}, detail});
        }

        send_update(sess.id, a::SU_ToolCallUpdate{std::move(upd)});
    }

    return true;
}

AgentServer::PermissionOutcome
AgentServer::ask_permission(const std::string& session_id, const ToolUse& tc) {
    a::RequestPermissionParams req;
    req.sessionId = a::SessionId{session_id};
    // ToolCallUpdate carries the tool card for the permission dialog.
    req.toolCall.toolCallId = a::ToolCallId{tc.id.value};
    req.toolCall.title      = a::Just(tool_title(tc));
    req.toolCall.kind       = a::Just(acp_tool_kind(tc.name.value));
    if (!tc.args.is_null()) req.toolCall.rawInput = a::Just<json>(tc.args);
    req.toolCall.locations  = a::Just(tool_locations(tc));
    req.options = {
        a::PermissionOption{"allow_once",   "Allow",        a::PermissionOptionKind::AllowOnce,   json::object()},
        a::PermissionOption{"allow_always", "Always allow", a::PermissionOptionKind::AllowAlways, json::object()},
        a::PermissionOption{"reject_once",  "Reject",       a::PermissionOptionKind::RejectOnce,  json::object()},
    };

    try {
        auto outcome = conn_.request_permission(req).get();   // blocks worker, not reader
        return a::match(outcome.outcome,
            [](const a::PO_Cancelled&) { return PermissionOutcome::Deny; },
            [](const a::PO_Selected& s) {
                if (s.optionId == "allow_always") return PermissionOutcome::AllowAlways;
                if (s.optionId == "allow_once")   return PermissionOutcome::AllowOnce;
                return PermissionOutcome::Deny;
            });
    } catch (const std::exception&) {
        return PermissionOutcome::Deny;   // client disconnected/errored
    }
}

} // namespace agentty::acp
