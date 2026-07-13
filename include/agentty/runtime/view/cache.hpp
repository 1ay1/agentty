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

#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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
    // Size of the source last fed into `streaming` after settle.
    // Once a settled msg.text matches it, skip the per-frame
    // set_content() / finish() round trip — set_content's bytes-
    // equal fast path is already a no-op but still pays O(text)
    // memcmp every frame for every visible settled turn. SIZE_MAX
    // is the "not yet settled" sentinel.
    //
    // No content hash: msg.text is never rewritten in place after
    // StreamFinished moves streaming_text → text. Edits replace the
    // Message wholesale (new MessageId, new cache slot), so size
    // alone is a sufficient invariant.
    std::size_t                               last_settled_size =
        static_cast<std::size_t>(-1);

    // ── Reveal bookkeeping ──
    //
    // There is no host-side typewriter cursor: cached_markdown_for feeds
    // the FULL set of arrived bytes to the streaming widget every frame
    // and lets maya's reveal_fx animate the live edge. `revealed_size` is
    // therefore always held == source.size(); it survives only so the
    // settled fast-path / freeze snapshot can confirm the whole message
    // has been fed before treating it as final.
    std::size_t                               revealed_size = 0;

    // Wall-clock of the last frame at which `source` grew (new bytes
    // arrived from the wire). Drives the "stream in motion" grace window
    // that keeps the 16 ms animation frame armed while bytes are flowing
    // so reveal_fx animates smoothly. Once the wire goes quiet for longer
    // than the window (a model stall) RAF lapses and the loop falls back
    // to the calmer Tick cadence; the next delta's eventfd wake revives
    // it. Reset to zero on source shrink / roll.
    std::chrono::steady_clock::time_point     last_grow_tick{};
    std::size_t                               last_grow_size = 0;

    // Scratch buffer for multi-sub-turn rendering. When a single
    // Assistant message has produced text in a prior sub-turn (now
    // in `msg.text`) and is now streaming more text in a follow-up
    // sub-turn (in `msg.streaming_text`), we need to feed the
    // widget the CONCATENATION of both so the live tail keeps
    // growing visibly. Holding the joined buffer here gives a
    // stable backing for the string_view fed into
    // set_content_async. Cleared on settle.
    std::string                               combined_source;

    // Sizes of the three components last folded into combined_source.
    // Used by cached_markdown_for to short-circuit the per-frame
    // concat + set_content_async round-trip when NONE of msg.text /
    // streaming_text / pending_stream grew (the dominant case during
    // reveal_fx animation: the widget renders 60 fps but bytes arrive
    // 10-30 / s, so >90% of frames produce no source change).
    //
    // Without this guard the live tail rebuilds an N-byte string AND
    // does an N-byte memcmp every frame for a long sub-turn-2 message
    // (text holds prior sub-turn's settled body, streaming_text grows).
    // O(turn-length) per frame on the in-flight run is what makes the
    // perceived render speed degrade with turn size.
    std::size_t last_text_size           = static_cast<std::size_t>(-1);
    std::size_t last_streaming_size      = static_cast<std::size_t>(-1);
    std::size_t last_pending_size        = static_cast<std::size_t>(-1);

    // Viewport height seen on the previous frame (available_height() from
    // the RenderContext). A DROP between frames is a terminal shrink: the
    // terminal autonomously pushes the top viewport rows into immutable
    // native scrollback. If the live reveal edge (ghost-blanked + scrambled
    // per-codepoint) is among those rows it freezes stale in scrollback and
    // the row is lost. cached_markdown_for snaps the reveal to the edge on
    // such a frame (snap_reveal_to_edge) so no ghosted row exists to strand.
    // 0 is the "first frame / unknown" sentinel (no shrink inferred).
    int last_render_height = 0;

    // ── Tool-panel deferral ──
    //
    // True while cached_markdown_for is holding this message's tool panel
    // OFF-SCREEN because the reveal cursor is still gliding to the live
    // edge. On the wire, content_block_stop(text) and content_block_start
    // (tool_use) are consecutive SSE events — often the same TCP segment —
    // so the pre-emptive end-of-text drain gets ZERO frames to run before
    // a card exists, and the mandatory scrollback-safety hard-snap pastes
    // the whole wire_cps×drain_secs backlog in one frame (the "md sticks
    // then bursts at the tool" symptom; tool_boundary_burst_probe). While
    // this flag is set the view skips append_assistant_tool_panel, so
    // nothing renders below the prose that could push a mid-reveal row
    // into immutable scrollback — the glide is safe and the snap at the
    // card's first paint is a no-op. Recomputed EVERY frame by
    // cached_markdown_for; consumed the same frame (same cfg build) by
    // append_assistant_body_slots / turn_config_for_assistant_run.
    // Messages that never build markdown (tool-only sub-turns) never set
    // it — they have no prose to reveal.
    bool defer_tool_panel = false;
    // First frame the defer engaged — drives the kMaxCardDeferMs hard cap
    // in cached_markdown_for so the card can never hide indefinitely.
    // Zero-initialised = "not deferring".
    std::chrono::steady_clock::time_point card_defer_since{};
    // Two-phase defer exit. When the glide completes, the exit frame runs
    // finish() + fx-off — which MUTATES rows at the viewport bottom (the
    // scramble tail un-ghosts, the trailing paragraph rewraps into the
    // committed block path) — while the panel STAYS hidden, so the frame
    // is mutation-only (diff-repaintable in place). The panel unhides on
    // the NEXT frame, a pure bottom-append grow whose rows crossing the
    // viewport top are all final. Collapsing both into one frame is
    // grow+mutation — maya's HardReset arm (oracle-proven: the one-frame
    // exit added 4 gate recoveries at 60x18).
    bool defer_exit_finished = false;

    // ── Memoized assistant-turn elapsed (header meta) ──
    //
    // assistant_elapsed() reverse-scans m.d.current.messages to the
    // previous User message to compute the turn's wall-clock duration.
    // That scan is O(sub-turns since the last user turn) = O(turn
    // length), run EVERY frame for the run head while the turn is in
    // flight — a genuine per-frame cost that grows with turn depth
    // (the whole in-flight turn stays in the live tail until settle;
    // it is never frozen mid-turn — see frozen.cpp). The value only
    // depends on two immutable timestamps (this head's + the prior
    // user message's), so once computed it never changes. Cache it
    // here keyed on the head message's id and skip the scan on every
    // subsequent frame. `elapsed_valid` distinguishes "cached
    // nullopt" (no prior user turn / out-of-range dt) from "not yet
    // computed".
    bool                 elapsed_valid = false;
    std::optional<float> elapsed_cached{};
};

// Per-(thread, msg) render cache, partitioned by LIFECYCLE.
//
// The cache wears two hats that have genuinely different invariants, and
// conflating them under a single LRU is what produced the whole class of
// "typewriter stalls on a deep run" bugs. So they are split:
//
//   ┌─────────────┬──────────────────────────┬───────────────────────────┐
//   │             │  LIVE (pinned)           │  SETTLED (staged)         │
//   ├─────────────┼──────────────────────────┼───────────────────────────┤
//   │ holds       │  animation state — the   │  a pure render memo — the │
//   │             │  StreamingMarkdown +     │  built Element / md state  │
//   │             │  reveal cursor + defer   │  of a fully-drained sub-   │
//   │             │  machine bookkeeping     │  turn still in the live   │
//   │             │                          │  tail (not yet frozen)    │
//   │ evicting is │  a CORRECTNESS BUG       │  a cheap rebuild — but    │
//   │             │  (destroys the reveal    │  never happens: the entry │
//   │             │  widget mid-glide →      │  is DROPPED at freeze, the│
//   │             │  the typewriter stalls)  │  last frame it is read    │
//   │ count       │  bounded by the ACTIVE   │  bounded by the ACTIVE    │
//   │             │  turn (≈1 streaming edge │  turn (drained sub-turns  │
//   │             │  + its live sub-turns)   │  awaiting the freeze Tick)│
//   │ policy      │  NEVER evicted           │  dropped at freeze        │
//   └─────────────┴──────────────────────────┴───────────────────────────┘
//
// The caller declares which hat an access wants: `message_md_live()`
// pins the entry; `message_md()` / `turn_config()` stage it as a settled
// memo. A message is "live" exactly while its reveal widget is
// live/finalizing/revealing/parsing OR its tool-panel defer machine is
// running — i.e. while evicting it would corrupt on-screen output. The
// view already computes that predicate (see turn.cpp
// `subturn_stably_keyable`, whose negation IS liveness).
//
// ── Why there is NO cap ──
//
// The old design capped the settled side under an LRU, trading
// correctness against RAM: too small and the live edge got evicted
// (stall); too large and settled prose trees hoarded gigabytes. Both the
// dilemma AND the cap are gone, because a settled entry has a PROVABLE
// death instant: the frame its message freezes.
//
// Every settled-accessor call site (cached_markdown_for,
// assistant_elapsed, settle_message_md, the error/cancel salvage) reads
// a message in the LIVE TAIL — the range [frozen_through, size()). The
// moment finalize_turn settles a run and the next Tick calls
// freeze_through(), those messages move ABOVE frozen_through: their rows
// are sealed into m.ui.frozen (the maya ledger, blitted from its own
// component cache) and NO code path ever reads their ViewCache entry
// again — not on the next frame, not on scroll-back (scroll-back is the
// terminal's native scrollback, never a ViewCache rebuild). The entry is
// dead the instant its turn freezes.
//
// So freeze_range() DROPS the entry for every message it seals (see its
// settle-to-drop loop). The settled map therefore only ever holds
// entries for the current, not-yet-frozen tail — bounded by ONE turn,
// exactly like the pinned set. There is nothing to cap: the map is
// self-emptying at every freeze, and a session of ten thousand turns
// carries the same handful of settled entries as a session of one.
//
// A settle is a single migration: the one frame the widget finishes
// draining, the caller stops passing the live flag and the entry moves
// from the pinned map into the settled map (message_md() after a run of
// message_md_live() calls performs that migration, payload preserved);
// a few frames later freeze drops it.
//
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

    // ── Settled accessor (staged, dropped at freeze) ──
    // For an entry whose animation state is fully drained: a pure render
    // memo for a sub-turn still in the live tail. freeze_range() drops it
    // the frame its message freezes (its last read), so the settled map
    // stays bounded by the active turn — no cap, no LRU.
    [[nodiscard]] MessageMdCache&  message_md (const ThreadId& tid,
                                               const MessageId& mid);

    // ── Live accessors (pinned — never evicted) ──
    // For entries holding load-bearing animation state (a live reveal
    // widget or an active defer machine). Pinning lives OUTSIDE the
    // settled map, so no run depth can evict the streaming edge
    // mid-glide. Calling a live accessor on a settled key MIGRATES it to
    // the pinned map (and a settled accessor migrates it back), so a
    // message's slot follows its lifecycle with its payload intact.
    [[nodiscard]] MessageMdCache&  message_md_live (const ThreadId& tid,
                                                    const MessageId& mid);

    // Introspection for probes / assertions: is this key currently
    // pinned (i.e. structurally exempt from eviction)?
    [[nodiscard]] bool is_pinned(const ThreadId& tid,
                                 const MessageId& mid) const noexcept;
    [[nodiscard]] std::size_t pinned_count()  const noexcept { return pinned_.size(); }
    [[nodiscard]] std::size_t settled_count() const noexcept { return entries_.size(); }

    // Drop a message's entry from BOTH homes (pinned and settled),
    // reclaiming its payload immediately. No-op if absent. This is the
    // death instant of a settled entry: freeze_range() calls it on every
    // message it seals, because once a message is frozen its rows live in
    // m.ui.frozen (the maya ledger) and no code path ever reads its
    // ViewCache entry again — so keeping it is pure leak. Dropping from
    // the pinned side too closes the mirror hazard (a widget that stopped
    // animating but never re-entered the render seam to be down-migrated,
    // e.g. error/cancel mid-reveal): a frozen message is, by
    // construction, in NEITHER map. This is what makes the settled map
    // self-emptying and the cap unnecessary.
    void drop(const ThreadId& tid, const MessageId& mid);

    // Non-migrating, non-touching const peek. Returns the message's md
    // slot from whichever home it lives in (pinned or settled), or
    // nullptr if absent. Unlike message_md() / message_md_live(), it
    // does NOT create, migrate, or reorder anything — it is a pure
    // read. Use it for read-only STATE PROBES ("is this widget still
    // animating?") that must not perturb the partition or the LRU
    // order: routing such a probe through a mutating accessor would
    // migrate a live entry down into the LRU (un-pinning the edge) or
    // bump LRU recency spuriously. The pointer is valid until the next
    // mutating cache call.
    [[nodiscard]] const MessageMdCache* peek(const ThreadId& tid,
                                             const MessageId& mid) const noexcept;

    // Drop EVERYTHING — both maps, all threads. Called on a wholesale
    // conversation swap (NewThread / ThreadLoaded / CheckpointRestored),
    // where m.d.current is replaced and every cached (tid,msg) entry
    // belongs to a conversation that is no longer on screen. Without this
    // the previous thread's settled entries would linger forever: freeze
    // only drops entries for the CURRENT thread's messages as they
    // freeze, and a swapped-away thread's messages never freeze again.
    // Since the keys embed thread_id there is no correctness bug (no
    // collision) — only an unbounded-over-session-of-many-threads leak,
    // which this closes. The new thread repopulates from scratch on its
    // first render.
    void clear() noexcept { entries_.clear(); pinned_.clear(); }

private:
    // One payload type, two homes. A key lives in AT MOST ONE of these at
    // a time; migrate_() moves the Entry between them, preserving the
    // MessageMdCache payload (the reveal widget, block cache, defer
    // state) so a settle never drops animation state. Both are bounded by
    // the ACTIVE turn: pinned by the live sub-turns, settled by
    // drained-but-not-yet-frozen sub-turns — freeze drops from settled,
    // so neither grows over a session. No LRU, no cap: the death instant
    // (freeze) is explicit, not amortised.
    //   • entries_ — settled memo, dropped at freeze.
    //   • pinned_  — live state, never evicted, drained → migrates down.
    struct Entry {
        MessageMdCache md;
        bool           pinned = false;
    };
    std::unordered_map<std::string, Entry> entries_;   // settled
    std::unordered_map<std::string, Entry> pinned_;    // live (never evict)

    // Access `key` on the SETTLED side: find-or-insert in entries_. If
    // the key was pinned, migrate it down first (payload preserved).
    Entry& touch_settled_(const std::string& key);
    // Access `key` on the LIVE side: find-or-insert in pinned_. If the
    // key was settled, migrate it up (payload preserved). Never evicts.
    Entry& touch_pinned_(const std::string& key);
    // Move an existing key from one home to the other, carrying its
    // payload. Returns a reference to the entry in its new home.
    Entry& migrate_to_pinned_(const std::string& key);
    Entry& migrate_to_settled_(const std::string& key);

    static std::string make_key_(const ThreadId& tid, const MessageId& mid);
};

} // namespace agentty::ui
