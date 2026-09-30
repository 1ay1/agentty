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
#include "agentty/domain/sandbox_config.hpp"   // kAlwaysMasked*

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cb = agentty::tools::util::sandbox::claybin_backend;

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
        if (n[0] != '.' || std::string_view{n} == ".pem") continue;
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
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
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

    std::printf("\n%s\n", failures ? "LIVE CHECK FAILURES" : "all live checks passed");
    return failures ? 1 : 0;
}
