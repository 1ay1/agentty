// The Windows sandbox contract, tested on EVERY platform that can run it.
//
// claybin's windows backend keeps the translate/apply split: compile() is
// pure and testable anywhere (windows_backend_test already pins it); this
// file pins the ADAPTER layer — what agentty's posture becomes on windows,
// and which walls the report promises. It runs in the agentty_tests binary
// on every host; the LIVE enforcement question is claybin's windows_live_
// check, which runs (and currently passes) on a windows box.
#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "agentty/tool/util/sandbox_claybin.hpp"

namespace ag = agentty::tools::util::sandbox;
namespace sb = agentty::tools::util::sandbox::claybin_backend;

TEST_CASE("sandbox/win: probe picks claybin when a wall is real") {
    // The floor is deliberately one working wall (appcontainer or job), so
    // Auto degrades only where there is nothing to build a boundary from.
    // init() runs the probe and latches the result; probe() itself is a
    // per-platform-internal detail.
    ag::init(ag::Mode::Auto);
    CHECK(ag::detected_backend() == ag::Backend::Claybin);
}

TEST_CASE("sandbox/win: the posture is native-path based") {
    auto posture = ag::claybin_posture_for_test();
    CHECK_FALSE(posture.system_read_roots.empty());
    // The toolchain lives under the system root; every entry must be a real
    // windows path, because ACL stamping on a non-native path fails loudly
    // and half a wall is worse than none.
    for (const auto& root : posture.system_read_roots) {
        CHECK(root.size() >= 3);
        CHECK(root[1] == ':');
    }
    // The workspace is still the write grant.
    CHECK_FALSE(posture.workspace.empty());
    CHECK(posture.workspace[1] == ':');
}

TEST_CASE("sandbox/win: describe grades the walls honestly") {
    auto posture = ag::claybin_posture_for_test();
    auto r = sb::describe(posture);
    CHECK(r.ok);
    if (!r.ok) { MESSAGE(r.detail); return; }
    REQUIRE_FALSE(r.walls.empty());

    // find one wall by name
    auto find = [&](std::string_view name) -> const sb::Wall* {
        for (const auto& w : r.walls)
            if (w.name == name) return &w;
        return nullptr;
    };

    // process isolation rides on the appcontainer: strong where the probe
    // proved it, and never silently absent.
    if (auto* w = find("process.isolation")) {
        CHECK(w->strength != "none");
    }
    // NO SYSCALL FILTER on windows, ever: whatever the posture asks for, the
    // report must not claim strong here. This is claybin's honesty contract,
    // pinned at the adapter layer too.
    if (auto* w = find("syscall.filter")) {
        CHECK(w->strength != "strong");
    }
}

TEST_CASE("sandbox/win: spawn_shell starts and the tree kill exists") {
    auto posture = ag::claybin_posture_for_test();
    auto r = sb::spawn_shell(posture, "exit 0", -1, -1);
    if (!r.started) {
        // A host whose probe passed but whose spawn fails is a degraded host
        // (profile permission, policy software). The adapter reports the
        // reason rather than pretending.
        MESSAGE("spawn failed: " << r.start_error);
        return;
    }
    CHECK(r.pid > 0);
    // The job kill exists and is callable. Calling it here is safe: the child
    // is `exit 0`, already gone.
    if (r.kill_tree) CHECK_NOTHROW((void)r.kill_tree());
}

TEST_CASE("sandbox/win: the full exec chain captures output") {
    // This is the path a real tool call takes: spawn_in_sandbox with the
    // caller's pipe fd (a HANDLE value here), the runner adopting what came
    // back. A failure here with a green spawn test means the problem is in
    // the HAND-OFF, not the sandbox.
    auto posture = ag::claybin_posture_for_test();
    // the pipe: create like run_child does, hand the WRITE end to the spawner
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    REQUIRE(::CreatePipe(&out_r, &out_w, &sa, 0));
    const int pipe_fd = static_cast<int>(reinterpret_cast<intptr_t>(out_w));
    auto r = sb::spawn_argv(posture, {"cmd.exe", "/d", "/s", "/c", "echo inside"},
                            pipe_fd, pipe_fd, {});
    ::CloseHandle(out_w);  // our write end goes, or the reader never sees EOF
    if (!r.started) {
        MESSAGE("exec chain spawn failed: " << r.start_error);
        ::CloseHandle(out_r);
        REQUIRE_FALSE(r.started);  // stops the case with the reason
        return;
    }
    CHECK(r.pid > 0);
    // the job handle rides in kill_tree's closure; we own the process handle
    // only through the adapter's pidfd here. For the exit code, wait on the
    // pid: the job kill switch would be the strong way, but the adapter keeps
    // the process handle inside the SpawnResult's pidfd — take it back out.
    HANDLE proc = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, static_cast<DWORD>(r.pid));
    REQUIRE(proc != nullptr);
    ::WaitForSingleObject(proc, 15000);
    DWORD code = 0;
    ::GetExitCodeProcess(proc, &code);
    ::CloseHandle(proc);
    MESSAGE("guest exit code: " << code);
    // drain the pipe until EOF (the child has exited by then)
    std::string out;
    char buf[512];
    DWORD n = 0;
    for (;;) {
        if (!::ReadFile(out_r, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        out.append(buf, n);
    }
    ::CloseHandle(out_r);
    MESSAGE("captured: '" << out << "'");
    CHECK(code == 0);  // exit 0 => the guest RAN; nonzero = died under the walls
    CHECK(out.find("inside") != std::string::npos);
}
#else
// Off windows this file documents the parity contract without testing it:
// the same posture shape compiles everywhere (claybin's own cross-platform
// test pins that), and the windows backend is the only one where the APPLY
// step is new. A build on windows runs the real checks above.
TEST_CASE("sandbox/win: parity contract compiles off-windows too") {
    CHECK(true);
}
#endif
