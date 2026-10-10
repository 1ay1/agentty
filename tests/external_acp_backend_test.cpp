// external_acp_backend_test — the ExternalAcpBackend core, driven against an
// in-memory FAKE ACP agent.
//
// Harness: the fake agent is its own rpc::Peer, wired to the backend's peer
// through rpc::channel_pair(). We drive prompt() end to end with no
// subprocess or sandbox.
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
#include <acp/protocol.hpp>

#include "agentty/rpc/peer.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // set_workspace_root

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
namespace P   = agentty::provider;
namespace rpc = agentty::rpc;

namespace {

// How the fake agent should end a prompt, and whether it streams updates first.
struct AgentScript {
    StopReason stop = StopReason::EndTurn;
    bool       stream_updates = false;   // emit message+thought+tool_call+usage
    // Block the prompt until session/cancel arrives, then answer Cancelled.
    bool       block_until_released = false;
    // A BUGGY agent: accepts the prompt, never answers it, ignores
    // session/cancel. prompt() must still return, bounded.
    bool       ignore_cancel = false;
    // Call back into the client during the prompt: fs/write, fs/read,
    // terminal/create+output+release.
    bool       exercise_callbacks = false;
    // Callback results, filled by the prompt worker.
    std::string read_back;
    std::string term_output;
    int         term_exit = -1;
};

// The fake agent: its own peer, answering on the far end of a channel pair.
struct FakeAgent {
    AgentScript script;
    std::unique_ptr<rpc::Peer> peer;
    acp::List<acp::ContentBlock> last_prompt;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    released = false;
    std::vector<std::thread> workers;

    void release() {
        { std::lock_guard lk(mu); released = true; }
        cv.notify_all();
    }

    ~FakeAgent() {
        release();
        if (peer) peer->stop();
        for (auto& t : workers) if (t.joinable()) t.join();
    }

    template <jsonrpc::IsMethod M>
    typename M::result call(const typename M::params& p) {
        auto r = peer->call<M>(p);
        if (!r) throw r.error();
        return std::move(*r);
    }

    void send_update(const SessionId& sid, SessionUpdate u) {
        SessionUpdateMsg m; m.sessionId = sid; m.update = std::move(u);
        peer->notify<to_client::SessionUpdate>(m);
    }

    // Runs on a worker so the agent's reader stays free for replies.
    void run_prompt(Id id, PromptParams p) {
        using Prompt = to_agent::SessionPrompt;
        if (script.ignore_cancel) return;   // never answer
        if (script.block_until_released) {
            std::unique_lock lk(mu);
            cv.wait(lk, [&] { return released; });
            lk.unlock();
            peer->reply<Prompt>(id, PromptResult{StopReason::Cancelled});
            return;
        }
        if (script.exercise_callbacks) {
            const auto& sid = p.sessionId;
            WriteTextFileParams wp;
            wp.sessionId = sid; wp.path = "acp_probe.txt"; wp.content = "delegate-wrote-this\n";
            (void)call<to_client::FsWriteTextFile>(wp);
            ReadTextFileParams rp;
            rp.sessionId = sid; rp.path = "acp_probe.txt";
            script.read_back = call<to_client::FsReadTextFile>(rp).content;
            CreateTerminalParams cp;
            cp.sessionId = sid; cp.command = "printf"; cp.args = {"acp-term-ok"};
            auto term = call<to_client::TerminalCreate>(cp);
            TerminalRef ref; ref.sessionId = sid; ref.terminalId = term.terminalId;
            auto out = call<to_client::TerminalOutput>(ref);
            script.term_output = out.output;
            if (out.exitStatus && out.exitStatus->exitCode)
                script.term_exit = static_cast<int>(*out.exitStatus->exitCode);
            (void)call<to_client::TerminalRelease>(ref);
            peer->reply<Prompt>(id, PromptResult{StopReason::EndTurn});
            return;
        }
        if (script.stream_updates) {
            send_update(p.sessionId, SU_AgentMessageChunk{TextContent{"hello", Nothing, Json::object()}, Nothing});
            send_update(p.sessionId, SU_AgentThoughtChunk{TextContent{"thinking", Nothing, Json::object()}, Nothing});
            ToolCall tc;
            tc.toolCallId = ToolCallId{std::string("tc_1")};
            tc.title      = "shell";
            send_update(p.sessionId, SU_ToolCall{tc});
            send_update(p.sessionId, SU_Usage{/*used*/123, /*size*/200000, Nothing});
        }
        peer->reply<Prompt>(id, PromptResult{script.stop});
    }

    rpc::Dispatch dispatch() {
        rpc::Dispatch d;
        d.on_call = [this](const Call& c) -> std::optional<std::string> {
            if (c.method == to_agent::Initialize::name) {
                InitializeResult r;
                r.agentCapabilities.promptCapabilities.embeddedContext = true;
                return reply<to_agent::Initialize>(c.id, r);
            }
            if (c.method == to_agent::SessionNew::name) {
                return reply<to_agent::SessionNew>(c.id,
                    NewSessionResult{SessionId{std::string("sess_fake")}, Nothing, Nothing, Json::object()});
            }
            if (c.method == to_agent::SessionPrompt::name) {
                auto p = jsonrpc::from_json<PromptParams>(c.params);
                last_prompt = p.prompt;
                workers.emplace_back([this, id = c.id, p = std::move(p)]() mutable {
                    run_prompt(std::move(id), std::move(p));
                });
                return std::nullopt;
            }
            return reply(c.id, std::unexpected(RpcError(errc::MethodNotFound, c.method)));
        };
        d.on_notification = [this](const Notification& n) {
            if (n.method == to_agent::SessionCancel::name && !script.ignore_cancel) release();
        };
        return d;
    }
};

// A running fake-agent + backend pair. Destroy to tear down.
struct Harness {
    std::unique_ptr<P::ExternalAcpBackend> backend;
    std::unique_ptr<rpc::Peer>             client;   // agentty's side
    std::unique_ptr<FakeAgent>             agent;

    const acp::List<acp::ContentBlock>& last_prompt() const { return agent->last_prompt; }
    AgentScript& script() { return agent->script; }
    ~Harness() {
        // The client peer first: its dispatch points at the backend.
        if (client) client->stop();
        agent.reset();
        client.reset();
        backend.reset();
    }
};

std::unique_ptr<Harness> make_harness(AgentScript script,
                                     P::AcpClientDelegate delegate = {}) {
    auto h = std::make_unique<Harness>();
    auto [ours, theirs] = rpc::channel_pair();

    h->agent = std::make_unique<FakeAgent>();
    h->agent->script = std::move(script);
    h->agent->peer = std::make_unique<rpc::Peer>(std::move(theirs), h->agent->dispatch(), "fake-agent");
    h->agent->peer->start();

    P::ExternalAcpOptions opts;
    opts.cwd = "/tmp/proj";
    opts.reuse_session = true;
    opts.delegate = std::move(delegate);
    h->backend = std::make_unique<P::ExternalAcpBackend>(std::move(opts));

    h->client = std::make_unique<rpc::Peer>(std::move(ours), h->backend->make_dispatch(), "client");
    h->client->start();
    h->backend->connect(*h->client);

    InitializeParams ip;
    ip.clientCapabilities.fs.readTextFile = true;
    (void)h->client->call<to_agent::Initialize>(ip);
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
    CHECK(h->last_prompt().size() == 2);
    if (h->last_prompt().size() == 2) {
        const auto* text = std::get_if<acp::TextContent>(&h->last_prompt()[0]);
        const auto* image = std::get_if<acp::ImageContent>(&h->last_prompt()[1]);
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
    CHECK(h->last_prompt().size() == 1);
    if (h->last_prompt().size() == 1) {
        const auto* image = std::get_if<acp::ImageContent>(&h->last_prompt()[0]);
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
    CHECK(h->script().read_back == "delegate-wrote-this\n");
    CHECK(h->script().term_output == "printf acp-term-ok");
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
    agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), root);

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
// stdin/stdout (rpc::fd_channel). This is the subprocess spawn_acp_agent
// launches in the e2e test below — a genuine cross-process round-trip with no
// external dependency on claude-agent-acp / codex-acp being installed.
//
// It answers initialize and session/new, streams one "pong" chunk per prompt,
// and ends the turn. `session` names the session it hands out.
std::unique_ptr<rpc::Peer> start_stdio_agent(std::string session, std::atomic<bool>& answered) {
    // The dispatch notifies through the peer it belongs to, set right after.
    auto self = std::make_shared<rpc::Peer*>(nullptr);
    rpc::Dispatch d;
    d.on_call = [self, session, &answered](const Call& c) -> std::optional<std::string> {
        if (c.method == to_agent::Initialize::name) {
            InitializeResult r;
            r.agentCapabilities.promptCapabilities.embeddedContext = true;
            return reply<to_agent::Initialize>(c.id, r);
        }
        if (c.method == to_agent::SessionNew::name)
            return reply<to_agent::SessionNew>(c.id,
                NewSessionResult{SessionId{session}, Nothing, Nothing, Json::object()});
        if (c.method == to_agent::SessionPrompt::name) {
            auto pp = jsonrpc::from_json<PromptParams>(c.params);
            SessionUpdateMsg m; m.sessionId = pp.sessionId;
            m.update = SU_AgentMessageChunk{TextContent{"pong", Nothing, Json::object()}, Nothing};
            (*self)->notify<to_client::SessionUpdate>(m);
            answered.store(true);
            return reply<to_agent::SessionPrompt>(c.id, PromptResult{StopReason::EndTurn});
        }
        return reply(c.id, std::unexpected(RpcError(errc::MethodNotFound, c.method)));
    };
    auto peer = std::make_unique<rpc::Peer>(rpc::fd_channel(0, 1), std::move(d), "stdio-agent");
    *self = peer.get();
    return peer;
}

int run_agent_mode() {
    std::atomic<bool> answered{false};
    auto peer = start_stdio_agent("e2e", answered);
    peer->start();
    peer->wait_closed();   // the parent closes our stdin
    peer->stop();
    return 0;
}

// ── Self-spawn WEDGED AGENT MODE ─────────────────────────────────────────────
// When invoked as `<this-binary> --acp-agent-wedged`, this deliberately
// MISBEHAVES: it answers the first prompt, then IGNORES stdin EOF entirely and
// spins forever without exiting. A parent that only closed our stdin and
// waited for our stdout to end would hang; the spawned agent's teardown must
// kill us instead. If that regresses, the e2e test below HANGS.
int run_wedged_agent_mode() {
    std::atomic<bool> answered{false};
    auto peer = start_stdio_agent("wedged", answered);
    peer->start();
    // Never exit on stdin EOF: spin forever. Only an external kill stops us.
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// ── End-to-end: spawn_acp_agent drives a REAL subprocess ────────────────────
void test_spawn_real_subprocess(const std::string& self_path) {
    // Build the backend first, get its dispatch, spawn the agent with it,
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
                                    backend->make_dispatch(), err);
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
    // Dropping `agent` here tears down the peer and the child.
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
                                    backend->make_dispatch(), err);
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
    // kill reaps it. Time the teardown: it must be bounded.
    auto t0 = std::chrono::steady_clock::now();
    // The peer first: its dispatch points at the backend.
    agent.reset();
    backend.reset();
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
