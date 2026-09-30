// sandbox_escape_test — the macOS sandbox-exec (SBPL) profile injection guard.
//
// The workspace root is interpolated into the SBPL profile's
// (subpath "<path>") write clause. That path is user/attacker-influenceable
// (--workspace / cwd) and macOS/APFS permits ", \, and control chars in
// directory names. Without escaping, a path like  /tmp/ws")(allow default)("
// would close the string early and inject SBPL syntax. sbpl_escape() must make
// the path inert (escape \ and ") or, for a path that can't be represented in
// an SBPL string at all (control chars), return empty so the caller omits the
// clause and FAILS CLOSED (no workspace write access) rather than emitting a
// broken/injected profile.

#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // workspace_root

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace sb = agentty::tools::util::sandbox;


TEST_CASE("sandbox escape") {
    std::printf("=== sandbox_escape_test ===\n");

    // Ordinary paths pass through unchanged.
    check(sb::sbpl_escape("/Users/me/project") == "/Users/me/project",
          "plain path is unchanged");
    check(sb::sbpl_escape("/tmp/a-b_c.1") == "/tmp/a-b_c.1",
          "safe punctuation is unchanged");
    check(sb::sbpl_escape("") == "", "empty path stays empty");

    // A double quote is backslash-escaped — the injection vector. Without
    // this, everything after the " would be reparsed as SBPL.
    check(sb::sbpl_escape("/tmp/ws\"x") == "/tmp/ws\\\"x",
          "double quote is backslash-escaped");
    check(sb::sbpl_escape("/tmp/a\"b\"c") == "/tmp/a\\\"b\\\"c",
          "every double quote is escaped");

    // The actual attack shape: a path that tries to close the subpath string
    // and inject an (allow default). After escaping, the quotes are inert, so
    // the whole thing stays a single string literal.
    {
        const std::string evil = "/tmp/ws\")(allow default)(\"";
        const std::string esc  = sb::sbpl_escape(evil);
        check(esc.find("\\\"") != std::string::npos, "attack quotes are escaped");
        // No UNescaped quote survives: every " in the result is preceded by \.
        bool clean = true;
        for (std::size_t i = 0; i < esc.size(); ++i)
            if (esc[i] == '"' && (i == 0 || esc[i - 1] != '\\')) clean = false;
        check(clean, "no unescaped quote can terminate the SBPL string");
    }

    // Backslash is escaped too (so it can't escape our escaping).
    check(sb::sbpl_escape("/tmp/a\\b") == "/tmp/a\\\\b",
          "backslash is doubled");
    // A backslash right before a quote must not let the quote slip through:
    // "\\" + "\"" → "\\\\" + "\\\"" (both escaped independently).
    check(sb::sbpl_escape("a\\\"b") == "a\\\\\\\"b",
          "backslash-then-quote: both escaped, quote stays inert");

    // Control characters can't live in an SBPL string → fail closed (empty).
    check(sb::sbpl_escape("/tmp/a\nb").empty(), "newline → empty (fail closed)");
    check(sb::sbpl_escape("/tmp/a\rb").empty(), "carriage return → empty");
    check(sb::sbpl_escape("/tmp/a\tb").empty(), "tab → empty");
    check(sb::sbpl_escape(std::string("/tmp/a\0b", 8)).empty(),
          "embedded NUL → empty");

#if defined(__linux__)
    // ── bwrap hardening (issue #21) ─────────────────────────────────────
    // The wrapped argv must carry the kernel-boundary flags AND the read-only
    // user-toolchain binds, on ANY host (this asserts the ARGV we'd pass to
    // bwrap, so it needs neither bwrap nor user namespaces to run).
    const auto argv = sb::bwrap_argv_for_test("echo hi");
    auto has = [&](std::string_view s) {
        for (const auto& a : argv) if (a == s) return true;
        return false;
    };
    check(has("--unshare-user"),    "bwrap: own user namespace");
    check(has("--unshare-pid"),     "bwrap: own pid namespace");
    check(has("--unshare-ipc"),     "bwrap: own ipc namespace");
    check(has("--unshare-uts"),     "bwrap: own uts namespace");
    check(has("--new-session"),     "bwrap: detached session (no TIOCSTI)");
    check(has("--die-with-parent"), "bwrap: no detached zombies");
    check(has("--share-net"),       "bwrap: network kept (git/npm work)");
    check(has("/usr"),              "bwrap: /usr bound read-only");
    // Issue #21: user-local toolchain roots bound read-only so approved
    // commands can find go/gofmt/cargo/etc. installed outside /usr — and
    // secret dirs are NEVER bound.
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string h = home;
        check(has(h + "/.local/bin"), "bwrap: ~/.local/bin bound (webinstall)");
        check(has(h + "/.cargo/bin"), "bwrap: ~/.cargo/bin bound (rust)");
        check(has(h + "/go/bin"),     "bwrap: ~/go/bin bound (go)");

        // A secret path must never be GRANTED -- but it may legitimately
        // appear as the destination of a MASK, which is the opposite thing.
        //
        // The distinction is the argument before it. `--ro-bind /usr /usr`
        // grants; `--ro-bind /dev/null ~/.ssh` hides. Testing for mere
        // presence in the argv conflated the two: this check went red the
        // moment bwrap learned to mask, for a change that made the sandbox
        // strictly tighter.
        //
        // So: find every occurrence, and require that each one is a mask.
        auto granted = [&](const std::string& p) {
            for (std::size_t i = 0; i < argv.size(); ++i) {
                if (argv[i] != p) continue;
                // Masked as a directory: `--tmpfs <p>`.
                if (i >= 1 && argv[i - 1] == "--tmpfs") continue;
                // Masked as a file: `--ro-bind /dev/null <p>`.
                if (i >= 2 && argv[i - 1] == "/dev/null") continue;
                return true;   // a real bind of the secret itself
            }
            return false;
        };
        check(!granted(h + "/.ssh"),         "bwrap: ~/.ssh never granted");
        check(!granted(h + "/.aws"),         "bwrap: ~/.aws never granted");
        check(!granted(h + "/.config"),      "bwrap: ~/.config never granted");
        check(!granted(h + "/.local/share"), "bwrap: ~/.local/share never granted");

        // And the masks are actually EMITTED, not merely not-granted. A
        // missing mask also passes the check above, so assert the positive:
        // if the path exists on this host, something must be covering it.
        auto masked = [&](const std::string& p) {
            for (std::size_t i = 1; i < argv.size(); ++i)
                if (argv[i] == p &&
                    (argv[i - 1] == "--tmpfs" || argv[i - 1] == "/dev/null"))
                    return true;
            return false;
        };
        std::error_code ec;
        if (std::filesystem::exists(h + "/.ssh", ec))
            check(masked(h + "/.ssh"), "bwrap: ~/.ssh masked when present");

        // The masks must come AFTER the workspace bind, or the bind remounts
        // over them. This is the ordering bug claybin shipped -- masks in the
        // policy, workspace bound on top, secret readable again, every
        // policy-level test green. bwrap applies argv in sequence, so here the
        // ordering IS the argv order and can be asserted directly.
        //
        // Only CREDENTIAL masks count. `--tmpfs /tmp` is a legitimate scratch
        // mount emitted long before the workspace bind, and treating every
        // --tmpfs as a mask made this assertion fire on it -- a false positive
        // that cost a debugging round. So anchor on a path we know is a mask:
        // ~/.ssh, which kAlwaysMasked always covers when it exists.
        std::error_code ec2;
        if (std::filesystem::exists(h + "/.ssh", ec2)) {
            std::size_t ws_at = 0, ssh_at = 0;
            const std::string ws = agentty::tools::util::workspace_root().string();
            for (std::size_t i = 1; i < argv.size(); ++i) {
                if (argv[i - 1] == "--bind" && argv[i] == ws) ws_at = i;
                if (argv[i] == h + "/.ssh" &&
                    (argv[i - 1] == "--tmpfs" || argv[i - 1] == "/dev/null"))
                    ssh_at = i;
            }
            check(ws_at != 0, "bwrap: the workspace is bound");
            check(ssh_at > ws_at,
                  "bwrap: credential masks come after the workspace bind");
        }
    }
    // The command is the tail of the argv (after the closing "--").
    check(!argv.empty() && argv.back() == "echo hi",
          "bwrap: shell cmd is the argv tail");
#endif

}
