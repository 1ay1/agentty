// agentty::util::user_root — impl. See the header for the single-root
// rationale (why ~/.agentty and not $XDG_CONFIG_HOME/agentty).
#include "agentty/util/user_root.hpp"
#include "agentty/util/home_dir.hpp"

#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <system_error>

#ifndef _WIN32
#  include <sys/stat.h>   // chmod
#endif

namespace agentty::util {

namespace fs = std::filesystem;

namespace {

// Resolve the root WITHOUT side effects (shared by user_root and the
// migration probe).
fs::path resolve_root() {
    if (const char* h = std::getenv("AGENTTY_HOME"); h && *h) return fs::path{h};
    fs::path home = home_dir();
    if (home.empty()) return {};
    return home / ".agentty";
}

// The legacy split-root location: $XDG_CONFIG_HOME/agentty or
// ~/.config/agentty. Pre-consolidation this held credentials, accounts,
// provider OAuth files + locks, the models.dev cache and the update
// stamp. Resolved WITHOUT creating anything.
fs::path legacy_config_dir() {
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = xdg;
    else {
        fs::path home = home_dir();
        if (home.empty()) return {};
        base = home / ".config";
    }
    return base / "agentty";
}

// Copy+verify one file into `to_dir`, then best-effort remove the source.
// Never overwrites an existing destination (a newer file under the new
// layout always wins over a stale legacy copy). Never throws.
void migrate_file(const fs::path& from, const fs::path& to_dir) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) return;
    const fs::path to = to_dir / from.filename();
    if (fs::exists(to, ec)) {                 // already migrated / re-created
        fs::remove(from, ec);                 // drop the stale legacy copy
        return;
    }
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) return;                           // leave the source; retry next run
    // Verify the copy landed with the same size before deleting the
    // original — ~/.config → ~/.agentty can cross filesystems, so a
    // plain rename() is not available and a truncated copy must not
    // destroy the only good bytes.
    std::error_code e1, e2;
    if (fs::file_size(to, e1) != fs::file_size(from, e2) || e1 || e2) {
        fs::remove(to, ec);
        return;
    }
#ifndef _WIN32
    // Credentials keep owner-only bits; harmless for the cache files.
    ::chmod(to.c_str(), S_IRUSR | S_IWUSR);
#endif
    fs::remove(from, ec);
}

// One-time (per install; idempotent per process) migration from the
// legacy ~/.config/agentty split root. Secrets land in credentials/,
// caches in cache/. Anything unrecognized is left where it is — this
// migrates the files agentty itself wrote, nothing else.
void migrate_legacy_config(const fs::path& root) {
    std::error_code ec;
    const fs::path legacy = legacy_config_dir();
    if (legacy.empty() || !fs::is_directory(legacy, ec)) return;

    const fs::path creds = root / "credentials";
    const fs::path cache = root / "cache";
    fs::create_directories(creds, ec);
    fs::create_directories(cache, ec);
#ifndef _WIN32
    ::chmod(creds.c_str(), S_IRWXU);
#endif

    // Secrets + per-provider state → credentials/. The .lock companions
    // are advisory-lock inodes; migrating them keeps lock paths beside
    // the files they guard.
    for (const char* name : {"credentials.json", "credentials.json.lock",
                             "accounts.json",
                             "codex_credentials.json", "codex_credentials.json.lock",
                             "copilot_credentials.json", "copilot_credentials.json.lock",
                             "kimi_credentials.json", "kimi_credentials.json.lock",
                             "kimi_device_id"})
        migrate_file(legacy / name, creds);

    // Refetchable → cache/.
    for (const char* name : {"modelsdev.json", "update_check.json"})
        migrate_file(legacy / name, cache);

    // Diagnostics → logs/ (append-only; a merge would interleave sessions,
    // so move-if-absent is the right semantics here too). agentty.log is
    // the pre-consolidation root-level sink; stderr.log lived in the
    // legacy config dir.
    const fs::path logs = root / "logs";
    fs::create_directories(logs, ec);
    migrate_file(legacy / "stderr.log", logs);
    migrate_file(root / "agentty.log", logs);

    // Retire the legacy dir if the migration emptied it. Leave it alone
    // if the user (or another tool) put anything else there.
    if (fs::is_directory(legacy, ec) && fs::is_empty(legacy, ec))
        fs::remove(legacy, ec);
}

}  // namespace

fs::path user_root() {
    static std::once_flag once;
    fs::path root = resolve_root();
    if (root.empty()) return root;

    // ── Test-isolation tripwire ──────────────────────────────────
    // A test binary that touches the DEVELOPER'S REAL ~/.agentty writes
    // into their live credential store, account registry and threads.
    // That is exactly what happened when the single-root consolidation
    // repointed config_dir() at AGENTTY_HOME while five credential tests
    // still isolated via XDG_CONFIG_HOME — their sandbox silently became
    // a no-op and the suite mutated real accounts.
    //
    // Under AGENTTY_UNDER_TEST (set by both test mains), abort if the
    // resolved root is the REAL one. "Real" is judged against the
    // process's actual home as recorded at first use — a test that
    // repoints $HOME at a temp dir (the common pattern for skills /
    // hooks / settings coverage) is properly isolated and must pass,
    // even though it sets no $AGENTTY_HOME.
    if (const char* t = std::getenv("AGENTTY_UNDER_TEST"); t && *t) {
        // Captured ONCE, before any test mutates $HOME, so a later
        // setenv("HOME", tmp) can't launder the real path.
        static const fs::path real_root = [] {
            if (const char* ah = std::getenv("AGENTTY_HOME"); ah && *ah)
                return fs::path{};        // already sandboxed by the harness
            fs::path h = home_dir();
            return h.empty() ? fs::path{} : h / ".agentty";
        }();
        if (!real_root.empty() && root == real_root) {
            std::fprintf(stderr,
                "\n[agentty] FATAL: a test reached the real user root (%s).\n"
                "Isolate it — setenv AGENTTY_HOME (or HOME) to a temp dir — "
                "otherwise it mutates real credentials/threads.\n\n",
                root.string().c_str());
            std::abort();
        }
    }

    std::error_code ec;
    fs::create_directories(root, ec);
#ifndef _WIN32
    // Owner-only: the root holds conversation history, learned memory and
    // (post-consolidation) credentials. One chmod is the whole security
    // story of the single-root design.
    ::chmod(root.c_str(), S_IRWXU);
#endif
    std::call_once(once, [&] { migrate_legacy_config(root); });
    return root;
}

fs::path user_credentials_dir() {
    fs::path p = user_root();
    if (p.empty()) return p;
    p /= "credentials";
    std::error_code ec;
    fs::create_directories(p, ec);
#ifndef _WIN32
    ::chmod(p.c_str(), S_IRWXU);
#endif
    return p;
}

fs::path user_cache_dir() {
    fs::path p = user_root();
    if (p.empty()) return p;
    p /= "cache";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path user_logs_dir() {
    fs::path p = user_root();
    if (p.empty()) return p;
    p /= "logs";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

}  // namespace agentty::util
