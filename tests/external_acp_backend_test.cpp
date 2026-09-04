// external_acp_backend_test — the ExternalAcpBackend core, driven against an
// in-memory FAKE ACP agent.
//
// Harness (mirrors acp-cpp/tests/loopback.cpp): a fake agent (acp::Client
// Connection) is wired to an acp::AgentConnection through a pair of mailbox
// queues + pump threads. ExternalAcpBackend takes that AgentConnection and its
// handlers, so we drive prompt() end to end WITHOUT any subprocess or sandbox.
//
// Coverage:
//   1. A clean turn: agent streams message + thought + tool_call + usage
//      updates, then returns StopReason::EndTurn. We assert every SessionUpdate
//      reached the TurnSink in order, and the round settled EndTurn (exactly one
//      terminal event — the return value, never a spurious "cancelled").
//   2. tool_use mapping: agent returns MaxTurnRequests → round EndTurn (the
//      agent's turn cap isn't agentty's round reason).
//   3. Refusal → TurnError (round failed, not cancelled).
//   4. Cancelled stop reason → TurnResult::cancelled().
//   5. User Esc (cancel token) → we send session/cancel and settle cancelled.
//   6. map_acp_stop_reason pure-function table.

#include "agentty/provider/external_acp_backend.hpp"

#include <acp/acp.hpp>

#include "agentty/tool/util/fs_helpers.hpp"   // set_workspace_root

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>   // getpid
#endif

using namespace acp;
namespace P = agentty::provider;

namespace {

class Mailbox {
public:
    void push(std::string s) {
        { std::lock_guard lk(mu_); q_.push_back(std::move(s)); }
        cv_.notify_one();
    }
    bool pop(std::string& out, bool& closed) {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&]{ return !q_.empty() || closed_; });
        if (q_.empty()) { closed = closed_; return false; }
        out = std::move(q_.front()); q_.pop_front(); return true;
    }
    void close() { { std::lock_guard lk(mu_); closed_ = true; } cv_.notify_all(); }
private:
    std::mutex mu_; std::condition_variable cv_;
    std::deque<std::string> q_; bool closed_ = false;
};

// How the fake agent should end a prompt, and whether it streams updates first.
struct AgentScript {
    StopReason stop = StopReason::EndTurn;
    bool       stream_updates = false;   // emit message+thought+tool_call+usage
    // Set true to make the prompt handler BLOCK until `release` fires — used to
    // exercise the user-cancel path (backend sends session/cancel, we then
    // return Cancelled).
    bool       block_until_released = false;
    // Set true to simulate a BUGGY agent that receives session/cancel but NEVER
    // settles the in-flight prompt future (and whose on_session_cancel is a
    // no-op). Exercises the backend's engine-level escape hatch: prompt() must
    // still return (bounded) via on_transport_closed, not wedge forever.
    bool       ignore_cancel = false;
    // Set true to make the agent call back into the client during the prompt:
    // fs/write_text_file, fs/read_text_file, and terminal/create+output. Used
    // to exercise the delegate wiring. Requires the ASYNC handler (the callbacks
    // are outbound requests whose replies arrive on the same reader thread).
    bool       exercise_callbacks = false;
    // Captured callback results (filled by the async prompt worker).
    std::string read_back;
    std::string term_output;
    int         term_exit = -1;
};

// A running fake-agent + client pair with pump threads. Destroy to tear down.
struct Harness {
    Mailbox client_to_agent, agent_to_client;
    std::unique_ptr<ClientConnection> agent_side;      // the fake agent
    std::unique_ptr<AgentConnection>  client;          // agentty's handle
    std::unique_ptr<P::ExternalAcpBackend> backend;
    std::atomic<bool> alive{true};
    std::vector<std::thread> pumps;

    std::atomic<bool> release_prompt{false};
    std::condition_variable release_cv;
    std::mutex release_mu;
    acp::List<acp::ContentBlock> last_prompt;
    std::shared_ptr<AgentScript> script_out;   // async worker writes callback results here

    ~Harness() {
        alive.store(false);
        { std::lock_guard lk(release_mu); release_prompt.store(true); }
        release_cv.notify_all();
        client_to_agent.close();
        agent_to_client.close();
        for (auto& t : pumps) if (t.joinable()) t.join();
    }
};

std::unique_ptr<Harness> make_harness(AgentScript script,
                                     P::AcpClientDelegate delegate = {}) {
    auto h = std::make_unique<Harness>();
    Harness* hp = h.get();
    // Stash script results somewhere the async worker can write; the caller
    // reads them back through the returned Harness via `h->script_out`.
    auto script_out = std::make_shared<AgentScript>(script);
    h->script_out = script_out;

    // ── Fake AGENT side ────────────────────────────────────────────────────
    AgentHandlers a;
    a.on_initialize = [](const InitializeParams&) {
        InitializeResult r;
        r.agentCapabilities.promptCapabilities.embeddedContext = true;
        return r;
    };
    a.on_session_new = [](const NewSessionParams& p) {
        assert(!p.cwd.empty());
        return NewSessionResult{SessionId{std::string("sess_fake")}, Nothing, Nothing, Json::object()};
    };
    a.on_session_prompt = [hp, script](const PromptParams& p) -> PromptResult {
        assert(p.sessionId == SessionId{std::string("sess_fake")});
        assert(!p.prompt.empty());
        hp->last_prompt = p.prompt;

        if (script.stream_updates) {
            auto send = [&](SessionUpdate u) {
                SessionUpdateMsg m; m.sessionId = p.sessionId; m.update = std::move(u);
                hp->agent_side->session_update(m);
            };
            send(SU_AgentMessageChunk{TextContent{"hello", Nothing, Json::object()}, Nothing});
            send(SU_AgentThoughtChunk{TextContent{"thinking", Nothing, Json::object()}, Nothing});
            ToolCall tc;
            tc.toolCallId = ToolCallId{std::string("tc_1")};
            tc.title      = "shell";
            send(SU_ToolCall{tc});
            send(SU_Usage{/*used*/123, /*size*/200000, Nothing});
        }
        return PromptResult{script.stop};
    };
    // The BLOCKING (user-cancel) case uses the ASYNC handler: it hands the
    // Responder to a detached worker and returns immediately, so the single
    // agent reader thread stays free to process the incoming session/cancel
    // notification (which flips `release_prompt`). This mirrors how a real
    // agent — with its own turn worker thread — behaves.
    if (script.block_until_released) {
        a.on_session_prompt = nullptr;
        a.on_session_prompt_async =
            [hp](const PromptParams&, RpcEngine::Responder<PromptResult> resp) {
            std::thread([hp, resp = std::move(resp)]() mutable {
                std::unique_lock lk(hp->release_mu);
                hp->release_cv.wait(lk, [hp]{ return hp->release_prompt.load(); });
                lk.unlock();
                resp.ok(PromptResult{StopReason::Cancelled});
            }).detach();
        };
    }

    // The IGNORE-CANCEL case: a buggy agent that accepts the prompt but never
    // settles the future — the responder is captured and simply LEAKED (held
    // forever), and on_session_cancel is a no-op. The backend must still bound
    // prompt() via its engine escape hatch.
    if (script.ignore_cancel) {
        a.on_session_prompt = nullptr;
        a.on_session_prompt_async =
            [](const PromptParams&, RpcEngine::Responder<PromptResult> resp) {
            // Deliberately never settle: stash the responder so it outlives the
            // call and the future stays pending forever.
            static std::vector<RpcEngine::Responder<PromptResult>> leaked;
            leaked.push_back(std::move(resp));
        };
    }

    // The CALLBACK-exercising case uses the async handler so the reader thread
    // stays free to deliver the fs/terminal replies while the worker awaits
    // them. The worker writes a file, reads it back, runs a terminal, records
    // the results into script_out, then settles the turn.
    if (script.exercise_callbacks) {
        a.on_session_prompt = nullptr;
        a.on_session_prompt_async =
            [hp, script_out](const PromptParams& p, RpcEngine::Responder<PromptResult> resp) {
            std::thread([hp, script_out, sid = p.sessionId, resp = std::move(resp)]() mutable {
                auto& cx = *hp->agent_side;
                // 1. write a file via the client's fs gate.
                WriteTextFileParams wp;
                wp.sessionId = sid; wp.path = "acp_probe.txt"; wp.content = "delegate-wrote-this\n";
                (void)cx.fs_write_text_file(wp).get();
                // 2. read it back.
                ReadTextFileParams rp;
                rp.sessionId = sid; rp.path = "acp_probe.txt";
                script_out->read_back = cx.fs_read_text_file(rp).get().content;
                // 3. run a terminal command.
                CreateTerminalParams cp;
                cp.sessionId = sid; cp.command = "printf"; cp.args = {"acp-term-ok"};
                auto term = cx.terminal_create(cp).get();
                TerminalRef ref; ref.sessionId = sid; ref.terminalId = term.terminalId;
                auto out = cx.terminal_output(ref).get();
                script_out->term_output = out.output;
                if (out.exitStatus && out.exitStatus->exitCode)
                    script_out->term_exit = static_cast<int>(*out.exitStatus->exitCode);
                (void)cx.terminal_release(ref).get();
                resp.ok(PromptResult{StopReason::EndTurn});
            }).detach();
        };
    }

    a.on_session_cancel = [hp, ignore = script.ignore_cancel](const CancelParams&) {
        if (ignore) return;   // buggy agent: never acts on cancel
        { std::lock_guard lk(hp->release_mu); hp->release_prompt.store(true); }
        hp->release_cv.notify_all();
    };

    h->agent_side = std::make_unique<ClientConnection>(
        [hp](std::string_view line){ hp->agent_to_client.push(std::string(line)); },
        std::move(a));

    // ── agentty CLIENT side + backend ──────────────────────────────────────
    P::ExternalAcpOptions opts;
    opts.cwd = "/tmp/proj";
    opts.reuse_session = true;
    opts.delegate = std::move(delegate);

    // acp::AgentConnection installs handlers at CONSTRUCTION. The backend's
    // handlers don't need the connection (make_handlers() only closes over the
    // backend's sink slot + delegate), so build the backend FIRST, take its full
    // handler set (session/update + fs/* + terminal/*), and construct the
    // connection with them. session/update is the one handler whose target (the
    // round's live sink) is set later, but the backend already routes that
    // through its own internal slot, so no shim is needed — we pass make_handlers()
    // verbatim.
    h->backend = std::make_unique<P::ExternalAcpBackend>(std::move(opts));
    auto handlers = h->backend->make_handlers();

    h->client = std::make_unique<AgentConnection>(
        [hp](std::string_view line){ hp->client_to_agent.push(std::string(line)); },
        std::move(handlers));

    // Bind the connection now that both exist.
    h->backend->connect(*h->client);

    // ── Pumps ──────────────────────────────────────────────────────────────
    auto pump = [hp](Mailbox& mb, RpcEngine& dst) {
        hp->pumps.emplace_back([hp, &mb, &dst]{
            while (hp->alive.load()) {
                std::string line; bool closed = false;
                if (!mb.pop(line, closed)) { if (closed) return; continue; }
                dst.feed_line(line);
            }
        });
    };
    pump(h->client_to_agent, h->agent_side->engine());
    pump(h->agent_to_client, h->client->engine());

    // Initialize the connection (negotiate) so session_new/prompt are allowed.
    InitializeParams ip;
    ip.clientCapabilities.fs.readTextFile = true;
    (void)h->client->initialize(ip).get();

    return h;
}

P::Request make_req(const char* text) {
    P::Request r;
    agentty::Message m;
    m.role = agentty::Role::User;
    m.text = text;
    r.messages.push_back(std::move(m));
    return r;
}

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n"; ++failures; } } while (0)

void test_clean_turn_streams_and_settles() {
    auto h = make_harness(AgentScript{StopReason::EndTurn, /*stream*/true, false});

    std::vector<std::string> kinds;
    P::TurnSink sink;
    sink.update = [&](acp::SessionUpdate su) {
        if (std::holds_alternative<SU_AgentMessageChunk>(su)) kinds.push_back("message");
        else if (std::holds_alternative<SU_AgentThoughtChunk>(su)) kinds.push_back("thought");
        else if (std::holds_alternative<SU_ToolCall>(su))         kinds.push_back("tool_call");
        else if (std::holds_alternative<SU_Usage>(su))            kinds.push_back("usage");
        else kinds.push_back("other");
    };

    auto res = h->backend->prompt(make_req("Hi!"), sink, /*cancel*/nullptr);

    CHECK(res.ok());
    CHECK(res.stop == agentty::StopReason::EndTurn);
    CHECK(h->backend->session_id() == "sess_fake");
    // All four update kinds arrived, in order.
    CHECK(kinds.size() == 4);
    if (kinds.size() == 4) {
        CHECK(kinds[0] == "message");
        CHECK(kinds[1] == "thought");
        CHECK(kinds[2] == "tool_call");
        CHECK(kinds[3] == "usage");
    }
}

void test_prompt_forwards_text_and_image_content() {
    auto h = make_harness(AgentScript{StopReason::EndTurn, false, false});
    auto req = make_req("look at this");
    req.messages.back().images.push_back(
        agentty::ImageContent{"image/jpeg", std::string{"\x01\x02\x03", 3}});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};

    auto res = h->backend->prompt(req, sink, nullptr);

    CHECK(res.ok());
    CHECK(h->last_prompt.size() == 2);
    if (h->last_prompt.size() == 2) {
        const auto* text = std::get_if<acp::TextContent>(&h->last_prompt[0]);
        const auto* image = std::get_if<acp::ImageContent>(&h->last_prompt[1]);
        CHECK(text && text->text == "look at this");
        CHECK(image && image->mimeType == "image/jpeg");
        CHECK(image && image->data == "AQID");
    }
}

void test_prompt_forwards_image_only_turn() {
    auto h = make_harness(AgentScript{StopReason::EndTurn, false, false});
    auto req = make_req("");
    req.messages.back().images.push_back(
        agentty::ImageContent{"image/png", std::string{"PNG", 3}});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};

    auto res = h->backend->prompt(req, sink, nullptr);

    CHECK(res.ok());
    CHECK(h->last_prompt.size() == 1);
    if (h->last_prompt.size() == 1) {
        const auto* image = std::get_if<acp::ImageContent>(&h->last_prompt[0]);
        CHECK(image && image->mimeType == "image/png");
        CHECK(image && image->data == "UE5H");
    }
}

void test_max_turn_requests_maps_to_endturn() {
    auto h = make_harness(AgentScript{StopReason::MaxTurnRequests, false, false});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};
    auto res = h->backend->prompt(make_req("go"), sink, nullptr);
    CHECK(res.ok());
    CHECK(res.stop == agentty::StopReason::EndTurn);
}

void test_refusal_is_error() {
    auto h = make_harness(AgentScript{StopReason::Refusal, false, false});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};
    auto res = h->backend->prompt(make_req("do bad thing"), sink, nullptr);
    CHECK(!res.ok());
    CHECK(res.error.has_value());
    CHECK(!res.error->user_cancel);
}

void test_agent_cancelled_stop_reason() {
    auto h = make_harness(AgentScript{StopReason::Cancelled, false, false});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};
    auto res = h->backend->prompt(make_req("x"), sink, nullptr);
    CHECK(!res.ok());
    CHECK(res.error.has_value() && res.error->user_cancel);
}

void test_user_cancel_via_token() {
    auto h = make_harness(AgentScript{StopReason::EndTurn, false, /*block*/true});
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};

    auto cancel = std::make_shared<agentty::http::CancelToken>();
    // Trip the token from another thread shortly after prompt() starts blocking.
    std::thread tripper([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        cancel->cancel();
    });

    auto res = h->backend->prompt(make_req("long task"), sink, cancel);
    tripper.join();

    CHECK(!res.ok());
    CHECK(res.error.has_value() && res.error->user_cancel);
}

// A BUGGY agent that never settles the cancelled prompt future. Without the
// backend's engine-level escape hatch, prompt() would block forever in the
// getter-join. We assert it still returns cancelled, bounded in wall-clock.
void test_cancel_against_non_settling_agent_is_bounded() {
    AgentScript script;
    script.ignore_cancel = true;   // async prompt leaks the responder; cancel is a no-op
    auto h = make_harness(script);
    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};

    auto cancel = std::make_shared<agentty::http::CancelToken>();
    std::thread tripper([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        cancel->cancel();
    });

    auto t0 = std::chrono::steady_clock::now();
    auto res = h->backend->prompt(make_req("never answered"), sink, cancel);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    tripper.join();

    // The 2s cancel deadline + escape hatch must have bounded it. Cushion for CI.
    CHECK(ms < 6000);
    if (ms >= 6000)
        std::cerr << "cancel-wedge prompt took " << ms << "ms (escape hatch regressed?)\n";
    CHECK(!res.ok());
    CHECK(res.error.has_value() && res.error->user_cancel);
}

void test_map_stop_reason_pure() {
    CHECK(P::map_acp_stop_reason(StopReason::EndTurn)         == agentty::StopReason::EndTurn);
    CHECK(P::map_acp_stop_reason(StopReason::MaxTokens)       == agentty::StopReason::MaxTokens);
    CHECK(P::map_acp_stop_reason(StopReason::MaxTurnRequests) == agentty::StopReason::EndTurn);
}

// The agent, mid-prompt, calls fs/write → fs/read → terminal/* back into the
// client; a STUB delegate records them. Verifies the handler wiring end to end.
void test_delegate_callbacks_wired() {
    // In-memory stub delegate.
    auto files = std::make_shared<std::map<std::string, std::string>>();
    P::AcpClientDelegate del;
    del.write_text_file = [files](const std::string& path, const std::string& content) {
        (*files)[path] = content; return true;
    };
    del.read_text_file = [files](const std::string& path, std::optional<int>, std::optional<int>)
            -> std::optional<std::string> {
        auto it = files->find(path);
        return it == files->end() ? std::nullopt : std::optional<std::string>(it->second);
    };
    del.run_terminal = [](const std::string& command, const std::vector<std::string>& args,
                          const std::optional<std::string>&) {
        P::AcpClientDelegate::TerminalResult r;
        r.output = command;
        for (const auto& a : args) r.output += " " + a;
        r.exit_code = 0;
        return r;
    };

    AgentScript script;
    script.exercise_callbacks = true;
    auto h = make_harness(script, std::move(del));

    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};
    auto res = h->backend->prompt(make_req("use tools"), sink, nullptr);

    CHECK(res.ok());
    CHECK((*files)["acp_probe.txt"] == "delegate-wrote-this\n");
    CHECK(h->script_out->read_back == "delegate-wrote-this\n");
    CHECK(h->script_out->term_output == "printf acp-term-ok");
}

// The REAL default_sandbox_delegate() against a temp workspace: a write lands
// on disk inside the workspace, a read gets it back, and an out-of-workspace
// write is refused.
void test_default_sandbox_delegate_roundtrip() {
    namespace fs = std::filesystem;
    static std::atomic<int> counter{0};
    auto root = fs::temp_directory_path() /
                ("acp_ws_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    agentty::tools::util::set_workspace_root(root);

    auto del = P::default_sandbox_delegate();
    CHECK(static_cast<bool>(del.read_text_file));
    CHECK(static_cast<bool>(del.write_text_file));
    CHECK(static_cast<bool>(del.run_terminal));

    // In-workspace write + read round-trip. The ACP fs protocol uses ABSOLUTE
    // paths (that's what an agent sends), so exercise the delegate the same way.
    const std::string abs_hello = (root / "hello.txt").string();
    CHECK(del.write_text_file(abs_hello, "real-delegate\n") == true);
    CHECK(fs::exists(root / "hello.txt"));
    auto body = del.read_text_file(abs_hello, std::nullopt, std::nullopt);
    CHECK(body.has_value());
    if (body) CHECK(*body == "real-delegate\n");

    // Out-of-workspace write is refused by the write gate.
    CHECK(del.write_text_file("/etc/acp_should_not_write", "nope") == false);

    fs::remove_all(root);
}

// ── Self-spawn AGENT MODE ────────────────────────────────────────────────────
// When invoked as `<this-binary> --acp-agent`, run a REAL ACP agent over
// stdin/stdout (FdTransport::process). This is the subprocess spawn_acp_agent
// launches in the e2e test below — a genuine cross-process round-trip with no
// external dependency on claude-agent-acp / codex-acp being installed.
int run_agent_mode() {
    FdTransport transport = FdTransport::process();   // fds 0/1

    std::atomic<bool> done{false};
    AgentHandlers a;
    a.on_initialize = [](const InitializeParams&) {
        InitializeResult r;
        r.agentCapabilities.promptCapabilities.embeddedContext = true;
        return r;
    };
    a.on_session_new = [](const NewSessionParams&) {
        return NewSessionResult{SessionId{std::string("e2e")}, Nothing, Nothing, Json::object()};
    };
    // Streams one message chunk, then ends the turn. After the prompt settles we
    // flip `done` so the process can exit cleanly once the reply is flushed.
    ClientConnection* self = nullptr;
    auto handler = [&self, &done](const PromptParams& p) -> PromptResult {
        SessionUpdateMsg m; m.sessionId = p.sessionId;
        m.update = SU_AgentMessageChunk{TextContent{"pong", Nothing, Json::object()}, Nothing};
        self->session_update(m);
        done.store(true);
        return PromptResult{StopReason::EndTurn};
    };
    a.on_session_prompt = handler;

    ClientConnection agent(transport.sink(), std::move(a));
    self = &agent;
    transport.start(agent.engine());

    // Run until the prompt has been answered, then give the transport a beat to
    // flush the final reply frame before we exit and close the pipe.
    while (!done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}

// ── Self-spawn WEDGED AGENT MODE ─────────────────────────────────────────────
// When invoked as `<this-binary> --acp-agent-wedged`, this deliberately
// MISBEHAVES: it answers the first prompt, then IGNORES stdin EOF entirely and
// spins forever without exiting. This is the teardown-deadlock scenario — a
// well-behaved parent that only close_stdin()'d + joined the reader would hang
// forever waiting on the child's stdout to EOF. The AgentProcessHolder dtor's
// bounded-grace + force-kill watchdog is what must reap it. If the watchdog
// regresses, the e2e test below HANGS (caught by ctest's timeout).
int run_wedged_agent_mode() {
    FdTransport transport = FdTransport::process();   // fds 0/1
    std::atomic<bool> answered{false};
    AgentHandlers a;
    a.on_initialize = [](const InitializeParams&) {
        InitializeResult r;
        r.agentCapabilities.promptCapabilities.embeddedContext = true;
        return r;
    };
    a.on_session_new = [](const NewSessionParams&) {
        return NewSessionResult{SessionId{std::string("wedged")}, Nothing, Nothing, Json::object()};
    };
    ClientConnection* self = nullptr;
    a.on_session_prompt = [&self, &answered](const PromptParams& p) -> PromptResult {
        SessionUpdateMsg m; m.sessionId = p.sessionId;
        m.update = SU_AgentMessageChunk{TextContent{"pong", Nothing, Json::object()}, Nothing};
        self->session_update(m);
        answered.store(true);
        return PromptResult{StopReason::EndTurn};
    };
    ClientConnection agent(transport.sink(), std::move(a));
    self = &agent;
    transport.start(agent.engine());
    // Never exit on stdin EOF: spin forever. Only an external kill stops us.
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// ── End-to-end: spawn_acp_agent drives a REAL subprocess ────────────────────
void test_spawn_real_subprocess(const std::string& self_path) {
    // Build the backend first, get its handlers, spawn the agent with them,
    // then bind — the documented production flow.
    P::ExternalAcpOptions opts;
    opts.cwd = "/tmp";
    auto backend = std::make_unique<P::ExternalAcpBackend>(std::move(opts));

    std::vector<std::string> captured;
    P::TurnSink sink;
    sink.update = [&](acp::SessionUpdate su) {
        if (auto* mc = std::get_if<SU_AgentMessageChunk>(&su))
            if (auto* t = std::get_if<TextContent>(&mc->content)) captured.push_back(t->text);
    };

    InitializeParams init;
    std::string err;
    auto agent = P::spawn_acp_agent(
        // Bundled into agentty_standalone_tests (argv-dispatched), so the child
        // must carry the dispatch name before its own child-mode flag:
        //   <exe> external_acp_backend_test --acp-agent
        {self_path, "external_acp_backend_test", "--acp-agent"}, init,
                                    backend->make_handlers(), err);
    if (!agent.ok()) {
        std::cerr << "spawn_acp_agent failed: " << err << "\n";
        ++failures;
        return;
    }
    backend->connect(*agent.connection);

    auto res = backend->prompt(make_req("ping"), sink, nullptr);
    CHECK(res.ok());
    CHECK(res.stop == agentty::StopReason::EndTurn);
    CHECK(backend->session_id() == "e2e");
    CHECK(captured.size() == 1);
    if (captured.size() == 1) CHECK(captured[0] == "pong");
    // Dropping `agent` here tears down the child + transport.
}

// The child ignores stdin EOF and spins forever. Tearing down the spawned agent
// must NOT hang: the holder dtor's watchdog force-kills it. We assert teardown
// completes well within a few seconds (the dtor's grace is ~1s + reap).
void test_wedged_child_teardown_is_bounded(const std::string& self_path) {
    P::ExternalAcpOptions opts;
    opts.cwd = "/tmp";
    auto backend = std::make_unique<P::ExternalAcpBackend>(std::move(opts));

    P::TurnSink sink; sink.update = [](acp::SessionUpdate){};
    InitializeParams init;
    std::string err;
    auto agent = P::spawn_acp_agent(
        {self_path, "external_acp_backend_test", "--acp-agent-wedged"}, init,
                                    backend->make_handlers(), err);
    if (!agent.ok()) {
        std::cerr << "spawn wedged agent failed: " << err << "\n";
        ++failures;
        return;
    }
    backend->connect(*agent.connection);

    // One clean round so the child is fully live and its reader is parked.
    auto res = backend->prompt(make_req("ping"), sink, nullptr);
    CHECK(res.ok());

    // Now tear down. The child will IGNORE the stdin-EOF we send; only the
    // force-kill watchdog reaps it. Time the teardown: it must be bounded.
    auto t0 = std::chrono::steady_clock::now();
    backend.reset();   // backend holds nothing that blocks
    // SpawnedAcpAgent::reset stops/joins the transport reader while its engine
    // is alive, then destroys the connection.
    agent.reset();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    // Grace is ~1s; give generous headroom for CI. A regression to the old
    // "close_stdin + join" would block here indefinitely (caught by ctest
    // timeout even if this bound didn't).
    CHECK(ms < 5000);
    if (ms >= 5000)
        std::cerr << "wedged-child teardown took " << ms << "ms (watchdog regressed?)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--acp-agent")
        return run_agent_mode();
    if (argc > 1 && std::string(argv[1]) == "--acp-agent-wedged")
        return run_wedged_agent_mode();

    test_clean_turn_streams_and_settles();
    test_prompt_forwards_text_and_image_content();
    test_prompt_forwards_image_only_turn();
    test_max_turn_requests_maps_to_endturn();
    test_refusal_is_error();
    test_agent_cancelled_stop_reason();
    test_user_cancel_via_token();
    test_cancel_against_non_settling_agent_is_bounded();
    test_map_stop_reason_pure();
    test_delegate_callbacks_wired();
    test_default_sandbox_delegate_roundtrip();
    test_spawn_real_subprocess(argv[0]);
    test_wedged_child_teardown_is_bounded(argv[0]);

    if (failures == 0) {
        std::cout << "external_acp_backend_test OK\n";
        return 0;
    }
    std::cerr << "external_acp_backend_test FAILED (" << failures << " check(s))\n";
    return 1;
}
