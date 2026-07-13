#include "agentty/runtime/view/cache.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace agentty::ui {

std::string ViewCache::make_key_(const ThreadId& tid, const MessageId& mid) {
    // tid + ':' + mid. Both are hex-shaped strings; the separator means
    // a key can be parsed if we ever need to (currently we don't —
    // the cache treats keys as opaque). Length pre-reserved so the
    // append doesn't reallocate.
    std::string k;
    k.reserve(tid.value.size() + mid.value.size() + 1);
    k.append(tid.value);
    k.push_back(':');
    k.append(mid.value);
    return k;
}

ViewCache::Entry& ViewCache::migrate_to_pinned_(const std::string& key) {
    // Precondition: key is in entries_ (settled). Move its payload into
    // pinned_, unlinking the LRU node. std::list::erase on a valid
    // iterator is O(1); the map node move preserves md/cfg (reveal
    // widget, block cache, defer state) so animation state survives the
    // lifecycle transition intact.
    auto it = entries_.find(key);
    lru_.erase(it->second.lru_it);
    it->second.pinned = true;
    auto [ins, _] = pinned_.emplace(key, std::move(it->second));
    entries_.erase(it);
    return ins->second;
}

ViewCache::Entry& ViewCache::migrate_to_settled_(const std::string& key) {
    // Precondition: key is in pinned_ (live). Move its payload into the
    // LRU at the front (most-recently-used), then evict past cap_.
    auto it = pinned_.find(key);
    lru_.push_front(key);
    it->second.pinned = false;
    it->second.lru_it = lru_.begin();
    auto [ins, _] = entries_.emplace(key, std::move(it->second));
    pinned_.erase(it);
    // Evict oldest settled entries until under cap. The freshly-migrated
    // key is at the front, so it is never the victim.
    while (entries_.size() > cap_ && !lru_.empty()) {
        const std::string& victim = lru_.back();
        if (victim == key) break;   // never evict what we just inserted
        entries_.erase(victim);
        lru_.pop_back();
    }
    return ins->second;
}

ViewCache::Entry& ViewCache::touch_settled_(const std::string& key) {
    // Already settled → move-to-front in the LRU.
    if (auto it = entries_.find(key); it != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        it->second.lru_it = lru_.begin();
        return it->second;
    }
    // Currently pinned → the caller now considers it settled; migrate
    // down (payload preserved).
    if (pinned_.find(key) != pinned_.end())
        return migrate_to_settled_(key);

    // Fresh key: evict oldest settled until under cap, then insert.
    while (entries_.size() >= cap_ && !lru_.empty()) {
        const std::string& victim = lru_.back();
        entries_.erase(victim);
        lru_.pop_back();
    }
    lru_.push_front(key);
    auto [ins, _] = entries_.emplace(key, Entry{});
    ins->second.lru_it = lru_.begin();
    ins->second.pinned = false;
    return ins->second;
}

ViewCache::Entry& ViewCache::touch_pinned_(const std::string& key) {
    // Already pinned → nothing to do; pinned entries have no LRU order.
    if (auto it = pinned_.find(key); it != pinned_.end())
        return it->second;
    // Currently settled → promote into the pinned set (payload preserved,
    // LRU link dropped).
    if (entries_.find(key) != entries_.end())
        return migrate_to_pinned_(key);
    // Fresh key: insert directly into the pinned set. No eviction — the
    // pinned set is uncapped (self-bounded by the active turn).
    auto [ins, _] = pinned_.emplace(key, Entry{});
    ins->second.pinned = true;
    return ins->second;
}

MessageMdCache& ViewCache::message_md(const ThreadId& tid, const MessageId& mid) {
    return touch_settled_(make_key_(tid, mid)).md;
}

TurnConfigCache& ViewCache::turn_config(const ThreadId& tid, const MessageId& mid) {
    return touch_settled_(make_key_(tid, mid)).cfg;
}

MessageMdCache& ViewCache::message_md_live(const ThreadId& tid, const MessageId& mid) {
    return touch_pinned_(make_key_(tid, mid)).md;
}

bool ViewCache::is_pinned(const ThreadId& tid, const MessageId& mid) const noexcept {
    return pinned_.find(make_key_(tid, mid)) != pinned_.end();
}

void ViewCache::settle(const ThreadId& tid, const MessageId& mid) {
    // Migrate a pinned key down into the LRU, payload intact. No-op if
    // the key is already settled or absent — freeze_range calls this
    // unconditionally over the run it's sealing, and most of those
    // messages were never pinned (tool-only sub-turns, already-settled
    // prose).
    const auto key = make_key_(tid, mid);
    if (pinned_.find(key) != pinned_.end())
        migrate_to_settled_(key);
}

const MessageMdCache* ViewCache::peek(const ThreadId& tid,
                                      const MessageId& mid) const noexcept {
    const auto key = make_key_(tid, mid);
    if (auto it = pinned_.find(key); it != pinned_.end())
        return &it->second.md;
    if (auto it = entries_.find(key); it != entries_.end())
        return &it->second.md;
    return nullptr;
}

void ViewCache::set_capacity(std::size_t max_entries) noexcept {
    cap_ = (max_entries == 0) ? 1 : max_entries;
    // Shrinking the cap mid-session: evict oldest settled entries down to
    // the new bound immediately (pinned entries are untouched — they are
    // never subject to the cap).
    while (entries_.size() > cap_ && !lru_.empty()) {
        const std::string& victim = lru_.back();
        entries_.erase(victim);
        lru_.pop_back();
    }
}

void ViewCache::retain_messages(const ThreadId& tid,
                                const std::unordered_set<std::string>& live)
{
    // Build the per-thread prefix once ("<tid>:") so we can identify
    // entries belonging to `tid` by string-prefix without re-parsing
    // the composite key on every iteration.
    std::string prefix;
    prefix.reserve(tid.value.size() + 1);
    prefix.append(tid.value);
    prefix.push_back(':');

    // Settled side: drop the LRU link, then the map entry.
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        const std::string& key = it->first;
        const bool in_thread =
            key.size() > prefix.size()
            && key.compare(0, prefix.size(), prefix) == 0;
        if (!in_thread) { ++it; continue; }

        const auto msg_id = key.substr(prefix.size());
        if (live.count(msg_id) > 0) { ++it; continue; }

        // Stale: drop the LRU link first (its iterator was captured at
        // touch time and is still valid — std::list iterators survive
        // unrelated erases), then the map entry.
        lru_.erase(it->second.lru_it);
        it = entries_.erase(it);
    }

    // Pinned side: normally empty of stale keys (a message stops being
    // pinned when its widget drains, and compaction only runs on a
    // settled transcript). But reap defensively so a compaction that
    // races an in-flight reveal can't strand a pinned orphan — the
    // pinned set has no LRU backstop to eventually push it out.
    for (auto it = pinned_.begin(); it != pinned_.end(); ) {
        const std::string& key = it->first;
        const bool in_thread =
            key.size() > prefix.size()
            && key.compare(0, prefix.size(), prefix) == 0;
        if (!in_thread) { ++it; continue; }
        const auto msg_id = key.substr(prefix.size());
        if (live.count(msg_id) > 0) { ++it; continue; }
        it = pinned_.erase(it);
    }
}

} // namespace agentty::ui
