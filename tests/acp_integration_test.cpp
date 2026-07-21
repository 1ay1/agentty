// acp_integration_test.cpp — end-to-end exercise of agentty's ACP agent.
//
// Drives a real agentty::acp::AgentServer (built on acp-cpp) through a full
// protocol lifecycle over an in-memory transport, with a SCRIPTED provider so
// no network/auth is needed. Verifies:
//
//   • initialize advertises the v1 agent surface
//   • session/new mints a session with modes
//   • session/prompt drives a turn that streams an agent_message_chunk + a
//     tool_call, requests permission (the deferred-response + outbound
//     callback path that would DEADLOCK a blocking handler), runs the tool,
//     feeds the result back, streams a final chunk, and resolves end_turn
//   • the `write` tool actually wrote the file
//   • session/set_mode + session/cancel + session/close round-trip
//
// Exit code 0 = all assertions held.

#include <acp/acp.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "agentty/acp/server.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

using namespace acp;
namespace ag = agentty;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    std::exit(1); } } while (0)

int main() {
    namespace fs = std::filesystem;
    // The write tool's sandbox refuses paths outside the workspace root.
    // Point the root at a tmp dir we own so the scripted `write` succeeds.
    const fs::path tmp = fs::temp_directory_path() / "agentty_acp_it";
    fs::create_directories(tmp);
    // Sandbox persistence too: AgentServer::persist() writes every turn
    // to persistence::threads_dir() = $HOME/.agentty/threads. Without
    // redirecting HOME, every ctest run deposited the scripted "please
    // write the file" thread (plus acp_sessions.json entries) into the
    // DEVELOPER'S real thread history — they showed up when cycling
    // threads in the real app. data_dir() re-reads the env on each call,
    // so setting it here (before any persistence touch) is sufficient.
    setenv("HOME", tmp.string().c_str(), 1);
    setenv("USERPROFILE", tmp.string().c_str(), 1);   // win32 branch of data_dir
    ag::tools::util::set_workspace_root(tmp);
    const fs::path target = tmp / "out.txt";
    fs::remove(target);

    // ── Scripted provider ──────────────────────────────────────────────────
    std::atomic<int> completions{0};
    auto stream = [&](ag::provider::Request req, ag::provider::EventSink sink) {
        int n = completions.fetch_add(1);
        if (n % 2 == 0) {
            // First completion of a turn: stream text + a `write` tool call.
            std::string tcid = "tc_write_" + std::to_string(n);
            sink(ag::Msg{ag::StreamStarted{}});
            sink(ag::Msg{ag::StreamTextDelta{"Writing the file. "}});
            sink(ag::Msg{ag::StreamToolUseStart{
                ag::ToolCallId{tcid}, ag::ToolName{"write"}}});
            std::string args = std::string("{\"path\":\"") + target.string()
                             + "\",\"content\":\"hello from acp\\n\"}";
            sink(ag::Msg{ag::StreamToolUseDelta{args}});
            sink(ag::Msg{ag::StreamToolUseEnd{}});
            sink(ag::Msg{ag::StreamUsage{1200, 40, 0, 0}});
            sink(ag::Msg{ag::StreamFinished{ag::StopReason::ToolUse}});
        } else {
            // Second completion: the tool result for the prior call must be in
            // history (whether it succeeded or was rejected).
            std::string want = "tc_write_" + std::to_string(n - 1);
            bool saw_tool_result = false;
            for (const auto& m : req.messages)
                for (const auto& tc : m.tool_calls)
                    if (tc.id.value == want) saw_tool_result = true;
            CHECK(saw_tool_result);
            sink(ag::Msg{ag::StreamTextDelta{"Done."}});
            sink(ag::Msg{ag::StreamUsage{1300, 10, 0, 0}});
            sink(ag::Msg{ag::StreamFinished{ag::StopReason::EndTurn}});
        }
    };

    // ── Wire two real OS pipes between agent and client ────────────────────
    // AgentServer now speaks FdTransport (raw fds), so the loopback uses two
    // pipe(2) pairs instead of iostream streambufs: c2a = client→agent,
    // a2c = agent→client.
    int c2a[2], a2c[2];
    CHECK(::pipe(c2a) == 0);
    CHECK(::pipe(a2c) == 0);
    FdTransport agent_tx(c2a[0], a2c[1]);   // read client→agent, write agent→client

    ag::auth::AuthHeader cred = ag::auth::ApiKeyHeader{"sk-test-not-empty"};
    CHECK(!ag::auth::is_empty(cred));

    ag::acp::AgentServer server(agent_tx, stream, cred, "claude-test", ag::Profile::Ask);
    std::thread agent_thread([&]{ server.serve(); });   // start()+join() on agent_tx

    // ── Client side (AgentConnection) ──────────────────────────────────────
    std::mutex client_write_mu;
    auto client_sink = [&](std::string_view line) {
        std::lock_guard lk(client_write_mu);
        std::string frame(line);
        frame.push_back('\n');
        std::size_t off = 0;
        while (off < frame.size()) {
            ssize_t w = ::write(c2a[1], frame.data() + off, frame.size() - off);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
            off += static_cast<std::size_t>(w);
        }
    };

    std::atomic<int> agent_text_chunks{0};
    std::atomic<int> tool_calls{0};
    std::atomic<int> tool_completed{0};
    std::atomic<int> tool_failed{0};
    std::string last_tool_text;
    std::string tc_title;
    std::string perm_title;
    std::atomic<bool> tc_is_edit_kind{false};
    std::atomic<bool> tc_announce_diff{false};
    std::atomic<bool> perm_announce_diff{false};
    std::atomic<int> usage_updates{0};
    std::atomic<int> perm_requests{0};
    std::string transcript;
    std::mutex transcript_mu;

    ClientHandlers ch;
    ch.on_session_update = [&](const SessionUpdateMsg& m) {
        match(m.update,
            [&](const SU_AgentMessageChunk& c) {
                ++agent_text_chunks;
                match(c.content,
                    [&](const TextContent& t) {
                        std::lock_guard lk(transcript_mu); transcript += t.text;
                    },
                    [&](const auto&) {});
            },
            [&](const SU_ToolCall& t) {
                ++tool_calls;
                std::lock_guard lk(transcript_mu);
                tc_title = t.toolCall.title;
                if (t.toolCall.kind == ToolKind::Edit) tc_is_edit_kind.store(true);
                for (const auto& cc : t.toolCall.content)
                    match(cc, [&](const TCC_Diff&) { tc_announce_diff.store(true); },
                              [&](const auto&) {});
            },
            [&](const SU_ToolCallUpdate& u) {
                if (u.update.status && *u.update.status == ToolCallStatus::Completed)
                    ++tool_completed;
                if (u.update.status && *u.update.status == ToolCallStatus::Failed)
                    ++tool_failed;
                if (u.update.title) {
                    std::lock_guard lk(transcript_mu); tc_title = *u.update.title;
                }
                if (u.update.kind && *u.update.kind == ToolKind::Edit)
                    tc_is_edit_kind.store(true);
                if (u.update.content)
                    for (const auto& cc : *u.update.content)
                        match(cc,
                            [&](const TCC_Diff&) { tc_announce_diff.store(true); },
                            [&](const TCC_Content& c) {
                                match(c.content,
                                    [&](const TextContent& t) {
                                        std::lock_guard lk(transcript_mu);
                                        last_tool_text = t.text;
                                    }, [&](const auto&){});
                            }, [&](const auto&){});
            },
            [&](const SU_Usage&) { ++usage_updates; },
            [&](const auto&) {});
    };
    std::atomic<bool> reject_mode{false};
    // Approve the write when asked (or reject when reject_mode is set).
    ch.on_request_permission = [&](const RequestPermissionParams& p) {
        ++perm_requests;
        {
            std::lock_guard lk(transcript_mu);
            if (p.toolCall.title) perm_title = *p.toolCall.title;
            if (p.toolCall.content)
                for (const auto& cc : *p.toolCall.content)
                    match(cc, [&](const TCC_Diff&) { perm_announce_diff.store(true); },
                              [&](const auto&) {});
        }
        auto want = reject_mode.load() ? PermissionOptionKind::RejectOnce
                                       : PermissionOptionKind::AllowOnce;
        std::string chosen;
        for (const auto& o : p.options)
            if (o.kind == want) chosen = o.optionId;
        return RequestPermissionResult{
            RequestPermissionOutcome{PO_Selected{chosen, Json::object()}}, Json::object()};
    };

    AgentConnection agent(client_sink, std::move(ch));
    // The client reads a2c[0]; pump raw bytes into the agent connection's
    // engine, splitting on '\n' like a real fd transport would.
    std::thread client_pump([&]{
        std::string partial;
        char buf[8192];
        for (;;) {
            ssize_t n = ::read(a2c[0], buf, sizeof(buf));
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;   // EOF
            const char* p = buf; const char* end = buf + n;
            while (p < end) {
                const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
                if (!nl) { partial.append(p, end - p); break; }
                partial.append(p, nl - p);
                if (!partial.empty()) agent.engine().feed_line(partial);
                partial.clear();
                p = nl + 1;
            }
        }
    });

    // ── Drive the protocol ─────────────────────────────────────────────────
    InitializeParams ip;
    ip.clientCapabilities.fs.readTextFile = true;
    ip.clientCapabilities.fs.writeTextFile = true;
    auto init = agent.initialize(ip).get();
    CHECK(init.protocolVersion == kProtocolVersion);
    CHECK(init.agentCapabilities.loadSession == true);
    CHECK(init.agentCapabilities.promptCapabilities.embeddedContext == true);
    CHECK(init.agentInfo.has_value() && init.agentInfo->name == "agentty");

    NewSessionParams nsp; nsp.cwd = tmp.string();
    auto ns = agent.session_new(nsp).get();
    CHECK(!ns.sessionId.value.empty());
    CHECK(ns.modes.has_value());
    CHECK(ns.modes->currentModeId.value == "ask");
    CHECK(ns.modes->availableModes.size() == 3);

    // set_mode round-trip (switch to minimal then back to ask). Keep the
    // session in Ask before the prompt so the `write` tool gates on the user.
    agent.session_set_mode(SetModeParams{ns.sessionId, SessionModeId{"minimal"}, Json::object()}).get();
    agent.session_set_mode(SetModeParams{ns.sessionId, SessionModeId{"ask"}, Json::object()}).get();

    PromptParams pp;
    pp.sessionId = ns.sessionId;
    pp.prompt.push_back(TextContent{"please write the file", Nothing, Json::object()});
    auto pr = agent.session_prompt(pp).get();   // resolves only after full turn

    CHECK(pr.stopReason == StopReason::EndTurn);
    CHECK(completions.load() == 2);             // two model completions ran
    CHECK(perm_requests.load() == 1);           // write asked once
    CHECK(tool_calls.load() == 1);
    if (tool_completed.load() != 1) {
        std::lock_guard lk(transcript_mu);
        std::fprintf(stderr, "tool_failed=%d last_tool_text=[%s]\n",
                     tool_failed.load(), last_tool_text.c_str());
    }
    CHECK(tool_completed.load() == 1);
    CHECK(usage_updates.load() == 2);
    CHECK(agent_text_chunks.load() >= 2);

    // The tool card is first-class: title is project-relative ("write out.txt",
    // not the absolute tmp path), kind maps write→Edit, and an announce-time
    // diff card is shown before the tool runs — on both the tool_call update
    // and the permission request. This is the claude-code-acp parity work.
    { std::lock_guard lk(transcript_mu);
      CHECK(tc_title.rfind("Write ", 0) == 0);
      CHECK(tc_title.find('/') == std::string::npos);   // relative, no dir sep
      CHECK(perm_title.rfind("Write ", 0) == 0); }
    CHECK(tc_is_edit_kind.load());
    CHECK(tc_announce_diff.load());
    CHECK(perm_announce_diff.load());

    { std::lock_guard lk(transcript_mu);
      CHECK(transcript.find("Writing the file.") != std::string::npos);
      CHECK(transcript.find("Done.") != std::string::npos); }

    // The tool actually wrote the file.
    CHECK(fs::exists(target));
    { std::ifstream f(target); std::string body((std::istreambuf_iterator<char>(f)), {});
      CHECK(body.find("hello from acp") != std::string::npos); }

    // ── Turn 2: client REJECTS the write. The tool must be marked failed,
    //    the model gets the rejection as a tool result, and the turn still
    //    resolves cleanly (end_turn). ────────────────────────────────────────
    fs::remove(target);
    reject_mode.store(true);
    int perms_before   = perm_requests.load();
    int failed_before  = tool_failed.load();

    PromptParams pp2;
    pp2.sessionId = ns.sessionId;
    pp2.prompt.push_back(TextContent{"write it again", Nothing, Json::object()});
    auto pr2 = agent.session_prompt(pp2).get();

    CHECK(pr2.stopReason == StopReason::EndTurn);
    CHECK(perm_requests.load() == perms_before + 1);   // asked again
    CHECK(tool_failed.load()   == failed_before + 1);   // rejected → failed
    CHECK(!fs::exists(target));                          // never written

    // close round-trip.
    agent.session_close(CloseSessionParams{ns.sessionId, Json::object()}).get();

    // ── Tear down ──────────────────────────────────────────────────────────
    ::close(c2a[1]);   // EOF on the agent's reader → serve() returns
    agent_thread.join();
    ::close(a2c[1]);   // EOF on the client pump
    client_pump.join();
    ::close(c2a[0]);
    ::close(a2c[0]);

    fs::remove(target);
    std::fprintf(stderr, "acp_integration: OK (turn=%d perms=%d toolcalls=%d "
                 "completed=%d usage=%d chunks=%d)\n",
                 completions.load(), perm_requests.load(), tool_calls.load(),
                 tool_completed.load(), usage_updates.load(), agent_text_chunks.load());
    std::printf("acp_integration OK\n");
    return 0;
}
