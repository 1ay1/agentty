#pragma once
// agentty::util::storage_lock — "is agentty using the store right now?"
//
// Every running agentty holds a SHARED lock on <user root>/agentty.running
// for its whole life. Something that rewrites the store under everyone
// (`agentty config move`) takes it EXCLUSIVE, which succeeds only when no
// instance is running. The kernel drops the lock when a process dies, so a
// crash never leaves a stale "running" behind, unlike a pid file.

#include <maya/runtime.hpp>

#include <optional>

namespace agentty::util {

// Held for the session. Empty when the filesystem can't lock; that only
// means `config move` can't prove the store is idle, never a failure here.
[[nodiscard]] std::optional<maya::platform::native_file_lock> hold_store_shared();

// Exclusive, without waiting. nullopt when another agentty is running (or
// locking is unsupported, which `config move` treats the same way: refuse).
[[nodiscard]] std::optional<maya::platform::native_file_lock> try_store_exclusive();

}  // namespace agentty::util
