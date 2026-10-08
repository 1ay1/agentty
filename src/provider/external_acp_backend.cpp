// SPDX-License-Identifier: same as project.
//
// ExternalAcpBackend — see include/agentty/provider/external_acp_backend.hpp.

#include "agentty/provider/external_acp_backend.hpp"

#include <chrono>
#include <algorithm>
#include <optional>
#include <maya/runtime.hpp>
#include <thread>
#include <utility>
#include <variant>

#include <acp/caps.hpp>          // negotiate_version
#include <acp/content.hpp>       // acp::TextContent, ContentBlock
#include <acp/methods.hpp>       // params/results
#include <acp/tools.hpp>         // RequestPermissionOutcome, PO_*

#include <mcp/cap/process.hpp>   // ::mcp::cap::ChildProcess (portable spawn)

#include "agentty/domain/conversation.hpp"  // agentty::Message, Role
#include "agentty/util/base64.hpp"
#include "agentty/tool/util/fs_helpers.hpp"  // read_file/write_file + path gates
#include "agentty/tool/util/sandbox.hpp"     // sandbox::run_argv/run_shell_command
#include "agentty/tool/util/subprocess.hpp"  // SubprocessResult

namespace agentty::provider {

// Pin the unqualified `acp::` in this TU to the GLOBAL ACP library namespace.
// agentty has its OWN `agentty::acp` (the ACP *server* — src/acp/server.cpp),
// so inside agentty::provider a bare `acp::` is ambiguous; under a unity build
// (batched with a TU that opened agentty::acp) it wrongly bound to
// agentty::acp. This alias makes every `acp::…` below resolve to ::acp
// unconditionally — correct regardless of batching.
namespace acp = ::acp;

namespace {

// ── Current host turn → ACP prompt blocks ───────────────────────────────────
// The delegated agent keeps its own transcript across rounds, so each prompt
// carries only the newest REAL user turn plus synthetic context attached to
// that turn. Proactive RAG is stored as a User message for native model wires;
// treating it as the user question here would drop the actual request.
[[nodiscard]] acp::List<acp::ContentBlock> prompt_blocks_from(const Request& req) {
    acp::List<acp::ContentBlock> out;

    auto has_image = [](const agentty::Message& m) {
        return std::any_of(m.images.begin(), m.images.end(),
                           [](const agentty::ImageContent& image) {
                               return !image.bytes().empty();
                           });
    };

    std::size_t user_index = req.messages.size();
    for (std::size_t i = req.messages.size(); i-- > 0;) {
        const auto& m = req.messages[i];
        if (m.role == agentty::Role::User && !m.is_proactive_context() && !m.fork_note
            && (!m.text.empty() || has_image(m))) {
            user_index = i;
            break;
        }
    }

    auto append_message = [&out](const agentty::Message& message) {
        if (!message.text.empty()) {
            acp::TextContent text;
            text.text = message.text;
            out.emplace_back(std::move(text));
        }
        for (const auto& image : message.images) {
            if (image.bytes().empty()) continue;
            acp::ImageContent block;
            block.data = image.base64();
            block.mimeType = image.media_type.empty() ? "image/png" : image.media_type;
            out.emplace_back(std::move(block));
        }
    };

    if (user_index < req.messages.size()) {
        // Fork provenance: a fork_note is a synthetic User message seeded at
        // the HEAD of a forked thread, BEFORE the user's first prompt. It
        // carries the pointer to the parent transcript the model reads on
        // demand. Prepend any such leading notes so the very first turn of a
        // fork still tells the agent where its prior context lives (the
        // proactive-context loop below only scans AFTER user_index).
        for (std::size_t i = 0; i < user_index; ++i) {
            if (req.messages[i].fork_note)
                append_message(req.messages[i]);
        }
        append_message(req.messages[user_index]);
        for (std::size_t i = user_index + 1; i < req.messages.size(); ++i) {
            const auto& m = req.messages[i];
            if (m.role == agentty::Role::User && m.is_proactive_context())
                append_message(m);
        }
    }

    // A prompt with no content is invalid; send an empty text block so the
    // agent gets a well-formed request instead of us silently no-op'ing.
    if (out.empty()) {
        acp::TextContent t;
        t.text = "";
        out.emplace_back(std::move(t));
    }
    return out;
}

} // namespace

// ── Turn-level StopReason mapping ────────────────────────────────────────────
StopReason map_acp_stop_reason(acp::StopReason r) noexcept {
    switch (r) {
        case acp::StopReason::EndTurn:         return StopReason::EndTurn;
        case acp::StopReason::MaxTokens:       return StopReason::MaxTokens;
        // "too many tool rounds" is a turn-level guardrail; from agentty's
        // round view the model simply finished this round — EndTurn is the
        // honest round-level reason (the agent loop's own turn cap governs).
        case acp::StopReason::MaxTurnRequests: return StopReason::EndTurn;
        // Cancelled / Refusal are handled as TurnErrors by the caller and never
        // reach here; map defensively to EndTurn.
        case acp::StopReason::Refusal:         return StopReason::EndTurn;
        case acp::StopReason::Cancelled:       return StopReason::EndTurn;
    }
    return StopReason::EndTurn;
}

// ── Default sandbox-wired delegate ───────────────────────────────────
AcpClientDelegate default_sandbox_delegate() {
    namespace util = agentty::tools::util;
    AcpClientDelegate d;

    // fs/read_text_file → read gate (workspace + skill read-allowlist; symlink
    // escape blocked). An out-of-bounds path or a missing file returns nullopt,
    // which the handler maps to an empty read — the agent never escapes the
    // boundary and never gets a raw filesystem error.
    d.read_text_file = [](const std::string& path,
                          std::optional<int> line,
                          std::optional<int> limit) -> std::optional<std::string> {
        auto wp = util::make_readable_path_checked(path, "acp");
        if (!wp) return std::nullopt;
        std::string body = util::read_file(*wp);

        // ACP line/limit is a 1-based line window (like agentty's read tool).
        // Absent ⇒ whole file.
        if (!line && !limit) return body;

        std::vector<std::string> lines;
        std::string cur;
        for (char c : body) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(std::move(cur));

        const int start = line ? std::max(1, *line) : 1;         // 1-based
        const int count = limit ? std::max(0, *limit)
                                : static_cast<int>(lines.size());
        std::string out;
        for (int i = start - 1;
             i < static_cast<int>(lines.size()) && i < start - 1 + count; ++i) {
            out += lines[static_cast<std::size_t>(i)];
            out += '\n';
        }
        return out;
    };

    // fs/write_text_file → write gate (workspace ONLY — never the read
    // allowlist). Refuses (false) on an out-of-workspace path or write error.
    d.write_text_file = [](const std::string& path, const std::string& content) -> bool {
        auto wp = util::make_workspace_path_checked(path, "acp");
        if (!wp) return false;
        return util::write_file(*wp, content).empty();   // "" == success
    };

    // terminal/create → OS-native sandbox (bwrap / sandbox-exec; no-op on
    // Windows). Runs the command to completion; the backend caches the result
    // for the async output/wait_for_exit follow-ups.
    d.run_terminal = [](const std::string& command,
                        const std::vector<std::string>& args,
                        const std::optional<std::string>& cwd)
            -> AcpClientDelegate::TerminalResult {
        using namespace std::chrono_literals;
        constexpr std::size_t kMax = 64'000;
        constexpr auto        kTimeout = 120s;

        util::SubprocessResult r;
        if (cwd && !cwd->empty()) {
            // A cwd is requested — run through the shell form so we can `cd`
            // first. Quote the cwd and each arg minimally (single-quote wrap).
            auto shq = [](const std::string& s) {
                std::string q = "'";
                for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
                q += "'";
                return q;
            };
            std::string cmd = "cd " + shq(*cwd) + " && " + shq(command);
            for (const auto& a : args) cmd += " " + shq(a);
            r = util::sandbox::run_shell_command(cmd, kMax, kTimeout);
        } else {
            std::vector<std::string> argv;
            argv.reserve(args.size() + 1);
            argv.push_back(command);
            for (const auto& a : args) argv.push_back(a);
            r = util::sandbox::run_argv(argv, kMax, kTimeout);
        }

        AcpClientDelegate::TerminalResult out;
        out.output    = std::move(r.output);
        out.exit_code = r.started ? r.exit_code : 127;   // 127 == spawn failure
        out.truncated = r.truncated;
        if (!r.started && out.output.empty()) out.output = r.start_error;
        return out;
    };

    // request_permission is intentionally left UNSET: the external agent runs
    // its own permission gate, and every fs/terminal call above is already
    // boundary-checked here, so we allow by default rather than double-prompting
    // the user for the same action.
    return d;
}

// ── Construction ────────────────────────────────────────────────────
ExternalAcpBackend::ExternalAcpBackend(ExternalAcpOptions opts)
    : opts_(std::move(opts)), router_(make_router()) {}

void ExternalAcpBackend::connect(rpc::Peer& peer) noexcept {
    peer_ = &peer;
}

std::string ExternalAcpBackend::session_id() const {
    return session_.read([](const std::optional<std::string>& s) {
        return s.value_or(std::string{});
    });
}

// ── Inbound calls ───────────────────────────────────────────────────────────
// The agent's calls and notifications, answered on the peer's reader. The
// round's sink is published for the life of the in-flight round and null
// otherwise, so a late update after settle is dropped, not crashed.
rpc::Dispatch ExternalAcpBackend::make_dispatch() {
    rpc::Dispatch d;
    d.on_call         = [this](const acp::Call& c) { return router_.handle(*this, c); };
    d.on_notification = [this](const acp::Notification& n) { router_.handle(*this, n); };
    return d;
}

ExternalAcpBackend::Router ExternalAcpBackend::make_router() const {
    namespace m = acp::to_client;
    using B = ExternalAcpBackend;
    Router r;

    // session/update → normalized SessionUpdate to the round's sink.
    r.on<m::SessionUpdate>([](B& b, const acp::SessionUpdateMsg& msg) {
        auto round = b.round_.current();
        if (!round) return;
        if (auto member = round->inflight.join()) round->sink(msg.update);
    });

    // request_permission → delegate decides; default allow. We answer with the
    // agent's own first option when allowing, else Cancelled.
    r.on<m::RequestPermission>([](B& b, const acp::RequestPermissionParams& p) {
        bool allow = true;
        if (b.opts_.delegate.request_permission) allow = b.opts_.delegate.request_permission(p);
        acp::RequestPermissionResult res;
        if (allow && !p.options.empty()) {
            acp::PO_Selected sel;
            sel.optionId = p.options.front().optionId;
            res.outcome  = sel;
        } else {
            // Denied, or allowed with nothing to select.
            res.outcome = acp::PO_Cancelled{};
        }
        return res;
    });

    // fs/* → delegate. Unset ⇒ MethodNotFound, which tells the agent not to use it.
    if (opts_.delegate.read_text_file) {
        r.on<m::FsReadTextFile>([](B& b, const acp::ReadTextFileParams& p) {
            std::optional<int> line  = p.line  ? std::optional<int>(static_cast<int>(*p.line))  : std::nullopt;
            std::optional<int> limit = p.limit ? std::optional<int>(static_cast<int>(*p.limit)) : std::nullopt;
            acp::ReadTextFileResult res;
            // A refused or missing read is an empty read, not a protocol error.
            if (auto body = b.opts_.delegate.read_text_file(p.path, line, limit))
                res.content = std::move(*body);
            return res;
        });
    }
    if (opts_.delegate.write_text_file) {
        r.on<m::FsWriteTextFile>([](B& b, const acp::WriteTextFileParams& p) {
            b.opts_.delegate.write_text_file(p.path, p.content);
            return acp::Unit{};
        });
    }

    // terminal/* → synchronous executor. create() runs the command to
    // completion via the delegate and caches the result under a fresh id;
    // output/wait_for_exit read it back; release() drops it.
    if (opts_.delegate.run_terminal) {
        r.on<m::TerminalCreate>([](B& b, const acp::CreateTerminalParams& p) {
            std::optional<std::string> cwd = p.cwd ? std::optional<std::string>(*p.cwd)
                                                   : std::nullopt;
            auto res = b.opts_.delegate.run_terminal(p.command, p.args, cwd);
            acp::CreateTerminalResult out;
            out.terminalId = b.terminals_.with([](Terminals& t, TerminalState st) {
                std::string tid = "term_" + std::to_string(t.next_id++);
                t.by_id[tid] = std::move(st);
                return tid;
            }, TerminalState{std::move(res.output), res.exit_code, res.truncated});
            return out;
        });
        r.on<m::TerminalOutput>([](B& b, const acp::TerminalOutputParams& p) {
            acp::TerminalOutputResult res;
            const auto term = b.terminals_.read([](const Terminals& t, std::string id) {
                auto it = t.by_id.find(id);
                return it == t.by_id.end() ? std::optional<TerminalState>{}
                                           : std::optional<TerminalState>{it->second};
            }, p.terminalId);
            if (term) {
                res.output    = term->output;
                res.truncated = term->truncated;
                acp::TerminalExitStatus st;
                st.exitCode = term->exit_code;
                res.exitStatus = st;   // command already ran to completion
            }
            return res;
        });
        r.on<m::TerminalWaitForExit>([](B& b, const acp::TerminalWaitForExitParams& p) {
            acp::TerminalExitStatus st;
            st.exitCode = b.terminals_.read([](const Terminals& t, std::string id) {
                auto it = t.by_id.find(id);
                return it == t.by_id.end() ? std::optional<int>{}
                                           : std::optional<int>{it->second.exit_code};
            }, p.terminalId);
            return st;   // already exited (synchronous executor)
        });
        // The command already ran; nothing to signal.
        r.on<m::TerminalKill>([](B&, const acp::TerminalKillParams&) { return acp::Unit{}; });
        r.on<m::TerminalRelease>([](B& b, const acp::TerminalReleaseParams& p) {
            b.terminals_.with([](Terminals& t, std::string id) { t.by_id.erase(id); },
                              p.terminalId);
            return acp::Unit{};
        });
    }
    return r;
}

// ── Session lifecycle ────────────────────────────────────────────────────────
std::optional<acp::SessionId>
ExternalAcpBackend::ensure_session_(const Request& req, std::optional<TurnError>& err) {
    (void)req;
    if (opts_.reuse_session) {
        auto have = session_.read([](const std::optional<std::string>& s) { return s; });
        if (have) return acp::SessionId{std::move(*have)};
    }
    if (peer_ == nullptr) {
        err = TurnError{"ExternalAcpBackend: connect() not called (no connection bound)"};
        return std::nullopt;
    }

    acp::NewSessionParams np;
    np.cwd = opts_.cwd;
    np.mcpServers = opts_.mcp_servers;

    auto r = peer_->call<acp::to_agent::SessionNew>(np);
    if (!r) {
        err = TurnError{std::string("session/new failed: ") + r.error().what()};
        return std::nullopt;
    }
    if (!opts_.reuse_session) return r->sessionId;
    // Rounds are one at a time, but if two ever raced, the first id wins.
    auto kept = session_.with([](std::optional<std::string>& s, std::string id) {
        if (!s) s = std::move(id);
        return *s;
    }, r->sessionId.value);
    return acp::SessionId{std::move(kept)};
}

// ── The round ────────────────────────────────────────────────────────────────
TurnResult ExternalAcpBackend::prompt(const Request&              req,
                                      const TurnSink&             sink,
                                      const http::CancelTokenPtr& cancel) {
    // Open (or reuse) the session.
    std::optional<TurnError> sess_err;
    auto sid = ensure_session_(req, sess_err);
    if (!sid) return TurnResult::failed(*sess_err);

    // Publish this round's sink so inbound session/update notifications (on the
    // reader thread) reach it. Cleared on scope exit — a late update becomes a
    // no-op, never a second terminal event.
    {
        auto round = std::make_shared<Round>();
        round->sink = [&sink](acp::SessionUpdate su) { sink(std::move(su)); };
        round_.publish(std::move(round));
    }
    struct RoundGuard {
        ExternalAcpBackend* self;
        ~RoundGuard() {
            if (auto r = self->round_.take()) (void)r->inflight.stop_and_wait(std::nullopt);
        }
    } round_guard{this};

    acp::PromptParams pp;
    pp.sessionId = *sid;
    pp.prompt    = prompt_blocks_from(req);

    // Wait for the PromptResult while session/update streams to `sink`. On
    // Esc, send session/cancel: a cooperative agent answers Cancelled within
    // a beat; one that doesn't is failed locally after a 2 s grace, so the
    // wait is bounded however the agent behaves. A turn has no deadline.
    using namespace std::chrono_literals;
    bool sent_cancel = false;
    rpc::CallOptions o;
    o.timeout = 0ms;
    if (cancel) o.cancel = cancel->token();
    o.on_cancel = [this, &sent_cancel, sid = *sid] {
        sent_cancel = true;
        acp::CancelParams cp;
        cp.sessionId = sid;
        peer_->notify<acp::to_agent::SessionCancel>(cp);
    };
    o.cancel_grace = 2s;
    auto result = peer_->call<acp::to_agent::SessionPrompt>(pp, std::move(o));

    if (sent_cancel) return TurnResult::cancelled();

    // Terminal event — emitted EXACTLY ONCE, here, as the return value.
    if (!result) {
        // If the agent 401'd on its upstream, the message carries it — surface
        // as auth_expired so the OAuth-refresh retry path can re-run.
        const std::string msg = result.error().what();
        TurnError te{std::string("agent error: ") + msg};
        if (msg.find("401") != std::string::npos || msg.find("403") != std::string::npos ||
            msg.find("unauthor") != std::string::npos || msg.find("Unauthor") != std::string::npos)
            te.auth_expired = true;
        return TurnResult::failed(te);
    }

    // Turn-level StopReason → round result.
    switch (result->stopReason) {
        case acp::StopReason::Cancelled:
            return TurnResult::cancelled();
        case acp::StopReason::Refusal:
            return TurnResult::failed(TurnError{"agent refused to continue"});
        default:
            return TurnResult::finished(map_acp_stop_reason(result->stopReason));
    }
}

// ── Subprocess factory ───────────────────────────────────────────────────────
// Spawn `argv[0]` with the remaining args as an ACP agent, run an rpc::Peer
// over its stdio, run `initialize`, and return the connected handle.
// ::mcp::cap::ChildProcess is fork/exec/pipe on POSIX and CreateProcess on
// Windows, exposing the child's stdout as an istream and its stdin as an
// ostream.
namespace {

// Owns the spawned child. Torn down after the peer has stopped: by then the
// reader is either done or abandoned (it co-owns its state, not the child's
// streams, so the child must outlive the read it may be parked in — see
// the order in SpawnedAcpAgent::reset).
struct AgentProcessHolder {
    std::unique_ptr<::mcp::cap::ChildProcess> child;
    ~AgentProcessHolder() { if (child) child->terminate(); }
};

} // namespace

void SpawnedAcpAgent::reset() noexcept {
    // Stop the peer first: it closes the child's stdin (a cooperative agent
    // exits), and the stop's interrupt kills the child so a reader parked on
    // its stdout sees EOF and finishes inside the peer's grace. Only then is
    // the child (and its streams) destroyed.
    if (connection) connection->stop();
    connection.reset();
    process.reset();
}

SpawnedAcpAgent spawn_acp_agent(const std::vector<std::string>& argv,
                                const acp::InitializeParams&    init,
                                rpc::Dispatch                   dispatch,
                                std::string&                    err) {
    if (argv.empty()) {
        err = "spawn_acp_agent: empty argv";
        return {};
    }

    auto holder = std::make_shared<AgentProcessHolder>();
    try {
        ::mcp::cap::ChildProcess::Spawn spawn;
        spawn.command = argv.front();
        spawn.args.assign(argv.begin() + 1, argv.end());
        holder->child = std::make_unique<::mcp::cap::ChildProcess>(spawn);
    } catch (const std::exception& e) {
        err = std::string("spawn_acp_agent: cannot start '") + argv.front() + "': " + e.what();
        return {};
    }

    // Read the child's stdout, write its stdin. On stop: polite EOF on its
    // stdin, a short grace for a cooperative exit, then kill it so its stdout
    // closes and the reader wakes.
    auto ch = rpc::stream_channel(holder->child->out(), holder->child->in());
    ch.interrupt = [child = holder->child.get()] {
        using namespace std::chrono_literals;
        child->close_stdin();
        for (int i = 0; i < 100 && child->alive(); ++i) std::this_thread::sleep_for(10ms);
        child->terminate();
        child->interrupt_output();
    };

    SpawnedAcpAgent out;
    out.process    = holder;
    out.connection = std::make_unique<rpc::Peer>(std::move(ch), std::move(dispatch), "acp-agent");
    out.connection->start();

    // Negotiate. A handshake failure (incompatible version, agent died on
    // startup, malformed reply) is a spawn error, never a half-open peer.
    auto r = out.connection->call<acp::to_agent::Initialize>(init);
    if (r) {
        try { (void)acp::negotiate_version(acp::kProtocolVersion, r->protocolVersion); }
        catch (const std::exception& e) { r = std::unexpected(acp::RpcError(acp::errc::InternalError, e.what())); }
    }
    if (!r) {
        err = std::string("spawn_acp_agent: initialize failed: ") + r.error().what();
        return {};   // `out` tears the peer and the child down
    }
    return out;
}

} // namespace agentty::provider
