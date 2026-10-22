// src/rag/vector_cache.cpp — see agentty/rag/vector_cache.hpp.

#include "agentty/rag/vector_cache.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace agentty::rag {

std::string VectorCache::key(std::string_view model, std::string_view text) {
    std::string k;
    k.reserve(model.size() + 1 + text.size());
    k.append(model);
    k.push_back('\0');   // can't appear in a model id, so (a, bc) != (ab, c)
    k.append(text);
    return k;
}

bool VectorCache::get(std::string_view model, std::string_view text, ::rag::Vector& out) const {
    const auto m = map_.current();
    if (!m) return false;
    const auto it = m->find(key(model, text));
    if (it == m->end()) return false;
    out = it->second.v;
    return true;
}

void VectorCache::merge(std::vector<std::pair<std::string, ::rag::Vector>> fresh) const {
    if (fresh.empty()) return;
    const std::uint64_t g = gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Copy-on-write: build the next map from the current one and publish it
    // only if no other prefetch published meanwhile; else start again from
    // theirs, so neither loses its vectors.
    for (;;) {
        const auto cur = map_.current();
        auto next = std::make_shared<Map>(cur ? *cur : Map{});
        for (auto& [k, v] : fresh) (*next)[k] = Entry{v, g};
        if (next->size() > capacity_) {
            // Drop the oldest generations until under the cap.
            std::vector<std::pair<std::uint64_t, const std::string*>> by_age;
            by_age.reserve(next->size());
            for (const auto& [k, e] : *next) by_age.emplace_back(e.gen, &k);
            std::ranges::sort(by_age, {}, &std::pair<std::uint64_t, const std::string*>::first);
            std::vector<std::string> drop;
            for (std::size_t i = 0; i < next->size() - capacity_; ++i) drop.push_back(*by_age[i].second);
            for (const auto& k : drop) next->erase(k);
        }
        if (map_.publish_if(cur, std::shared_ptr<const Map>(std::move(next)))) return;
    }
}

::rag::Result<void> VectorCache::prefetch(const maya::unlocked& cap, const ::rag::dense::AnyEmbedder& real,
                                          std::span<const std::string> texts, std::size_t batch) const {
    const std::string model{real.identity()};
    // What isn't cached yet, each text once.
    std::vector<std::string> todo;
    {
        const auto m = map_.current();
        std::unordered_set<std::string_view> seen;
        for (const auto& t : texts) {
            if (!seen.insert(t).second) continue;
            if (m && m->contains(key(model, t))) continue;
            todo.push_back(t);
        }
    }
    if (todo.empty()) return {};

    const std::size_t bs = std::max<std::size_t>(batch, 1);
    const std::size_t nb = (todo.size() + bs - 1) / bs;
    std::vector<std::optional<std::vector<::rag::Vector>>> out(nb);
    std::vector<std::optional<::rag::Error>> errs(nb);
    auto run = [&](std::size_t b) {
        const std::size_t off = b * bs, end = std::min(off + bs, todo.size());
        auto r = real.embed(std::span<const std::string>(todo).subspan(off, end - off));
        if (!r) { errs[b] = r.error(); return; }
        out[b] = std::move(*r);
    };
    // The backend says how many requests it wants in flight (a hosted API 8,
    // a local Ollama 4, an in-process model 1). No lock is held here, so the
    // batches can really run side by side.
    const std::size_t width = std::clamp<std::size_t>(real.max_concurrency(), 1, nb);
    maya::parallel_for(cap, nb, width, run);

    std::vector<std::pair<std::string, ::rag::Vector>> fresh;
    fresh.reserve(todo.size());
    for (std::size_t b = 0; b < nb; ++b) {
        if (!out[b]) continue;
        const std::size_t off = b * bs;
        for (std::size_t j = 0; j < out[b]->size() && off + j < todo.size(); ++j) {
            // Exactly what the real embedder returned: the cache must be a
            // drop-in for it, and the corpus does its own normalising after.
            fresh.emplace_back(key(model, todo[off + j]), std::move((*out[b])[j]));
        }
    }
    prefetched_.fetch_add(fresh.size(), std::memory_order_relaxed);
    const bool any = !fresh.empty();
    merge(std::move(fresh));
    if (!any)
        for (auto& e : errs)
            if (e) return std::unexpected(std::move(*e));
    return {};
}

VectorCache::Stats VectorCache::stats() const noexcept {
    const auto m = map_.current();
    return {hits_.load(std::memory_order_relaxed), misses_.load(std::memory_order_relaxed),
            prefetched_.load(std::memory_order_relaxed), m ? m->size() : 0};
}

::rag::Result<std::vector<::rag::Vector>> CachedEmbedder::embed(std::span<const std::string> texts) const {
    std::vector<::rag::Vector> out(texts.size());
    std::vector<std::size_t> miss;
    const std::string_view model = real_.identity();
    for (std::size_t i = 0; i < texts.size(); ++i) {
        if (cache_ && cache_->get(model, texts[i], out[i])) cache_->note_hit();
        else miss.push_back(i);
    }
    if (miss.empty()) return out;
    // Not prefetched (new text raced in after staging): ask the real embedder.
    // Right answer, just slower, and counted so a regression is visible.
    if (cache_)
        for (std::size_t n = 0; n < miss.size(); ++n) cache_->note_miss();
    std::vector<std::string> rest;
    rest.reserve(miss.size());
    for (auto i : miss) rest.push_back(texts[i]);
    auto r = real_.embed(rest);
    if (!r) return std::unexpected(r.error());
    for (std::size_t j = 0; j < miss.size() && j < r->size(); ++j) out[miss[j]] = std::move((*r)[j]);
    return out;
}

}  // namespace agentty::rag
