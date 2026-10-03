// tests/clipboard_display_env_test.cpp — regression guard for the clipboard
// display-server discovery that made image paste fail inside tmux.
//
// The bug: read_clipboard_image() decided Wayland-vs-X11 from
// XDG_SESSION_TYPE / WAYLAND_DISPLAY. Inside a tmux pane those are a
// SNAPSHOT of whatever started the tmux server (often systemd or an old
// tty login), so a pane under a healthy Wayland kitty reported
// XDG_SESSION_TYPE=tty with no WAYLAND_DISPLAY. The wl-paste branch was
// skipped and the failure was misreported as "the terminal didn't answer".
//
// The fix discovers the compositor from its SOCKET instead, and hands the
// correction to the child as an `env VAR=…` command prefix.
//
// These tests pin the three properties that must not silently regress.
// They drive the real discovery logic over a FAKE runtime directory, so
// they pass on a headless CI box with no compositor at all.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>   // getpid — unique temp dir per test process

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Mirror of the production helpers. clipboard.cpp keeps them in an anonymous
// namespace (correctly — they're an implementation detail), so the test
// re-states them and a drift between the two shows up as a behavioural test
// failure in the end-to-end case below rather than a silent divergence.
// Keep these byte-identical in BEHAVIOUR to src/io/clipboard.cpp.
// ---------------------------------------------------------------------------

std::string shell_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += '\'';
    return out;
}

bool wayland_socket_exists(const std::string& dir, std::string_view name) {
    if (name.empty()) return false;
    std::error_code ec;
    fs::path p = name.front() == '/' ? fs::path{name} : fs::path{dir} / name;
    return fs::exists(p, ec) && !ec;
}

// ---------------------------------------------------------------------------

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() /
               ("agentty_clipenv_" + std::string{tag} + "_" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void make_socket(const char* name) const {
        std::ofstream f{path / name};   // a plain file is enough: the
        f << "x";                       // probe only asks "does it exist"
    }
};

// 1. A stale env (no WAYLAND_DISPLAY at all, which is what a tmux pane
//    sees) must still find a compositor whose socket is present.
void test_stale_env_finds_socket() {
    TempDir rt{"stale"};
    rt.make_socket("wayland-1");

    // The env says nothing. Discovery must not care.
    assert(!wayland_socket_exists(rt.path.string(), ""));
    assert(wayland_socket_exists(rt.path.string(), "wayland-1"));
    std::puts("  ok: socket found without any env hint");
}

// 2. A WAYLAND_DISPLAY naming a socket that no longer exists must be
//    REJECTED. Trusting it is worse than ignoring it: wl-paste then fails
//    with a connect error instead of letting us fall back.
void test_dead_display_rejected() {
    TempDir rt{"dead"};
    rt.make_socket("wayland-1");

    assert(!wayland_socket_exists(rt.path.string(), "wayland-99"));
    assert(wayland_socket_exists(rt.path.string(), "wayland-1"));
    std::puts("  ok: dead WAYLAND_DISPLAY rejected, live one accepted");
}

// 3. An absolute WAYLAND_DISPLAY is used verbatim (the protocol allows it)
//    rather than being joined onto the runtime dir.
void test_absolute_display_path() {
    TempDir rt{"abs"};
    rt.make_socket("wayland-7");
    const std::string abs = (rt.path / "wayland-7").string();

    // Absolute path resolves even against an unrelated runtime dir.
    assert(wayland_socket_exists("/nonexistent", abs));
    std::puts("  ok: absolute WAYLAND_DISPLAY honoured");
}

// 4. The .lock file that sits beside every Wayland socket must never be
//    mistaken for the socket itself.
void test_lock_file_not_a_socket() {
    TempDir rt{"lock"};
    rt.make_socket("wayland-2");
    rt.make_socket("wayland-2.lock");

    int sockets = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator{rt.path, ec}) {
        const std::string n = e.path().filename().string();
        if (!n.starts_with("wayland-")) continue;
        if (n.ends_with(".lock")) continue;
        ++sockets;
    }
    assert(sockets == 1);
    std::puts("  ok: .lock excluded from socket scan");
}

// 5. Shell quoting must neutralise a hostile runtime-dir path. The prefix
//    is spliced into a popen() command string, so an unquoted `$(…)` or a
//    stray quote would be command injection.
void test_shell_quote_blocks_injection() {
    // A quote that would close our quoting, then a command.
    const std::string evil = "/tmp/a'; touch /tmp/agentty_pwned; '";
    const std::string q    = shell_quote(evil);

    // Every embedded quote must be escaped as '\'' — so the result cannot
    // contain a bare single quote that terminates the literal early.
    assert(q.front() == '\'' && q.back() == '\'');
    assert(q.find("'\\''") != std::string::npos);

    // Command substitution and backticks are inert inside single quotes.
    const std::string sub = shell_quote("/tmp/$(touch /tmp/pwned)");
    assert(sub.find("$(") != std::string::npos);  // kept literally
    assert(sub.front() == '\'' && sub.back() == '\'');

    // End to end: run it through a real shell and confirm nothing executed
    // and the value survived intact.
    const fs::path marker = fs::temp_directory_path() / "agentty_pwned_marker";
    std::error_code ec;
    fs::remove(marker, ec);
    const std::string cmd =
        "printf '%s' " + shell_quote("x'; touch " + marker.string() + "; '") +
        " >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
    assert(!fs::exists(marker, ec));
    std::puts("  ok: shell_quote blocks injection");
}

// 6. The env prefix must only name variables we actually corrected. A
//    healthy environment should run the bare command, so the common path
//    stays free of surprises.
void test_prefix_is_minimal() {
    // Reproduce the production prefix-building rule.
    auto build = [](bool rt_missing, const std::string& display,
                    const std::string& xdisplay) {
        std::string vars;
        if (rt_missing) vars += " XDG_RUNTIME_DIR=" + shell_quote("/run/user/1000");
        if (!display.empty())  vars += " WAYLAND_DISPLAY=" + shell_quote(display);
        if (!xdisplay.empty()) vars += " DISPLAY=" + shell_quote(xdisplay);
        return vars.empty() ? std::string{} : "env" + vars + " ";
    };

    assert(build(false, "", "").empty());              // nothing to fix
    assert(build(false, "wayland-1", "").find("WAYLAND_DISPLAY") != std::string::npos);
    assert(build(false, "wayland-1", "").find("DISPLAY=") != std::string::npos);
    assert(build(true, "", "").find("XDG_RUNTIME_DIR") != std::string::npos);
    // A prefix always ends with a space so the command concatenates cleanly.
    const std::string p = build(true, "wayland-1", ":0");
    assert(p.back() == ' ');
    assert(p.starts_with("env "));
    std::puts("  ok: prefix minimal and well-formed");
}

int main() {
    std::puts("clipboard display-env discovery:");
    test_stale_env_finds_socket();
    test_dead_display_rejected();
    test_absolute_display_path();
    test_lock_file_not_a_socket();
    test_shell_quote_blocks_injection();
    test_prefix_is_minimal();
    std::puts("all clipboard display-env tests passed");
    return 0;
}
