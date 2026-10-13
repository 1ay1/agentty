// Stubs shared by the narrow sandbox targets.
//
// sandbox.cpp calls workspace_root() when it builds a posture or a bwrap argv,
// and the real definition lives in fs_helpers.cpp, which includes mcp's
// fs_helpers and pulls the whole tool layer behind it. Linking that in to
// satisfy one function would mean instrumenting several hundred TUs under TSan
// to test a pointer swap, or dragging auth+mcp+teardown into a check that only
// wants to spawn a shell.
//
// So the seam gets a stub. It is SETTABLE rather than hardcoded, because the
// two users want different things:
//
//   sandbox_config_race_test  never spawns; the value is irrelevant.
//   sandbox_live_check        binds the workspace and reads a planted file in
//                             it, so it must be the real directory the check
//                             created -- a hardcoded /tmp would make the
//                             masking case assert against the wrong path and
//                             pass for the wrong reason.
//
// Declared by hand rather than by including fs_helpers.hpp, for the same
// reason -- that header is what reaches into mcp.
#include <filesystem>

#include "agentty/util/io.hpp"
#include <string_view>

namespace agentty::tools::util {

namespace {
std::filesystem::path& stub_root() {
    static std::filesystem::path root{"/tmp"};
    return root;
}
}  // namespace

const std::filesystem::path& workspace_root() { return stub_root(); }

// Test-only setter, matching the real set_workspace_root's name so a reader
// of sandbox.cpp does not have to wonder which one it got.
void set_workspace_root(Io, std::filesystem::path p) { stub_root() = std::move(p); }

}  // namespace agentty::tools::util

namespace agentty::tools::progress {

// subprocess.cpp reports live output through this. Neither target consumes
// progress events, so this only has to resolve.
void emit(std::string_view) {}

}  // namespace agentty::tools::progress
