// src/io/shared_file.cpp — see include/agentty/io/shared_file.hpp

#include "agentty/io/shared_file.hpp"

#include <set>
#include <string>

#include "agentty/util/logx.hpp"

namespace agentty::persistence {

namespace {

// Log the degrade ONCE per file. A lock that cannot be taken on a read-only
// filesystem cannot be taken on the next save either, and a line per save
// would bury the log in a message the user can do nothing about.
bool first_failure_for(const std::string& path) {
    static std::mutex            mu;
    static std::set<std::string> seen;
    const std::lock_guard        lk(mu);
    return seen.insert(path).second;
}

}  // namespace

SharedFile::SharedFile(std::mutex& local, const std::filesystem::path& target)
    : local_(local) {
    // Mutex first, file lock second, always this order. There is only ever
    // one SharedFile held at a time, so this is the whole lock-order story.
    auto got = jaal::platform::native_file_lock::acquire(target.string());
    if (got) {
        cross_.emplace(std::move(*got));
        return;
    }
    // Degrade to the in-process guarantee. Same behaviour agentty had before
    // the cross-process lock existed, so a filesystem that cannot lock is
    // slower to notice a conflict rather than unable to save.
    const std::string t = target.string();
    if (first_failure_for(t))
        AGT_LOG(Persist, Warn, "shared_file.degraded",
                "file={} err={} — in-process guarantee only; a second "
                "agentty instance can interleave on this file",
                t, got.error().what);
}

}  // namespace agentty::persistence
