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

#include <cstdio>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cb = agentty::tools::util::sandbox::claybin_backend;

namespace {

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
    p.workspace         = "/tmp/sbtest";
    p.syscall_mode      = 1;   // Compiler -- the default, and where the bug was
    p.net_mode          = 0;   // Full
    p.tmp_bytes         = 512ull * 1024 * 1024;
    return p;
}

// Run a shell command under the posture, capture its output, return exit code.
int run(const cb::Posture& p, const std::string& cmd, std::string& out) {
    int fds[2];
    if (::pipe(fds) != 0) { std::printf("  pipe failed\n"); return -1; }

    auto sp = cb::spawn_shell(p, cmd, fds[1], fds[1]);
    ::close(fds[1]);
    if (!sp.started) {
        std::printf("  spawn failed: %s\n", sp.start_error.c_str());
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

    std::printf("\n%s\n", failures ? "LIVE CHECK FAILURES" : "all live checks passed");
    return failures ? 1 : 0;
}
