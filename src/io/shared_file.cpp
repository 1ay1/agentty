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
//
// A snapshot, not a lock: this runs INSIDE a lane's body, and a lock here
// would have to rank above every lane. Lock-free, it ranks nowhere.
bool first_failure_for(const std::string& path) {
    static maya::snapshot<std::set<std::string>> seen;
    if (seen.load()->count(path)) return false;
    const auto after = seen.update([](const std::set<std::string>& s, const std::string& p) {
        auto out = s;
        out.insert(p);
        return out;
    }, path);
    // Two first failures racing both insert; both log once. Rare and harmless.
    return after->count(path) == 1;
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
