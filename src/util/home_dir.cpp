// agentty::util::home_dir — see the header for the MSYS2 split-brain rationale.
#include "agentty/util/home_dir.hpp"

#include <cstdlib>

namespace agentty::util {

namespace fs = std::filesystem;

fs::path home_dir() {
    if (auto p = home_dir_or_empty(); !p.empty()) return p;
    return fs::current_path();
}

fs::path home_dir_or_empty() {
    // $HOME first (POSIX/MSYS2/mintty/Cygwin), then $USERPROFILE (native
    // Windows). Empty when neither is set — the caller decides what that means.
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home);
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return fs::path(up);
    return {};
}

} // namespace agentty::util
