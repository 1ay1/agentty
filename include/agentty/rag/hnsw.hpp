#pragma once
// agentty::rag — HNSW (Hierarchical Navigable Small World) approximate
// nearest-neighbour index over chunk embeddings.
//
// WHY: brute-force cosine is O(n) per query — fine for a few hundred chunks,
// but a real knowledge base (a whole docs/ tree, a book, a wiki export) is
// tens to hundreds of thousands of chunks, and a linear scan per query (and
// per multi-query-expansion sub-query, and over a wide candidate pool) is
// what turns "production RAG" back into a toy. HNSW gives O(log n) search by
// navigating a layered proximity graph: greedy descent through sparse upper
// layers to land near the target, then a beam search on the dense base layer.
//
// This is the SAME algorithm behind FAISS / hnswlib / every modern vector DB
// — implemented here in pure C++/STL so agentty pulls in NO vector-DB
// dependency and stays a single ~9MB binary. Malkov & Yashunin, 2016
// ("Efficient and robust approximate nearest neighbor search using HNSW").
//
// PERF/MEMORY: the graph is built lazily (only when the corpus crosses a size
// threshold where it beats brute force) and serialized into the same on-disk
// RAG cache, so re-opening a knowledge base doesn't rebuild it. Vectors are
// referenced by id into the Corpus's chunk array — the index stores only the
// adjacency lists, not copies of the embeddings.

#include <cstdint>
#include <random>
#include <vector>

namespace agentty::rag {

// Distance metric. Embeddings from sentence/embedding models are compared by
// cosine; we store unit-normalized vectors so cosine reduces to a dot product
// (and HNSW's "smaller is closer" convention uses 1 - dot).
struct HnswConfig {
    std::size_t M               = 16;   // base-layer max neighbours per node
    std::size_t M0              = 32;   // layer-0 gets 2*M (denser base)
    std::size_t ef_construction = 200;  // beam width while building
    std::size_t ef_search       = 64;   // beam width while querying (≥ k)
    double      level_mult      = 1.0 / 0.69314718;  // 1/ln(2): layer sampler
    std::uint64_t seed          = 0x9E3779B97F4A7C15ull;
};

// One graph node: its per-layer neighbour lists. `vec` is a UNIT-NORMALIZED
// copy of the chunk embedding (so search is a dot product). Storing the
// normalized vector inline keeps the hot search loop cache-friendly and lets
// the index be self-contained for serialization.
struct HnswNode {
    std::uint32_t              id = 0;        // chunk id in the Corpus
    std::vector<float>         vec;           // unit-normalized embedding
    std::vector<std::vector<std::uint32_t>> links;  // links[layer] = neighbours
};

class HnswIndex {
public:
    HnswIndex() = default;
    explicit HnswIndex(HnswConfig cfg) : cfg_(cfg) {}

    // Insert one embedding (referenced later by `id`). `vec` need not be
    // normalized — the index normalizes a copy. Dimension is fixed by the
    // first insert; mismatched dims are ignored.
    void add(std::uint32_t id, const std::vector<float>& vec);

    // Build the whole index from a chunk-id→embedding view in one shot.
    // Clears any existing graph first. `embeddings[i]` is the embedding for
    // chunk id `ids[i]`; entries with the wrong dim or empty are skipped.
    void build(const std::vector<std::uint32_t>& ids,
               const std::vector<const std::vector<float>*>& embeddings);

    // k-NN search. Returns (chunk-id, similarity) pairs, similarity = cosine
    // (dot of unit vectors), sorted by similarity desc, at most k entries.
    // `ef` overrides cfg_.ef_search for this query when > 0 (use max(ef,k)).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, float>>
    search(const std::vector<float>& query, std::size_t k,
           std::size_t ef = 0) const;

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t dim()  const noexcept { return dim_; }
    [[nodiscard]] bool empty()       const noexcept { return nodes_.empty(); }

    // ── Serialization (folds into the RAG disk cache) ────────────────────
    // Append a compact binary encoding of the whole graph to `out`.
    void serialize(std::string& out) const;
    // Parse from a string_view cursor (advances it). Returns false on a
    // malformed/truncated blob (caller then rebuilds). Clears first.
    [[nodiscard]] bool deserialize(std::string_view& in);

    const HnswConfig& config() const noexcept { return cfg_; }

private:
    int  random_level_();
    std::size_t max_links_(int layer) const noexcept {
        return layer == 0 ? cfg_.M0 : cfg_.M;
    }
    // Greedy descent on one layer from `entry`, returning the closest node.
    std::uint32_t greedy_closest_(const std::vector<float>& q,
                                  std::uint32_t entry, int layer) const;
    // Beam search on `layer`, returning up to `ef` nearest candidates.
    std::vector<std::uint32_t>
    search_layer_(const std::vector<float>& q, std::uint32_t entry,
                  std::size_t ef, int layer) const;
    // Heuristic neighbour selection (Malkov §4, "select neighbors heuristic"):
    // prefer a diverse set over the raw nearest, which keeps the graph
    // navigable. Returns up to `m` ids from `candidates`.
    std::vector<std::uint32_t>
    select_neighbors_(const std::vector<float>& base,
                      const std::vector<std::uint32_t>& candidates,
                      std::size_t m) const;

    float dot_(const std::vector<float>& a, const std::vector<float>& b) const noexcept;

    HnswConfig                cfg_{};
    std::vector<HnswNode>     nodes_;          // index == internal node index
    std::vector<std::uint32_t> id_of_;         // internal idx → chunk id (== nodes_[i].id)
    std::size_t               dim_       = 0;
    int                       max_layer_ = -1;
    std::uint32_t             entry_     = 0;   // entry point (top layer node)
    mutable std::mt19937_64   rng_{0x9E3779B97F4A7C15ull};
};

} // namespace agentty::rag
