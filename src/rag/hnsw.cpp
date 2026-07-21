// agentty::rag — HNSW implementation. Pure C++/STL, no deps.
// Malkov & Yashunin 2016. See hnsw.hpp for the design rationale.

#include "agentty/rag/hnsw.hpp"
#include "agentty/rag/simd.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <queue>
#include <string_view>
#include <unordered_set>

namespace agentty::rag {

std::vector<std::uint64_t> HnswIndex::pack_bits_(const std::vector<float>& v) {
    // 1 bit per dim: set when the component is strictly positive (the sign
    // threshold binary quantization uses). ceil(n/64) little-endian words.
    const std::size_t words = (v.size() + 63) / 64;
    std::vector<std::uint64_t> bits(words, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] > 0.0f) bits[i >> 6] |= (std::uint64_t{1} << (i & 63));
    return bits;
}

float HnswIndex::bin_sim_(const std::vector<std::uint64_t>& a,
                          const std::vector<std::uint64_t>& b) const noexcept {
    // agreements - disagreements = dim_ - 2*Hamming. std::popcount is one
    // POPCNT instruction per word on x86/ARM — the whole point of binary mode.
    const std::size_t n = std::min(a.size(), b.size());
    std::uint32_t ham = 0;
    for (std::size_t i = 0; i < n; ++i)
        ham += static_cast<std::uint32_t>(std::popcount(a[i] ^ b[i]));
    return static_cast<float>(dim_) - 2.0f * static_cast<float>(ham);
}

std::vector<float> HnswIndex::conform_(const std::vector<float>& v) const {
    // Truncate to the working dim_ (Matryoshka prefix) then unit-normalize.
    // A vector shorter than dim_ can't be conformed — signal with empty so the
    // caller skips it (same contract the old ragged-dim guard had). dim_ must
    // already be established (add() sets it on the first insert before the
    // first conform_ call); a query against an empty graph never reaches here.
    if (dim_ == 0 || v.size() < dim_) return {};
    std::vector<float> out(v.begin(),
                           v.begin() + static_cast<std::ptrdiff_t>(dim_));
    double n = 0.0;
    for (float x : out) n += static_cast<double>(x) * x;
    if (n > 0.0) {
        float inv = static_cast<float>(1.0 / std::sqrt(n));
        for (float& x : out) x *= inv;
    }
    return out;   // a zero prefix stays zero (dot 0 — maximally dissimilar)
}

float HnswIndex::dot_(const std::vector<float>& a,
                      const std::vector<float>& b) const noexcept {
    // Both are unit-normalized, so dot == cosine. This is THE hot loop of the
    // graph walk (thousands of calls per query); route it through the runtime-
    // dispatched SIMD path (AVX2/SSE4.2/NEON) which is 2-4x the scalar loop on
    // 768-dim embeddings. simd::dot handles the length guard + tail. The span
    // overload carries the length WITH each pointer — no bare (ptr,ptr,n)
    // triple that could read past the shorter buffer.
    const std::size_t n = std::min(a.size(), b.size());
    return simd::dot(std::span<const float>(a).first(n),
                     std::span<const float>(b).first(n));
}

int HnswIndex::random_level_() {
    // Exponentially-decaying layer assignment: floor(-ln(U) * level_mult).
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    double r = uni(rng_);
    if (r <= 0.0) r = 1e-12;
    return static_cast<int>(-std::log(r) * cfg_.level_mult);
}

void HnswIndex::build(const std::vector<std::uint32_t>& ids,
                      const std::vector<const std::vector<float>*>& embeddings) {
    nodes_.clear();
    id_of_.clear();
    dim_ = 0;
    max_layer_ = -1;
    entry_ = 0;
    rng_.seed(cfg_.seed);

    const std::size_t n = std::min(ids.size(), embeddings.size());
    nodes_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!embeddings[i] || embeddings[i]->empty()) continue;
        add(ids[i], *embeddings[i]);
    }
}

void HnswIndex::add(std::uint32_t id, const std::vector<float>& vec) {
    if (vec.empty()) return;
    // Establish the working dimension on the FIRST insert. With Matryoshka
    // truncation (cfg_.ann_dim > 0 and smaller than the vector) the graph
    // works in the leading-dim prefix; otherwise it's the full width. Every
    // later insert + query is conform_'d to this same dim_, so the graph is
    // dimension-coherent no matter what width the raw embeddings are.
    if (dim_ == 0)
        dim_ = (cfg_.ann_dim > 0 && cfg_.ann_dim < vec.size())
                   ? cfg_.ann_dim : vec.size();

    std::vector<float> cv = conform_(vec);
    if (cv.empty()) return;   // shorter than dim_ — ragged, skip

    HnswNode node;
    node.id  = id;
    node.vec = std::move(cv);
    if (cfg_.binary) node.bits = pack_bits_(node.vec);
    int level = random_level_();
    node.links.resize(level + 1);

    const std::uint32_t cur_idx = static_cast<std::uint32_t>(nodes_.size());

    // First node ever: it becomes the entry point at its level.
    if (nodes_.empty()) {
        nodes_.push_back(std::move(node));
        id_of_.push_back(id);
        max_layer_ = level;
        entry_ = cur_idx;
        return;
    }

    nodes_.push_back(std::move(node));
    id_of_.push_back(id);
    const std::vector<float>& q = nodes_[cur_idx].vec;
    if (cfg_.binary) probe_bits_ = nodes_[cur_idx].bits;   // walk probe = new node

    // Phase 1: from the top layer down to level+1, greedy-descend to get a
    // good entry point for the layers we'll actually link into.
    std::uint32_t entry = entry_;
    for (int lc = max_layer_; lc > level; --lc)
        entry = greedy_closest_(q, entry, lc);

    // Phase 2: for each layer from min(level,max_layer) down to 0, beam-search
    // for ef_construction candidates, pick neighbours, and wire bidirectional
    // links (pruning the neighbour's list back to its max degree).
    for (int lc = std::min(level, max_layer_); lc >= 0; --lc) {
        auto cands = search_layer_(q, entry, cfg_.ef_construction, lc);
        auto chosen = select_neighbors_(q, cands, max_links_(lc));

        nodes_[cur_idx].links[lc] = chosen;
        for (std::uint32_t nb : chosen) {
            auto& nb_links = nodes_[nb].links[lc];
            nb_links.push_back(cur_idx);
            // Prune nb's neighbour list if it now exceeds the layer degree.
            if (nb_links.size() > max_links_(lc)) {
                auto pruned = select_neighbors_(nodes_[nb].vec, nb_links,
                                                max_links_(lc));
                nb_links = std::move(pruned);
            }
        }
        if (!cands.empty()) entry = cands.front();
    }

    // New top layer? promote the entry point.
    if (level > max_layer_) {
        max_layer_ = level;
        entry_ = cur_idx;
    }
}

std::uint32_t HnswIndex::greedy_closest_(const std::vector<float>& q,
                                         std::uint32_t entry, int layer) const {
    std::uint32_t cur = entry;
    float cur_sim = cfg_.binary ? bin_sim_(probe_bits_, nodes_[cur].bits)
                                : dot_(q, nodes_[cur].vec);
    bool improved = true;
    while (improved) {
        improved = false;
        if (layer >= static_cast<int>(nodes_[cur].links.size())) break;
        for (std::uint32_t nb : nodes_[cur].links[layer]) {
            float s = cfg_.binary ? bin_sim_(probe_bits_, nodes_[nb].bits)
                                  : dot_(q, nodes_[nb].vec);
            if (s > cur_sim) { cur_sim = s; cur = nb; improved = true; }
        }
    }
    return cur;
}

std::vector<std::uint32_t>
HnswIndex::search_layer_(const std::vector<float>& q, std::uint32_t entry,
                         std::size_t ef, int layer) const {
    // Two heaps: a max-heap of "candidates to explore" (by similarity) and a
    // min-heap of the "ef best found so far" (worst at top so we can evict).
    // Similarity is "bigger is better"; we negate for the result min-heap.
    std::unordered_set<std::uint32_t> visited;
    visited.reserve(ef * 4);

    // candidate frontier: max-heap on similarity (explore most-promising first)
    using SimNode = std::pair<float, std::uint32_t>;
    std::priority_queue<SimNode> frontier;             // max-heap by sim
    // results: min-heap on similarity (top = worst, evictable)
    std::priority_queue<SimNode, std::vector<SimNode>,
                        std::greater<SimNode>> results;

    float es = cfg_.binary ? bin_sim_(probe_bits_, nodes_[entry].bits)
                           : dot_(q, nodes_[entry].vec);
    frontier.push({es, entry});
    results.push({es, entry});
    visited.insert(entry);

    while (!frontier.empty()) {
        auto [cs, c] = frontier.top();
        frontier.pop();
        // If the best remaining candidate is worse than the worst kept
        // result and we already have ef of them, stop.
        if (!results.empty() && cs < results.top().first && results.size() >= ef)
            break;
        if (layer >= static_cast<int>(nodes_[c].links.size())) continue;
        for (std::uint32_t nb : nodes_[c].links[layer]) {
            if (visited.count(nb)) continue;
            visited.insert(nb);
            float s = cfg_.binary ? bin_sim_(probe_bits_, nodes_[nb].bits)
                                  : dot_(q, nodes_[nb].vec);
            if (results.size() < ef || s > results.top().first) {
                frontier.push({s, nb});
                results.push({s, nb});
                if (results.size() > ef) results.pop();
            }
        }
    }

    // Drain results (min-heap) into a vector sorted by similarity desc.
    std::vector<SimNode> tmp;
    tmp.reserve(results.size());
    while (!results.empty()) { tmp.push_back(results.top()); results.pop(); }
    std::sort(tmp.begin(), tmp.end(),
              [](const SimNode& a, const SimNode& b) { return a.first > b.first; });
    std::vector<std::uint32_t> out;
    out.reserve(tmp.size());
    for (auto& [s, idx] : tmp) out.push_back(idx);
    return out;
}

std::vector<std::uint32_t>
HnswIndex::select_neighbors_(const std::vector<float>& base,
                             const std::vector<std::uint32_t>& candidates,
                             std::size_t m) const {
    // Malkov §4 heuristic: walk candidates nearest-first; keep a candidate
    // only if it is closer to `base` than to any already-kept neighbour. This
    // produces a diverse, navigable neighbourhood instead of a tight cluster
    // (which would strand whole regions of the graph).
    // In binary mode all distances are Hamming over the sign codes; pack base
    // once (it's node-vs-node here, so `base` is a stored vector).
    const std::vector<std::uint64_t> base_bits =
        cfg_.binary ? pack_bits_(base) : std::vector<std::uint64_t>{};
    std::vector<std::pair<float, std::uint32_t>> ranked;
    ranked.reserve(candidates.size());
    for (std::uint32_t c : candidates)
        ranked.push_back({cfg_.binary ? bin_sim_(base_bits, nodes_[c].bits)
                                       : dot_(base, nodes_[c].vec), c});
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::uint32_t> kept;
    kept.reserve(m);
    for (auto& [sim_to_base, c] : ranked) {
        if (kept.size() >= m) break;
        bool good = true;
        for (std::uint32_t k : kept) {
            // sim(c, kept) > sim(c, base)  →  c is closer to an existing
            // neighbour than to base; drop it to preserve diversity.
            const float s_ck = cfg_.binary
                ? bin_sim_(nodes_[c].bits, nodes_[k].bits)
                : dot_(nodes_[c].vec, nodes_[k].vec);
            if (s_ck > sim_to_base) {
                good = false;
                break;
            }
        }
        if (good) kept.push_back(c);
    }
    // If the heuristic was too aggressive and left us short, top up with the
    // next-nearest raw candidates (keeps degree close to m for recall).
    if (kept.size() < m) {
        for (auto& [sim, c] : ranked) {
            if (kept.size() >= m) break;
            if (std::find(kept.begin(), kept.end(), c) == kept.end())
                kept.push_back(c);
        }
    }
    return kept;
}

std::vector<std::pair<std::uint32_t, float>>
HnswIndex::search(const std::vector<float>& query, std::size_t k,
                  std::size_t ef) const {
    if (nodes_.empty() || k == 0) return {};
    std::vector<float> q = conform_(query);
    if (q.empty()) return {};   // empty / shorter-than-dim_ query
    if (cfg_.binary) probe_bits_ = pack_bits_(q);   // walk probe = query code

    std::size_t efs = std::max<std::size_t>(ef ? ef : cfg_.ef_search, k);

    // Descend the sparse upper layers greedily to a good base-layer entry.
    std::uint32_t entry = entry_;
    for (int lc = max_layer_; lc > 0; --lc)
        entry = greedy_closest_(q, entry, lc);

    // Beam-search the dense base layer.
    auto cand = search_layer_(q, entry, efs, 0);

    // Rescore the returned pool with the EXACT float cosine, then take top-k.
    // In binary mode this recovers the precision the Hamming walk approximated
    // (binary recall → float rerank, the HuggingFace pattern). In float mode
    // it reproduces the beam's own order (recomputing the same dot_); efs is
    // small (~64) so the extra dots are negligible.
    std::vector<std::pair<std::uint32_t, float>> scored;
    scored.reserve(cand.size());
    for (std::uint32_t idx : cand)
        scored.push_back({idx, dot_(q, nodes_[idx].vec)});
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::pair<std::uint32_t, float>> out;
    out.reserve(std::min(k, scored.size()));
    for (std::size_t i = 0; i < scored.size() && i < k; ++i)
        out.push_back({nodes_[scored[i].first].id, scored[i].second});
    return out;
}

// ── Serialization ───────────────────────────────────────────────────────────

namespace {
template <class T> void put(std::string& b, const T& v) {
    b.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T> bool get(std::string_view& b, T& v) {
    if (b.size() < sizeof(T)) return false;
    std::memcpy(&v, b.data(), sizeof(T));
    b.remove_prefix(sizeof(T));
    return true;
}
constexpr std::uint32_t kHnswMagic = 0x484E5301;  // "HNS\x01"
} // namespace

void HnswIndex::serialize(std::string& out) const {
    put(out, kHnswMagic);
    put(out, static_cast<std::uint32_t>(dim_));
    put(out, static_cast<std::int32_t>(max_layer_));
    put(out, entry_);
    put(out, static_cast<std::uint32_t>(nodes_.size()));
    for (const auto& nd : nodes_) {
        put(out, nd.id);
        put(out, static_cast<std::uint32_t>(nd.vec.size()));
        if (!nd.vec.empty())
            out.append(reinterpret_cast<const char*>(nd.vec.data()),
                       nd.vec.size() * sizeof(float));
        put(out, static_cast<std::uint32_t>(nd.links.size()));
        for (const auto& layer : nd.links) {
            put(out, static_cast<std::uint32_t>(layer.size()));
            if (!layer.empty())
                out.append(reinterpret_cast<const char*>(layer.data()),
                           layer.size() * sizeof(std::uint32_t));
        }
    }
}

bool HnswIndex::deserialize(std::string_view& in) {
    auto reset_fail = [&]() -> bool {
        nodes_.clear();
        id_of_.clear();
        dim_ = 0;
        max_layer_ = -1;
        entry_ = 0;
        return false;   // caller falls through to a clean rebuild
    };

    nodes_.clear();
    id_of_.clear();
    dim_ = 0;
    max_layer_ = -1;
    entry_ = 0;

    std::uint32_t magic = 0;
    if (!get(in, magic) || magic != kHnswMagic) return false;
    std::uint32_t dim = 0;
    std::int32_t  maxl = -1;
    std::uint32_t entry = 0, n = 0;
    if (!get(in, dim) || !get(in, maxl) || !get(in, entry) || !get(in, n))
        return false;
    // A corrupt/truncated header can carry a garbage `n` (e.g. 0xFFFFFFFF).
    // reserve(n) on that is a multi-GB allocation → bad_alloc. Each node needs
    // at least 3 u32 on the wire (id, vlen, nlayers), so cap n by the bytes
    // actually remaining before reserving.
    if (n > in.size() / (3 * sizeof(std::uint32_t))) return reset_fail();
    dim_ = dim;
    max_layer_ = maxl;
    entry_ = entry;
    nodes_.reserve(n);
    id_of_.reserve(n);

    for (std::uint32_t i = 0; i < n; ++i) {
        HnswNode nd;
        std::uint32_t vlen = 0, nlayers = 0;
        if (!get(in, nd.id) || !get(in, vlen)) return reset_fail();
        // Every stored vector must share the header dim (search dots them).
        if (vlen != dim_) return reset_fail();
        if (vlen) {
            if (in.size() < vlen * sizeof(float)) return reset_fail();
            nd.vec.resize(vlen);
            std::memcpy(nd.vec.data(), in.data(), vlen * sizeof(float));
            in.remove_prefix(vlen * sizeof(float));
            // Sign codes are DERIVED (never serialized) so the on-disk format
            // is binary-mode-agnostic: recompute them here when this index is
            // configured for the Hamming walk. Toggling AGENTTY_RAG_BINARY
            // across sessions therefore needs no cache rebuild.
            if (cfg_.binary) nd.bits = pack_bits_(nd.vec);
        }
        if (!get(in, nlayers)) return reset_fail();
        nd.links.resize(nlayers);
        for (std::uint32_t l = 0; l < nlayers; ++l) {
            std::uint32_t cnt = 0;
            if (!get(in, cnt)) return reset_fail();
            if (in.size() < cnt * sizeof(std::uint32_t)) return reset_fail();
            nd.links[l].resize(cnt);
            if (cnt) {
                std::memcpy(nd.links[l].data(), in.data(),
                            cnt * sizeof(std::uint32_t));
                in.remove_prefix(cnt * sizeof(std::uint32_t));
            }
        }
        id_of_.push_back(nd.id);
        nodes_.push_back(std::move(nd));
    }

    // VALIDATE graph topology before anyone walks it: search() indexes
    // nodes_[entry_] and follows link ids as node indices, and the corpus
    // materializes hits as &chunks_[node.id]. A corrupt-but-parseable file
    // (intact byte lengths, garbage values) would otherwise OOB-read on the
    // first query. Reject → clean rebuild.
    const std::uint32_t nn = static_cast<std::uint32_t>(nodes_.size());
    if (nn == 0) {
        // Empty graph is only coherent with the empty-graph sentinels.
        entry_ = 0;
        max_layer_ = -1;
        return true;
    }
    if (entry_ >= nn) return reset_fail();
    for (const auto& node : nodes_)
        for (const auto& layer : node.links)
            for (std::uint32_t lid : layer)
                if (lid >= nn) return reset_fail();
    return true;
}

} // namespace agentty::rag
