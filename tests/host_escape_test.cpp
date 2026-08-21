// host_escape_test — the cooperating-host integration OSC payloads.
//
// ui::host is pure + env-gated: it only builds strings and reports whether a
// cooperating editor host is present. This drives it under both states via
// AGENTTY_HOST and asserts the exact OSC payload (the caller wraps it in
// maya's Cmd::emit_osc, which adds the "ESC ] 5379 ;" prefix + ST).
//
// Standalone (not consolidated): integration_active() caches its env probe in
// a function-local static, so the on/off cases must each run in a fresh
// process. We fork per case on POSIX; the whole test is a no-op elsewhere.
#include "agentty/runtime/view/host_escape.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace host = agentty::ui::host;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

// Inactive: no AGENTTY_HOST / INSIDE_EMACS → integration off, no OSC.
static int case_inactive() {
    ::unsetenv("AGENTTY_HOST");
    ::unsetenv("INSIDE_EMACS");
    CHECK(!host::integration_active(), "integration off with no host env");
    CHECK(!host::file_event_osc("read", "/x.cpp").has_value(),
          "no OSC when integration inactive");
    return g_fail;
}

// Active via AGENTTY_HOST=emacs → exact payloads, escaping, line field.
static int case_active_emacs() {
    ::setenv("AGENTTY_HOST", "emacs", 1);
    CHECK(host::integration_active(), "integration on with AGENTTY_HOST=emacs");

    auto edit = host::file_event_osc("edit", "/src/foo.cpp");
    CHECK(edit.has_value()
          && *edit == R"(agentty;{"event":"file","kind":"edit","path":"/src/foo.cpp"})",
          "edit OSC payload");

    auto read = host::file_event_osc("read", "/a b.txt", 42);
    CHECK(read.has_value()
          && *read == R"(agentty;{"event":"file","kind":"read","path":"/a b.txt","line":42})",
          "read OSC carries line + preserves spaces in path");

    // A path with a quote must be JSON-escaped so it can't break out of the OSC.
    auto quoted = host::file_event_osc("write", "/weird\"name.txt");
    CHECK(quoted.has_value()
          && quoted->find(R"(\"name.txt)") != std::string::npos,
          "quote in path is JSON-escaped");

    // Empty path → nothing.
    CHECK(!host::file_event_osc("read", "").has_value(),
          "empty path yields no OSC");
    return g_fail;
}

// Active via INSIDE_EMACS=...,vterm (the fallback marker).
static int case_active_inside_emacs() {
    ::unsetenv("AGENTTY_HOST");
    ::setenv("INSIDE_EMACS", "30.1,vterm", 1);
    CHECK(host::integration_active(), "integration on with INSIDE_EMACS vterm");
    // A comint (non-vterm) Emacs frontend can't run our hooks → off.
    return g_fail;
}

#if HAVE_FORK
static int run_forked(int (*body)()) {
    pid_t pid = ::fork();
    if (pid == 0) { std::_Exit(body() ? 1 : 0); }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

int main() {
#if HAVE_FORK
    int fails = 0;
    std::printf("[inactive]\n");          fails += run_forked(case_inactive);
    std::printf("[active: AGENTTY_HOST]\n");  fails += run_forked(case_active_emacs);
    std::printf("[active: INSIDE_EMACS]\n");  fails += run_forked(case_active_inside_emacs);
    std::printf(fails ? "\nFAILED (%d)\n" : "\nPASSED\n", fails);
    return fails ? 1 : 0;
#else
    std::printf("SKIP (no fork; env-cached probe needs process isolation)\n");
    return 0;
#endif
}
