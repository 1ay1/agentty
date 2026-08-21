// agentty::util::home_dir — see the header for the MSYS2 split-brain rationale.
#include "agentty/util/home_dir.hpp"

#include <cstdlib>

namespace agentty::util {

namespace fs = std::filesystem;

fs::path home_dir() {
    // $HOME first: it's set by MSYS2/mintty/Cygwin and by every POSIX shell,
    // and is unset on native Windows (where $USERPROFILE is the home). This
    // ordering keeps a mintty user's `~` and agentty's own roots pointing at
    // the same place, and is a no-op change on Linux/macOS.
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home);
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return fs::path(up);
    return fs::current_path();
}

} // namespace agentty::util
