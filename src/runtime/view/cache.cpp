#include "moha/runtime/view/cache.hpp"

#include <cstddef>
#include <string>

namespace moha::ui {

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

ViewCache::Entry& ViewCache::touch_(const std::string& key) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        it->second.lru_it = lru_.begin();
        return it->second;
    }

    // Evict oldest until under cap, then insert.
    while (entries_.size() >= cap_ && !lru_.empty()) {
        const std::string& victim = lru_.back();
        entries_.erase(victim);
        lru_.pop_back();
    }

    lru_.push_front(key);
    auto [ins, _] = entries_.emplace(key, Entry{});
    ins->second.lru_it = lru_.begin();
    return ins->second;
}

MessageMdCache& ViewCache::message_md(const ThreadId& tid, const MessageId& mid) {
    return touch_(make_key_(tid, mid)).md;
}

TurnConfigCache& ViewCache::turn_config(const ThreadId& tid, const MessageId& mid) {
    return touch_(make_key_(tid, mid)).cfg;
}

void ViewCache::set_capacity(std::size_t max_entries) noexcept {
    cap_ = (max_entries == 0) ? 1 : max_entries;
}

} // namespace moha::ui
