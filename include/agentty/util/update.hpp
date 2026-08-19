#pragma once
// agentty::update — self-update against GitHub releases.
//
// Design goals, in order:
//   1. NEVER slow anything down. The TUI check runs on a background thread,
//      deferred past first paint, rate-limited by a 24 h on-disk cache, and
//      its only effect is one Msg dispatch. No network on the render path.
//   2. Make updating trivial. `agentty update` from any shell; `/update` in
//      the palette (surfaced ONLY when an update is actually available);
//      an unobtrusive "⬆ v0.3.1" chip in the status bar as the signal.
//   3. Fail safe. Download to a sibling temp file, verify, atomic rename
//      over the current binary (the running process keeps its old image —
//      classic Unix; on Windows the old file is moved aside first). Any
//      failure leaves the installed binary untouched.
//
// Channel: the GitHub releases of 1ay1/agentty, single-file binaries named
// agentty-<os>-<arch>[.exe]. Package-manager installs (rpm/pacman/ebuild)
// are detected by install-path heuristics and get a "use your package
// manager" message instead of a self-replace.

#include <functional>
#include <optional>
#include <string>

namespace agentty::update {

// Result of a version check.
struct CheckResult {
    bool        update_available = false;
    std::string current;        // e.g. "0.3.0"
    std::string latest;         // e.g. "0.3.1" (empty on error)
    std::string url;            // browser URL of the release page
    std::string error;          // non-empty ⇒ the check itself failed
};

// Compare two dotted versions ("0.3.0" vs "0.3.1"); true if b is newer.
// Tolerates a leading 'v' and trailing junk. Missing components are 0.
[[nodiscard]] bool version_less(const std::string& a, const std::string& b);

// The compiled-in version ("0.3.0").
[[nodiscard]] std::string current_version();

// Query GitHub for the latest release tag. `force` bypasses the 24 h cache.
// Network round-trip — call from a worker thread, never the UI thread.
[[nodiscard]] CheckResult check_latest(bool force = false);

// Non-blocking: the last CACHED check result, if it exists and is fresh.
// Reads one small JSON file; safe for startup.
[[nodiscard]] std::optional<CheckResult> cached_check();

// The release-asset name for this build's platform ("agentty-linux-x86_64"),
// or empty when the platform has no prebuilt asset.
[[nodiscard]] std::string platform_asset();

// Is the running binary self-replaceable? False for package-manager installs
// (/usr/bin, /usr/local from rpm layouts) where the right path is the
// package manager. `reason` explains when false.
[[nodiscard]] bool self_update_possible(std::string& reason);

// Perform the update: download the platform asset for `tag` and atomically
// replace the running binary. `progress` (optional) is called with
// (downloaded_bytes, total_bytes) — total 0 when unknown.
// Returns an empty string on success, otherwise a human error message.
[[nodiscard]] std::string perform_update(
    const std::string& tag,
    const std::function<void(std::size_t, std::size_t)>& progress = {});

} // namespace agentty::update
