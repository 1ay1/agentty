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

#include <cstdlib>
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
        for (const auto& a : argv) {
            check(a != h + "/.ssh",    "bwrap: ~/.ssh NEVER bound");
            check(a != h + "/.aws",    "bwrap: ~/.aws NEVER bound");
            check(a != h + "/.config", "bwrap: ~/.config NEVER bound");
            check(a != h + "/.local/share", "bwrap: ~/.local/share NEVER bound");
        }
    }
    // The command is the tail of the argv (after the closing "--").
    check(!argv.empty() && argv.back() == "echo hi",
          "bwrap: shell cmd is the argv tail");
#endif

    // ── bastion: the SAME boundary, in the other vocabulary ──────────────
    // Everything above asserts bwrap's MOUNTS. bastion grants path RIGHTS, so
    // none of it constrains the bastion path — and that gap shipped a leak:
    // the bastion policy granted FsRead on "/", which handed ~/.ssh and ~/.aws
    // to any approved command (which also has the network). The bwrap test
    // above kept passing throughout, and both backends reported "active".
    //
    // So the secret-exclusion invariant is asserted for BOTH backends, against
    // the real policy builder rather than a copy of the list.
    {
        const auto scopes = sb::bastion_read_scopes_for_test();
#if defined(__linux__)
        check(!scopes.empty(), "bastion: a read set exists");
#endif
        // The bug, stated directly: root must never be a read scope. Every
        // secret-dir check below is implied by "/", so without this the rest
        // could pass while everything was readable.
        for (const auto& s : scopes)
            check(s != "/", "bastion: read is NEVER granted on / (the leak)");

        if (const char* home = std::getenv("HOME"); home && *home) {
            const std::string h = home;
            // No granted scope may BE or CONTAIN a secret dir. Containment is
            // the real test: a grant of ~ or / reads ~/.ssh without ever
            // naming it, which is exactly how the leak passed review.
            for (const auto& secret : {h + "/.ssh", h + "/.aws",
                                       h + "/.config", h + "/.local/share"}) {
                bool exposed = false;
                for (const auto& s : scopes) {
                    if (s == secret) { exposed = true; break; }
                    // s covers secret when secret is under s.
                    if (secret.size() > s.size()
                        && secret.compare(0, s.size(), s) == 0
                        && (s == "/" || secret[s.size()] == '/')) {
                        exposed = true; break;
                    }
                }
                check(!exposed, ("bastion: " + secret + " is NOT readable").c_str());
            }
        }

        // The floor, so "deny everything" can't pass this test: the toolchain
        // and the TLS trust store must still be reachable, or every sandboxed
        // git/curl breaks and users switch the sandbox off.
        auto granted = [&](std::string_view p) {
            for (const auto& s : scopes) if (s == p) return true;
            return false;
        };
#if defined(__linux__)
        check(granted("/usr"),     "bastion: /usr readable (toolchain)");
        check(granted("/etc/ssl"), "bastion: TLS trust store readable");
        check(granted("/etc/resolv.conf"), "bastion: DNS config readable");
#endif
    }
}
