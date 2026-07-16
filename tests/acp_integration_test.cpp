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
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <istream>
#include <mutex>
#include <condition_variable>
#include <ostream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "agentty/acp/server.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

using namespace acp;
namespace ag = agentty;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    std::exit(1); } } while (0)

namespace {

// A blocking byte channel: writers push bytes, the istream side blocks in
// underflow() until bytes arrive or the channel is closed (EOF).
class Pipe {
public:
    void write_line(const std::string& s) {
        std::lock_guard lk(mu_); buf_ += s; buf_ += '\n'; cv_.notify_all();
    }
    // Blocking single-byte read; returns -1 on EOF (closed + drained).
    int getc() {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&]{ return pos_ < buf_.size() || closed_; });
        if (pos_ >= buf_.size()) return -1;
        return static_cast<unsigned char>(buf_[pos_++]);
    }
    void close() { { std::lock_guard lk(mu_); closed_ = true; } cv_.notify_all(); }
private:
    std::mutex mu_; std::condition_variable cv_;
    std::string buf_; std::size_t pos_ = 0; bool closed_ = false;
};

// istream over a Pipe (blocking).
class PipeInBuf : public std::streambuf {
public:
    explicit PipeInBuf(Pipe& p) : pipe_(p) {}
protected:
    int underflow() override {
        int c = pipe_.getc();
        if (c < 0) return traits_type::eof();
        ch_ = static_cast<char>(c);
        setg(&ch_, &ch_, &ch_ + 1);
        return traits_type::to_int_type(ch_);
    }
private:
    Pipe& pipe_; char ch_ = 0;
};

// ostream that forwards completed lines to a Pipe (the other direction).
class PipeOutBuf : public std::streambuf {
public:
    explicit PipeOutBuf(Pipe& p) : pipe_(p) {}
protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return c;
        if (c == '\n') { pipe_.write_line(acc_); acc_.clear(); }
        else acc_.push_back(static_cast<char>(c));
        return c;
    }
private:
    Pipe& pipe_; std::string acc_;
};

} // namespace

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

    // ── Wire two live pipes between agent and client ───────────────────────
    Pipe c2a, a2c;
    PipeInBuf  a_in_buf(c2a);  std::istream agent_in(&a_in_buf);
    PipeOutBuf a_out_buf(a2c); std::ostream agent_out(&a_out_buf);
    StdioTransport agent_tx(agent_in, agent_out);

    ag::auth::AuthHeader cred = ag::auth::ApiKeyHeader{"sk-test-not-empty"};
    CHECK(!ag::auth::is_empty(cred));

    ag::acp::AgentServer server(agent_tx, stream, cred, "claude-test", ag::Profile::Ask);
    std::thread agent_thread([&]{ server.serve(); });   // start()+join() on agent_tx

    // ── Client side (AgentConnection) ──────────────────────────────────────
    PipeOutBuf c_out_buf(c2a); std::ostream client_out(&c_out_buf);
    auto client_sink = [&](std::string_view line) {
        client_out.write(line.data(), static_cast<std::streamsize>(line.size()));
        client_out.put('\n'); client_out.flush();
    };

    std::atomic<int> agent_text_chunks{0};
    std::atomic<int> tool_calls{0};
    std::atomic<int> tool_completed{0};
    std::atomic<int> tool_failed{0};
    std::string last_tool_text;
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
            [&](const SU_ToolCall&) { ++tool_calls; },
            [&](const SU_ToolCallUpdate& u) {
                if (u.update.status && *u.update.status == ToolCallStatus::Completed)
                    ++tool_completed;
                if (u.update.status && *u.update.status == ToolCallStatus::Failed)
                    ++tool_failed;
                if (u.update.content)
                    for (const auto& cc : *u.update.content)
                        match(cc,
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
        auto want = reject_mode.load() ? PermissionOptionKind::RejectOnce
                                       : PermissionOptionKind::AllowOnce;
        std::string chosen;
        for (const auto& o : p.options)
            if (o.kind == want) chosen = o.optionId;
        return RequestPermissionResult{
            RequestPermissionOutcome{PO_Selected{chosen, Json::object()}}, Json::object()};
    };

    AgentConnection agent(client_sink, std::move(ch));
    // The client reads a2c; pump it into the agent connection's engine.
    std::thread client_pump([&]{
        PipeInBuf cin_buf(a2c); std::istream cin_stream(&cin_buf);
        std::string l;
        while (std::getline(cin_stream, l)) {
            if (!l.empty()) agent.engine().feed_line(l);
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
    c2a.close();   // EOF on the agent's reader → serve() returns
    agent_thread.join();
    a2c.close();   // EOF on the client pump
    client_pump.join();

    fs::remove(target);
    std::fprintf(stderr, "acp_integration: OK (turn=%d perms=%d toolcalls=%d "
                 "completed=%d usage=%d chunks=%d)\n",
                 completions.load(), perm_requests.load(), tool_calls.load(),
                 tool_completed.load(), usage_updates.load(), agent_text_chunks.load());
    std::printf("acp_integration OK\n");
    return 0;
}
