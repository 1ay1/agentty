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
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace agentty::tools::util::sandbox {
Backend detected_backend() noexcept { return Backend::None; }
bool is_active() noexcept { return false; }
claybin_backend::SpawnResult spawn_in_sandbox(const std::vector<std::string>&, std::string_view,
                                              int, const std::vector<std::string>&) {
    return {};
}
}  // namespace agentty::tools::util::sandbox

namespace {
namespace mt = ::mcp::tools;
std::atomic<int> failures{0};
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); failures.fetch_add(1); }
}

mt::ExecRequest sh(const std::string& script) {
    mt::ExecRequest r;
    r.program = {"/bin/sh", {"-c", script}};
    r.max_output_bytes = 64 * 1024;
    return r;
}

// Start, poll until it ends (or give up), stop, drop. The session's last
// reference dies on whichever thread happens to hold it.
void session_round(mt::Exec& exec, int i) {
    auto s = exec.start(sh(i % 3 == 0 ? "echo x" : i % 3 == 1 ? "sleep 0.05; echo y; exit 3"
                                                              : "yes | head -c 20000"));
    check(s.has_value(), "session starts");
    if (!s) return;
    auto sess = *s;
    for (int k = 0; k < 40; ++k) {
        auto u = sess->poll(std::chrono::milliseconds{i % 2 ? 0 : 20});
        if (!u.running && u.outcome) break;
    }
    if (i % 4 == 0) sess->stop();   // stop a finished one: must be a no-op
}

// Stop while it is still running, from another thread than the poller.
void stop_while_polled(mt::Exec& exec) {
    auto s = exec.start(sh("sleep 5"));
    check(s.has_value(), "long session starts");
    if (!s) return;
    std::shared_ptr<mt::Session> sess = *s;
    std::thread poller([sess] {
        for (int k = 0; k < 30; ++k) {
            auto u = sess->poll(std::chrono::milliseconds{10});
            if (!u.running) break;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    sess->stop();
    poller.join();
}

void foreground(mt::Exec& exec, const mt::Call& call, int i) {
    auto r = exec.run(call, sh(i % 2 ? "printf a" : "printf b; exit 1"));
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
        if (auto s = exec->start(sh("sleep 0.02; echo z"))) pile.push_back(*s);
    std::thread dropper([p = std::move(pile)]() mutable { p.clear(); });
    dropper.join();

    if (failures.load()) {
        std::fprintf(stderr, "exec_session_race_test: %d failure(s)\n", failures.load());
        return 1;
    }
    std::puts("exec_session_race_test: ok");
    return 0;
}
