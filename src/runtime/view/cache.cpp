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

    for (auto it = entries_.begin(); it != entries_.end(); ) {
        const std::string& key = it->first;
        const bool in_thread =
            key.size() > prefix.size()
            && key.compare(0, prefix.size(), prefix) == 0;
        if (!in_thread) { ++it; continue; }

        // Suffix after the prefix is the MessageId value.
        const auto msg_id = key.substr(prefix.size());
        if (live.count(msg_id) > 0) { ++it; continue; }

        // Stale: drop the LRU link first (its iterator was captured at
        // touch_ time and is still valid here — std::list iterators
        // survive unrelated erases), then the map entry. Doing it in
        // this order avoids touching a stale lru_it after the Entry's
        // destructor runs.
        lru_.erase(it->second.lru_it);
        it = entries_.erase(it);
    }
}

} // namespace agentty::ui
