// user_root_test.cpp — the single per-user root (~/.agentty) + the one-time
// legacy ~/.config/agentty migration. See util/user_root.hpp for the layout
// and the why-not-.config rationale this test pins down.
//
// Drives user_root() against a fabricated $HOME/$XDG_CONFIG_HOME sandbox:
//   1. fresh install  → root created 0700 with no legacy dir touched;
//   2. legacy install → credentials/caches/logs migrated into place
//      (copy+verify+delete), legacy dir retired when emptied;
//   3. idempotence    → a second call migrates nothing and clobbers nothing;
//   4. newer-file-wins → an existing file under the new layout is never
//      overwritten by a stale legacy copy;
//   5. $AGENTTY_HOME  → overrides the root wholesale.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>   // getpid
#endif

#include "agentty/util/user_root.hpp"

namespace fs = std::filesystem;

namespace {

fs::path g_sandbox;

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    // Sandbox: unique tmp home so the test never touches the real ~.
    g_sandbox = fs::temp_directory_path() /
                ("agentty_user_root_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(g_sandbox, ec);
    const fs::path home = g_sandbox / "home";
    fs::create_directories(home);
    ::setenv("HOME", home.c_str(), 1);
    ::unsetenv("AGENTTY_HOME");
    ::unsetenv("XDG_CONFIG_HOME");

    // ── 2+3+4: legacy install (built BEFORE the first user_root() call,
    // because the migration is once-per-process). ─────────────────────
    const fs::path legacy = home / ".config" / "agentty";
    write_file(legacy / "credentials.json", "SECRET");
    write_file(legacy / "accounts.json", "ACCOUNTS");
    write_file(legacy / "modelsdev.json", "MODELS");
    write_file(legacy / "update_check.json", "UPDATE");
    write_file(legacy / "stderr.log", "STDERR");
    write_file(legacy / "kimi_device_id", "DEVID");
    // Legacy root-level log (pre-consolidation ~/.agentty/agentty.log).
    write_file(home / ".agentty" / "agentty.log", "OLDLOG");
    // 4: the new layout already has a FRESH credentials.json — the stale
    // legacy copy must NOT clobber it.
    write_file(home / ".agentty" / "credentials" / "credentials.json", "FRESH");

    const fs::path root = agentty::util::user_root();
    if (root != home / ".agentty") return fail("root != ~/.agentty");

#ifndef _WIN32
    struct stat st{};
    if (::stat(root.c_str(), &st) != 0) return fail("root missing");
    if ((st.st_mode & 0777) != 0700) return fail("root not 0700");
#endif

    // Migrated secrets → credentials/ …
    if (read_file(root / "credentials" / "accounts.json") != "ACCOUNTS")
        return fail("accounts.json not migrated to credentials/");
    if (read_file(root / "credentials" / "kimi_device_id") != "DEVID")
        return fail("kimi_device_id not migrated");
    // …4: without clobbering the fresh file…
    if (read_file(root / "credentials" / "credentials.json") != "FRESH")
        return fail("stale legacy credentials clobbered a fresh file");
    // …caches → cache/, diagnostics → logs/ (both dirs).
    if (read_file(root / "cache" / "modelsdev.json") != "MODELS")
        return fail("modelsdev.json not migrated to cache/");
    if (read_file(root / "cache" / "update_check.json") != "UPDATE")
        return fail("update_check.json not migrated to cache/");
    if (read_file(root / "logs" / "stderr.log") != "STDERR")
        return fail("stderr.log not migrated to logs/");
    if (read_file(root / "logs" / "agentty.log") != "OLDLOG")
        return fail("agentty.log not moved into logs/");
    if (fs::exists(root / "agentty.log")) return fail("root-level agentty.log left behind");

    // Legacy dir must be emptied of everything agentty owned and retired.
    if (fs::exists(legacy / "credentials.json")) return fail("legacy secret left behind");
    if (fs::exists(legacy)) return fail("emptied legacy dir not retired");

    // Subdir accessors resolve inside the root, credentials 0700.
    if (agentty::util::user_credentials_dir() != root / "credentials")
        return fail("user_credentials_dir mismatch");
    if (agentty::util::user_cache_dir() != root / "cache")
        return fail("user_cache_dir mismatch");
    if (agentty::util::user_logs_dir() != root / "logs")
        return fail("user_logs_dir mismatch");
#ifndef _WIN32
    if (::stat((root / "credentials").c_str(), &st) != 0
        || (st.st_mode & 0777) != 0700)
        return fail("credentials/ not 0700");
#endif

    // 3: idempotence — second call (same process; the once-flag makes it a
    // pure resolve) must not disturb anything.
    if (agentty::util::user_root() != root) return fail("second call diverged");
    if (read_file(root / "credentials" / "credentials.json") != "FRESH")
        return fail("second call clobbered credentials");

    // 5: $AGENTTY_HOME overrides the root wholesale.
    const fs::path override_root = g_sandbox / "custom_root";
    ::setenv("AGENTTY_HOME", override_root.c_str(), 1);
    if (agentty::util::user_root() != override_root)
        return fail("AGENTTY_HOME override ignored");
    if (agentty::util::user_cache_dir() != override_root / "cache")
        return fail("cache dir ignores AGENTTY_HOME");

    // ── 6: per-category overrides (issue #58) ─────────────────────────
    // The ask is "config on the system disk, bulk on a second drive", so
    // the thing to pin is the SPLIT: the three bulk dirs move and the two
    // small ones do not.
    const fs::path drive2 = g_sandbox / "drive2";
    ::setenv("AGENTTY_CACHE_DIR", (drive2 / "c").c_str(), 1);
    ::setenv("AGENTTY_LOGS_DIR", (drive2 / "l").c_str(), 1);
    ::setenv("AGENTTY_THREADS_DIR", (drive2 / "t").c_str(), 1);
    if (agentty::util::user_cache_dir() != drive2 / "c")
        return fail("AGENTTY_CACHE_DIR ignored");
    if (agentty::util::user_logs_dir() != drive2 / "l")
        return fail("AGENTTY_LOGS_DIR ignored");
    if (agentty::util::user_threads_dir() != drive2 / "t")
        return fail("AGENTTY_THREADS_DIR ignored");
    if (!fs::is_directory(drive2 / "t"))
        return fail("override dir not created");
    // The whole point of the split: secrets and settings stay put. An
    // override that dragged credentials onto a removable drive would be
    // the opposite of what was asked for.
    if (agentty::util::user_credentials_dir() != override_root / "credentials")
        return fail("credentials followed a bulk override");
    if (agentty::util::user_root() != override_root)
        return fail("root moved under a subdir override");
#ifndef _WIN32
    // Threads are conversation history; an override must not land them
    // world-readable just because the user pre-made the directory 0755.
    if (::stat((drive2 / "t").c_str(), &st) != 0 || (st.st_mode & 0777) != 0700)
        return fail("overridden threads dir not 0700");
#endif

    // 7: a RELATIVE override resolves against the root, not the CWD.
    // Otherwise one setting would mean a different directory for every
    // directory agentty is launched from.
    ::setenv("AGENTTY_LOGS_DIR", "logs2", 1);
    if (agentty::util::user_logs_dir() != override_root / "logs2")
        return fail("relative override not anchored at the root");

    // 8: an UNUSABLE override falls back to the default rather than
    // leaving the caller with no directory at all. (It also warns, which
    // is what keeps the fallback from being a silent downgrade.)
    const fs::path blocker = g_sandbox / "blocker";
    write_file(blocker, "not a directory");
    ::setenv("AGENTTY_CACHE_DIR", blocker.c_str(), 1);
    if (agentty::util::user_cache_dir() != override_root / "cache")
        return fail("unusable override did not fall back");

    // 9: empty means UNSET, not "use the empty path". An exported-but-
    // blank var is a common shell accident and must not relocate anything.
    ::setenv("AGENTTY_LOGS_DIR", "", 1);
    if (agentty::util::user_logs_dir() != override_root / "logs")
        return fail("empty override not treated as unset");

    // 10: credentials and state move only with their own variable.
    ::setenv("AGENTTY_CREDENTIALS_DIR", (drive2 / "secrets").c_str(), 1);
    ::setenv("AGENTTY_STATE_DIR", (drive2 / "st").c_str(), 1);
    if (agentty::util::user_credentials_dir() != drive2 / "secrets")
        return fail("AGENTTY_CREDENTIALS_DIR ignored");
    if (agentty::util::user_state_dir() != drive2 / "st")
        return fail("AGENTTY_STATE_DIR ignored");
#ifndef _WIN32
    if (::stat((drive2 / "secrets").c_str(), &st) != 0 || (st.st_mode & 0777) != 0700)
        return fail("overridden credentials dir not 0700");
#endif
    ::unsetenv("AGENTTY_CREDENTIALS_DIR");
    ::unsetenv("AGENTTY_STATE_DIR");
    if (agentty::util::user_state_dir() != override_root / "state")
        return fail("state default not under the root");

    ::unsetenv("AGENTTY_CACHE_DIR");
    ::unsetenv("AGENTTY_LOGS_DIR");
    ::unsetenv("AGENTTY_THREADS_DIR");
    ::unsetenv("AGENTTY_HOME");

    fs::remove_all(g_sandbox, ec);
    std::printf("PASS: single-root layout, legacy migration, idempotence, "
                "fresh-wins, AGENTTY_HOME override, per-category overrides\n");
    return 0;
}
