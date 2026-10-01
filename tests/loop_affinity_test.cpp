// loop_affinity_test — agentty's loop-only state is loop-only, provably.
//
// WHY THIS IS AN AGENTTY TEST AND NOT A JAAL ONE.
//
// jaal's own tests prove loop_bound<T> behaves. They say nothing about
// whether THIS codebase uses it, and that gap is exactly where the guarantee
// used to die: jaal shipped loop_bound, the header explained the bug it
// prevents ("a worker calling the renderer would silently get its own empty
// caches — a wrong answer, which is worse than a crash"), and across jaal,
// maya and agentty the number of loop_bound<T> in production code was ZERO.
// All 68 of maya's thread_locals and all 25 of agentty's stayed raw, each one
// asserting the one-thread rule in a comment.
//
// The reason adoption was zero turned out to be a defect, not apathy: a token
// could only be minted by the kernel, the kernel never handed one to update()
// or view(), and loop_key's constructor was public-and-explicit — so the only
// way to get a token was to FORGE one (`loop_token t{loop_key{}}`, which
// compiled). A guarantee nothing can adopt protects nothing.
//
// So this test covers the half jaal can't:
//
//   1. the forge is gone (static_asserts below — the shape of the hole)
//   2. agentty's view caches are reachable on the loop and not off it
//   3. a worker that touches them DIES rather than rendering from an empty
//      copy, which is the whole behavioural claim
//
// Standalone, not consolidated: case 3 asserts an ABORT, so it has to run in
// a child process and doctest can't host that.

#include <jaal/jaal.hpp>
#include <jaal/kernel/loop.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace k = jaal::kernel;

// ── 1. The hole, pinned shut ─────────────────────────────────────────────
//
// Each of these corresponds to a route into loop-bound state. They are
// static_asserts rather than compile_fail cases because agentty has no
// compile-fail harness; jaal carries the negative cases (CASE 1..7 in
// jaal/tests/compile_fail/loop_bound.cpp) and these pin the same facts from
// the consumer side, so a jaal bump that regresses them breaks agentty's
// build rather than silently widening agentty's exposure.

static_assert(!std::is_default_constructible_v<k::loop_key>,
              "loop_key's ctor must stay private: naming it IS the forge that "
              "made every loop_bound in this codebase reachable from a worker");
static_assert(!std::is_copy_constructible_v<k::loop_token>,
              "a copyable token could be stashed in a global and used off-loop");
static_assert(!std::is_move_constructible_v<k::loop_token>,
              "a movable token could be captured into a task body");
static_assert(!std::is_default_constructible_v<k::loop_token>,
              "a default-constructible token would be no proof at all");

// There must be no UNCHECKED global accessor. A task body is captureless but
// can still CALL anything, so a loop_token::current() would hand worker code
// a token and undo the rest. on_loop() is the checked form: it aborts off the
// loop instead of returning proof.
template <class T> concept has_current = requires { T::current(); };
static_assert(!has_current<k::loop_token>);

// Loop-bound state must not be able to travel to a worker as a task argument.
static_assert(!jaal::Sendable<k::loop_bound<int>>,
              "loop_bound is pinned to one thread; it must not be Sendable");
static_assert(!jaal::Sendable<k::loop_token>,
              "a token is proof about ONE thread; it must not travel");

// ── 2. A stand-in for agentty's view caches ──────────────────────────────
//
// Same shape as the real ones in
// src/runtime/view/thread/turn/agent_timeline/agent_timeline.cpp: a cache the
// render path reads and writes with no lock, correct only because one thread
// owns it. The real caches are file-static, so this mirrors their usage
// rather than importing them.
static k::loop_bound<std::string> g_render_cache{};

struct Paint {};
struct Done {};

struct App {
    using Model = int;
    using Msg   = std::variant<Paint, Done>;
    using Cmd   = jaal::Cmd<Msg>;

    static Cmd init(Model& m) { m = 0; return Cmd::send(Msg{Paint{}}); }

    // Stands in for view(): runs on the loop, touches the cache with no lock.
    static Cmd update(Model& m, Paint) {
        if (!k::on_loop()) { m = -1; return Cmd::quit(); }

        g_render_cache.with([](std::string& c) { c = "painted"; });

        // A COPY may leave the with() body. A reference may not — jaal
        // static_asserts on that, because these caches evict and the handle
        // would dangle at the next put().
        if (g_render_cache.with([](const std::string& c) { return c; }) != "painted") {
            m = -2;
            return Cmd::quit();
        }

        m = 1;
        return Cmd::send(Msg{Done{}});
    }

    static Cmd update(Model& m, Done) { m = 2; return Cmd::quit(); }
};

static int on_loop_cases() {
    // Before any kernel exists, this thread is not a loop.
    if (k::on_loop()) { std::puts("FAIL: armed with no kernel"); return 1; }

    // A bare worker is not a loop either.
    bool worker_armed = true;
    std::thread([&] { worker_armed = k::on_loop(); }).join();
    if (worker_armed) { std::puts("FAIL: bare worker reported on_loop"); return 2; }

    // A real kernel arms its own thread for its WHOLE lifetime, not just
    // inside step(). That matters here: a host calls view() between steps,
    // and view() is the code with the caches worth protecting.
    {
        jaal::headless<App> h;
        h.run_until_idle();
        if (h.model() != 2) {
            std::printf("FAIL: app ended at model=%d\n", h.model());
            return 3;
        }
    }

    // And it is disarmed again once the kernel is gone, so a pool thread
    // reused afterwards cannot inherit proof.
    if (k::on_loop()) { std::puts("FAIL: identity leaked past ~kernel"); return 4; }

    return 0;
}

// ── 3. The behavioural claim: a worker DIES, it does not get a copy ───────
#if HAVE_FORK
static int worker_abort_case() {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) { std::puts("SKIP: fork failed"); return 0; }

    if (pid == 0) {
        // Child: a worker reaches for the render cache. Before loop_bound
        // this silently succeeded against the worker's own empty copy and
        // the frame painted wrong. It must now abort.
        std::thread([] {
            g_render_cache.with([](std::string& c) { c = "stomped by a worker"; });
        }).join();
        // Reached only if the guarantee is broken.
        std::_Exit(0);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::puts("FAIL: a worker touched loop-bound state and lived");
        return 5;
    }
    if (!WIFSIGNALED(status)) {
        std::printf("FAIL: expected a signal, got exit %d\n", WEXITSTATUS(status));
        return 6;
    }
    if (WTERMSIG(status) != SIGABRT) {
        std::printf("FAIL: expected SIGABRT, got signal %d\n", WTERMSIG(status));
        return 7;
    }
    return 0;
}
#else
static int worker_abort_case() { return 0; }   // no fork: cases 1-2 still run
#endif

int main() {
    if (const int rc = on_loop_cases())    return rc;
    if (const int rc = worker_abort_case()) return rc;
    std::puts("loop_affinity_test: ok");
    return 0;
}
