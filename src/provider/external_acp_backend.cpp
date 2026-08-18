// SPDX-License-Identifier: same as project.
//
// ExternalAcpBackend — see include/agentty/provider/external_acp_backend.hpp.

#include "agentty/provider/external_acp_backend.hpp"

#include <chrono>
#include <algorithm>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include <acp/content.hpp>       // acp::TextContent, ContentBlock
#include <acp/methods.hpp>       // params/results
#include <acp/stdio.hpp>         // acp::StdioTransport
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
                               return !image.bytes.empty();
                           });
    };

    std::size_t user_index = req.messages.size();
    for (std::size_t i = req.messages.size(); i-- > 0;) {
        const auto& m = req.messages[i];
        if (m.role == agentty::Role::User && !m.proactive_context && !m.fork_note
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
            if (image.bytes.empty()) continue;
            acp::ImageContent block;
            block.data = agentty::util::base64_encode(image.bytes);
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
            if (m.role == agentty::Role::User && m.proactive_context)
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
    : opts_(std::move(opts)) {}

void ExternalAcpBackend::connect(acp::AgentConnection& conn) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    conn_ = &conn;
}

std::string ExternalAcpBackend::session_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_ ? session_->value : std::string{};
}

// ── Inbound callback wiring ──────────────────────────────────────────────────
// Every handler routes through a member so it can reach `active_sink_` / the
// delegate. These fire on the transport's reader thread; `active_sink_` is
// published for the life of the in-flight round and null otherwise (a late
// update after settle is dropped, not crashed).
acp::ClientHandlers ExternalAcpBackend::make_handlers() {
    acp::ClientHandlers h;

    // session/update → normalized SessionUpdate to the round's sink.
    h.on_session_update = [this](const acp::SessionUpdateMsg& m) {
        std::lock_guard<std::mutex> lk(sink_mu_);
        if (active_sink_) active_sink_(m.update);
    };

    // request_permission → delegate decides; default allow. We answer with the
    // agent's own first "allow-once" option when allowing, else Cancelled.
    h.on_request_permission =
        [this](const acp::RequestPermissionParams& p) -> acp::RequestPermissionResult {
        bool allow = true;
        if (opts_.delegate.request_permission)
            allow = opts_.delegate.request_permission(p);

        acp::RequestPermissionResult res;
        if (allow && !p.options.empty()) {
            acp::PO_Selected sel;
            sel.optionId = p.options.front().optionId;
            res.outcome  = sel;
        } else if (allow) {
            // No options offered but we allow — a bare selected with no id is
            // invalid, so treat as cancelled (nothing to select).
            res.outcome = acp::PO_Cancelled{};
        } else {
            res.outcome = acp::PO_Cancelled{};
        }
        return res;
    };

    // fs/read_text_file → delegate (unset ⇒ decline by returning empty; the
    // agent sees a well-formed empty read rather than a protocol error).
    if (opts_.delegate.read_text_file) {
        h.on_fs_read_text_file =
            [this](const acp::ReadTextFileParams& p) -> acp::ReadTextFileResult {
            std::optional<int> line  = p.line  ? std::optional<int>(static_cast<int>(*p.line))  : std::nullopt;
            std::optional<int> limit = p.limit ? std::optional<int>(static_cast<int>(*p.limit)) : std::nullopt;
            acp::ReadTextFileResult res;
            if (auto body = opts_.delegate.read_text_file(p.path, line, limit))
                res.content = std::move(*body);
            return res;
        };
    }

    // fs/write_text_file → delegate.
    if (opts_.delegate.write_text_file) {
        h.on_fs_write_text_file =
            [this](const acp::WriteTextFileParams& p) -> acp::Unit {
            opts_.delegate.write_text_file(p.path, p.content);
            return {};
        };
    }

    // terminal/* → synchronous executor. create() runs the command to
    // completion via the delegate and caches the result under a fresh id; the
    // follow-up output/wait_for_exit read it back; release() drops it. Only
    // installed when the delegate can run terminals (else the agent is told the
    // methods are unsupported and won't use them).
    if (opts_.delegate.run_terminal) {
        h.on_terminal_create =
            [this](const acp::CreateTerminalParams& p) -> acp::CreateTerminalResult {
            std::optional<std::string> cwd = p.cwd ? std::optional<std::string>(*p.cwd)
                                                   : std::nullopt;
            auto r = opts_.delegate.run_terminal(p.command, p.args, cwd);

            std::string id;
            {
                std::lock_guard<std::mutex> lk(term_mu_);
                id = "term_" + std::to_string(next_terminal_id_++);
                terminals_[id] = TerminalState{std::move(r.output), r.exit_code, r.truncated};
            }
            acp::CreateTerminalResult res;
            res.terminalId = std::move(id);
            return res;
        };

        h.on_terminal_output =
            [this](const acp::TerminalOutputParams& p) -> acp::TerminalOutputResult {
            acp::TerminalOutputResult res;
            std::lock_guard<std::mutex> lk(term_mu_);
            auto it = terminals_.find(p.terminalId);
            if (it != terminals_.end()) {
                res.output    = it->second.output;
                res.truncated = it->second.truncated;
                acp::TerminalExitStatus st;
                st.exitCode = it->second.exit_code;
                res.exitStatus = st;   // command already ran to completion
            }
            return res;
        };

        h.on_terminal_wait_for_exit =
            [this](const acp::TerminalWaitForExitParams& p) -> acp::TerminalWaitForExitResult {
            acp::TerminalExitStatus st;
            std::lock_guard<std::mutex> lk(term_mu_);
            auto it = terminals_.find(p.terminalId);
            if (it != terminals_.end()) st.exitCode = it->second.exit_code;
            return st;   // already exited (synchronous executor)
        };

        h.on_terminal_kill =
            [](const acp::TerminalKillParams&) -> acp::Unit {
            // The command already ran synchronously; nothing to signal.
            return {};
        };

        h.on_terminal_release =
            [this](const acp::TerminalReleaseParams& p) -> acp::Unit {
            std::lock_guard<std::mutex> lk(term_mu_);
            terminals_.erase(p.terminalId);
            return {};
        };
    }

    return h;
}

// ── Session lifecycle ────────────────────────────────────────────────────────
std::optional<acp::SessionId>
ExternalAcpBackend::ensure_session_(const Request& req, std::optional<TurnError>& err) {
    (void)req;
    std::lock_guard<std::mutex> lk(mu_);
    if (opts_.reuse_session && session_) return session_;
    if (conn_ == nullptr) {
        err = TurnError{"ExternalAcpBackend: connect() not called (no connection bound)"};
        return std::nullopt;
    }

    acp::NewSessionParams np;
    np.cwd = opts_.cwd;
    np.mcpServers = opts_.mcp_servers;

    try {
        auto fut = conn_->session_new(np);
        acp::NewSessionResult r = fut.get();
        if (opts_.reuse_session) session_ = r.sessionId;
        return r.sessionId;
    } catch (const std::exception& e) {
        err = TurnError{std::string("session/new failed: ") + e.what()};
        return std::nullopt;
    }
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
        std::lock_guard<std::mutex> lk(sink_mu_);
        active_sink_ = [&sink](acp::SessionUpdate su) { sink(std::move(su)); };
    }
    struct SinkGuard {
        ExternalAcpBackend* self;
        ~SinkGuard() {
            std::lock_guard<std::mutex> lk(self->sink_mu_);
            self->active_sink_ = nullptr;
        }
    } sink_guard{this};

    // Fire the prompt.
    acp::PromptParams pp;
    pp.sessionId = *sid;
    pp.prompt    = prompt_blocks_from(req);

    std::future<acp::PromptResult> fut;
    try {
        fut = conn_->session_prompt(pp);
    } catch (const std::exception& e) {
        return TurnResult::failed(TurnError{std::string("session/prompt failed: ") + e.what()});
    }

    // Await completion while honoring cancellation. The reader thread streams
    // session/update to `sink` in the meantime; here we only wait for the
    // terminal PromptResult (or a user Esc → session/cancel).
    //
    // The result future may be std::launch::deferred (advances only on .get()),
    // so we CANNOT poll it with wait_for on this thread and still service
    // cancellation. Instead run .get() on a worker and poll the cancel token
    // here; on Esc we fire session/cancel and let the agent settle the same
    // future with StopReason::Cancelled (or a thrown/aborted future).
    using namespace std::chrono_literals;
    std::promise<void> done;
    std::future<void> done_f = done.get_future();
    acp::PromptResult result{};
    std::exception_ptr result_err;
    std::thread getter([&]{
        try { result = fut.get(); }
        catch (...) { result_err = std::current_exception(); }
        done.set_value();
    });

    // RAII join: whatever happens below — a throw from session_cancel, an
    // exception on the poll path, or a normal return — the getter thread is
    // ALWAYS joined before it destructs. An un-joined std::thread dtor calls
    // std::terminate(); this guard makes that impossible. Declared AFTER
    // `getter` so it destroys (joins) FIRST, before `done`/`result` unwind.
    struct JoinGuard {
        std::thread& t;
        ~JoinGuard() { if (t.joinable()) t.join(); }
    } join_guard{getter};

    bool sent_cancel = false;
    std::optional<std::chrono::steady_clock::time_point> cancel_deadline;
    for (;;) {
        if (!sent_cancel && cancel && cancel->is_cancelled()) {
            acp::CancelParams cp;
            cp.sessionId = *sid;
            // Fire session/cancel; the agent settles the in-flight future with
            // StopReason::Cancelled, waking the getter. If the transport is
            // already torn down this can throw — swallow it: we've recorded the
            // cancel intent, and the getter will wake on the future erroring
            // out (on_transport_closed fails all in-flight requests) regardless.
            try {
                conn_->session_cancel(cp);
            } catch (...) { /* transport gone; getter wakes via future error */ }
            sent_cancel = true;
            // A COOPERATIVE agent settles the future within a beat. A BUGGY one
            // might never settle it — in which case getter's fut.get() (and thus
            // join_guard's join) would block forever. Arm a grace deadline; if
            // it lapses, we force the escape hatch below.
            cancel_deadline = std::chrono::steady_clock::now() + 2s;
        }
        // Escape hatch: the agent didn't settle the cancelled future in time.
        // Fail every in-flight request at the engine (idempotent) — this errors
        // the future the getter is blocked on, so it wakes and the join is
        // bounded no matter how the agent (mis)behaves.
        if (cancel_deadline && std::chrono::steady_clock::now() >= *cancel_deadline) {
            conn_->engine().on_transport_closed("cancel timed out");
            cancel_deadline.reset();
        }
        if (done_f.wait_for(20ms) == std::future_status::ready) break;
    }
    // getter is joined by join_guard on scope exit (and again here is a no-op).

    if (sent_cancel) return TurnResult::cancelled();

    // Terminal event — emitted EXACTLY ONCE, here, as the return value.
    if (result_err) {
        // A thrown future is a transport/protocol failure. If the agent 401'd on
        // its upstream, the message carries it — surface as auth_expired so the
        // OAuth-refresh retry path can re-run. Otherwise a plain round failure.
        std::string msg;
        try { std::rethrow_exception(result_err); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...)                      { msg = "unknown agent error"; }
        TurnError te{std::string("agent error: ") + msg};
        if (msg.find("401") != std::string::npos || msg.find("403") != std::string::npos ||
            msg.find("unauthor") != std::string::npos || msg.find("Unauthor") != std::string::npos)
            te.auth_expired = true;
        return TurnResult::failed(te);
    }

    // Turn-level StopReason → round result.
    switch (result.stopReason) {
        case acp::StopReason::Cancelled:
            return TurnResult::cancelled();
        case acp::StopReason::Refusal:
            return TurnResult::failed(TurnError{"agent refused to continue"});
        default:
            return TurnResult::finished(map_acp_stop_reason(result.stopReason));
    }
}

// ── Subprocess factory ───────────────────────────────────────────────────────
// Spawn `argv[0]` with the remaining args as an ACP agent, wire its stdio to an
// acp::AgentConnection over acp's StdioTransport, run `initialize`, and return
// the connected handle. The child process + transport are parked in a holder
// kept alive by SpawnedAcpAgent::process (a shared_ptr<void>); dropping the
// SpawnedAcpAgent tears the agent down (stops the reader, closes the pipes,
// waits/kills the child) in the holder's destructor.
//
// Portable: ::mcp::cap::ChildProcess is fork/exec/pipe on POSIX and
// CreateProcess/CreatePipe on Windows, exposing the child's stdout as an
// istream (.out()) and its stdin as an ostream (.in()) — exactly what
// StdioTransport consumes. No platform #ifdef leaks into this file.
namespace {

// Owns the spawned child + its transport, in destruction-safe order. The
// AgentConnection (built by the caller) is destroyed FIRST (it holds the
// engine), then we tear this down. We only own child+transport here; the
// connection lives in SpawnedAcpAgent::connection and points back at nothing.
//
// THE HAZARD (why this dtor is not just three resets): the StdioTransport
// reader thread is parked in std::getline on the CHILD'S STDOUT and can only
// wake on EOF. StdioTransport::stop()/~ UNCONDITIONALLY joins that reader with
// no timeout and no way to interrupt getline. EOF on the child's stdout arrives
// only when the CHILD exits. A well-behaved agent exits when it sees EOF on ITS
// stdin (our write end) — but a HUNG or MISBEHAVING agent that ignores stdin
// EOF (or is wedged in a syscall) never closes its stdout, so `transport.reset()`
// would block the reader-join FOREVER, and we'd never reach the `child.reset()`
// that force-kills it. Classic deadlock.
//
// THE FIX makes the join provably bounded: we guarantee the child is DEAD
// before we let the transport join its reader. Killing the child closes its
// stdout → the reader's getline returns EOF → the join is guaranteed to
// complete. Sequence:
//   1. close_stdin()      — polite EOF; a cooperative child starts exiting now.
//   2. bounded grace wait — give a cooperative child a moment to die on its own
//                           (fast, common path: it exits, stdout EOFs).
//   3. force_kill()       — if still alive, SIGTERM/TerminateProcess it, which
//                           closes its stdout unconditionally.
//   4. transport.reset()  — reader-join now cannot hang (stdout is EOF either
//                           way), so this returns promptly.
//   5. child.reset()      — reap (idempotent; already dead).
struct AgentProcessHolder {
    std::unique_ptr<::mcp::cap::ChildProcess> child;
    std::unique_ptr<acp::StdioTransport>    transport;

    ~AgentProcessHolder() {
        using namespace std::chrono_literals;

        // 1. Polite EOF: cooperative agents exit on seeing their stdin close.
        if (child) child->close_stdin();

        // 2. Bounded grace: poll for a cooperative exit for up to ~1s. If the
        //    child dies here (common path), its stdout hits EOF and the reader
        //    is already unblocking — step 4's join will be instant.
        if (child) {
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (child->alive() &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);

            // 3. Reap/terminate while deliberately retaining the parent read
            //    stream object. The transport reader may currently be blocked
            //    inside that stream; destroying it here races the reader. The
            //    ChildProcess::terminate seam kills the child (closing its
            //    stdout) but leaves the stream alive until after reader join.
            child->terminate();
        }

        // 4. Reader-join is now guaranteed bounded: the child is dead (or
        //    exiting), so getline sees EOF and the reader thread finishes.
        transport.reset();

        // 5. Reap (idempotent — shutdown() above may already have done it).
        child.reset();
    }
};

} // namespace

SpawnedAcpAgent spawn_acp_agent(const std::vector<std::string>& argv,
                                const acp::InitializeParams&    init,
                                acp::ClientHandlers             handlers,
                                std::string&                    err) {
    if (argv.empty()) {
        err = "spawn_acp_agent: empty argv";
        return {};
    }

    auto holder = std::make_shared<AgentProcessHolder>();

    // 1. Spawn the child with piped stdio.
    try {
        ::mcp::cap::ChildProcess::Spawn spawn;
        spawn.command = argv.front();
        spawn.args.assign(argv.begin() + 1, argv.end());
        holder->child = std::make_unique<::mcp::cap::ChildProcess>(spawn);
    } catch (const std::exception& e) {
        err = std::string("spawn_acp_agent: cannot start '") + argv.front() + "': " + e.what();
        return {};
    }

    // 2. Bridge the child's stdio to an ACP transport: read from the child's
    //    stdout (.out()), write to the child's stdin (.in()).
    holder->transport =
        std::make_unique<acp::StdioTransport>(holder->child->out(), holder->child->in());

    // 3. Build the client-side connection over the transport's write sink, with
    //    the caller's handlers (session/update → backend, fs/* + terminal/* +
    //    request_permission → delegate). Handlers are ctor-installed, hence the
    //    build-backend-first → make_handlers() → spawn → connect() flow. Then
    //    start the reader thread pumping the child's frames into the engine.
    auto conn = std::make_unique<acp::AgentConnection>(holder->transport->sink(),
                                                       std::move(handlers));
    holder->transport->start(conn->engine());

    // 4. Negotiate. A handshake failure (incompatible version, agent died on
    //    startup, malformed reply) is surfaced as a spawn error so the caller
    //    never gets a half-open connection.
    try {
        (void)conn->initialize(init).get();
    } catch (const std::exception& e) {
        err = std::string("spawn_acp_agent: initialize failed: ") + e.what();
        return {};   // holder (child+transport) tears down here via shared_ptr drop
    }

    SpawnedAcpAgent out;
    out.connection = std::move(conn);
    out.process    = std::move(holder);   // type-erased lifetime owner
    return out;
}

} // namespace agentty::provider
