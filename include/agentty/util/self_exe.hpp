#pragma once
// agentty::util::self_exe_path — the path of the running agentty binary.
//
// Used where something else must start this same binary again: an editor's
// terminal login method, the self-updater. Empty if the OS won't say.

#include <filesystem>

namespace agentty::util {

[[nodiscard]] std::filesystem::path self_exe_path();

}  // namespace agentty::util
