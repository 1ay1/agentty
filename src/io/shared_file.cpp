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
    static maya::guarded<std::set<std::string>> seen;
    return seen.with([](std::set<std::string>& s, std::string p) {
        return s.insert(std::move(p)).second;
    }, path);
}

}  // namespace

namespace detail {
std::optional<maya::platform::native_file_lock> acquire_cross(const std::string& t) {
    // Called inside the lane, so the order is always in-process first, file
    // lock second. Only one shared file is ever held at a time, so that is
    // the whole lock-order story.
    auto got = maya::platform::native_file_lock::acquire(t);
    if (got) return std::move(*got);
    // Degrade to the in-process guarantee. Same behaviour agentty had before
    // the cross-process lock existed, so a filesystem that cannot lock is
    // slower to notice a conflict rather than unable to save.
    if (first_failure_for(t))
        AGT_LOG(Persist, Warn, "shared_file.degraded",
                "file={} err={} — in-process guarantee only; a second "
                "agentty instance can interleave on this file",
                t, got.error().what);
    return std::nullopt;
}
}  // namespace detail

}  // namespace agentty::persistence
