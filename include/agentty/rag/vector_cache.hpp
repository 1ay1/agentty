#pragma once
// agentty/rag/vector_cache.hpp — embeddings computed OUTSIDE the RAG index
// lock, read inside it.
//
// The index is a maya::guarded held for a whole retrieve or rebuild, and
// rag-cpp asks its embedder for vectors from inside that body: every pending
// chunk on build(), the query on search. An embedder is a network call (a
// local Ollama, a hosted API). Doing those calls under the lock meant either
// one batch at a time, or fanning out with helper threads under a lock, which
// jaal forbids (#112 aborted on exactly that).
//
// So the work is split in three, and only the cheap parts hold the lock:
//
//   1. stage    (locked)    work out which texts will need a vector:
//                           Corpus::pending_texts(), the query
//   2. prefetch (unlocked)  embed them, batches in parallel, into the cache
//   3. finish   (locked)    build/search as before; the corpus's embedder is
//                           a CachedEmbedder, so every lookup is a hit
//
// A miss in step 3 (something new raced in between) falls back to the real
// embedder, so the answer is still right, just slower; misses are counted so
// a regression shows up in stats().
//
// Storage is a maya::published map that is replaced, never edited: readers
// (the locked embedder, a library virtual with no capability to take a lock)
// load it with one atomic, and prefetch publishes a new map with the new
// vectors added. Keys carry the embedder's identity, so switching models
// never serves a vector from the old space.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <maya/runtime.hpp>
#include <rag/dense/embedder.hpp>

namespace agentty::rag {

class VectorCache {
public:
    // Vectors kept at most. A 768-d float vector is 3 KB, so ~200 MB at the
    // outside; a typical project uses a small fraction. Over the cap, the
    // oldest generation of entries goes first.
    explicit VectorCache(std::size_t capacity = 64 * 1024) : capacity_(capacity) {}

    VectorCache(const VectorCache&)            = delete;
    VectorCache& operator=(const VectorCache&) = delete;

    // The cached vector for (model, text). Lock-free; never does IO.
    [[nodiscard]] bool get(std::string_view model, std::string_view text, ::rag::Vector& out) const;

    // Embed every text not already cached, `width` batches in flight, and
    // publish them. Joins helper threads, so it takes `unlocked`: it can't be
    // called from a lock body. Partial failure keeps what worked (the locked
    // build falls back to the real embedder for the rest); the error is
    // returned only if nothing could be embedded.
    ::rag::Result<void> prefetch(const maya::unlocked& cap, const ::rag::dense::AnyEmbedder& real,
                                 std::span<const std::string> texts, std::size_t batch = 32) const;

    struct Stats {
        std::uint64_t hits = 0, misses = 0, prefetched = 0, entries = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;
    // Drop every vector (the embed config changed). Lock-free, like a publish.
    void clear() const noexcept { map_.publish(nullptr); }
    void note_hit() const noexcept { hits_.fetch_add(1, std::memory_order_relaxed); }
    void note_miss() const noexcept { misses_.fetch_add(1, std::memory_order_relaxed); }

private:
    struct Entry {
        ::rag::Vector v;
        std::uint64_t gen = 0;   // when it was added; the oldest go first over the cap
    };
    using Map = std::unordered_map<std::string, Entry>;
    static std::string key(std::string_view model, std::string_view text);
    void merge(std::vector<std::pair<std::string, ::rag::Vector>> fresh) const;

    std::size_t                        capacity_;
    // The map is replaced whole, never edited, so readers need no lock and
    // the cache can be shared as const: adding vectors is a publish, which
    // is atomic (mutable here for that reason only). publish_if retries if
    // two prefetches race, so neither loses its vectors.
    mutable maya::published<const Map> map_;
    mutable std::atomic<std::uint64_t> gen_{0};
    mutable std::atomic<std::uint64_t> hits_{0}, misses_{0}, prefetched_{0};
};

// The embedder the locked engines use: answers from the cache, and only on a
// miss calls the real one (slow, but correct). Same identity and dimension as
// the real embedder, so a persisted index is still matched to its model.
class CachedEmbedder {
public:
    CachedEmbedder(::rag::dense::AnyEmbedder real, std::shared_ptr<const VectorCache> cache)
        : real_(std::move(real)), cache_(std::move(cache)) {}

    [[nodiscard]] std::size_t dimension() const { return real_.dimension(); }
    [[nodiscard]] std::string_view identity() const { return real_.identity(); }
    // Batches here are lookups, and this runs inside a lock where helper
    // threads can't be joined: one at a time.
    [[nodiscard]] std::size_t max_concurrency() const noexcept { return 1; }
    [[nodiscard]] ::rag::Result<std::vector<::rag::Vector>> embed(std::span<const std::string> texts) const;

private:
    ::rag::dense::AnyEmbedder          real_;
    std::shared_ptr<const VectorCache> cache_;
};

}  // namespace agentty::rag

MAYA_SYNC(agentty::rag::VectorCache);
