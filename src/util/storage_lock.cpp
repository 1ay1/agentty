// util/storage_lock.cpp — the "agentty is running" lock. See the header.

#include "agentty/util/storage_lock.hpp"

#include "agentty/util/user_root.hpp"

namespace agentty::util {

namespace {
std::string target() {
    const auto r = user_root();
    return r.empty() ? std::string{} : (r / "agentty.running").string();
}
}  // namespace

std::optional<maya::platform::native_file_lock> hold_store_shared() {
    const auto t = target();
    if (t.empty()) return std::nullopt;
    auto got = maya::platform::native_file_lock::try_acquire(t, maya::platform::lock_mode::shared);
    if (!got || !*got) return std::nullopt;   // a `config move` mid-flight, or no locking
    return std::move(**got);
}

std::optional<maya::platform::native_file_lock> try_store_exclusive() {
    const auto t = target();
    if (t.empty()) return std::nullopt;
    auto got = maya::platform::native_file_lock::try_acquire(t, maya::platform::lock_mode::exclusive);
    if (!got || !*got) return std::nullopt;
    return std::move(**got);
}

}  // namespace agentty::util
