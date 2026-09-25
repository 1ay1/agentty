#pragma once
// agentty::app::settings_cache — the write-behind cache behind Deps::save_settings.
//
// THE PROBLEM. Settings are written from ~8 reducers: profile cycle, Smart
// Mode toggle, slot clear, provider switch, model select, quit, RAG commit,
// login. Each write is a load-modify-save: read + parse settings.json, mutate,
// fsync, rename. That is a disk round-trip, and a reducer runs ON THE UI
// THREAD between two frames — so every one of those interactions hitched, most
// visibly as an animation stalling the moment you flipped a toggle.
//
// The per-call-site fix (wrap this one in a Cmd) is eight chances to forget,
// and a ninth every time someone adds a settings write. So the fix lives at
// the SEAM: `save_settings` publishes to memory and returns; a single worker
// does the IO. A reducer cannot block on settings IO even if it tries.
//
// ── The two invariants that make this safe ───────────────────────────────
//
// 1. READ-YOUR-WRITES. `load_settings` returns the cached value, so the very
//    common load-modify-save inside one reducer sees what it just wrote. If
//    reads went to disk while writes were in flight, that pattern would
//    silently lose the earlier field.
//
// 2. ORDERED, COALESCED WRITES. One worker applies writes in submission
//    order, so a later save cannot land before an earlier one. Consecutive
//    saves collapse to the newest pending value — the intermediate states of
//    a held-down key are not interesting, only where it stopped.
//
// ── What is NOT solved here ──────────────────────────────────────────────
// A write still in flight at exit would be lost, so `flush()` blocks until the
// queue drains and is called on the quit path. That is the one place a
// synchronous wait is correct: the frame after it is never drawn.
//
// The cache also assumes it is the ONLY writer. Code that writes settings.json
// behind its back (provider::credentials::add_key and the other credential
// helpers call persistence::save_settings directly, because they run on paths
// that have no Deps) leaves the cache serving a value that predates the write
// — and the next save through the seam publishes that stale value back over
// it. A bypassing writer must call `invalidate()`; see the note there.

#include <functional>

#include "agentty/store/store.hpp"

namespace agentty::app::settings_cache {

// Wrap a synchronous (load, save) pair into a write-behind (load, save) pair.
// `install_deps` uses this so every Deps built anywhere gets the behaviour;
// there is no opt-in to forget.
struct Seam {
    std::function<store::Settings()>            load;
    std::function<void(const store::Settings&)> save;
};

[[nodiscard]] Seam wrap(std::function<store::Settings()> load_from_disk,
                        std::function<void(const store::Settings&)> save_to_disk);

// Drop the cached value so the next `load` faults in from disk again.
//
// For code that wrote settings.json WITHOUT going through this seam. The
// canonical case is the credential helpers (provider::credentials::add_key,
// clear_active, ...): they run from CLI paths with no Deps installed, so they
// call persistence::save_settings directly. Without invalidation the cache
// keeps its pre-write copy, and the next reducer save writes that copy back —
// silently undoing the key that was just added.
//
// Drains any queued write first, so an in-flight save cannot land on disk
// AFTER the bypassing write and re-clobber it.
void invalidate() noexcept;

// Block until every queued write has hit disk. Called from the quit path.
// Never throws; safe to call when nothing is queued or nothing was wrapped.
void flush() noexcept;

// Stop the worker and drain. Idempotent; called during teardown.
void shutdown() noexcept;

} // namespace agentty::app::settings_cache
