#pragma once
// agentty/util/lock_levels.hpp — the order agentty's nesting locks are
// taken in, in one place.
//
// jaal checks every guarded<T> against this (kernel/lock_order.hpp): a lock
// may only be taken while holding ones of a LOWER level. A plain guarded is
// a leaf: taken last, and never held with another leaf. The locks below are
// the ones whose body takes another lock, outermost first.

#include <maya/runtime.hpp>

namespace agentty::lock_levels {

// Serialises whole MCP connect+publish rounds; everything else runs inside.
inline constexpr maya::lock_level kMcpConnecting{10, "mcp.connecting"};

// A shared-file lane: its body reads and writes the state it protects.
inline constexpr maya::lock_level kFileLane{20, "persistence.file_lane"};

// A worker slot: its body touches the state its worker drains.
inline constexpr maya::lock_level kWorkerSlot{20, "worker_slot"};

// The RAG index: held for a whole retrieve/refresh, which reads the skills
// catalog, the memory store and other caches inside.
inline constexpr maya::lock_level kRagIndex{20, "rag.index"};

// A memory scope file's read-modify-write (takes the id rng inside). Read
// by a RAG reindex, so it ranks above the index.
inline constexpr maya::lock_level kMemoryStore{25, "memory.store"};

// State a lane or slot body touches, which itself is the outer lock for
// nothing but leaves.
inline constexpr maya::lock_level kWriterState{30, "persistence.writer_state"};
inline constexpr maya::lock_level kSettingsCache{30, "settings_cache.state"};

}  // namespace agentty::lock_levels
