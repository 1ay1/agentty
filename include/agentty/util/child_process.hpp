// agentty/util/child_process.hpp — a long-lived child process with its
// stdio as std::istream/std::ostream.
//
//   Used to spawn MCP servers and ACP agents. A small streambuf over the raw
//   pipe fds/handles, no libstdc++ extensions, so it builds on libc++ and
//   MSVC too. (Moved from mcp-cpp, which no longer starts processes.)
#pragma once

// A long-lived child process with bidirectional stdio is available on every
// platform agentty targets: POSIX (fork/exec/pipe) and Windows
// (CreateProcess/CreatePipe). Both expose the same ChildProcess interface.
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
#  define AGENTTY_HAVE_CHILD_PROCESS 1
#else
#  define AGENTTY_HAVE_CHILD_PROCESS 0
#endif

#if AGENTTY_HAVE_CHILD_PROCESS

#if defined(_WIN32)
//
// ── Windows backend (CreateProcess + CreatePipe) ────────────────────────────
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace agentty::util {

// Move-only owner of a single Windows pipe HANDLE. Closes on scope exit unless
// release()'d — the CreatePipe/CreateProcess prologue's leak-ladder becomes
// automatic: every pipe end is parked in a HandleGuard the instant CreatePipe
// hands it back, any throw before the commit point unwinds them all, and at the
// commit point the surviving ends are release()'d into the streambufs (which
// own them thereafter). Zero runtime cost: one HANDLE, no vtable, no heap.
class HandleGuard {
public:
    HandleGuard() noexcept = default;
    explicit HandleGuard(HANDLE h) noexcept : h_(h) {}
    HandleGuard(HandleGuard&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    HandleGuard& operator=(HandleGuard&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    HandleGuard(const HandleGuard&)            = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] HANDLE release() noexcept { HANDLE h = h_; h_ = nullptr; return h; }
    void reset() noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) { ::CloseHandle(h_); }
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// A std::streambuf backed by a Windows pipe HANDLE. Replaces libstdc++'s
// __gnu_cxx::stdio_filebuf (which is unavailable on MSVC) so ChildProcess can
// expose std::istream/std::ostream over the child's stdio pipes. Blocking
// ReadFile/WriteFile, single-byte put-back area — the the peer reads
// line-buffered JSON-RPC frames, so a small buffer is fine.
class handle_streambuf final : public std::streambuf {
public:
    explicit handle_streambuf(HANDLE h) : h_(h) {
        setg(in_, in_ + 1, in_ + 1);   // empty get area (force underflow)
        setp(out_, out_ + sizeof(out_));
    }
    ~handle_streambuf() override { sync(); close(); }

    handle_streambuf(const handle_streambuf&)            = delete;
    handle_streambuf& operator=(const handle_streambuf&) = delete;

    void close() noexcept {
        // Wake any reader parked in ReadFile FIRST, then close. Closing a
        // HANDLE out from under a blocked ReadFile is a use-after-free in
        // disguise: the kernel can recycle the handle value for an
        // unrelated object opened by another thread, and the blocked read
        // then completes against THAT object. Cancel, then close.
        interrupt();
        if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr) {
            ::CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    // Wake a thread blocked in underflow() WITHOUT closing the handle under
    // it — the Windows counterpart to the POSIX fd_streambuf wake pipe.
    // CancelIoEx(h, nullptr) cancels I/O issued on this handle by every
    // thread in the process, so the reader's blocking ReadFile returns
    // FALSE/ERROR_OPERATION_ABORTED and underflow() reports EOF. The owner
    // joins that reader before destroying the streambuf.
    void interrupt() noexcept {
        interrupted_.store(true, std::memory_order_release);
        if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr)
            (void)::CancelIoEx(h_, nullptr);
    }

    [[nodiscard]] bool valid() const noexcept {
        return h_ != INVALID_HANDLE_VALUE && h_ != nullptr;
    }

protected:
    // ── reading: fill the 1-byte get area from the pipe ──────────────────
    int_type underflow() override {
        if (!valid() || interrupted_.load(std::memory_order_acquire))
            return traits_type::eof();
        DWORD got = 0;
        // ReadFile blocks until ≥1 byte, the write end closes (→ got==0 /
        // ERROR_BROKEN_PIPE), or interrupt() cancels it (→ FALSE /
        // ERROR_OPERATION_ABORTED). All three surface as EOF — mirroring
        // POSIX read() plus the wake-pipe path.
        if (!::ReadFile(h_, in_, 1, &got, nullptr) || got == 0)
            return traits_type::eof();
        if (interrupted_.load(std::memory_order_acquire))
            return traits_type::eof();
        setg(in_, in_, in_ + 1);
        return traits_type::to_int_type(in_[0]);
    }

    // ── writing: flush the put area to the pipe ──────────────────────────
    int_type overflow(int_type ch) override {
        if (sync() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            char c = traits_type::to_char_type(ch);
            *pptr() = c;
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override {
        std::ptrdiff_t n = pptr() - pbase();
        if (n <= 0) return 0;
        if (!valid()) return -1;
        const char* p = pbase();
        std::ptrdiff_t left = n;
        while (left > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(h_, p, static_cast<DWORD>(left), &wrote, nullptr) ||
                wrote == 0)
                return -1;
            p    += wrote;
            left -= wrote;
        }
        setp(out_, out_ + sizeof(out_));
        return 0;
    }

private:
    HANDLE h_;
    std::atomic<bool> interrupted_{false};
    char   in_[1]{};
    char   out_[4096]{};
};

// A spawned child process whose stdin/stdout are wired to iostreams. stderr is
// inherited from the parent (MCP servers log there). Construction throws
// std::runtime_error on spawn failure. Windows CreateProcess backend; same
// interface as the POSIX version below.
class ChildProcess {
public:
    struct Spawn {
        std::string              command;   // executable (PATH-resolved)
        std::vector<std::string> args;      // NOT including argv[0]
        std::vector<std::string> env_kv;    // extra "KEY=VALUE" entries
        std::string              cwd;        // empty inherits parent cwd
        bool                     merge_stderr = false;
    };

    explicit ChildProcess(const Spawn& s) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;          // pipe ends are inheritable

        // Each pipe end is owned by a HandleGuard the moment CreatePipe yields
        // it, so any throw before the commit point closes every open handle
        // automatically — no hand-rolled CloseHandle ladder to drift out of
        // sync with the open set.
        HANDLE rd = nullptr, wr = nullptr;
        if (!::CreatePipe(&rd, &wr, &sa, 0))
            throw std::runtime_error("mcp::cap: CreatePipe(stdin) failed");
        HandleGuard child_stdin_rd{rd}, child_stdin_wr{wr};
        if (!::CreatePipe(&rd, &wr, &sa, 0))
            throw std::runtime_error("mcp::cap: CreatePipe(stdout) failed");
        HandleGuard child_stdout_rd{rd}, child_stdout_wr{wr};

        // The PARENT ends must NOT be inherited by the child, else they never
        // close and we'd never see EOF.
        ::SetHandleInformation(child_stdin_wr.get(),  HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(child_stdout_rd.get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = child_stdin_rd.get();
        si.hStdOutput = child_stdout_wr.get();
        si.hStdError  = s.merge_stderr ? child_stdout_wr.get()
                                       : ::GetStdHandle(STD_ERROR_HANDLE);

        std::string cmdline = build_command_line_(s);
        std::string env_block;
        const bool have_env = build_env_block_(s, env_block);

        PROCESS_INFORMATION pi{};
        // CREATE_SUSPENDED so the child is assigned to the job BEFORE it can
        // run and spawn grandchildren of its own; CREATE_BREAKAWAY_FROM_JOB
        // is deliberately NOT set, so descendants stay in the job too.
        BOOL ok = ::CreateProcessA(
            /*lpApplicationName=*/nullptr,
            /*lpCommandLine=*/cmdline.data(),
            /*procAttrs=*/nullptr, /*threadAttrs=*/nullptr,
            /*bInheritHandles=*/TRUE,
            /*creationFlags=*/CREATE_SUSPENDED,
            /*lpEnvironment=*/have_env ? env_block.data() : nullptr,
            /*lpCurrentDirectory=*/s.cwd.empty() ? nullptr : s.cwd.c_str(),
            &si, &pi);

        // The child owns its ends now; drop ours regardless of success.
        child_stdin_rd.reset();
        child_stdout_wr.reset();
        if (!ok)
            // child_stdin_wr / child_stdout_rd close here via guard unwind.
            throw std::runtime_error("mcp::cap: CreateProcess('" + s.command +
                                     "') failed (err " +
                                     std::to_string(::GetLastError()) + ")");

        // Put the child (and every descendant it spawns) in a job whose
        // closure kills the tree. Without this, terminating a `cmd.exe /c`
        // launcher leaves the real program orphaned and holding the stdout
        // write end, so the reader thread never sees EOF and process_stop
        // hangs. Best-effort: if any step fails we fall back to
        // TerminateProcess on the launcher alone rather than failing spawn.
        job_ = ::CreateJobObjectA(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job_,
                    JobObjectExtendedLimitInformation, &limits, sizeof(limits))
                || !::AssignProcessToJobObject(job_, pi.hProcess)) {
                ::CloseHandle(job_);
                job_ = nullptr;
            }
        }

        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);
        proc_   = pi.hProcess;
        pid_    = static_cast<int>(pi.dwProcessId);

        // Commit point: release the surviving parent ends into the streambufs,
        // which own the handles thereafter and close them via shutdown().
        in_buf_  = std::make_unique<handle_streambuf>(child_stdout_rd.release());  // child stdout
        out_buf_ = std::make_unique<handle_streambuf>(child_stdin_wr.release());   // child stdin
        in_stream_  = std::make_unique<std::istream>(in_buf_.get());
        out_stream_ = std::make_unique<std::ostream>(out_buf_.get());
    }

    ~ChildProcess() { shutdown(); }

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] std::istream& out() noexcept { return *in_stream_; }   // child stdout
    [[nodiscard]] std::ostream& in()  noexcept { return *out_stream_; }  // child stdin
    [[nodiscard]] int pid() const noexcept { return pid_; }

    [[nodiscard]] bool alive() const noexcept {
        if (!proc_) return false;
        if (::WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT) return true;
        DWORD code = 0;
        if (::GetExitCodeProcess(proc_, &code) && !exit_code_)
            exit_code_ = static_cast<int>(code);
        return false;
    }

    // Exit status of the child once it has exited. nullopt while still running.
    [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }

    // Close ONLY the child's stdin (our write end) → the server sees EOF and
    // begins exiting, which closes its stdout and unblocks a reader parked in
    // ReadFile. Does NOT touch the read stream or reap the process.
    void close_stdin() noexcept {
        // Do NOT flush out_stream_ here — races the reader thread's sink().
        // See POSIX close_stdin() for the full rationale.
        if (out_buf_) out_buf_->close();   // closes child's stdin HANDLE
    }

    void interrupt_output() noexcept {
        // Cancel the blocking read; do NOT close the handle here. The reader
        // thread is still inside ReadFile on it, and closing underneath a
        // blocked read races handle-value recycling. close() happens later,
        // in shutdown(), after the owner has joined the reader.
        if (in_buf_) in_buf_->interrupt();
    }

    // Stop/reap the process but keep the read stream object alive so a
    // concurrent output reader can observe EOF and join safely.
    void terminate() noexcept {
        // Do NOT flush out_stream_ here — races the reader thread's sink().
        // See POSIX terminate() for the full rationale.
        if (out_buf_) out_buf_->close();
        if (proc_) {
            if (::WaitForSingleObject(proc_, 500) == WAIT_TIMEOUT) {
                // Kill the whole tree, not just the launcher. The child is
                // `cmd.exe /c …`, which spawns the real program as a
                // grandchild; TerminateProcess on cmd alone orphans that
                // grandchild, which keeps the stdout write end open forever
                // and wedges the reader in ReadFile. Closing the job (every
                // process is assigned one at spawn, with
                // KILL_ON_JOB_CLOSE) terminates the entire tree atomically.
                if (job_) {
                    ::CloseHandle(job_);   // KILL_ON_JOB_CLOSE fires here
                    job_ = nullptr;
                } else {
                    ::TerminateProcess(proc_, 1);
                }
            }
            ::WaitForSingleObject(proc_, INFINITE);
            DWORD code = 0;
            if (::GetExitCodeProcess(proc_, &code) && !exit_code_)
                exit_code_ = static_cast<int>(code);
            ::CloseHandle(proc_);
            proc_ = nullptr;
            pid_ = -1;
        }
        if (job_) { ::CloseHandle(job_); job_ = nullptr; }
    }

    // Close child stdin (EOF → graceful exit), wait briefly, then terminate.
    // Idempotent; also called by the destructor.
    void shutdown() noexcept {
        terminate();
        in_stream_.reset();
        in_buf_.reset();
    }

private:
    // Quote one argv token per the CommandLineToArgvW rules MSVCRT uses, so a
    // child parsing its command line recovers the exact arguments.
    static std::string quote_arg_(const std::string& a) {
        if (!a.empty() &&
            a.find_first_of(" \t\n\v\"") == std::string::npos)
            return a;                         // no quoting needed
        std::string out = "\"";
        for (auto it = a.begin();; ++it) {
            std::size_t backslashes = 0;
            while (it != a.end() && *it == '\\') { ++it; ++backslashes; }
            if (it == a.end()) {
                out.append(backslashes * 2, '\\');   // escape trailing run
                break;
            } else if (*it == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
            } else {
                out.append(backslashes, '\\');
                out.push_back(*it);
            }
        }
        out.push_back('"');
        return out;
    }

    static std::string build_command_line_(const Spawn& s) {
        std::string cl = quote_arg_(s.command);
        // cmd.exe is NOT an MSVCRT argv parser. When the child is
        // `cmd.exe /d /s /c <payload>`, the payload must be appended with a
        // single pair of plain surrounding quotes and NO backslash escaping
        // — /s tells cmd to strip exactly the outer quotes and execute the
        // remainder verbatim. Running it through quote_arg_ (which escapes
        // embedded quotes as \") made cmd see literal backslashes, which is
        // why every quoted path in a process_start command failed. Detect
        // the cmd /c form and hand its trailing payload through untouched.
        std::size_t i = 0;
        const bool is_cmd_shell = is_cmd_exe_(s.command);
        for (const auto& a : s.args) {
            ++i;
            const bool last = (i == s.args.size());
            if (is_cmd_shell && last && is_slash_c_(s.args, i - 1)) {
                cl += " \"";
                cl += a;          // verbatim — cmd has no escape syntax
                cl += "\"";
                continue;
            }
            cl.push_back(' ');
            cl += quote_arg_(a);
        }
        return cl;
    }

    // Is this executable cmd.exe? Matches "cmd", "cmd.exe", and any path
    // ending in one of those, case-insensitively.
    static bool is_cmd_exe_(const std::string& c) {
        auto slash = c.find_last_of("\\/");
        std::string base = (slash == std::string::npos) ? c : c.substr(slash + 1);
        for (auto& ch : base)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return base == "cmd" || base == "cmd.exe";
    }

    // Is the argument at `payload_idx` preceded by a /c or /k switch? Only
    // then is the trailing token a command line rather than a normal argv
    // token. Switches before it (/d, /s, /q) are ordinary arguments.
    static bool is_slash_c_(const std::vector<std::string>& args,
                            std::size_t payload_idx) {
        if (payload_idx == 0) return false;
        const std::string& prev = args[payload_idx - 1];
        return prev.size() == 2 && (prev[0] == '/' || prev[0] == '-') &&
               (prev[1] == 'c' || prev[1] == 'C' ||
                prev[1] == 'k' || prev[1] == 'K');
    }

    // Build a merged environment block (parent env + s.env_kv overrides) in the
    // double-NUL-terminated form CreateProcess wants. Returns false (and leaves
    // `out` empty) when there are no overrides, so the caller passes nullptr to
    // simply inherit the parent environment.
    static bool build_env_block_(const Spawn& s, std::string& out) {
        if (s.env_kv.empty()) return false;
        // Start from the parent environment.
        std::vector<std::string> entries;
        if (LPCH env = ::GetEnvironmentStringsA()) {
            for (const char* p = env; *p; ) {
                std::string e = p;
                p += e.size() + 1;
                if (!e.empty() && e[0] != '=') entries.push_back(std::move(e));
            }
            ::FreeEnvironmentStringsA(env);
        }
        auto key_of = [](const std::string& kv) {
            auto eq = kv.find('=');
            return eq == std::string::npos ? kv : kv.substr(0, eq);
        };
        auto ci_eq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::toupper((unsigned char)a[i]) !=
                    std::toupper((unsigned char)b[i])) return false;
            return true;
        };
        for (const auto& kv : s.env_kv) {
            if (kv.find('=') == std::string::npos) continue;
            std::string k = key_of(kv);
            for (auto& e : entries)
                if (ci_eq(key_of(e), k)) { e.clear(); break; }  // drop old
            entries.push_back(kv);
        }
        for (const auto& e : entries) {
            if (e.empty()) continue;
            out += e;
            out.push_back('\0');
        }
        out.push_back('\0');   // final terminator
        return true;
    }

    HANDLE proc_ = nullptr;
    HANDLE job_  = nullptr;   // kills the whole child tree when closed
    mutable int    pid_  = -1;
    mutable std::optional<int> exit_code_;
    std::unique_ptr<handle_streambuf> in_buf_;
    std::unique_ptr<handle_streambuf> out_buf_;
    std::unique_ptr<std::istream>     in_stream_;
    std::unique_ptr<std::ostream>     out_stream_;
};

} // namespace agentty::util

#else  // !_WIN32 — POSIX backend: jaal's posix_process

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <maya/runtime.hpp>

namespace agentty::util {

// The child's stdout as a streambuf. Reads wait on a jaal reactor (the pipe
// is non-blocking) so interrupt() can wake a reader parked in underflow()
// without closing the fd under it.
class child_out_buf final : public std::streambuf {
public:
    explicit child_out_buf(int fd) {
        setg(in_, in_ + sizeof(in_), in_ + sizeof(in_));
        auto r = maya::platform::poll_reactor::create();
        if (!r) throw std::runtime_error("child_process: reactor");
        reactor_.emplace(std::move(*r));
        auto reg = reactor_->watch(fd, maya::platform::interest::read, 1);
        if (!reg) throw std::runtime_error("child_process: watch stdout");
        reg_.emplace(std::move(*reg));
        fd_ = fd;
    }
    void interrupt() noexcept {
        interrupted_.with([](bool& v) { v = true; });
        if (reactor_) reactor_->waker().wake();
    }

protected:
    int_type underflow() override {
        for (;;) {
            if (interrupted_.read([](const bool& v) { return v; })) return traits_type::eof();
            const auto n = ::read(fd_, in_, sizeof(in_));
            if (n > 0) { setg(in_, in_, in_ + n); return traits_type::to_int_type(in_[0]); }
            if (n == 0) return traits_type::eof();
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return traits_type::eof();
            if (!reactor_->wait(std::nullopt)) return traits_type::eof();
        }
    }

private:
    int fd_ = -1;
    std::optional<maya::platform::poll_reactor> reactor_;
    std::optional<maya::platform::poll_reactor::registration> reg_;
    maya::guarded<bool> interrupted_{false};
    char in_[4096]{};
};

// The child's stdin as a streambuf: blocking writes, failure (EPIPE, closed)
// surfaces as badbit on the ostream.
class child_in_buf final : public std::streambuf {
public:
    explicit child_in_buf(maya::platform::posix_process& p) : p_(p) { setp(out_, out_ + sizeof(out_)); }
    void close() noexcept { closed_.with([](bool& v) { v = true; }); p_.close_stdin(); }

protected:
    int_type overflow(int_type ch) override {
        if (sync() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }
    int sync() override {
        const char* p = pbase();
        std::size_t left = static_cast<std::size_t>(pptr() - pbase());
        if (left == 0) return 0;
        if (closed_.read([](const bool& v) { return v; })) return -1;
        const auto h = p_.stdin_handle();
        if (!h) return -1;
        while (left > 0) {
            const auto n = ::write(h->get(), p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        setp(out_, out_ + sizeof(out_));
        return 0;
    }

private:
    maya::platform::posix_process& p_;
    maya::guarded<bool> closed_{false};
    char out_[4096]{};
};

// A long-lived child (an MCP server, an ACP agent, a rag peer) spoken to over
// its stdin/stdout. stderr is inherited: servers log there. Started by jaal,
// so it gets the same guarantees as every other child: its own session,
// default signals, no inherited descriptors. Construction throws
// std::runtime_error on spawn failure.
class ChildProcess {
public:
    struct Spawn {
        std::string              command;   // executable (PATH-resolved)
        std::vector<std::string> args;      // NOT including argv[0]
        std::vector<std::string> env_kv;    // extra "KEY=VALUE" entries
        std::string              cwd;        // empty inherits parent cwd
        bool                     merge_stderr = false;
    };

    explicit ChildProcess(const Spawn& s) {
        namespace pf = maya::platform;
        pf::process_spec spec;
        spec.argv.reserve(s.args.size() + 1);
        spec.argv.push_back(s.command);
        for (const auto& a : s.args) spec.argv.push_back(a);
        for (const auto& kv : s.env_kv) {
            const auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            spec.env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        spec.cwd          = s.cwd;
        spec.stdin_from   = pf::stream_to::pipe;
        spec.stdout_to    = pf::stream_to::pipe;
        spec.stderr_to    = s.merge_stderr ? pf::stream_to::pipe : pf::stream_to::inherit;
        spec.merge_stderr = s.merge_stderr;
        spec.new_session  = true;
        auto p = pf::posix_process::spawn(spec);
        if (!p) throw std::runtime_error("cannot start '" + s.command + "': "
                                     + std::string{p.error().what} + ": "
                                     + std::strerror(p.error().native));
        proc_.emplace(std::move(*p));

        const auto out = proc_->stdout_handle();
        if (!out) throw std::runtime_error("child_process: no stdout pipe");
        out_buf_   = std::make_unique<child_out_buf>(out->get());
        in_buf_    = std::make_unique<child_in_buf>(*proc_);
        out_stream_ = std::make_unique<std::istream>(out_buf_.get());
        in_stream_  = std::make_unique<std::ostream>(in_buf_.get());

    }

    ~ChildProcess() { terminate(); }

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] std::istream& out() noexcept { return *out_stream_; }   // child stdout
    [[nodiscard]] std::ostream& in()  noexcept { return *in_stream_; }    // child stdin
    [[nodiscard]] int pid() const noexcept { return static_cast<int>(proc_->id()); }

    [[nodiscard]] bool alive() noexcept {
        if (exited()) return false;
        return !wait_exit(std::chrono::milliseconds::zero());
    }

    // 0..255 for an exit, 128+N for death by signal N; nullopt while running.
    [[nodiscard]] std::optional<int> exit_code() const noexcept {
        return state_.read([](const State& s) { return s.exit_code; });
    }

    // EOF on the child's stdin: a well-behaved server starts exiting. The
    // streams stay alive (a peer may still hold them); writes now fail.
    void close_stdin() noexcept { in_buf_->close(); }

    // Wake a reader parked on the child's stdout; it sees EOF.
    void interrupt_output() noexcept { out_buf_->interrupt(); }

    // Ask it to finish, then make it. stdin EOF first; a cooperative child
    // exits within `grace`. Then SIGTERM to its tree, another grace, SIGKILL.
    void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds{500}) noexcept {
        if (exited()) return;
        namespace pf = maya::platform;
        close_stdin();
        if (wait_exit(grace)) return;
        (void)proc_->stop(pf::stop_mode::graceful, pf::stop_scope::tree);
        if (wait_exit(std::chrono::milliseconds{2000})) return;
        (void)proc_->stop(pf::stop_mode::forceful, pf::stop_scope::tree);
        (void)wait_exit(std::chrono::milliseconds{2000});
    }

    void shutdown() noexcept {
        terminate();
        interrupt_output();
    }

private:
    struct State {
        bool reaping = false;
        bool reaped = false;
        std::optional<int> exit_code;
    };

    [[nodiscard]] bool exited() const noexcept {
        return state_.read([](const State& s) { return s.reaping; });
    }

    // Wait up to `d` for the exit handle, and reap once it fires. True when
    // the child is gone. Only the destructor's thread and alive() callers
    // get here; the reap itself runs under the guard so it happens once.
    bool wait_exit(std::chrono::milliseconds d) noexcept {
        if (exited()) return true;
        // poll(2) on the exit handle: a level-triggered question any thread
        // may ask, with no shared reactor between them.
        pollfd pfd{proc_->exit_handle().get(), POLLIN, 0};
        int rc;
        do rc = ::poll(&pfd, 1, static_cast<int>(d.count())); while (rc < 0 && errno == EINTR);
        if (rc <= 0) return false;
        // Claim the reap: only the first thread through gets to call it.
        const bool mine = state_.with([](State& s) {
            if (s.reaping) return false;
            s.reaping = true;
            return true;
        });
        if (!mine) return true;
        std::optional<int> code;
        if (auto st = proc_->reap())
            code = st->how == maya::platform::exit_status::kind::exited ? st->code : 128 + st->code;
        state_.with([](State& s, std::optional<int> c) { s.exit_code = c; s.reaped = true; }, code);
        return true;
    }

    std::optional<maya::platform::posix_process>               proc_;
    mutable maya::guarded<State>                               state_;
    std::unique_ptr<child_out_buf> out_buf_;
    std::unique_ptr<child_in_buf>  in_buf_;
    std::unique_ptr<std::istream>  out_stream_;
    std::unique_ptr<std::ostream>  in_stream_;
};

} // namespace agentty::util

#endif // _WIN32 vs POSIX backend

#endif // AGENTTY_HAVE_CHILD_PROCESS
