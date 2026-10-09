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

#else  // !_WIN32 — POSIX backend (fork/exec/pipe)

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agentty::util {

// A std::streambuf backed by a POSIX pipe FD. Historically this was
// __gnu_cxx::stdio_filebuf, but that lives in <ext/stdio_filebuf.h> — a
// libstdc++ extension that libc++ (Android/Termux, some macOS toolchains)
// doesn't ship. Same shape as the Windows handle_streambuf above: owns the
// FD, blocking read()/write(), buffered get/put areas sized for the
// line-buffered JSON-RPC frames the peer exchanges.
class fd_streambuf final : public std::streambuf {
public:
    explicit fd_streambuf(int fd) noexcept : fd_(fd) {
        setg(in_, in_ + sizeof(in_), in_ + sizeof(in_));  // empty → underflow
        setp(out_, out_ + sizeof(out_));
        int wake[2] = {-1, -1};
        if (::pipe(wake) == 0) {
            wake_rd_ = wake[0];
            wake_wr_ = wake[1];
            (void)::fcntl(wake_rd_, F_SETFD, ::fcntl(wake_rd_, F_GETFD) | FD_CLOEXEC);
            (void)::fcntl(wake_wr_, F_SETFD, ::fcntl(wake_wr_, F_GETFD) | FD_CLOEXEC);
            (void)::fcntl(wake_wr_, F_SETFL, ::fcntl(wake_wr_, F_GETFL) | O_NONBLOCK);
        }
    }
    ~fd_streambuf() override { sync(); close(); }

    fd_streambuf(const fd_streambuf&)            = delete;
    fd_streambuf& operator=(const fd_streambuf&) = delete;

    // Wake a thread blocked in underflow() without closing an FD underneath
    // read(2). The owner joins that thread before destroying this streambuf.
    void interrupt() noexcept {
        interrupted_.store(true, std::memory_order_release);
        if (wake_wr_ >= 0) {
            const char byte = 1;
            (void)::write(wake_wr_, &byte, 1);
        }
    }

    void close() noexcept {
        interrupt();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (wake_rd_ >= 0) { ::close(wake_rd_); wake_rd_ = -1; }
        if (wake_wr_ >= 0) { ::close(wake_wr_); wake_wr_ = -1; }
    }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

protected:
    // ── reading: refill the get area from the pipe ────────────────────
    int_type underflow() override {
        if (!valid() || interrupted_.load(std::memory_order_acquire))
            return traits_type::eof();
        if (wake_rd_ >= 0) {
            pollfd fds[2] = {{fd_, POLLIN, 0}, {wake_rd_, POLLIN, 0}};
            int ready;
            do {
                ready = ::poll(fds, 2, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0 || (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) ||
                interrupted_.load(std::memory_order_acquire))
                return traits_type::eof();
        }
        ssize_t got;
        do {
            got = ::read(fd_, in_, sizeof(in_));
        } while (got < 0 && errno == EINTR);
        if (got <= 0) return traits_type::eof();   // 0 = writer closed
        setg(in_, in_, in_ + got);
        return traits_type::to_int_type(in_[0]);
    }

    // ── writing: flush the put area to the pipe ─────────────────────
    int_type overflow(int_type ch) override {
        if (!flush_put_area()) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override { return flush_put_area() ? 0 : -1; }

private:
    [[nodiscard]] bool flush_put_area() noexcept {
        const char* p = pbase();
        const char* e = pptr();
        while (p < e) {
            if (!valid()) return false;
            ssize_t n;
            do {
                n = ::write(fd_, p, static_cast<std::size_t>(e - p));
            } while (n < 0 && errno == EINTR);
            if (n <= 0) return false;   // EPIPE etc. — reader gone
            p += n;
        }
        setp(out_, out_ + sizeof(out_));
        return true;
    }

    int  fd_ = -1;
    int  wake_rd_ = -1;
    int  wake_wr_ = -1;
    std::atomic<bool> interrupted_{false};
    char in_[4096];
    char out_[4096];
};

// Move-only owner of a single pipe-end FD. Closes on scope exit unless
// release()'d, so the spawn prologue's leak-ladder (close every FD opened so
// far on any early throw) becomes automatic and impossible to get wrong: each
// ::pipe() end is parked in an FdGuard the instant it exists, a throw between
// here and the commit point unwinds them in reverse, and at the commit point
// the surviving ends are release()'d — into the child (via dup2) or into the
// parent's streambufs. Zero runtime cost: one int, no vtable, no heap.
class FdGuard {
public:
    FdGuard() noexcept = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(FdGuard&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FdGuard& operator=(FdGuard&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    FdGuard(const FdGuard&)            = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { int f = fd_; fd_ = -1; return f; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    int fd_ = -1;
};

// A pipe whose two ends are CLOEXEC FROM BIRTH — the property is part of the
// TYPE, not a call site's discipline. Why it must be structural: providers
// spawn in parallel, so a sibling's fork() between a bare ::pipe() and our
// exec would inherit the ends; that sibling's long-lived child then holds our
// child's stdout write end open, EOF never reaches our reader, and teardown
// wedges forever in pthread_join (a real field hang). With CloexecPipe there
// is no window in which an inheritable end exists, and no future edit can
// forget the fcntl — the only way to get a pipe here is the safe way.
// (pipe2(O_CLOEXEC) is atomic where the platform has it; the fcntl fallback
// still closes the race down to our own two instructions, and every OTHER
// concurrently-spawned child sees CLOEXEC set.) dup2() onto 0/1 in our own
// child CLEARS the flag on the duplicate, so the exec'd server keeps stdio.
struct CloexecPipe {
    FdGuard rd, wr;
    // Named constructor: the throwing path never yields a partially-armed pipe.
    static CloexecPipe make(const char* what) {
        int fds[2] = {-1, -1};
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        if (::pipe2(fds, O_CLOEXEC) != 0)
            throw std::runtime_error(std::string{"mcp::cap: pipe2() failed ("} + what + ")");
#else
        if (::pipe(fds) != 0)
            throw std::runtime_error(std::string{"mcp::cap: pipe() failed ("} + what + ")");
        (void)::fcntl(fds[0], F_SETFD, ::fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
        (void)::fcntl(fds[1], F_SETFD, ::fcntl(fds[1], F_GETFD) | FD_CLOEXEC);
#endif
        CloexecPipe p;
        p.rd = FdGuard{fds[0]};
        p.wr = FdGuard{fds[1]};
        return p;
    }
};

// A spawned child process whose stdin/stdout are wired to iostreams. stderr is
// left attached to the parent's (MCP servers log there; the spec keeps stderr
// free for logging). Construction throws std::runtime_error on spawn failure.
class ChildProcess {
public:
    struct Spawn {
        std::string              command;   // executable (PATH-resolved via execvp)
        std::vector<std::string> args;      // NOT including argv[0]
        std::vector<std::string> env_kv;    // extra "KEY=VALUE" entries
        std::string              cwd;        // empty inherits parent cwd
        bool                     merge_stderr = false;
    };

    explicit ChildProcess(const Spawn& s) {
        // parent.wr → child stdin; child stdout → parent.rd. Every end is
        // CLOEXEC-from-birth by TYPE (see CloexecPipe) and owned by an
        // FdGuard, so any throw before the commit point unwinds every open
        // FD automatically — no hand-rolled close-ladder, no inheritable
        // window for a concurrently-forking sibling.
        CloexecPipe in_p    = CloexecPipe::make("stdin");
        CloexecPipe out_p   = CloexecPipe::make("stdout");
        CloexecPipe ready_p = CloexecPipe::make("ready");
        FdGuard& in_rd = in_p.rd;       FdGuard& in_wr = in_p.wr;
        FdGuard& out_rd = out_p.rd;     FdGuard& out_wr = out_p.wr;
        FdGuard& ready_rd = ready_p.rd; FdGuard& ready_wr = ready_p.wr;
        int in_pipe[2]    = {in_p.rd.get(), in_p.wr.get()};
        int out_pipe[2]   = {out_p.rd.get(), out_p.wr.get()};
        int ready_pipe[2] = {ready_p.rd.get(), ready_p.wr.get()};

        pid_ = ::fork();
        if (pid_ < 0)
            throw std::runtime_error("mcp::cap: fork() failed");
            // in_rd/in_wr/out_rd/out_wr all close here via guard unwind.

        if (pid_ == 0) {
            // ── child ────────────────────────────────────────────────────
            // Raw FDs: this is the forked child; the parent's guards are
            // copies in a separate address space and irrelevant here.
            // Give the whole subprocess tree a private session/process group.
            // The parent waits for the byte below before exposing pid(), so a
            // prompt terminate() can safely signal -pid without racing setsid.
            if (::setsid() < 0) (void)::setpgid(0, 0);
            ready_rd.reset();
            const char ready = 1;
            (void)::write(ready_pipe[1], &ready, 1);
            ready_wr.reset();

            ::dup2(in_pipe[0],  STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            if (s.merge_stderr) ::dup2(out_pipe[1], STDERR_FILENO);
            if (!s.cwd.empty() && ::chdir(s.cwd.c_str()) != 0) {
                std::fprintf(stderr, "mcp::cap: chdir '%s' failed: %s\n",
                             s.cwd.c_str(), std::strerror(errno));
                ::_exit(126);
            }
            ::close(in_pipe[0]);  ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            for (const auto& kv : s.env_kv) {
                if (kv.find('=') == std::string::npos) continue;
                ::putenv(::strdup(kv.c_str()));   // intentionally leaked pre-exec
            }
            std::vector<std::string> store;
            store.reserve(s.args.size() + 1);
            store.push_back(s.command);
            for (const auto& a : s.args) store.push_back(a);
            std::vector<char*> argv;
            argv.reserve(store.size() + 1);
            for (auto& v : store) argv.push_back(v.data());
            argv.push_back(nullptr);
            ::execvp(s.command.c_str(), argv.data());
            std::fprintf(stderr, "mcp::cap: exec '%s' failed: %s\n",
                         s.command.c_str(), std::strerror(errno));
            ::_exit(127);
        }

        // ── parent ──────────────────────────────────────────────────────
        // Drop the child-side ends (guards close them on destruction); the
        // parent-side ends are release()'d into the fd_streambufs, which now
        // own the FDs and close them via shutdown(). Commit point: from here
        // on the streams hold the only references.
        // Wait until the child has established its private group/session.
        // EOF is also sufficient (the child failed before signalling).
        ready_wr.reset();
        char ready = 0;
        ssize_t ready_n;
        do { ready_n = ::read(ready_rd.get(), &ready, 1); }
        while (ready_n < 0 && errno == EINTR);
        ready_rd.reset();

        in_rd.reset();    // child's stdin read end — parent doesn't use it
        out_wr.reset();   // child's stdout write end — parent doesn't use it
        in_buf_  = std::make_unique<fd_streambuf>(out_rd.release());
        out_buf_ = std::make_unique<fd_streambuf>(in_wr.release());
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
        if (pid_ <= 0) return false;
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {                       // just reaped — remember why
            record_exit_(status);
            pid_ = -1;
        } else if (r < 0 && errno == ECHILD) {
            pid_ = -1;
        }
        return pid_ > 0;
    }

    // Exit status of the child once it has been reaped (by alive()/terminate()).
    // nullopt while the child is still running or was never observed to exit.
    // A value >= 0 is the normal exit code; a value of 128+N encodes death by
    // signal N (the shell convention), so the model reads one comparable number.
    [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }

    // Close ONLY the child's stdin (our write end). A well-behaved server sees
    // EOF and begins exiting, which in turn closes its stdout — unblocking a
    // reader thread parked in getline on our read end. Does NOT reap or touch
    // the read stream, so a reader can drain remaining output + the EOF
    // cleanly. Safe to call before joining the reader, then shutdown() after.
    // Close the child's stdin FD WITHOUT destroying out_stream_ OR out_buf_.
    // The the peer holds an std::ostream& into out_stream_, and
    // out_stream_'s rdbuf() points into out_buf_. Destroying out_buf_
    // here leaves a dangling rdbuf: if the reader thread processes a
    // notification and writes a reply between this call and
    // the peer stopping (which ends the reader), the ostream dereferences
    // freed memory → SIGSEGV. Calling close() shuts the FD (writes fail
    // with EBADF/EPIPE, which the streambuf surfaces as badbit) but keeps
    // the object alive until ~ChildProcess().
    void close_stdin() noexcept {
        // Do NOT call out_stream_->flush() here: the reader thread's sink()
        // lambda writes to out_stream_ under the peer::write_mu_, and
        // an unsynchronized flush() races it (ostream internals are not
        // thread-safe). Any pending data in the put area is lost, which is
        // acceptable during teardown.
        if (out_buf_) out_buf_->close();   // closes FD; objects stay alive
    }

    void interrupt_output() noexcept {
        if (in_buf_) in_buf_->interrupt();
    }

    // Stop/reap the entire process group with bounded waits. SIGTERM gives
    // cooperative children two seconds; SIGKILL guarantees TERM-resistant
    // children and descendants cannot retain stdout indefinitely.
    // Like close_stdin(), this does NOT reset out_stream_ or out_buf_ — the
    // reader thread may still be mid-notification holding a reference to
    // them via the peer::out_. close() shuts the FD safely; the
    // objects are destroyed when ChildProcess itself is destructed
    // (proc.reset() in teardown_, after the reader is joined).
    void terminate() noexcept {
        // Do NOT call out_stream_->flush() here — same race as close_stdin():
        // the reader thread may be writing via sink() and an unsynchronized
        // flush() on the same ostream is a data race.
        if (out_buf_) out_buf_->close();
        if (pid_ <= 0) return;

        const pid_t leader = pid_;
        bool leader_reaped = false;
        auto reap_leader = [&] {
            if (leader_reaped) return;
            int status = 0;
            const pid_t result = ::waitpid(leader, &status, WNOHANG);
            if (result == leader) {
                record_exit_(status);
                leader_reaped = true;
                pid_ = -1;
            } else if (result < 0 && errno == ECHILD) {
                leader_reaped = true;
                pid_ = -1;
            }
        };
        auto group_alive = [&] {
            if (::kill(-leader, 0) == 0) return true;
            return errno == EPERM;
        };
        auto wait_for = [&](int attempts, bool require_group_gone) {
            for (int i = 0; i < attempts; ++i) {
                reap_leader();
                if (leader_reaped && (!require_group_gone || !group_alive()))
                    return true;
                ::usleep(10'000);
            }
            return false;
        };
        auto signal_group = [&](int signal) {
            if (::kill(-leader, signal) != 0 && errno == ESRCH && !leader_reaped)
                (void)::kill(leader, signal);
        };

        // A child that exits on stdin EOF needs no signal, but descendants
        // may still own stdout; only return early when the whole group is gone.
        if (wait_for(50, true)) return;
        signal_group(SIGTERM);
        if (wait_for(200, true)) return;
        signal_group(SIGKILL);
        (void)wait_for(100, true);
        reap_leader();
    }

    // Close child stdin (EOF → graceful exit), poll briefly, then SIGTERM.
    // Idempotent; also called by the destructor.
    void shutdown() noexcept {
        terminate();
        in_stream_.reset();
        in_buf_.reset();
    }

private:
    // Translate a waitpid() status into the single comparable number the
    // model sees: normal exit → its code; killed by signal N → 128+N (shell
    // convention). Idempotent — only records the first observed exit.
    void record_exit_(int status) const noexcept {
        if (exit_code_) return;
        if (WIFEXITED(status))        exit_code_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    }

    mutable int pid_ = -1;
    mutable std::optional<int> exit_code_;
    std::unique_ptr<fd_streambuf> in_buf_;
    std::unique_ptr<fd_streambuf> out_buf_;
    std::unique_ptr<std::istream> in_stream_;
    std::unique_ptr<std::ostream> out_stream_;
};

} // namespace agentty::util

#endif // _WIN32 vs POSIX backend

#endif // AGENTTY_HAVE_CHILD_PROCESS
