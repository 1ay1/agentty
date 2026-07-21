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

    // MATRYOSHKA truncation dim (0 = off / full dimension). When > 0 and
    // smaller than the embedding width, the graph indexes only the first
    // `ann_dim` components of each vector. MRL-trained embedders
    // (nomic-embed-text-v1.5, the e5/BGE Matryoshka variants) place the most
    // information in the leading dims, so a dim PREFIX is itself a valid,
    // slightly-lower-fidelity embedding. Truncating to 256 of 768 cuts every
    // graph-walk dot product (the hot loop — thousands per query) to a THIRD
    // of the FLOPs and the graph's memory likewise, for a small, research-
    // documented recall cost that the full-dimension rerank stages recover.
    // Set once at build time and baked into the graph's working dim_; the
    // corpus rebuilds the graph if this changes across sessions.
    std::size_t ann_dim         = 0;

    // BINARY QUANTIZATION (0/false = off). When on, the graph WALK compares
    // 1-bit-per-dim sign codes with popcount Hamming instead of a float dot
    // — for a 256-dim vector that's 4 x 64-bit popcounts vs a 256-wide SIMD
    // dot, several times cheaper per hop (HuggingFace "binary embedding
    // quantization": faster + far less memory, ~92-96% quality WITH a float
    // rescore). The float vectors are RETAINED, so search() rescores its
    // returned pool with the exact cosine — binary recall, float precision.
    // Composes with ann_dim (quantize the Matryoshka prefix). Derived bits
    // are recomputed from the float vecs on load, so the on-disk cache format
    // is unchanged. Default off (byte-identical float behaviour).
    bool        binary          = false;
};

// One graph node: its per-layer neighbour lists. `vec` is a UNIT-NORMALIZED
// copy of the chunk embedding (so search is a dot product). Storing the
// normalized vector inline keeps the hot search loop cache-friendly and lets
// the index be self-contained for serialization. `bits` is the derived
// 1-bit-per-dim sign code (populated only in binary mode) the graph walk
// Hamming-compares; it is NOT serialized (recomputed from `vec` on load).
struct HnswNode {
    std::uint32_t              id = 0;        // chunk id in the Corpus
    std::vector<float>         vec;           // unit-normalized embedding
    std::vector<std::uint64_t> bits;          // sign code (binary mode only)
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

    // Truncate `v` to the graph's working dim_ (the Matryoshka prefix) and
    // unit-normalize the result, so cosine reduces to a dot. A vector SHORTER
    // than dim_ can't be conformed and returns empty (caller skips it). When
    // ann_dim truncation is off, dim_ == the full width and this is a plain
    // normalize. The single choke point every stored vector + query passes
    // through, so the whole index is dimension-coherent by construction.
    [[nodiscard]] std::vector<float> conform_(const std::vector<float>& v) const;

    // Pack a conform_'d vector into a 1-bit-per-dim sign code (bit set when
    // the component is > 0). ceil(dim_/64) words. The binary-mode graph walk
    // Hamming-compares these instead of float-dotting `vec`.
    [[nodiscard]] static std::vector<std::uint64_t>
    pack_bits_(const std::vector<float>& v);

    // Binary similarity of two sign codes: dim_ - 2*popcount(a XOR b), i.e.
    // (agreements - disagreements). Higher = closer, monotonic with cosine
    // for the sign approximation, so it slots into the same "bigger is better"
    // heaps/greedy the float dot uses.
    [[nodiscard]] float bin_sim_(const std::vector<std::uint64_t>& a,
                                 const std::vector<std::uint64_t>& b) const noexcept;

    float dot_(const std::vector<float>& a, const std::vector<float>& b) const noexcept;

    HnswConfig                cfg_{};
    std::vector<HnswNode>     nodes_;          // index == internal node index
    std::vector<std::uint32_t> id_of_;         // internal idx → chunk id (== nodes_[i].id)
    std::size_t               dim_       = 0;
    int                       max_layer_ = -1;
    std::uint32_t             entry_     = 0;   // entry point (top layer node)
    // Binary-mode walk probe: the packed sign code of the current query /
    // inserting node, set right before a graph walk and read by the query-vs-
    // node comparisons. Single-threaded search, so a scratch member is safe
    // and avoids threading the code through every internal walk signature.
    mutable std::vector<std::uint64_t> probe_bits_;
    mutable std::mt19937_64   rng_{0x9E3779B97F4A7C15ull};
};

} // namespace agentty::rag
