// exec_session_race_test — agentty's real Exec (src/tool/util/exec.cpp) under
// concurrent use, built small enough to run under TSan.
//
// Several threads start background sessions, poll them, stop them, and drop
// the last reference while the drain worker may still be writing, the shapes
// process_start/poll/stop drive. Also runs foreground commands alongside. A
// lock taken in the wrong order aborts (jaal lock_order_error); a data race
// shows up under -fsanitize=thread; a self-join throws EDEADLK.
//
// Sandbox off: the four sandbox symbols are stubbed below so this links
// exec.cpp alone, not claybin.

#include "agentty/tool/util/exec.hpp"
#include "agentty/tool/util/sandbox.hpp"

#include <mcp/tools/host.hpp>
#include <mcp/tools/state.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace agentty::tools::util::sandbox {
Backend detected_backend() noexcept { return Backend::None; }
bool is_active() noexcept { return false; }
claybin_backend::SpawnResult spawn_in_sandbox(const std::vector<std::string>&, std::string_view,
                                              int, const std::vector<std::string>&,
                                              const handoff::TrustedSnapshot*) {
    return {};
}
#if defined(__APPLE__)
std::expected<std::vector<std::string>, std::string>
wrap_argv_for_exec(const std::vector<std::string>& argv, std::string_view,
                   const std::vector<std::string>&) {
    return argv;   // unreachable: is_active() is false above
}
#endif
}  // namespace agentty::tools::util::sandbox

namespace {
namespace mt = ::mcp::tools;
std::atomic<int> failures{0};
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); failures.fetch_add(1); }
}

// The same command shapes on each platform's shell: quick exit, a short
// sleep then output and a non-zero exit, a burst of output, a long sleep.
// Windows has no /bin/sh; cmd.exe covers them, with `ping -n N 127.0.0.1`
// as the sleep (N-1 seconds; -n 1 is a few ms) since `timeout` needs a
// console the session doesn't have.
struct Scripts {
    const char* quick;
    const char* short_then_fail;
    const char* burst;
    const char* long_sleep;
    const char* print_ok;
    const char* print_fail;
    const char* short_then_print;
};
#if defined(_WIN32)
constexpr Scripts kS{
    "echo x",
    "ping -n 1 127.0.0.1 >nul & echo y& exit /b 3",
    "for /l %i in (1,1,3000) do @echo yyyyyy",
    "ping -n 6 127.0.0.1 >nul",
    "echo a",
    "echo b& exit /b 1",
    "ping -n 1 127.0.0.1 >nul & echo z",
};

mt::ExecRequest sh(const std::string& script) {
    mt::ExecRequest r;
    r.program = {"cmd.exe", {"/S", "/C", script}};
    r.max_output_bytes = 64 * 1024;
    return r;
}
#else
constexpr Scripts kS{
    "echo x",
    "sleep 0.05; echo y; exit 3",
    "yes | head -c 20000",
    "sleep 5",
    "printf a",
    "printf b; exit 1",
    "sleep 0.02; echo z",
};

mt::ExecRequest sh(const std::string& script) {
    mt::ExecRequest r;
    r.program = {"/bin/sh", {"-c", script}};
    r.max_output_bytes = 64 * 1024;
    return r;
}
#endif

// Start, read until it ends (or give up), stop, drop. The session's last
// reference dies on whichever thread happens to hold it.
void session_round(mt::Exec& exec, int i) {
    auto s = exec.start(mt::Call{}, sh(i % 3 == 0 ? kS.quick : i % 3 == 1 ? kS.short_then_fail
                                                              : kS.burst));
    check(s.has_value(), "session starts");
    if (!s) return;
    auto sess = *s;
    for (int k = 0; k < 40; ++k) {
        auto u = sess->read(std::chrono::milliseconds{i % 2 ? 0 : 20}, 1 << 20);
        if (!u.running && u.outcome) break;
    }
    if (i % 4 == 0) sess->stop();   // stop a finished one: must be a no-op
}

// Stop while it is still running, from another thread than the reader.
void stop_while_polled(mt::Exec& exec) {
    auto s = exec.start(mt::Call{}, sh(kS.long_sleep));
    check(s.has_value(), "long session starts");
    if (!s) return;
    std::shared_ptr<mt::Session> sess = *s;
    std::thread poller([sess] {
        for (int k = 0; k < 30; ++k) {
            auto u = sess->read(std::chrono::milliseconds{10}, 1 << 20);
            if (!u.running) break;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    sess->stop();
    poller.join();
}

void foreground(mt::Exec& exec, const mt::Call& call, int i) {
    auto r = exec.run(call, sh(i % 2 ? kS.print_ok : kS.print_fail));
    check(!std::holds_alternative<mt::StartFailed>(r.outcome), "foreground run starts");
}

}  // namespace

int main() {
    auto exec = agentty::tools::util::make_exec({});
    mt::Call call{};

    constexpr int kThreads = 8;
    constexpr int kRounds  = 25;
    std::barrier sync(kThreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int r = 0; r < kRounds; ++r) {
                const int i = t * kRounds + r;
                if (r % 5 == 0) stop_while_polled(*exec);
                else if (r % 5 == 1) foreground(*exec, call, i);
                else session_round(*exec, i);
            }
        });
    }
    for (auto& th : ts) th.join();

    // Sessions handed across threads and dropped there: the last owner is
    // not the thread that started them.
    std::vector<std::shared_ptr<mt::Session>> pile;
    for (int i = 0; i < 16; ++i)
        if (auto s = exec->start(mt::Call{}, sh(kS.short_then_print))) pile.push_back(*s);
    std::thread dropper([p = std::move(pile)]() mutable { p.clear(); });
    dropper.join();

#if !defined(_WIN32)
    using clk = std::chrono::steady_clock;
    const auto ms_since = [](clk::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
    };

    // Two readers on one session get disjoint bytes that add up to the
    // whole stream: the cursor moves inside the same lock that hands out
    // the bytes.
    {
        auto s = exec->start(mt::Call{}, sh("i=0; while [ $i -lt 2000 ]; do echo $i; i=$((i+1)); done"));
        check(s.has_value(), "counting session starts");
        if (s) {
            auto sess = *s;
            std::string a, b;
            auto reader = [&sess](std::string& out) {
                for (;;) {
                    auto r = sess->read(std::chrono::milliseconds{20}, 1 << 20);
                    out += r.output;
                    if (!r.running && r.eof) { out += sess->read(std::chrono::milliseconds{0}, 1 << 20).output; return; }
                }
            };
            std::thread ta(reader, std::ref(a)), tb(reader, std::ref(b));
            ta.join(); tb.join();
            std::size_t expect = 0;
            for (int i = 0; i < 2000; ++i) expect += std::to_string(i).size() + 1;
            check(a.size() + b.size() == expect, "concurrent readers split the stream exactly");
        }
    }

    // stop() and dropping the last handle never block, even when the child
    // ignores SIGTERM and so takes the full grace to die.
    {
        auto s = exec->start(mt::Call{}, sh("trap '' TERM; while :; do sleep 0.05; done"));
        check(s.has_value(), "stubborn session starts");
        if (s) {
            auto sess = *s;
            (void)sess->read(std::chrono::milliseconds{100}, 0);   // let the trap install
            auto t0 = clk::now();
            sess->stop();
            check(ms_since(t0) < 50, "stop() returns at once");
            t0 = clk::now();
            sess.reset();   // last handle: the pump carries on alone
            check(ms_since(t0) < 50, "dropping the last handle never blocks");
        }
    }

    // stop() wakes the pump: the child gets SIGTERM promptly, not on the next
    // poll tick.
    {
        auto s = exec->start(mt::Call{}, sh(kS.long_sleep));
        check(s.has_value(), "sleeper starts");
        if (s) {
            auto sess = *s;
            (void)sess->read(std::chrono::milliseconds{50}, 0);
            const auto t0 = clk::now();
            sess->stop();
            for (;;) {
                auto r = sess->read(std::chrono::milliseconds{50}, 0);
                if (!r.running || ms_since(t0) > 3000) break;
            }
            check(ms_since(t0) < 1000, "stop reaches the child promptly");
        }
    }
#endif

    if (failures.load()) {
        std::fprintf(stderr, "exec_session_race_test: %d failure(s)\n", failures.load());
        return 1;
    }
    std::puts("exec_session_race_test: ok");
    return 0;
}
