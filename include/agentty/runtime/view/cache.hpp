#pragma once
// View-side render cache.
//
// Keeps mutable UI state out of the pure domain types (Message, ToolUse).
// The domain describes *what* a conversation is; this cache describes
// *what we've already painted for it* so we can skip rebuilding identical
// Elements every frame.
//
//   • Message markdown — finalized assistant messages whose `text` is
//     immutable. Keyed by (thread_id, msg_idx). Streaming messages carry a
//     separate `StreamingMarkdown` that caches block boundaries so each
//     delta costs O(new_chars) instead of O(total).
//
//   • Turn config + built Element — the FULL maya::Turn::Config + the
//     Element returned by Turn::build() for a settled turn (one that has
//     a successor in the message list, so by construction fully resolved).
//     Caches the agent timeline (every tool card config), permission
//     rows, markdown body. Without this, each frame walks every visible
//     message and rebuilds N tool cards × M turns from scratch; after
//     ~10 turns frame time grows enough that even direct mode feels
//     sluggish, and over an SSH-tunnelled airgap the bigger frames pay
//     per-byte transmission cost. Reusing the cached Element makes the
//     per-frame cost O(active_turn) instead of O(total_turns × tools).
//
// ── Spirit-pure reducer / self-managing cache ──
//
// History:
//   v1: process-global thread_local map keyed by (ThreadId, msg_idx),
//       with free functions like `message_md_cache(tid, idx)` and
//       `evict_thread(tid)`. The reducer called `ui::evict_thread(...)`
//       on thread switch / NewThread / compaction to invalidate stale
//       entries — type-pure (Model unchanged) but spirit-impure
//       (mutated process-global state hidden from the Model).
//
//   v2: ViewCache moved into Model::UI as a mutable member. Reducer
//       mutations were now visible in the returned Model — type-pure
//       AND surface-pure. But the reducer still had to know about a
//       render-side concern: it called `m.ui.view_cache.evict_thread()`
//       to keep stale cache entries from colliding with new ones at
//       the same (thread_id, msg_idx) key after compaction.
//
//   v3 (this file): cache keyed by (thread_id, message.id) — a stable
//       per-Message identifier generated at construction and persisted
//       to disk. Compaction creates new Messages with new IDs, so
//       old entries become orphans naturally; no explicit eviction
//       needed. The reducer never touches the cache anymore. View-side
//       memoization stays through `mutable` on Model::UI::view_cache,
//       which is the standard logical-const pattern: filling a
//       memoization slot doesn't change observable Model behavior
//       (every cache hit returns what a cache miss would have built).
//
// The runtime serializes update + view on one thread by construction,
// so no internal locking is needed.

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <maya/widget/turn.hpp>

#include "agentty/domain/id.hpp"

namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace agentty::ui {

struct MessageMdCache {
    std::shared_ptr<maya::Element>            finalized;
    std::shared_ptr<maya::StreamingMarkdown>  streaming;
};

struct TurnConfigCache {
    std::shared_ptr<maya::Turn::Config>       cfg;
    // Pre-built Element for a settled turn. Storing the BUILT
    // Element (not just the Config) is what keeps a long session's
    // render time flat: maya::Conversation otherwise calls
    // `Turn{cfg}.build()` on every visible Config every frame, which
    // reconstructs the entire turn — header, agent_timeline + every
    // tool card, markdown body, permission rows. Mirrors the
    // agent_session example's `m.frozen` pattern (build once per
    // turn lifetime, render-by-reference forever after).
    //
    // Populated lazily: the conversation_config first turn-Config
    // miss builds the config, the Element-pointer miss then runs
    // Turn::build() and stashes the result here. Continuation flag
    // is mirrored alongside so the host can pass through to maya's
    // built_turns path without re-deriving it.
    std::shared_ptr<maya::Element>            element;
    bool                                      element_continuation = false;
};

// LRU-bounded render cache. Both the markdown render and the turn-config
// caches share one entry per (thread, msg) pair — half-evictions force a
// rebuild anyway, so coupling them simplifies invalidation.
//
// Capacity defaults to 32 entries. With a 100 KB `read` result and a few
// code blocks per turn, a single entry can cost several MiB of heap;
// 256 entries on a long session was observed to retain >1 GiB of
// Element nodes after the underlying tool_calls[].output strings had
// already been compacted out of the conversation. 32 is the sweet spot:
// a turn that scrolls off the visible window will *usually* still be
// cached when the user scrolls back, and a thread switch / compaction
// (which call evict_thread / evict_message directly) reclaims entries
// immediately rather than waiting for LRU pressure that may never come.
class ViewCache {
public:
    ViewCache() = default;

    // Move-only: copy semantics are nonsensical for a render cache and
    // would deep-copy the entire Element graph. Move is cheap (the
    // unordered_map and list have noexcept moves).
    ViewCache(const ViewCache&)            = delete;
    ViewCache& operator=(const ViewCache&) = delete;
    ViewCache(ViewCache&&) noexcept            = default;
    ViewCache& operator=(ViewCache&&) noexcept = default;

    [[nodiscard]] MessageMdCache&  message_md (const ThreadId& tid,
                                               const MessageId& mid);
    [[nodiscard]] TurnConfigCache& turn_config(const ThreadId& tid,
                                               const MessageId& mid);

    // Drop every entry under `tid` whose MessageId isn't in `live`.
    // Use after compaction (which clears most of `messages` while
    // keeping the preserved tail's MessageIds intact) so the dropped
    // pre-compact entries don't sit in the LRU consuming heap until
    // pushed out by new accesses. The post-compact conversation is
    // typically 3-5 messages; without this call, the 32-slot LRU
    // would refill only as new turns arrive, holding pre-compact
    // Element trees indefinitely on a quiet session.
    //
    // Entries belonging to OTHER threads (different `tid`) are left
    // alone — those are still reachable when the user switches back,
    // and the LRU bounds their total footprint independently.
    void retain_messages(const ThreadId& tid,
                         const std::unordered_set<std::string>& live);

    // Override the LRU cap. Capacity 0 is treated as 1 (must hold the
    // current touch). Aside from the targeted retain_messages() above
    // (called once per compaction to drop entries for messages that
    // didn't survive), there's no manual evict_* path — stale entries
    // get pushed out by LRU as the new thread / new post-compaction
    // messages access fresh keys. With the cap at 32 and a few MiB
    // worst-case per entry, the transient memory footprint during a
    // thread switch is bounded at ~tens of MiB until the new thread's
    // accesses reclaim those slots.
    void set_capacity(std::size_t max_entries) noexcept;

private:
    struct Entry {
        MessageMdCache  md;
        TurnConfigCache cfg;
        std::list<std::string>::iterator lru_it;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string>                 lru_;
    std::size_t                            cap_ = 32;

    Entry& touch_(const std::string& key);
    static std::string make_key_(const ThreadId& tid, const MessageId& mid);
};

} // namespace agentty::ui
