#include "agentty/provider/codex_cli/provider.hpp"
#include "agentty/provider/codex_cli/responses.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#  include <fcntl.h>
#  include <poll.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

#include <nlohmann/json.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty::provider::codex_cli {
namespace {
using json = nlohmann::json;

// ── Cross-platform bidirectional child process ─────────────────────────────
//
// The Codex app-server is a long-lived JSON-RPC peer: a thread must survive
// across agentty turns so Codex keeps its own compaction and tool state. We
// therefore need a persistent child with a writable stdin and a readable
// stdout — which the one-shot tools::util::Subprocess helper deliberately
// doesn't offer (it runs to completion). This small struct is the only piece
// of platform code in the provider; everything above it is portable.
//
// Both platforms expose the same three operations: spawn(), write_all(),
// read_some() (bounded, so cancellation is responsive), and terminate().
struct ChildProcess {
#if defined(_WIN32)
    HANDLE proc   = nullptr;
    HANDLE to_child   = INVALID_HANDLE_VALUE;   // our write end → child stdin
    HANDLE from_child = INVALID_HANDLE_VALUE;   // our read  end ← child stdout
#else
    ::pid_t pid = -1;
    int to_child   = -1;
    int from_child = -1;
#endif
    bool alive = false;

    // Spawns `codex app-server` with stdin/stdout wired to pipes. Returns
    // false and sets `error` on failure (e.g. `codex` not on PATH).
    bool spawn(std::string& error) {
        if (alive) return true;
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE in_read = nullptr, in_write = nullptr;
        HANDLE out_read = nullptr, out_write = nullptr;
        if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
            error = "cannot create Codex app-server stdin pipe";
            return false;
        }
        if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
            CloseHandle(in_read); CloseHandle(in_write);
            error = "cannot create Codex app-server stdout pipe";
            return false;
        }
        // The child inherits only its own ends; keep ours private so the
        // pipes actually close on child exit (no phantom writer/reader).
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = in_read;
        si.hStdOutput = out_write;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);   // let Codex log freely

        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"codex app-server";
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');

        const BOOL ok = CreateProcessW(
            nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

        // We're done with the child's ends regardless of outcome.
        CloseHandle(in_read);
        CloseHandle(out_write);
        if (!ok) {
            CloseHandle(in_write); CloseHandle(out_read);
            error = "cannot start Codex app-server (is `codex` on PATH?)";
            return false;
        }
        CloseHandle(pi.hThread);
        proc       = pi.hProcess;
        to_child   = in_write;
        from_child = out_read;
        alive      = true;
        return true;
#else
        int to_pipe[2]{-1, -1};
        int from_pipe[2]{-1, -1};
        if (::pipe(to_pipe) != 0) {
            error = std::string{"cannot create Codex app-server pipe: "}
                  + std::strerror(errno);
            return false;
        }
        if (::pipe(from_pipe) != 0) {
            error = std::string{"cannot create Codex app-server pipe: "}
                  + std::strerror(errno);
            ::close(to_pipe[0]); ::close(to_pipe[1]);
            return false;
        }

        posix_spawn_file_actions_t fa;
        if (::posix_spawn_file_actions_init(&fa) != 0) {
            error = "cannot init Codex app-server spawn actions";
            ::close(to_pipe[0]);  ::close(to_pipe[1]);
            ::close(from_pipe[0]); ::close(from_pipe[1]);
            return false;
        }
        // Child: stdin ← to_pipe[0], stdout → from_pipe[1]. Its inherited
        // copies of our ends (to_pipe[1], from_pipe[0]) are closed so the
        // pipes report EOF/EPIPE cleanly when either side exits. stderr is
        // inherited so Codex can log to the host terminal.
        ::posix_spawn_file_actions_adddup2(&fa, to_pipe[0], STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&fa, from_pipe[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&fa, to_pipe[1]);
        ::posix_spawn_file_actions_addclose(&fa, from_pipe[0]);
        ::posix_spawn_file_actions_addclose(&fa, to_pipe[0]);
        ::posix_spawn_file_actions_addclose(&fa, from_pipe[1]);

        const char* argv[] = {"codex", "app-server", nullptr};
        ::pid_t child = -1;
        const int rc = ::posix_spawnp(&child, "codex", &fa, nullptr,
                                      const_cast<char* const*>(argv), environ);
        ::posix_spawn_file_actions_destroy(&fa);
        ::close(to_pipe[0]);
        ::close(from_pipe[1]);
        if (rc != 0) {
            ::close(to_pipe[1]); ::close(from_pipe[0]);
            error = std::string{"cannot start Codex app-server (is `codex` on "
                                "PATH?): "} + std::strerror(rc);
            return false;
        }
        pid        = child;
        to_child   = to_pipe[1];
        from_child = from_pipe[0];
        // Non-blocking reads so the bounded poll loop stays responsive.
        ::fcntl(from_child, F_SETFL, ::fcntl(from_child, F_GETFL, 0) | O_NONBLOCK);
        alive = true;
        return true;
#endif
    }

    bool write_all(std::string_view bytes, std::string& error) {
#if defined(_WIN32)
        std::size_t off = 0;
        while (off < bytes.size()) {
            DWORD wrote = 0;
            if (!WriteFile(to_child, bytes.data() + off,
                           static_cast<DWORD>(bytes.size() - off), &wrote, nullptr)
                || wrote == 0) {
                error = "Codex app-server stdin write failed";
                return false;
            }
            off += wrote;
        }
        return true;
#else
        std::size_t off = 0;
        while (off < bytes.size()) {
            const auto n = ::write(to_child, bytes.data() + off, bytes.size() - off);
            if (n > 0) { off += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pfd{.fd = to_child, .events = POLLOUT, .revents = 0};
                ::poll(&pfd, 1, 100);
                continue;
            }
            error = std::string{"Codex app-server stdin write failed: "}
                  + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    // Blocks up to `timeout_ms` for readable bytes, appending them to `buf`.
    // Returns: >0 bytes read, 0 timed out (nothing yet), -1 EOF/error.
    int read_some(std::string& buf, int timeout_ms, std::string& error) {
        std::array<char, 8192> tmp{};
#if defined(_WIN32)
        // Peek so we never block past the timeout; poll in short slices.
        int waited = 0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(from_child, nullptr, 0, nullptr, &avail, nullptr)) {
                error = "Codex app-server closed its event stream";
                return -1;
            }
            if (avail > 0) {
                DWORD got = 0;
                const DWORD want =
                    static_cast<DWORD>(avail < tmp.size() ? avail : tmp.size());
                if (!ReadFile(from_child, tmp.data(), want, &got, nullptr) || got == 0) {
                    error = "Codex app-server closed its event stream";
                    return -1;
                }
                buf.append(tmp.data(), got);
                return static_cast<int>(got);
            }
            if (waited >= timeout_ms) return 0;
            const int slice = timeout_ms - waited < 20 ? timeout_ms - waited : 20;
            Sleep(static_cast<DWORD>(slice));
            waited += slice;
        }
#else
        pollfd pfd{.fd = from_child, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready == 0) return 0;
        if (ready < 0) {
            if (errno == EINTR) return 0;
            error = std::string{"Codex app-server poll failed: "}
                  + std::strerror(errno);
            return -1;
        }
        const auto n = ::read(from_child, tmp.data(), tmp.size());
        if (n > 0) { buf.append(tmp.data(), static_cast<std::size_t>(n)); return static_cast<int>(n); }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        error = n == 0 ? "Codex app-server closed its event stream"
                       : std::string{"Codex app-server read failed: "}
                             + std::strerror(errno);
        return -1;
#endif
    }

    void terminate() noexcept {
        if (!alive) return;
#if defined(_WIN32)
        if (to_child   != INVALID_HANDLE_VALUE) CloseHandle(to_child);
        if (from_child != INVALID_HANDLE_VALUE) CloseHandle(from_child);
        to_child = from_child = INVALID_HANDLE_VALUE;
        if (proc) {
            // Graceful drain first; Codex flushes running turns on the first
            // terminate signal, so give it a beat before hard-killing.
            if (WaitForSingleObject(proc, 1500) == WAIT_TIMEOUT)
                TerminateProcess(proc, 0);
            CloseHandle(proc);
            proc = nullptr;
        }
#else
        if (to_child   >= 0) ::close(to_child);
        if (from_child >= 0) ::close(from_child);
        to_child = from_child = -1;
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            // Reap without hanging forever if the child ignores SIGTERM.
            for (int i = 0; i < 30; ++i) {
                int status = 0;
                const auto r = ::waitpid(pid, &status, WNOHANG);
                if (r == pid || r < 0) break;
                struct timespec ts{0, 50'000'000};   // 50 ms
                ::nanosleep(&ts, nullptr);
                if (i == 20) ::kill(pid, SIGKILL);
            }
            pid = -1;
        }
#endif
        alive = false;
    }
};

// ── Transcript → Codex thread bootstrap ────────────────────────────────────
//
// A Codex thread is server-side memory. The first time we open one for an
// agentty conversation we replay the full transcript so Codex has real
// context; afterward the thread retains everything and we send only the new
// turn. We flatten prior messages into one priming block (Codex's turn input
// is user-role text) rather than re-driving each turn through the model — that
// gives the model the history verbatim without paying for a re-run of every
// past turn.
std::string flatten_history(const provider::Request& req) {
    std::string out;
    // Everything except the final user turn (that goes in as the live prompt).
    std::size_t last_user = req.messages.size();
    for (std::size_t i = req.messages.size(); i-- > 0;) {
        if (req.messages[i].role == Role::User) { last_user = i; break; }
    }
    for (std::size_t i = 0; i < req.messages.size(); ++i) {
        if (i == last_user) continue;
        const auto& m = req.messages[i];
        std::string text = m.attachments.empty()
                               ? m.text
                               : attachment::expand(m.text, m.attachments);
        if (text.empty()) continue;
        std::string_view who = m.role == Role::Assistant ? "Assistant"
                             : m.role == Role::System    ? "System"
                                                         : "User";
        out += "### ";
        out += who;
        out += "\n";
        out += text;
        out += "\n\n";
    }
    return out;
}

std::string latest_user_input(const provider::Request& req) {
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role != Role::User) continue;
        return it->attachments.empty()
                   ? it->text
                   : attachment::expand(it->text, it->attachments);
    }
    return {};
}

// ── Item rendering: Codex's server-side actions → legible markdown ─────────
//
// Codex runs tools itself; we render each completed item as a compact block so
// the transcript reads like a real Codex session. These map the app-server's
// item shapes (commandExecution / fileChange / mcpToolCall / webSearch) into
// text emitted as StreamTextDelta.
std::string fence(std::string_view lang, std::string_view body) {
    std::string out = "\n```";
    out += lang;
    out += "\n";
    out += body;
    if (!body.empty() && body.back() != '\n') out += "\n";
    out += "```\n";
    return out;
}

std::string render_completed_item(const json& item) {
    const auto type = item.value("type", std::string{});

    if (type == "commandExecution") {
        std::string out = "\n**$ ";
        out += item.value("command", std::string{"command"});
        out += "**\n";
        const auto output = item.value("aggregatedOutput",
                                       item.value("output", std::string{}));
        const auto exit_code = item.value("exitCode", 0);
        if (!output.empty()) out += fence("", output);
        if (exit_code != 0)
            out += "_exit " + std::to_string(exit_code) + "_\n";
        return out;
    }

    if (type == "fileChange") {
        std::string out = "\n**File change";
        const auto& changes = item.value("changes", json::array());
        if (changes.is_array() && !changes.empty()) {
            out += "** ";
            bool first = true;
            for (const auto& c : changes) {
                if (!first) out += ", ";
                first = false;
                out += "`" + c.value("path", std::string{"?"}) + "`";
                const auto kind = c.value("kind", std::string{});
                if (!kind.empty()) out += " (" + kind + ")";
            }
            out += "\n";
            for (const auto& c : changes) {
                const auto diff = c.value("diff", std::string{});
                if (!diff.empty()) out += fence("diff", diff);
            }
        } else {
            out += "**\n";
            const auto diff = item.value("diff", std::string{});
            if (!diff.empty()) out += fence("diff", diff);
        }
        return out;
    }

    if (type == "mcpToolCall") {
        std::string out = "\n**MCP · ";
        out += item.value("server", std::string{"server"});
        out += "/";
        out += item.value("tool", std::string{"tool"});
        out += "**\n";
        if (item.contains("result")) {
            const auto& r = item["result"];
            out += fence("json", r.is_string() ? r.get<std::string>() : r.dump(2));
        }
        return out;
    }

    if (type == "webSearch") {
        std::string out = "\n**🔍 Web search:** ";
        out += item.value("query", std::string{});
        out += "\n";
        return out;
    }

    // agentMessage / reasoning are handled via their delta streams; any other
    // item type gets a minimal, honest marker rather than being dropped.
    return {};
}

} // namespace

// A small, protocol-local JSON-RPC client over the child's stdio.
//
// The Codex app-server speaks newline-delimited JSON; per the app-server
// spec the `"jsonrpc": "2.0"` envelope field is omitted on the wire. All
// calls are serialised by `mutex` — agentty runs one active turn per
// conversation, and serialising also makes a mid-stream provider switch safe.
struct CodexCliProvider::Impl {
    ChildProcess child;
    int next_id = 1;
    bool initialized = false;
    std::string read_buffer;
    // session_key → { threadId, primed } so a thread is opened once and
    // history is replayed only on its first turn.
    struct Thread { std::string id; bool primed = false; };
    std::unordered_map<std::string, Thread> threads;
    std::mutex mutex;

    ~Impl() { child.terminate(); }

    void reset() noexcept {
        child.terminate();
        initialized = false;
        threads.clear();
        read_buffer.clear();
    }

    bool write_message(const json& message, std::string& error) {
        return child.write_all(message.dump() + "\n", error);
    }

    // Returns one complete JSON-RPC message. The bounded poll lets a UI cancel
    // interrupt a silent provider without waiting on a network watchdog. A
    // line that fails to parse is surfaced as an error rather than silently
    // skipped, so a wedged/mis-speaking server can't masquerade as "slow".
    std::optional<json> read_message(const http::CancelTokenPtr& cancel,
                                     std::string& error) {
        for (;;) {
            if (const auto nl = read_buffer.find('\n'); nl != std::string::npos) {
                std::string line = read_buffer.substr(0, nl);
                read_buffer.erase(0, nl + 1);
                if (line.empty() || line == "\r") continue;
                try {
                    return json::parse(line);
                } catch (const std::exception& ex) {
                    error = std::string{"Codex app-server sent malformed JSON: "}
                          + ex.what();
                    return std::nullopt;
                }
            }
            if (cancel && cancel->is_cancelled()) return std::nullopt;
            const int rc = child.read_some(read_buffer, 100, error);
            if (rc < 0) return std::nullopt;
            // rc == 0 → timed out with no data; loop and re-check cancel.
        }
    }

    // Fire a request and pump the stream until its matching response arrives.
    // Server-initiated requests seen in the meantime are auto-declined (we run
    // with approvalPolicy=never, so approvals shouldn't fire, but a stray one
    // must never wedge the loop).
    std::optional<json> request(std::string_view method, json params,
                                const http::CancelTokenPtr& cancel,
                                std::string& error) {
        const int id = next_id++;
        if (!write_message(json{{"id", id}, {"method", method},
                                {"params", std::move(params)}}, error))
            return std::nullopt;
        while (auto message = read_message(cancel, error)) {
            if (message->value("id", -1) == id) {
                if (message->contains("error")) {
                    error = (*message)["error"].value(
                        "message", "Codex app-server request failed");
                    return std::nullopt;
                }
                return message->value("result", json::object());
            }
            decline_if_server_request(*message);
        }
        return std::nullopt;
    }

    // Server → client approval/permission requests: decline politely so the
    // turn continues under agentty's own outer permission profile.
    void decline_if_server_request(const json& message) {
        if (!message.contains("id") || !message.contains("method")) return;
        std::string ignored;
        (void)write_message(
            json{{"id", message["id"]}, {"result", {{"decision", "denied"}}}},
            ignored);
    }

    bool initialise(const http::CancelTokenPtr& cancel, std::string& error) {
        if (initialized) return true;
        auto response = request("initialize",
            json{{"clientInfo", {{"name", "agentty"}, {"version", "1"}}},
                 {"capabilities", json::object()}},
            cancel, error);
        if (!response) return false;
        // Handshake completion: the app-server expects the `initialized`
        // notification before it will accept `thread/start`.
        std::string ignored;
        (void)write_message(json{{"method", "initialized"},
                                 {"params", json::object()}}, ignored);
        initialized = true;
        return true;
    }

    // Resolves (opening if needed) the Codex thread for this conversation and
    // reports whether it still needs history priming.
    Thread* thread_for(const provider::Request& req,
                       const http::CancelTokenPtr& cancel, std::string& error) {
        const auto key = req.session_key.empty() ? std::string{"default"}
                                                 : req.session_key;
        if (const auto it = threads.find(key); it != threads.end())
            return &it->second;

        json sandbox{{"type", "workspaceWrite"}};
        json params{
            {"cwd", tools::util::workspace_root().string()},
            {"sandbox", std::move(sandbox)},
            // agentty owns the outer permission profile; keep Codex sandboxed
            // and never auto-approve destructive actions server-side.
            {"approvalPolicy", "never"},
            {"instructions",
             "You are the coding agent inside agentty. Work in the configured "
             "workspace and finish with a concise, direct answer."},
        };
        if (!req.model.empty() && req.model != "codex-cli-default")
            params["model"] = req.model;

        auto response = request("thread/start", std::move(params), cancel, error);
        if (!response) return nullptr;
        // Accept either {thread:{id}} or a flat {threadId} shape across CLI
        // versions.
        std::string id = response->value("thread", json::object())
                             .value("id", std::string{});
        if (id.empty()) id = response->value("threadId", std::string{});
        if (id.empty()) {
            error = "Codex app-server returned a thread without an id";
            return nullptr;
        }
        auto [it, _] = threads.emplace(key, Thread{std::move(id), false});
        return &it->second;
    }
};

CodexCliProvider::CodexCliProvider() : impl_(std::make_unique<Impl>()) {}
CodexCliProvider::~CodexCliProvider() = default;

void CodexCliProvider::stream(provider::Request req, provider::EventSink sink) {
    // Preferred path: a native ChatGPT OAuth login (agentty login → ChatGPT).
    // Talk to the Responses backend directly — no `codex` binary, real tool
    // cards, auto-refreshing token. This is the "works like Claude" path.
    if (responses_available()) {
        stream_responses(std::move(req), std::move(sink));
        return;
    }

    // Fallback: drive the locally installed `codex app-server` over JSON-RPC
    // (uses the CLI's own ~/.codex login). Kept so users who prefer the CLI
    // login, or who haven't run `agentty login`, still work.
    sink(StreamStarted{});
    std::unique_lock lock{impl_->mutex};

    std::string error;
    if (!impl_->child.spawn(error) || !impl_->initialise(req.cancel, error)) {
        impl_->reset();
        sink(StreamError{error
            + ". Install Codex (`npm i -g @openai/codex`) and run `codex "
              "login`, then retry."});
        return;
    }

    auto* thread = impl_->thread_for(req, req.cancel, error);
    if (!thread) { sink(StreamError{error}); return; }

    // Build the live prompt. On the first turn of a fresh thread, prime it
    // with the full prior transcript so Codex has real conversation context.
    std::string prompt = latest_user_input(req);
    if (prompt.empty()) {
        sink(StreamError{"Codex received an empty user message"});
        return;
    }
    if (!thread->primed) {
        if (std::string history = flatten_history(req); !history.empty()) {
            prompt = "<conversation-so-far>\n" + history
                   + "</conversation-so-far>\n\nCurrent request:\n" + prompt;
        }
        thread->primed = true;
    }

    const int request_id = impl_->next_id++;
    json params{
        {"threadId", thread->id},
        {"input", json::array({json{{"type", "text"}, {"text", prompt}}})},
    };
    if (!req.effort.empty()) params["effort"] = req.effort;
    if (!req.model.empty() && req.model != "codex-cli-default")
        params["model"] = req.model;

    if (!impl_->write_message(json{{"id", request_id}, {"method", "turn/start"},
                                   {"params", std::move(params)}}, error)) {
        sink(StreamError{error});
        return;
    }

    bool completed = false;
    std::string turn_id;
    bool any_text = false;
    // Codex sends agentMessage deltas AND completed-item blocks; if the last
    // visible thing before a block was streamed text, separate them with a
    // blank line so the transcript stays readable.
    auto emit_block = [&](std::string block) {
        if (block.empty()) return;
        sink(StreamTextDelta{std::move(block)});
        any_text = true;
    };

    while (auto message = impl_->read_message(req.cancel, error)) {
        // Response to our turn/start.
        if (message->value("id", -1) == request_id) {
            if (message->contains("error")) {
                error = (*message)["error"].value("message",
                                                  "Codex could not start the turn");
                break;
            }
            turn_id = message->value("result", json::object())
                          .value("turn", json::object())
                          .value("id", std::string{});
            continue;
        }

        const auto method = message->value("method", std::string{});
        const auto& p = message->value("params", json::object());

        if (method == "item/agentMessage/delta"
            || method == "item/agentMessage/textDelta") {
            const auto delta = p.value("delta", p.value("text", std::string{}));
            if (!delta.empty()) { sink(StreamTextDelta{delta}); any_text = true; }
        } else if (method == "item/agentReasoning/delta"
                   || method == "item/reasoning/delta") {
            // Reasoning streams as a thinking delta — same channel Anthropic's
            // adaptive-thinking uses, so it renders as live reasoning, not body
            // text, and is never persisted into the wire prompt.
            const auto delta = p.value("delta", p.value("text", std::string{}));
            if (!delta.empty()) sink(StreamThinkingDelta{delta, {}});
        } else if (method == "item/completed") {
            emit_block(render_completed_item(p.value("item", json::object())));
        } else if (method == "turn/completed") {
            const auto turn = p.value("turn", json::object());
            if (turn.value("status", std::string{"completed"}) != "completed") {
                error = turn.value("error", json::object())
                            .value("message", "Codex turn failed");
            } else {
                completed = true;
            }
            break;
        } else {
            // Any server-initiated request (approvals under an unexpected
            // policy, etc.) is declined so the turn can't hang.
            impl_->decline_if_server_request(*message);
        }
    }

    if (req.cancel && req.cancel->is_cancelled()) {
        if (!turn_id.empty()) {
            std::string ignored;
            (void)impl_->write_message(
                json{{"id", impl_->next_id++}, {"method", "turn/interrupt"},
                     {"params", {{"threadId", thread->id}, {"turnId", turn_id}}}},
                ignored);
        }
        sink(StreamError{"cancelled"});
        return;
    }

    if (!completed) {
        sink(StreamError{error.empty() ? "Codex turn ended unexpectedly" : error});
        return;
    }
    (void)any_text;
    sink(StreamFinished{StopReason::EndTurn});
}

// ── Model discovery ────────────────────────────────────────────────────────
//
// Probe the installed CLI's app-server for the real model list. This spins up
// a throwaway child (model discovery is rare and the persistent thread state
// isn't needed here), and degrades to a single default when the CLI predates
// `model/list` or the probe fails — selection always has at least one entry.
std::vector<ModelInfo> list_models() {
    const ModelInfo fallback{ModelId{"codex-cli-default"}, "Codex CLI default",
                             "codex-cli", 200000, false, true};

    // Signed in with ChatGPT OAuth → the direct Responses transport is used,
    // so advertise the real Codex model line-up (no app-server probe needed).
    if (responses_available()) {
        return {
            {ModelId{"gpt-5-codex"},      "GPT-5 Codex",      "codex-cli", 272000, false, true},
            {ModelId{"gpt-5.1-codex"},    "GPT-5.1 Codex",    "codex-cli", 272000, false, true},
            {ModelId{"gpt-5.1-codex-mini"}, "GPT-5.1 Codex mini", "codex-cli", 272000, false, true},
            {ModelId{"gpt-5"},           "GPT-5",            "codex-cli", 272000, false, true},
        };
    }

    ChildProcess child;
    std::string error;
    if (!child.spawn(error)) return {fallback};

    int next_id = 1;
    std::string buf;
    auto send = [&](const json& m) {
        return child.write_all(m.dump() + "\n", error);
    };
    // Bounded read of one matching response; model discovery must never hang
    // startup, so cap total wait at ~4s.
    auto read_response = [&](int want_id) -> std::optional<json> {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        for (;;) {
            if (const auto nl = buf.find('\n'); nl != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty() || line == "\r") continue;
                json m;
                try { m = json::parse(line); } catch (...) { continue; }
                if (m.value("id", -1) == want_id) return m;
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::string err;
            if (child.read_some(buf, 100, err) < 0) return std::nullopt;
        }
    };

    const int init_id = next_id++;
    if (!send(json{{"id", init_id},
                   {"method", "initialize"},
                   {"params", {{"clientInfo", {{"name", "agentty"}, {"version", "1"}}},
                               {"capabilities", json::object()}}}})) {
        child.terminate();
        return {fallback};
    }
    if (!read_response(init_id)) { child.terminate(); return {fallback}; }
    (void)send(json{{"method", "initialized"}, {"params", json::object()}});

    const int list_id = next_id++;
    if (!send(json{{"id", list_id}, {"method", "model/list"},
                   {"params", json::object()}})) {
        child.terminate();
        return {fallback};
    }
    auto resp = read_response(list_id);
    child.terminate();
    if (!resp || resp->contains("error")) return {fallback};

    // Accept {result:{models:[...]}} or {result:[...]} across versions.
    const auto& result = (*resp)["result"];
    const json models = result.is_array()      ? result
                      : result.contains("models") ? result["models"]
                                                  : json::array();
    std::vector<ModelInfo> out;
    std::unordered_set<std::string> seen;
    for (const auto& m : models) {
        std::string id = m.is_string() ? m.get<std::string>()
                                       : m.value("id", m.value("name", std::string{}));
        if (id.empty() || !seen.insert(id).second) continue;
        std::string label = m.is_object() ? m.value("displayName", id) : id;
        const int ctx = m.is_object() ? m.value("contextWindow", 200000) : 200000;
        out.push_back(ModelInfo{ModelId{id}, std::move(label), "codex-cli",
                                ctx > 0 ? ctx : 200000, false, true});
    }
    if (out.empty()) return {fallback};
    return out;
}

} // namespace agentty::provider::codex_cli
