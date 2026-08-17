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

#include "agentty/tool/util/sandbox.hpp"

#include <cstdio>
#include <string>

namespace sb = agentty::tools::util::sandbox;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

int main() {
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

    if (g_fails) { std::printf("%d check(s) FAILED\n", g_fails); return 1; }
    std::printf("All sandbox escape tests passed.\n");
    return 0;
}
