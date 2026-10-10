// agentty/util/child_process.hpp — a long-lived child process with its
// stdio as std::istream/std::ostream.
//
// Used for MCP servers, ACP agents and rag peers. Started by maya's
// native_process (posix or windows), so it gets the same guarantees as every
// other child. Only the pipe IO under the streambufs differs per platform.
// A `cmd /c <payload>` argv passes the payload to cmd.exe as written.
#pragma once

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
#  define AGENTTY_HAVE_CHILD_PROCESS 1
#else
#  define AGENTTY_HAVE_CHILD_PROCESS 0
#endif

#if AGENTTY_HAVE_CHILD_PROCESS

#include <chrono>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include <maya/runtime.hpp>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <poll.h>
#  include <unistd.h>
#endif

namespace agentty::util {

// The child itself, and what it ended with. Only reachable under a
// maya::guarded, so stop(), reap() and close_stdin() never race each other:
// a stop that ran after another thread's reap could signal a reused pid.
struct child_proc {
    maya::platform::native_process proc;
    bool                           reaped = false;
    std::optional<int>             exit_code;
};


// The child's stdout as a streambuf. A reader parked in underflow() can be
// woken by interrupt() without closing the handle under it.
class child_out_buf final : public std::streambuf {
public:
    explicit child_out_buf(maya::platform::borrowed_handle h) : h_(h) {
        setg(in_, in_ + sizeof(in_), in_ + sizeof(in_));
        auto r = maya::platform::native_reactor::create();
        if (!r) throw std::runtime_error("child_process: reactor");
        reactor_.emplace(std::move(*r));
        auto reg = reactor_->watch(h.get(), maya::platform::interest::read, 1);
        if (!reg) throw std::runtime_error("child_process: watch stdout");
        reg_.emplace(std::move(*reg));
    }
    void interrupt() noexcept {
        interrupted_.with([](bool& v) { v = true; });
        if (reactor_) reactor_->waker().wake();
    }

protected:
    int_type underflow() override {
        for (;;) {
            if (interrupted_.read([](const bool& v) { return v; })) return traits_type::eof();
#if defined(_WIN32)
            bool eof = false;
            const auto n = maya::platform::read_some(h_, in_, sizeof(in_), eof);
            if (n > 0) { setg(in_, in_, in_ + n); return traits_type::to_int_type(in_[0]); }
            if (eof) return traits_type::eof();
#else
            const auto n = ::read(h_.get(), in_, sizeof(in_));
            if (n > 0) { setg(in_, in_, in_ + n); return traits_type::to_int_type(in_[0]); }
            if (n == 0) return traits_type::eof();
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return traits_type::eof();
#endif
            if (!reactor_->wait(std::nullopt)) return traits_type::eof();
        }
    }

private:
    maya::platform::borrowed_handle h_;
    std::optional<maya::platform::native_reactor> reactor_;
    std::optional<maya::platform::native_reactor::registration> reg_;
    maya::guarded<bool> interrupted_{false};
    char in_[4096]{};
};

// The child's stdin as a streambuf: blocking writes; failure (the child
// closed it) surfaces as badbit on the ostream.
//
// close() races a writer on another thread. Closing the fd under a write
// would let a newly opened file reuse the number and take the bytes, so the
// fd is only closed when no write is in flight. close() must also never wait
// on a writer: the writer can be blocked on a full pipe to a child that
// stopped reading, and close() is called on the way to killing that child.
// So close() marks the stream closed and, if a write is in flight, leaves
// the fd to that writer, which closes it as it leaves.
class child_in_buf final : public std::streambuf {
public:
    child_in_buf(maya::guarded<child_proc>& p, std::optional<maya::platform::borrowed_handle> h)
        : p_(p), h_(h) { setp(out_, out_ + sizeof(out_)); }
    void close() noexcept {
        // Decide under the lock, act outside it. The writer, if any, sees
        // `closed` when it leaves and closes the fd itself.
        const bool close_now = st_.with([](State& s) {
            if (s.closed) return false;
            s.closed = true;
            return !s.writing;
        });
        if (close_now) close_fd();
    }

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
        // Claim the fd: nobody closes it while `writing` is set.
        const bool mine = st_.with([](State& s) {
            if (s.closed || s.writing) return false;
            s.writing = true;
            return true;
        });
        if (!mine) return -1;
        // Copied at spawn; valid until close_fd(), which can't run now.
        if (!h_) { st_.with([](State& s) { s.writing = false; }); return -1; }
        const int rc = write_all(*h_, p, left);
        const bool closed = st_.with([](State& s) { s.writing = false; return s.closed; });
        if (closed) { close_fd(); return -1; }   // close() left it to us
        if (rc == 0) setp(out_, out_ + sizeof(out_));
        return rc;
    }

private:
    void close_fd() noexcept {
        p_.with([](child_proc& c) { c.proc.close_stdin(); });
    }

    static int write_all(maya::platform::borrowed_handle h, const char* p, std::size_t left) {
        while (left > 0) {
#if defined(_WIN32)
            DWORD put = 0;
            if (!::WriteFile(static_cast<HANDLE>(h.get()), p, static_cast<DWORD>(left), &put, nullptr)
                || put == 0)
                return -1;
            p += put;
            left -= put;
#else
            const auto n = ::write(h.get(), p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            p += n;
            left -= static_cast<std::size_t>(n);
#endif
        }
        return 0;
    }

    struct State {
        bool closed  = false;
        bool writing = false;   // a write is in flight; it owns the fd
    };
    maya::guarded<child_proc>&                       p_;
    const std::optional<maya::platform::borrowed_handle> h_;
    maya::guarded<State> st_;
    char out_[4096]{};
};

// A long-lived child (an MCP server, an ACP agent, a rag peer) spoken to over
// its stdin/stdout. stderr is inherited: servers log there. Construction
// throws std::runtime_error on spawn failure.
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
        auto p = pf::native_process::spawn(spec);
        if (!p) throw std::runtime_error("cannot start '" + s.command + "': "
                                     + std::string{p.error().what} + " ("
                                     + std::to_string(p.error().native) + ")");
        // Handles are copied out before the process goes behind the lock.
        // They stay valid as long as the process object lives (this does).
        exit_h_ = p->exit_handle();
        pid_    = static_cast<int>(p->id());
        const auto out = p->stdout_handle();
        const auto in  = p->stdin_handle();
        if (!out) throw std::runtime_error("child_process: no stdout pipe");
        proc_ = std::make_unique<maya::guarded<child_proc>>(child_proc{std::move(*p)});
        out_buf_    = std::make_unique<child_out_buf>(*out);
        in_buf_     = std::make_unique<child_in_buf>(*proc_, in);
        out_stream_ = std::make_unique<std::istream>(out_buf_.get());
        in_stream_  = std::make_unique<std::ostream>(in_buf_.get());
    }

    ~ChildProcess() { terminate(); }

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] std::istream& out() noexcept { return *out_stream_; }   // child stdout
    [[nodiscard]] std::ostream& in()  noexcept { return *in_stream_; }    // child stdin
    [[nodiscard]] int pid() const noexcept { return pid_; }

    [[nodiscard]] bool alive() noexcept {
        if (exited()) return false;
        return !wait_exit(std::chrono::milliseconds::zero());
    }

    // 0..255 for an exit, 128+N for death by signal N; nullopt while running.
    [[nodiscard]] std::optional<int> exit_code() const noexcept {
        return proc_->read([](const child_proc& c) { return c.exit_code; });
    }

    // EOF on the child's stdin: a well-behaved server starts exiting. The
    // streams stay alive (a peer may still hold them); writes now fail.
    void close_stdin() noexcept { in_buf_->close(); }

    // Wake a reader parked on the child's stdout; it sees EOF.
    void interrupt_output() noexcept { out_buf_->interrupt(); }

    // Ask it to finish, then make it. stdin EOF first; a cooperative child
    // exits within `grace`. Then stop its tree gracefully, then forcefully.
    void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds{500}) noexcept {
        if (exited()) return;
        namespace pf = maya::platform;
        close_stdin();
        if (wait_exit(grace)) return;
        stop(pf::stop_mode::graceful);
        if (wait_exit(std::chrono::milliseconds{2000})) return;
        stop(pf::stop_mode::forceful);
        (void)wait_exit(std::chrono::milliseconds{2000});
    }

    void shutdown() noexcept {
        terminate();
        interrupt_output();
    }

private:
    [[nodiscard]] bool exited() const noexcept {
        return proc_->read([](const child_proc& c) { return c.reaped; });
    }

    // Under the lock, so it can't land after a reap.
    void stop(maya::platform::stop_mode m) noexcept {
        proc_->with([](child_proc& c, maya::platform::stop_mode mode) {
            if (!c.reaped) (void)c.proc.stop(mode, maya::platform::stop_scope::tree);
        }, m);
    }

    // Is the exit handle signalled within `d`? Level-triggered, so any
    // thread may ask without a shared reactor.
    [[nodiscard]] bool exit_ready(std::chrono::milliseconds d) noexcept {
#if defined(_WIN32)
        return ::WaitForSingleObject(static_cast<HANDLE>(exit_h_.get()),
                                     static_cast<DWORD>(d.count())) == WAIT_OBJECT_0;
#else
        pollfd pfd{exit_h_.get(), POLLIN, 0};
        int rc;
        do rc = ::poll(&pfd, 1, static_cast<int>(d.count())); while (rc < 0 && errno == EINTR);
        return rc > 0;
#endif
    }

    // Wait up to `d` for the exit, and reap once it fires (under the lock,
    // so exactly once). True when the child is gone.
    bool wait_exit(std::chrono::milliseconds d) noexcept {
        if (exited()) return true;
        if (!exit_ready(d)) return false;
        proc_->with([](child_proc& c) {
            if (c.reaped) return;
            if (auto st = c.proc.reap())
                c.exit_code = st->how == maya::platform::exit_status::kind::exited
                                ? st->code : 128 + st->code;
            c.reaped = true;
        });
        return true;
    }

    // Behind a pointer: guarded can't move, and the in-buf holds a reference.
    std::unique_ptr<maya::guarded<child_proc>> proc_;
    maya::platform::borrowed_handle            exit_h_{};
    int                                        pid_ = 0;
    std::unique_ptr<child_out_buf> out_buf_;
    std::unique_ptr<child_in_buf>  in_buf_;
    std::unique_ptr<std::istream>  out_stream_;
    std::unique_ptr<std::ostream>  in_stream_;
};

} // namespace agentty::util

#endif // AGENTTY_HAVE_CHILD_PROCESS
