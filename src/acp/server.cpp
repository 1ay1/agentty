// agentty::acp::AgentServer — implementation. See server.hpp for the design.
//
// This is the headless analogue of the TUI turn loop in
// src/runtime/app/cmd_factory.cpp. It reuses the SAME provider, tools, wire
// shaping, and permission policy, but drives them synchronously on a worker
// thread and translates every step into ACP session/update notifications.

#include "agentty/acp/server.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentty/diff/diff.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/msg.hpp"
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
                         std::string      model_id)
    : peer_(peer),
      stream_(std::move(stream)),
      auth_(std::move(auth)),
      model_id_(std::move(model_id)) {}

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
        if (method == "session/new")
            return rpc::Outcome::ok(on_new_session(params));
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
            {"loadSession", false},
            {"promptCapabilities", {
                {"image", false},
                {"audio", false},
                {"embeddedContext", true},
            }},
        }},
        // We authenticate ourselves; advertise no client-driven auth methods.
        {"authMethods", json::array()},
    };
}

json AgentServer::on_new_session(const json& params) {
    std::string cwd = params.value("cwd", "");
    std::lock_guard<std::mutex> lk(session_mtx_);
    std::string sid = "sess-" + std::to_string(next_session_++);
    Session s;
    s.id  = sid;
    s.cwd = cwd;
    s.thread.id = ThreadId{sid};
    sessions_.emplace(sid, std::move(s));
    return json{{"sessionId", sid}};
}

void AgentServer::on_cancel(const json& params) {
    std::string sid = params.value("sessionId", "");
    if (Session* s = find_session(sid); s && s->cancel)
        s->cancel->cancel();
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

    if (Session* s = find_session(session_id)) s->cancel.reset();

    json result{{"stopReason", acp_stop_reason(last_stop, cancelled, errored)}};
    if (errored && !error_msg.empty()) result["_meta"] = json{{"error", error_msg}};
    peer_.respond(prompt_id, rpc::Outcome::ok(std::move(result)));
}

StopReason AgentServer::stream_completion(Session& sess, bool& out_cancelled,
                                          std::string& out_error) {
    // ── Build the wire request, mirroring launch_stream ──────────────────
    provider::Request req;
    req.model         = model_id_;
    req.system_prompt = provider::anthropic::default_system_prompt();
    req.cancel        = sess.cancel;
    req.auth          = auth_;
    req.messages      = sess.thread.messages;
    for (const auto& t : tools::registry())
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});

    // A fresh assistant message accumulates this completion's text + tools.
    Message assistant;
    assistant.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;   // accumulates input_json_delta for the open tool
    bool any_text_streamed = false;
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
                            });
                        }
                    } else if constexpr (std::is_same_v<E, StreamFinished>) {
                        stop = ev.stop_reason;
                    } else if constexpr (std::is_same_v<E, StreamError>) {
                        out_error = ev.message;
                    }
                    // StreamUsage / StreamHeartbeat / StreamStarted: ignored.
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
    // In ACP mode the CLIENT (Zed) owns the approval UI, so we always ask it
    // before a side-effecting tool runs. Profile::Ask is the right policy:
    // prompt for Exec / WriteFs / Net, auto-run ReadFs / Pure inspection so an
    // agent loop's read/grep/glob don't spam a permission dialog per step.
    const Profile profile = Profile::Ask;

    for (auto& tc : last.tool_calls) {
        if (sess.cancel && sess.cancel->is_cancelled()) {
            out_cancelled = true;
            return false;
        }

        // Permission gate. In Write profile everything auto-allows; we still
        // ask the client for Exec/WriteFs so Zed can show its own approval UI
        // when it wants to (the policy decides whether we MUST ask).
        bool needs_perm =
            tool::DynamicDispatch::needs_permission(tc.name.value, profile);
        if (needs_perm) {
            send_update(sess.id, json{
                {"sessionUpdate", "tool_call_update"},
                {"toolCallId", tc.id.value},
                {"status", "pending"},
            });
            bool ok = ask_permission(sess.id, tc);
            if (sess.cancel && sess.cancel->is_cancelled()) {
                out_cancelled = true;
                return false;
            }
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

bool AgentServer::ask_permission(const std::string& session_id, const ToolUse& tc) {
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
        }},
        {"options", std::move(options)},
    };
    try {
        json resp = peer_.request("session/request_permission", req);
        // resp = { outcome: { outcome: "selected", optionId: "..." } | { outcome: "cancelled" } }
        if (!resp.contains("outcome")) return false;
        const auto& oc = resp["outcome"];
        std::string kind = oc.value("outcome", "");
        if (kind == "cancelled") return false;
        if (kind == "selected") {
            std::string opt = oc.value("optionId", "");
            return opt == "allow_once" || opt == "allow_always";
        }
        return false;
    } catch (const std::exception&) {
        // Client disconnected or errored — treat as denied.
        return false;
    }
}

} // namespace agentty::acp
