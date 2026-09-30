// Stubs for sandbox_config_race_test.
//
// The test exercises ONE thing: publishing the sandbox config while readers
// hold snapshots of it. sandbox.cpp calls workspace_root() when it builds a
// posture or a bwrap argv, and the real definition lives in fs_helpers.cpp,
// which includes mcp's fs_helpers and pulls the whole tool layer behind it.
//
// Linking that into a TSan target to satisfy one function would instrument
// several hundred TUs to test a pointer swap. So the seam gets a stub: the
// race test never spawns anything, it only publishes and reads, and nothing
// it asserts depends on what the workspace root is.
//
// Declared by hand rather than by including fs_helpers.hpp, for the same
// reason -- that header is what reaches into mcp.
#include <filesystem>
#include <string_view>

namespace agentty::tools::util {

const std::filesystem::path& workspace_root() {
    static const std::filesystem::path kRoot{"/tmp"};
    return kRoot;
}

}  // namespace agentty::tools::util

namespace agentty::tools::progress {

// subprocess.cpp reports live output through this. The race test never spawns
// a child, so nothing calls it -- it only has to resolve.
void emit(std::string_view) {}

}  // namespace agentty::tools::progress
