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
    // pinned_. The map-node move preserves md/cfg (reveal widget, block
    // cache, defer state) so animation state survives the lifecycle
    // transition intact.
    auto it = entries_.find(key);
    it->second.pinned = true;
    auto [ins, _] = pinned_.emplace(key, std::move(it->second));
    entries_.erase(it);
    return ins->second;
}

ViewCache::Entry& ViewCache::migrate_to_settled_(const std::string& key) {
    // Precondition: key is in pinned_ (live). Move its payload into the
    // settled map. No cap / no eviction: the settled map is bounded by
    // the active turn and self-empties at freeze (freeze_range drops
    // each key it seals), so there is never anything to evict here.
    auto it = pinned_.find(key);
    it->second.pinned = false;
    auto [ins, _] = entries_.emplace(key, std::move(it->second));
    pinned_.erase(it);
    return ins->second;
}

ViewCache::Entry& ViewCache::touch_settled_(const std::string& key) {
    // Already settled → return it.
    if (auto it = entries_.find(key); it != entries_.end())
        return it->second;
    // Currently pinned → the caller now considers it settled; migrate
    // down (payload preserved).
    if (pinned_.find(key) != pinned_.end())
        return migrate_to_settled_(key);
    // Fresh key: insert into the settled map.
    auto [ins, _] = entries_.emplace(key, Entry{});
    ins->second.pinned = false;
    return ins->second;
}

ViewCache::Entry& ViewCache::touch_pinned_(const std::string& key) {
    // Already pinned → nothing to do.
    if (auto it = pinned_.find(key); it != pinned_.end())
        return it->second;
    // Currently settled → promote into the pinned set (payload preserved).
    if (entries_.find(key) != entries_.end())
        return migrate_to_pinned_(key);
    // Fresh key: insert directly into the pinned set.
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

void ViewCache::drop(const ThreadId& tid, const MessageId& mid) {
    // Free the entry from whichever home it lives in. This is the death
    // instant of a settled entry — freeze_range calls it on every message
    // it seals, because a frozen message's rows live in m.ui.frozen and
    // its ViewCache entry is never read again. Reaping the pinned side too
    // closes the mirror hazard (a widget that stopped animating but was
    // never down-migrated at the render seam). erase() reclaims the
    // shared_ptr payload immediately.
    const auto key = make_key_(tid, mid);
    entries_.erase(key);
    pinned_.erase(key);
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

    auto reap = [&](std::unordered_map<std::string, Entry>& map) {
        for (auto it = map.begin(); it != map.end(); ) {
            const std::string& key = it->first;
            const bool in_thread =
                key.size() > prefix.size()
                && key.compare(0, prefix.size(), prefix) == 0;
            if (!in_thread) { ++it; continue; }
            const auto msg_id = key.substr(prefix.size());
            if (live.count(msg_id) > 0) { ++it; continue; }
            it = map.erase(it);
        }
    };

    // Reap orphans from BOTH homes. Normally near-empty (freeze already
    // dropped the preserved tail's entries); this catches a compaction
    // that races an in-flight turn whose message id is about to vanish.
    reap(entries_);
    reap(pinned_);
}

} // namespace agentty::ui
