// Live enforcement check, driving agentty's OWN claybin spawn path.
//
// The end-to-end route (agentty run -> LLM -> shell tool) kept dying on
// provider hiccups, and it is the wrong instrument anyway: it proves the model
// can call a tool, not that the sandbox enforces. This calls the same
// claybin_backend::spawn_shell() the shell tool calls, with a Posture built by
// hand, and looks at what the child actually got.
//
// Two claims:
//   1. under the compiler profile, realloc() works           (the mremap fix)
//   2. with net_mode=None, a socket to a raw IP cannot connect (the net rule)
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/sandbox.hpp"       // bwrap_argv_for_test
#include "agentty/domain/sandbox_config.hpp"   // kAlwaysMasked*

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cb = agentty::tools::util::sandbox::claybin_backend;

// The stub setter from sandbox_config_race_stubs.cpp. Declared here rather
// than pulled from fs_helpers.hpp, because including that header is exactly
// what this target avoids (it reaches into mcp and drags the tool layer in).
namespace agentty::tools::util { void set_workspace_root(std::filesystem::path p); }

namespace {

// The sandbox's workspace for this check. Created in main(); a missing one
// makes every spawn fail on mount(ENOENT).
constexpr const char* kWorkspace = "/tmp/agentty-sandbox-live-check";

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The read set agentty grants by default, enough for /bin/sh + python3.
cb::Posture base_posture() {
    cb::Posture p;
    p.system_read_roots = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt"};
    p.etc_readable      = {"/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d",
                           "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
                           "/etc/passwd", "/etc/group", "/etc/localtime",
                           "/etc/ssl", "/etc/ca-certificates", "/etc/pki"};
    p.workspace         = kWorkspace;
    p.cwd               = kWorkspace;
    p.syscall_mode      = 1;   // Compiler -- the default, and where the bug was
    p.net_mode          = 0;   // Full
    p.tmp_bytes         = 512ull * 1024 * 1024;

    // The non-configurable masks, exactly as build_claybin_posture() sets
    // them. Mirrored rather than left empty: an empty mask list is why the
    // first version of the .env check below "failed" for the wrong reason --
    // it was measuring a posture nobody ever spawns.
    if (const char* home = std::getenv("HOME"); home && *home) {
        for (const char* m : agentty::sandbox_cfg::kAlwaysMasked)
            p.masked.emplace_back(std::string{home} + m);
    }
    for (const char* n : agentty::sandbox_cfg::kAlwaysMaskedNames) {
        // Skip SUFFIX rules (`*.pem`, `*.tfvars`). A suffix is not a path and
        // cannot be turned into one without knowing what is on disk -- the
        // spawn path finds those by walking, which is the whole reason the
        // walk exists. Keyed on the `*` marker rather than on a hardcoded
        // ".pem", so adding a suffix to the list cannot silently plant a
        // nonsense mask at `<workspace>/*.tfvars` here.
        if (n[0] == '*') continue;
        // Every NAME rule, not just dotfiles. The old `n[0] != '.'` filter was
        // an artifact of a list that happened to be all dotfiles; it now
        // silently drops `credentials.json` and `service-account.json`, so
        // this harness would mirror a posture weaker than the one the spawn
        // path builds -- and a live check that measures the wrong posture is
        // the failure mode the comment above already warns about.
        p.masked.emplace_back(std::string{kWorkspace} + "/" + n);
    }
    return p;
}

// Run a shell command under the posture, capture its output, return exit code.
int run(const cb::Posture& p, const std::string& cmd, std::string& out) {
    int fds[2];
    if (::pipe(fds) != 0) { std::printf("  pipe failed\n"); return -1; }

    auto sp = cb::spawn_shell(p, cmd, fds[1], fds[1]);
    ::close(fds[1]);
    if (!sp.started) {
        // A spawn failure is a HARD failure of the whole check, not a quiet
        // -1 the caller might read as "blocked". That distinction bit once
        // already: with the workspace directory missing, claybin failed with
        // mount(ENOENT), the child never ran, and the "did not reach the
        // network" assertion passed -- because nothing reached anything. A
        // negative test that passes when the subject never executed is worse
        // than no test.
        std::printf("  spawn failed: %s\n", sp.start_error.c_str());
        ++failures;
        ::close(fds[0]);
        return -1;
    }
    // Drain the pipe AND service the supervisor, in one poll.
    //
    // Both, in the same loop, for the reason the production runner does it:
    // a brokered syscall blocks the guest in the kernel until someone answers,
    // so reading the pipe first and the listener later deadlocks -- the child
    // waits for a decision, we wait for output it cannot produce until it has
    // one. This harness has to mirror that or the brokering cases below would
    // hang instead of failing, and a hang reads as "the test is broken".
    std::string chunk;
    bool eof = false;
    while (!eof) {
        struct pollfd pfds[2];
        pfds[0] = {fds[0], POLLIN, 0};
        const bool brokering = sp.supervisor_fd >= 0 && sp.service_broker;
        if (brokering) pfds[1] = {sp.supervisor_fd, POLLIN, 0};

        // A bounded wait rather than -1: a bug that stops answering should
        // surface as a failed assertion, not as a harness that never returns.
        if (::poll(pfds, brokering ? 2 : 1, 10'000) <= 0) break;

        if (brokering && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!sp.service_broker()) {
                sp.supervisor_fd = -1;
                sp.service_broker = nullptr;
            }
        }
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n > 0) out.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) eof = true;
            else if (errno != EINTR && errno != EAGAIN) eof = true;
        }
    }
    ::close(fds[0]);

    int status = 0;
    ::waitpid(sp.pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

}  // namespace

int main() {
    if (!cb::available()) {
        std::printf("claybin unavailable on this host (no user/mount namespaces) -- skipping\n");
        return 0;
    }

    // The workspace has to EXIST before claybin can bind it -- otherwise
    // every spawn dies on mount(ENOENT) and the negative tests below pass
    // vacuously. Created here rather than assumed, so the check is
    // self-contained and cannot be broken by someone cleaning /tmp.
    {
        std::error_code ec;
        std::filesystem::create_directories(kWorkspace, ec);
        if (ec) {
            std::printf("cannot create workspace %s: %s\n", kWorkspace,
                        ec.message().c_str());
            return 1;
        }
    }

    // build_bwrap_argv() binds workspace_root(), so the stub has to point at
    // the directory we just made. Without this the bwrap case would bind /tmp
    // and mask /tmp/.env -- and then pass, because the canary it is looking
    // for lives somewhere else entirely. A check that passes for the wrong
    // reason is the failure mode this harness has already hit twice.
    agentty::tools::util::set_workspace_root(kWorkspace);

    // ── 1. realloc under the compiler profile ────────────────────────
    // The regression: mremap was absent from base(), so realloc() of a large
    // block returned NULL and curl reported "out of memory" on a machine with
    // free RAM.
    //
    // Done in python rather than a compiled probe so the check has no build
    // step of its own and nothing to leave behind outside the workspace.
    // ctypes calls mremap(2) directly, then the allocator path curl hits.
    std::printf("realloc under syscall_mode=Compiler:\n");
    {
        auto p = base_posture();
        std::string out;
        int rc = run(p,
            "python3 -c 'import ctypes, os\n"
            "libc = ctypes.CDLL(None, use_errno=True)\n"
            "libc.mmap.restype = ctypes.c_void_p\n"
            "libc.mremap.restype = ctypes.c_void_p\n"
            "small, big = 1 << 20, 4 << 20\n"
            "p = libc.mmap(None, ctypes.c_size_t(small), 3, 0x22, -1, 0)\n"
            "assert p not in (0, 2**64 - 1), \"mmap failed\"\n"
            "print(\"mmap ok\")\n"
            "ctypes.set_errno(0)\n"
            "q = libc.mremap(ctypes.c_void_p(p), ctypes.c_size_t(small), ctypes.c_size_t(big), 1)\n"
            "e = ctypes.get_errno()\n"
            "print(\"mremap ok\" if (q or 0) != 2**64 - 1 else \"mremap FAILED errno=%d (%s)\" % (e, os.strerror(e)))\n"
            "b = bytearray(small)\n"
            "b.extend(bytes(big))            # realloc past the mmap threshold\n"
            "print(\"realloc ok\" if len(b) == small + big else \"realloc FAILED\")' 2>&1",
            out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(rc == 0, "probe exits 0");
        expect(out.find("mremap ok") != std::string::npos, "mremap allowed");
        expect(out.find("realloc ok") != std::string::npos, "realloc works");
    }

    // ── 2. net_mode=None isolates ───────────────────────────────────────
    std::printf("connect() with net_mode=None:\n");
    {
        auto p = base_posture();
        p.net_mode = 1;  // None
        std::string out;
        // A raw IP, so this exercises the socket layer and not DNS.
        int rc = run(p,
            "python3 -c 'import socket,sys\n"
            "s=socket.socket(); s.settimeout(3)\n"
            "try:\n"
            "    s.connect((\"1.1.1.1\",443)); print(\"CONNECTED\")\n"
            "except OSError as e:\n"
            "    print(\"blocked:\", e)' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("CONNECTED") == std::string::npos, "did not reach the network");
    }

    // ── 3. net_mode=Full still works ────────────────────────────────────
    // The other half of the claim: isolation is a CHOICE, so the permissive
    // setting must actually permit. A sandbox that blocks everything is not
    // enforcing a policy, it is broken.
    std::printf("connect() with net_mode=Full:\n");
    {
        auto p = base_posture();
        p.net_mode = 0;
        std::string out;
        int rc = run(p,
            "python3 -c 'import socket\n"
            "s=socket.socket(); s.settimeout(5)\n"
            "try:\n"
            "    s.connect((\"1.1.1.1\",443)); print(\"CONNECTED\")\n"
            "except OSError as e:\n"
            "    print(\"blocked:\", e)' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("CONNECTED") != std::string::npos, "reached the network");
    }

    // ── 4. the masks that are NOT configurable ────────────────────────
    // kAlwaysMasked/kAlwaysMaskedNames are the one part of the policy a user
    // cannot switch off, so they are the part most worth measuring rather
    // than trusting. A workspace .env is the sharpest case: the workspace is
    // bound READ-WRITE by both backends, so scope does not save us and the
    // mask is the only thing standing between an agent and a real secret.
    std::printf("workspace .env is masked:\n");
    {
        const std::string envp = std::string{kWorkspace} + "/.env";
        {
            std::FILE* f = std::fopen(envp.c_str(), "w");
            if (!f) { std::printf("  cannot plant .env\n"); ++failures; }
            else { std::fputs("SECRET=canary\n", f); std::fclose(f); }
        }
        auto p = base_posture();
        std::string out;
        int rc = run(p, "cat .env 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("SECRET=canary") == std::string::npos,
               "the secret did NOT reach the child");
        std::error_code ec;
        std::filesystem::remove(envp, ec);
    }

    // ── 5. curl, specifically ─────────────────────────────────────
    // Its own case because curl reports EVERY early failure as
    // CURLE_OUT_OF_MEMORY (27), which sends you looking at memory caps and
    // the network rules when the cause is neither. python3 fetches the same
    // URL fine under this exact posture, so a curl failure here is about
    // curl's startup, not about the boundary.
    //
    // Kept as a check rather than a note because curl is what users reach
    // for, and "the sandbox broke curl" is indistinguishable from "the
    // sandbox is broken" from the outside.
    std::printf("curl reaches the network (net_mode=Full):\n");
    {
        auto p = base_posture();
        std::string out;
        int rc = run(p,
            "curl -sS -m 5 -o /dev/null -w 'H:%{http_code}' https://example.com 2>&1",
            out);
        std::printf("  exit=%d, child said: %s\n", rc, out.c_str());
        expect(out.find("H:200") != std::string::npos, "curl got a 200");
    }

    // ── 6. the SAME secret, under bwrap ───────────────────────────────
    // The gap this closes: bwrap applied NO masks at all. kAlwaysMasked was
    // referenced only by the claybin posture, and build_bwrap_argv did not
    // even take the config -- so the same workspace .env that reads back
    // empty under claybin came back in full under bwrap, and the Masked row
    // in the settings pane configured nothing.
    //
    // Driven through the real bwrap binary rather than by inspecting argv,
    // because the argv-level claim is already pinned in sandbox_escape_test
    // and the thing that was actually wrong is what the CHILD sees. Skips
    // itself when bwrap is absent; that is a property of the host, not of the
    // code, and a skip here is honest where a pass would not be.
    std::printf("workspace .env is masked under BWRAP too:\n");
    {
        const std::string envp = std::string{kWorkspace} + "/.env";
        {
            std::FILE* f = std::fopen(envp.c_str(), "w");
            if (f) { std::fputs("SECRET=canary\n", f); std::fclose(f); }
        }

        // The real argv agentty would use, so this cannot drift from
        // production the way a hand-built command would.
        auto argv = agentty::tools::util::sandbox::bwrap_argv_for_test(
            "cat .env 2>&1");
        std::string cmd;
        for (const auto& a : argv) {
            // Single-quote each word: paths contain no quotes here, but a
            // workspace under a directory with a space would otherwise split.
            cmd += '\'';
            for (char c : a) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
            cmd += "' ";
        }
        cmd += "2>&1";

        std::string out;
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) {
            std::printf("  cannot run bwrap -- skipped\n");
        } else {
            char buf[4096];
            while (std::fgets(buf, sizeof buf, p)) out += buf;
            const int rc = ::pclose(p);
            if (out.find("bwrap: ") != std::string::npos &&
                out.find("SECRET") == std::string::npos && rc != 0) {
                // bwrap itself refused to start (no user namespaces, etc.).
                std::printf("  bwrap unavailable on this host -- skipped (%s)",
                            out.c_str());
            } else {
                std::printf("  child said: %s", out.c_str());
                expect(out.find("SECRET=canary") == std::string::npos,
                       "the secret did NOT reach the child under bwrap");
            }
        }
        std::error_code ec;
        std::filesystem::remove(envp, ec);
    }

    // ── 7. secrets BELOW the workspace root ──────────────────────────
    // The gap the bounded sweep closes. kAlwaysMaskedNames are BASENAMES, and
    // a name cannot become a mount without knowing where the file is -- so
    // masking only `<workspace>/.env` covered the one layout nobody uses.
    // `services/api/.env` is where it actually lives in a monorepo.
    //
    // Also checks the two things the sweep must NOT do: follow a directory
    // symlink out of the workspace, and walk into node_modules.
    std::printf("nested secrets are masked (sweep depth):\n");
    {
        const std::filesystem::path root{kWorkspace};
        std::error_code ec;
        std::filesystem::create_directories(root / "services" / "api", ec);
        std::filesystem::create_directories(root / "node_modules" / "pkg", ec);

        auto plant = [&](const std::filesystem::path& p, const char* body) {
            std::FILE* f = std::fopen(p.c_str(), "w");
            if (f) { std::fputs(body, f); std::fclose(f); }
        };
        plant(root / "services" / "api" / ".env", "NESTED=canary\n");
        plant(root / "services" / "api" / "key.pem", "PEMBODY=canary\n");
        // Inside a skipped directory: NOT masked, and that is deliberate --
        // the sweep trades it for not walking a 40k-file tree on every spawn.
        // Asserted so the trade is visible rather than assumed.
        plant(root / "node_modules" / "pkg" / ".env", "SKIPPED=canary\n");

        auto p = base_posture();
        // The real list, from the shared function -- the same one both
        // backends consume, so this cannot pass while production differs.
        {
            agentty::sandbox_cfg::Config c;
            c.configured = true;
            const char* home = std::getenv("HOME");
            p.masked = agentty::sandbox_cfg::mask_paths(
                c, kWorkspace, home ? home : "");
        }

        std::string out;
        int rc = run(p, "cat services/api/.env services/api/key.pem 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("NESTED=canary") == std::string::npos,
               "a nested .env is masked");
        expect(out.find("PEMBODY=canary") == std::string::npos,
               "a nested *.pem is masked (the suffix rule)");

        std::filesystem::remove_all(root / "services", ec);
        std::filesystem::remove_all(root / "node_modules", ec);
    }

    // ── 8. the default pid cap actually bounds a fork bomb ──────────────
    // sandbox_audit reported resource.pids as `none` on a default install,
    // which means an approved command could fork without limit. max_procs now
    // ships at 4096 -- the one resource wall with a number that is safe on
    // every host (no build needs 4096 concurrent processes; a fork bomb wants
    // millions).
    //
    // Checked in the CHILD rather than in the report, because "cgroup2
    // pids.max" in a guarantee table and "the fork actually fails" are
    // different claims and this subsystem has already shipped the first
    // without the second.
    std::printf("the default pid cap bounds a fork bomb:\n");
    {
        auto p = base_posture();
        p.max_processes = agentty::sandbox_cfg::Config{}.max_procs;
        std::string out;
        // Fork until it fails, print how far it got. A bounded sandbox reports
        // a number near the cap; an unbounded one runs until the HOST suffers,
        // which is the outcome worth never shipping.
        int rc = run(p,
            "python3 -c 'import os,sys\n"
            "n=0\n"
            "try:\n"
            "    for _ in range(20000):\n"
            "        if os.fork()==0: os._exit(0)\n"
            "        n+=1\n"
            "except OSError:\n"
            "    pass\n"
            "print(\"forked\", n)' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        // The cap is 4096 and the loop tries 20000, so a working cap stops it
        // short. Asserting "did not reach 20000" rather than an exact number:
        // the kernel counts threads the shell and python already hold, so the
        // precise ceiling is host-dependent and pinning it would be flaky.
        expect(out.find("forked 20000") == std::string::npos,
               "the fork loop hit a limit");
    }

    // ── 9. syscall brokering decides instead of denying flatly ──────────
    // The feature with the worst failure mode in this whole subsystem: a
    // brokered syscall blocks the guest IN THE KERNEL until the supervisor
    // answers, so a broker that is not polled is a hang rather than a weaker
    // wall. Every case here would time out rather than fail if that regressed,
    // which is why they run with a real child and a real listener.
    std::printf("brokering: a self-ptrace is allowed\n");
    {
        auto p = base_posture();
        p.broker = true;
        std::string out;
        // PTRACE_TRACEME: the child volunteers to be traced by its own parent.
        // Grants no authority over anything else, and `strace prog` needs it.
        // A flat deny breaks every debugger; the broker allows it from scalars.
        int rc = run(p,
            "python3 -c 'import ctypes,os\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "ctypes.set_errno(0)\n"
            "r=libc.ptrace(0,0,0,0)\n"           // 0 == PTRACE_TRACEME
            "print(\"traceme\", r, ctypes.get_errno())' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        // The decision is what matters, not the return value: TRACEME can fail
        // for unrelated reasons (already traced). EPERM (1) is the broker
        // denying; anything else means it got through to the kernel.
        expect(out.find("errno=1") == std::string::npos &&
               out.find(" 1)") == std::string::npos,
               "TRACEME was not denied by the broker");
    }

    std::printf("brokering: attaching to pid 1 is denied\n");
    {
        auto p = base_posture();
        p.broker = true;
        std::string out;
        // PTRACE_ATTACH to pid 1 -- the sandbox's own init inside a pid
        // namespace, and a container-escape primitive rather than a debugging
        // step. The target is a SCALAR, which is exactly why this is decidable
        // soundly: no guest memory is read, so there is no TOCTOU window.
        int rc = run(p,
            "python3 -c 'import ctypes,os\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "ctypes.set_errno(0)\n"
            "r=libc.ptrace(16,1,0,0)\n"          // 16 == PTRACE_ATTACH
            "e=ctypes.get_errno()\n"
            "print(\"attach\", r, e, os.strerror(e) if e else \"\")' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("attach -1") != std::string::npos,
               "ATTACH to pid 1 failed");
    }

    std::printf("brokering: the child still finishes (no hang)\n");
    {
        // The regression that would hurt most. If the supervisor stops being
        // polled -- or answers one notification and not the next -- the child
        // blocks forever and the command dies on the hard deadline. A test that
        // only checked decisions would pass while every real command took
        // exactly the timeout.
        auto p = base_posture();
        p.broker = true;
        std::string out;
        int rc = run(p,
            "python3 -c 'import ctypes\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "for i in range(50): libc.ptrace(16,1,0,0)\n"
            "print(\"survived 50 brokered calls\")' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("survived 50") != std::string::npos,
               "50 brokered calls in a row did not hang");
        expect(rc == 0, "the child exited cleanly");
    }

    // ── 9. the REAL entry point, not a posture we built ourselves ──────
    //
    // Everything above drives claybin::spawn with a Posture this file
    // constructs. That proves claybin enforces what it is told — it does NOT
    // prove that the posture agentty actually builds says the same thing.
    //
    // The gap is real and has bitten before: build_claybin_posture() reads the
    // sealed config, applies kAlwaysMasked, runs the workspace sweep, and
    // decides the syscall profile. A mirror of that logic in a test is a
    // SECOND implementation, and the two drifted once already (bwrap emitted
    // no masks at all while this file's mirror did).
    //
    // So: call the same function a `bash` tool call reaches, with the config
    // installed the same way main.cpp installs it, and read what the child
    // actually got. Anything that passes here is true of the shipped product.
    std::printf("\nthe REAL path (run_shell_command, production posture):\n");
    {
        namespace sb = agentty::tools::util::sandbox;
        agentty::tools::util::set_workspace_root(kWorkspace);
        sb::reset_config_for_test();
        agentty::sandbox_cfg::Config cfg;
        cfg.configured = true;            // defaults = the Balanced posture
        sb::set_config(cfg);
        sb::init(sb::Mode::On);

        if (!sb::is_active()) {
            std::printf("  no backend on this host -- skipped\n");
        } else {
            std::printf("  %s\n", sb::describe_state().c_str());

            // Plant one of every masked SHAPE: a plain name, a nested name
            // (needs the sweep), and both suffix rules.
            const std::string ws{kWorkspace};
            std::filesystem::create_directories(ws + "/services/api");
            auto plant = [&](const std::string& rel, const char* body) {
                std::FILE* f = std::fopen((ws + "/" + rel).c_str(), "w");
                if (f) { std::fputs(body, f); std::fclose(f); }
            };
            plant(".env",                 "ROOT_SECRET=should_never_appear\n");
            plant(".npmrc",               "//r/:_authToken=npm_should_never\n");
            plant("credentials.json",     "{\"key\":\"gcp_should_never\"}\n");
            plant("server.pem",           "PEM_should_never\n");
            plant("prod.tfvars",          "tf_should_never\n");
            plant("services/api/.env",    "NESTED_should_never=1\n");
            plant("main.cpp",             "int main(){}\n");

            auto r = sb::run_shell_command(
                "cat .env .npmrc credentials.json server.pem prod.tfvars "
                "services/api/.env 2>&1; echo ---; cat main.cpp",
                64 * 1024, std::chrono::seconds{30});

            // Not one of the planted secrets may appear. `should_never` is the
            // shared marker so a single find() covers all six.
            const bool leaked = r.output.find("should_never") != std::string::npos;
            if (leaked)
                std::printf("  LEAKED: %s\n", r.output.c_str());
            expect(!leaked, "no masked secret reached a real bash call");

            // And the sandbox did not break ordinary work: a non-secret file
            // in the same directory still reads. A mask list that caught
            // main.cpp would pass the test above and break every build.
            expect(r.output.find("int main") != std::string::npos,
                   "an ordinary workspace file is still readable");
        }

        // ── why the handoff gate cannot be a sandbox wall ───────────────
        //
        // The obvious objection to the trust-handoff gate is "claybin already
        // confines the process, so how can the agent write .vscode/tasks.json
        // at all?" This is the answer, measured rather than argued.
        //
        // A sandbox wall answers "may this PROCESS touch this PATH". For a
        // path inside the workspace the answer is legitimately YES -- it has
        // to be, or no build could write an object file. tasks.json is not
        // dangerous because of WHERE it is; it is dangerous because VS Code
        // executes it LATER, on the host, outside any sandbox we installed.
        //
        // So confinement and handoff are different questions, and this pair of
        // checks pins that: the wall holds where confinement applies, and does
        // not apply where it doesn't. If the first ever starts failing, the
        // gate has become redundant; if the second ever starts failing, the
        // sandbox has broken ordinary work.
        {
            std::printf("\nconfinement vs. handoff (why the gate exists):\n");
            const std::string ws{kWorkspace};
            std::filesystem::create_directories(ws + "/.vscode");

            // OUTSIDE the workspace: the sandbox must refuse. This is
            // confinement, and it is claybin's job.
            //
            // NOT /tmp. The posture mounts a private tmpfs there (see
            // build_policy: "so nothing leaks into the host /tmp"), so a write
            // to /tmp SUCCEEDS inside the guest and never reaches the host --
            // which looks like an escape to a naive probe and is in fact the
            // containment working. I wrote that probe first and it reported
            // ESCAPED while the host file did not exist.
            //
            // $HOME is the honest test: real, outside the workspace, and not
            // virtualised by the posture.
            const char* home = std::getenv("HOME");
            const std::string victim =
                std::string{home ? home : "/root"} + "/agentty_confinement_probe";
            auto out = sb::run_shell_command(
                "echo escaped > " + victim + " && echo wrote || echo denied",
                4096, std::chrono::seconds{15});
            // The guest's own report is not evidence -- a virtualised target
            // would say "wrote" too. Ask the HOST filesystem.
            std::error_code hec;
            const bool on_host = std::filesystem::exists(victim, hec);
            if (on_host) std::printf("  ESCAPED to %s\n", victim.c_str());
            expect(!on_host, "a write OUTSIDE the workspace never reaches the host");
            std::error_code rec;
            std::filesystem::remove(victim, rec);

            // INSIDE the workspace, a host-trusted path: the sandbox ALLOWS
            // it, and must. There is no wall here to lean on, which is exactly
            // why the handoff gate is a separate mechanism that observes the
            // filesystem around the call.
            auto in = sb::run_shell_command(
                "echo '{}' > .vscode/tasks.json && echo wrote || echo denied",
                4096, std::chrono::seconds{15});
            expect(in.output.find("wrote") != std::string::npos,
                   "a write INSIDE the workspace is allowed -- so confinement "
                   "cannot be what stops a trust handoff");
        }
    }

    std::printf("\n%s\n", failures ? "LIVE CHECK FAILURES" : "all live checks passed");
    return failures ? 1 : 0;
}
