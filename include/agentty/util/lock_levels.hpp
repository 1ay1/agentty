#pragma once
// agentty/util/lock_levels.hpp — the order agentty's locks are taken in, as
// TYPES the compiler checks.
//
// Every maya::guarded<T, Rank> names its rank. Taking it needs a capability
// proving every lock the thread holds ranks LOWER (jaal core/capability.hpp):
//
//     const maya::unlocked&           the thread holds nothing
//     const maya::held<R>&            a lock body ranked R
//     const maya::below<R>&           a PARAMETER: "my caller holds only ranks < R"
//
// A function that takes a lock ranked R (directly or through anything it
// calls) declares `const maya::below<R>&`, and its callers must be able to
// produce one. So a call made from inside a lock ranked >= R does not
// compile, wherever it is in the call graph. The table below is the order;
// adding a nesting edge means placing the inner lock above the outer one
// here, and the compiler checks every caller.
//
// Most locks are leaves (maya::leaf, the default): their body takes nothing.
// A value read far more often than it changes (UI prefs, catalog facts, the
// active selection) is a maya::snapshot instead: no lock, no rank, readable
// from anywhere including a view and a lock body.
//
// Outermost first. Numbers are spaced so a new rank fits between two.

#include <maya/runtime.hpp>

namespace agentty::lock_levels {

// Serialises whole MCP connect+publish rounds; a round reads OAuth tokens,
// the server list, the tool catalog and persistence inside.
using mcp_connecting = maya::rank<10, "mcp.connecting">;

// Copilot's Auto-session cache. Held across the /models/session probe so
// concurrent turns share one probe, and the probe needs a fresh proxy token.
using copilot_auto = maya::rank<12, "copilot.auto_session">;

// One credential refresh at a time per provider (Claude, Copilot, Kimi,
// Codex, MCP OAuth). Its body reads and writes the credential store (a
// leaf) and does the network refresh.
using auth_refresh = maya::rank<15, "auth.refresh">;

// One credential file's read-modify-write, held across the network refresh
// so other processes adopt the token instead of refreshing again. Entered
// from inside the provider's refresh lane, so it ranks above it.
using cred_file = maya::rank<17, "auth.cred_file">;

// The RAG index: held for a whole retrieve/refresh, which reads the skills
// catalog, the memory store and other caches inside.
using rag_index = maya::rank<20, "rag.index">;

// A memory scope file's read-modify-write (takes the id rng inside). Read
// by a RAG reindex, so it ranks above the index.
using memory_store = maya::rank<25, "memory.store">;

// One thread's log (.jsonl/.ofs/.meta.json) while the drain worker writes
// it. The thread index's lane (file_lane) is taken inside, so it ranks below.
using thread_log = maya::rank<28, "persistence.thread_log">;

// A shared-file lane: one read-modify-write of a file two agentty instances
// share (thread index, settings). Its body takes the cross-process file
// lock and may read persistence caches.
using file_lane = maya::rank<30, "persistence.file_lane">;

// A background worker's slot: its body touches the state its worker drains.
using worker_slot = maya::rank<35, "worker_slot">;

// State a lane or slot body touches, which is itself the outer lock for
// nothing but leaves.
using writer_state   = maya::rank<40, "persistence.writer_state">;
using settings_cache = maya::rank<40, "settings_cache.state">;

// The parameter types, spelled once. A function that takes rank R (or
// anything ranked above it) takes `below_R`.
using below_mcp_connecting = maya::below<mcp_connecting>;
using below_copilot_auto   = maya::below<copilot_auto>;
using below_auth_refresh   = maya::below<auth_refresh>;
using below_cred_file      = maya::below<cred_file>;
using below_rag_index      = maya::below<rag_index>;
using below_memory_store   = maya::below<memory_store>;
using below_thread_log     = maya::below<thread_log>;
using below_file_lane      = maya::below<file_lane>;
using below_worker_slot    = maya::below<worker_slot>;
using below_writer_state   = maya::below<writer_state>;
using below_settings_cache = maya::below<settings_cache>;
// Takes only leaves: callable from any lock body except a leaf's.
using below_leaf           = maya::below<maya::leaf>;

}  // namespace agentty::lock_levels

namespace agentty {
// Short spellings for signatures.
using maya::unlocked;
using maya::held;
using maya::below;
namespace locks = lock_levels;
}  // namespace agentty
