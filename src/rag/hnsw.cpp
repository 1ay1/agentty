// agentty::rag — HNSW implementation. Pure C++/STL, no deps.
// Malkov & Yashunin 2016. See hnsw.hpp for the design rationale.

#include "agentty/rag/hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <string_view>
#include <unordered_set>

namespace agentty::rag {

namespace {
// Normalize a copy to unit length so cosine == dot. Zero vectors pass through
// (their dot with anything is 0, i.e. "maximally dissimilar").
std::vector<float> normalize(const std::vector<float>& v) {
    double n = 0.0;
    for (float x : v) n += static_cast<double>(x) * x;
    if (n <= 0.0) return v;
    float inv = static_cast<float>(1.0 / std::sqrt(n));
    std::vector<float> out(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = v[i] * inv;
    return out;
}
} // namespace

float HnswIndex::dot_(const std::vector<float>& a,
                      const std::vector<float>& b) const noexcept {
    // Both are unit-normalized; dot == cosine. Plain loop — the compiler
    // auto-vectorizes this under -O2/-march; no manual intrinsics needed.
    const std::size_t n = std::min(a.size(), b.size());
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
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
    if (dim_ == 0) dim_ = vec.size();
    else if (vec.size() != dim_) return;   // ragged — skip

    HnswNode node;
    node.id  = id;
    node.vec = normalize(vec);
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
    float cur_sim = dot_(q, nodes_[cur].vec);
    bool improved = true;
    while (improved) {
        improved = false;
        if (layer >= static_cast<int>(nodes_[cur].links.size())) break;
        for (std::uint32_t nb : nodes_[cur].links[layer]) {
            float s = dot_(q, nodes_[nb].vec);
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

    float es = dot_(q, nodes_[entry].vec);
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
            float s = dot_(q, nodes_[nb].vec);
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
    std::vector<std::pair<float, std::uint32_t>> ranked;
    ranked.reserve(candidates.size());
    for (std::uint32_t c : candidates)
        ranked.push_back({dot_(base, nodes_[c].vec), c});
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
            if (dot_(nodes_[c].vec, nodes_[k].vec) > sim_to_base) {
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
    std::vector<float> q = normalize(query);
    if (q.size() != dim_) return {};

    std::size_t efs = std::max<std::size_t>(ef ? ef : cfg_.ef_search, k);

    // Descend the sparse upper layers greedily to a good base-layer entry.
    std::uint32_t entry = entry_;
    for (int lc = max_layer_; lc > 0; --lc)
        entry = greedy_closest_(q, entry, lc);

    // Beam-search the dense base layer.
    auto cand = search_layer_(q, entry, efs, 0);

    std::vector<std::pair<std::uint32_t, float>> out;
    out.reserve(std::min(k, cand.size()));
    for (std::size_t i = 0; i < cand.size() && i < k; ++i) {
        std::uint32_t idx = cand[i];
        out.push_back({nodes_[idx].id, dot_(q, nodes_[idx].vec)});
    }
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
    dim_ = dim;
    max_layer_ = maxl;
    entry_ = entry;
    nodes_.reserve(n);
    id_of_.reserve(n);

    for (std::uint32_t i = 0; i < n; ++i) {
        HnswNode nd;
        std::uint32_t vlen = 0, nlayers = 0;
        if (!get(in, nd.id) || !get(in, vlen)) return false;
        if (vlen) {
            if (in.size() < vlen * sizeof(float)) return false;
            nd.vec.resize(vlen);
            std::memcpy(nd.vec.data(), in.data(), vlen * sizeof(float));
            in.remove_prefix(vlen * sizeof(float));
        }
        if (!get(in, nlayers)) return false;
        nd.links.resize(nlayers);
        for (std::uint32_t l = 0; l < nlayers; ++l) {
            std::uint32_t cnt = 0;
            if (!get(in, cnt)) return false;
            if (in.size() < cnt * sizeof(std::uint32_t)) return false;
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
    return true;
}

} // namespace agentty::rag
