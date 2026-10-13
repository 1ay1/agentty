// host_escape_test — the cooperating-host integration OSC payloads.
//
// ui::host splits in two: detect_integration() reads the environment (called
// once by read_launch_env; the answer lives in Model::env.host_integration),
// and file_event_osc() is a pure string builder. With no cached probe there
// is nothing to isolate, so every case runs in one process, on every
// platform.
#include "agentty/runtime/view/host_escape.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
static void set_env(const char* k, const char* v) { _putenv_s(k, v); }
static void unset_env(const char* k)              { _putenv_s(k, ""); }
#else
static void set_env(const char* k, const char* v) { ::setenv(k, v, 1); }
static void unset_env(const char* k)              { ::unsetenv(k); }
#endif

namespace host = agentty::ui::host;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

int main() {
    // Detection, re-read each call.
    unset_env("AGENTTY_HOST");
    unset_env("INSIDE_EMACS");
    CHECK(!host::detect_integration(), "integration off with no host env");
    set_env("AGENTTY_HOST", "emacs");
    CHECK(host::detect_integration(), "integration on with AGENTTY_HOST=emacs");
    unset_env("AGENTTY_HOST");
    set_env("INSIDE_EMACS", "30.1,vterm");
    CHECK(host::detect_integration(), "integration on with INSIDE_EMACS vterm");
    // A comint (non-vterm) Emacs frontend can't run our hooks → off.
    set_env("INSIDE_EMACS", "30.1,comint");
    CHECK(!host::detect_integration(), "integration off with INSIDE_EMACS comint");
    unset_env("INSIDE_EMACS");

    // Payloads: pure, regardless of the environment.
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

    CHECK(!host::file_event_osc("read", "").has_value(),
          "empty path yields no OSC");

    std::printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
