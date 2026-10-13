#include "agentty/runtime/view/cache.hpp"

#include <cstddef>
#include <string>

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

} // namespace agentty::ui
