#pragma once
// agentty::util::storage_env — the `dirs` block in settings.json.
//
// Every storage directory can be moved with an env var, AGENTTY_<NAME>_DIR.
// Writing the same thing into settings.json is easier to keep:
//
//   "dirs": { "threads": "/mnt/big/agentty-threads", "logs": "/tmp/agentty" }
//
// main() reads that block through settings_dir_assignments() and sets the
// env vars, so every resolver keeps reading one source. A var already set
// in the environment wins. It runs first thing in main(), before any thread
// exists, because setenv racing a getenv is undefined.
//
// settings.json itself is found through $AGENTTY_HOME (or ~/.agentty), so
// that one can only be an env var.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::util {

struct DirKey {
    std::string_view key;   // name under "dirs" in settings.json
    std::string_view env;   // the variable it sets
};

// Every overridable directory, in the order docs/website/storage.md lists them.
[[nodiscard]] std::span<const DirKey> dir_keys() noexcept;

// What apply_settings_dirs would set, without touching the environment.
// Skips keys whose env var is already set. Pure apart from reading the
// file, so tests can check it.
struct DirAssignment { std::string env, value; };
[[nodiscard]] std::vector<DirAssignment> settings_dir_assignments();

// The env var name main() set from settings.json, so a command can tell the
// user's own variable from one we filled in. Marker: AGENTTY_DIRS_FROM_SETTINGS
// holds a space-separated list of them.
inline constexpr const char* kFromSettingsVar = "AGENTTY_DIRS_FROM_SETTINGS";
[[nodiscard]] bool set_by_user(std::string_view env);

}  // namespace agentty::util
