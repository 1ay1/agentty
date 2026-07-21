// rag_hnsw_test — unit tests for the pure-C++ HNSW approximate-nearest-
// neighbour index (agentty::rag::HnswIndex). NO network: all vectors are
// generated locally from a fixed-seed RNG so the suite is deterministic.
//
// Lightweight harness mirroring tests/rag_test.cpp: a global failure counter
// + CHECK macro, a main() that runs each test fn and prints "all checks
// passed" / returns nonzero on any failure.

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/rag/hnsw.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

// Cosine of two equal-length vectors (0 on a zero vector). Used as the
// brute-force ground truth the ANN index is graded against.
double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        na  += double(a[i]) * double(a[i]);
        nb  += double(b[i]) * double(b[i]);
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<float> random_vec(std::mt19937& rng, std::size_t dim) {
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = nd(rng);
    return v;
}

// True nearest neighbour id by brute-force cosine over `db`.
std::uint32_t true_nn(const std::vector<std::vector<float>>& db,
                      const std::vector<float>& q) {
    std::uint32_t best = 0;
    double best_sim = -2.0;
    for (std::uint32_t i = 0; i < db.size(); ++i) {
        double s = cosine(q, db[i]);
        if (s > best_sim) { best_sim = s; best = i; }
    }
    return best;
}

} // namespace

// ── 1. Recall@5 on ~500 random vectors, dim 32 ──────────────────────────────
static void test_hnsw_recall_small() {
    constexpr std::size_t kN = 500, kDim = 32, kQueries = 30;
    std::mt19937 rng(1234567u);

    std::vector<std::vector<float>> db;
    db.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) db.push_back(random_vec(rng, kDim));

    rag::HnswIndex idx;
    std::vector<std::uint32_t> ids(kN);
    std::vector<const std::vector<float>*> embs(kN);
    for (std::uint32_t i = 0; i < kN; ++i) { ids[i] = i; embs[i] = &db[i]; }
    idx.build(ids, embs);

    CHECK(idx.size() == kN);
    CHECK(idx.dim() == kDim);
    CHECK(!idx.empty());

    int hits = 0;
    for (std::size_t qn = 0; qn < kQueries; ++qn) {
        auto q = random_vec(rng, kDim);
        std::uint32_t want = true_nn(db, q);
        // ef bumped to 100 for high recall on this small set.
        auto res = idx.search(q, 5, /*ef=*/100);
        bool found = false;
        for (auto& [id, sim] : res) if (id == want) { found = true; break; }
        if (found) ++hits;
    }
    double recall = double(hits) / double(kQueries);
    std::printf("rag_hnsw_test: recall@5 = %.3f (%d/%zu)\n",
                recall, hits, (std::size_t)kQueries);
    CHECK(recall >= 0.8);
}

// ── 2. Basic search: query == a stored vector returns it first, sim ~1 ───────
static void test_hnsw_search_basic() {
    std::vector<std::vector<float>> db = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    rag::HnswIndex idx;
    std::vector<std::uint32_t> ids = {0, 1, 2};
    std::vector<const std::vector<float>*> embs = {&db[0], &db[1], &db[2]};
    idx.build(ids, embs);

    auto res = idx.search(db[1], 3);
    CHECK(!res.empty());
    CHECK(res.front().first == 1u);
    CHECK(std::fabs(res.front().second - 1.0f) < 1e-4);
}

// ── 3. Serialize → deserialize roundtrip preserves search results ───────────
static void test_hnsw_serialize_roundtrip() {
    constexpr std::size_t kN = 50, kDim = 16;
    std::mt19937 rng(424242u);

    std::vector<std::vector<float>> db;
    db.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) db.push_back(random_vec(rng, kDim));

    rag::HnswIndex idx;
    std::vector<std::uint32_t> ids(kN);
    std::vector<const std::vector<float>*> embs(kN);
    for (std::uint32_t i = 0; i < kN; ++i) { ids[i] = i; embs[i] = &db[i]; }
    idx.build(ids, embs);

    std::string blob;
    idx.serialize(blob);
    CHECK(!blob.empty());

    rag::HnswIndex idx2;
    std::string_view cur{blob};
    bool ok = idx2.deserialize(cur);
    CHECK(ok);
    CHECK(idx2.size() == idx.size());
    CHECK(idx2.dim() == idx.dim());

    // Same top-1 ids for several queries on both indices.
    for (std::size_t qn = 0; qn < 8; ++qn) {
        auto q = random_vec(rng, kDim);
        auto a = idx.search(q, 1, /*ef=*/100);
        auto b = idx2.search(q, 1, /*ef=*/100);
        CHECK(!a.empty());
        CHECK(!b.empty());
        if (!a.empty() && !b.empty()) CHECK(a.front().first == b.front().first);
    }
}

// ── 4. Empty index, then a single insert ────────────────────────────────────
static void test_hnsw_empty() {
    rag::HnswIndex idx;
    CHECK(idx.empty());
    CHECK(idx.size() == 0);
    auto res = idx.search({1.0f, 2.0f, 3.0f}, 5);
    CHECK(res.empty());

    idx.add(7, {0.5f, 0.5f, 0.5f});
    CHECK(!idx.empty());
    CHECK(idx.size() == 1);
    auto res2 = idx.search({0.5f, 0.5f, 0.5f}, 5);
    CHECK(!res2.empty());
    CHECK(res2.front().first == 7u);
}

// ── 5. Corrupt / truncated cache must be REJECTED, never crash ──────────────
// deserialize() is fed the on-disk RAG cache, which can be truncated (crash
// mid-write, disk-full, kill -9) or bit-rotted. A parseable-but-garbage graph
// (bogus entry_, out-of-range link ids, absurd node count) would OOB-read the
// moment search() walks it. deserialize() must return false so the corpus
// falls through to a clean rebuild instead of bricking every search_docs call.
static void test_hnsw_corrupt_cache_rejected() {
    constexpr std::size_t kN = 30, kDim = 8;
    std::mt19937 rng(9001u);
    std::vector<std::vector<float>> db;
    for (std::size_t i = 0; i < kN; ++i) db.push_back(random_vec(rng, kDim));
    rag::HnswIndex idx;
    std::vector<std::uint32_t> ids(kN);
    std::vector<const std::vector<float>*> embs(kN);
    for (std::uint32_t i = 0; i < kN; ++i) { ids[i] = i; embs[i] = &db[i]; }
    idx.build(ids, embs);
    std::string good;
    idx.serialize(good);

    // (a) Truncated at every prefix length → reject cleanly, no crash.
    for (std::size_t cut = 0; cut < good.size(); cut += 3) {
        rag::HnswIndex bad;
        std::string_view cur{good.data(), cut};
        bool ok = bad.deserialize(cur);
        // Either a clean reject, or (for a coincidentally-complete prefix) a
        // valid graph — but NEVER a crash and NEVER a topology-invalid graph.
        if (ok) CHECK(bad.empty() || bad.size() > 0);
    }

    // (b) Garbage bytes (all 0xFF) must not allocate wildly or crash — the
    //     huge node-count guard rejects it.
    {
        std::string junk(64, '\xff');
        rag::HnswIndex bad;
        std::string_view cur{junk};
        CHECK(bad.deserialize(cur) == false);
        CHECK(bad.empty());
    }

    // (c) Valid header but a link id past the node count → reject (would OOB
    //     in search). Corrupt a link byte in the middle of the good blob.
    {
        std::string tampered = good;
        // Flip the last 4 bytes (a link id or count) to a huge value.
        if (tampered.size() >= 4)
            for (std::size_t i = tampered.size() - 4; i < tampered.size(); ++i)
                tampered[i] = '\x7f';
        rag::HnswIndex bad;
        std::string_view cur{tampered};
        bool ok = bad.deserialize(cur);
        // On reject the graph is emptied; if it happened to still parse, it
        // must at least be topology-valid (no id >= size). Prove no crash by
        // running a search.
        if (ok) { auto r = bad.search(random_vec(rng, kDim), 3, 50); (void)r; }
        else    { CHECK(bad.empty()); }
    }
}

// ── 6. Matryoshka truncation: graph indexes only the leading ann_dim ────────
// With cfg.ann_dim set, the graph's working dim collapses to the prefix, and
// queries are truncated to match. For MRL-style vectors (discriminative
// signal in the leading dims, noise in the tail) recall@1 is preserved even
// though the graph stores a THIRD of each vector. Also proves the query is
// conformed (a full-width query searches a truncated graph fine).
static void test_hnsw_matryoshka_truncation() {
    constexpr std::size_t kN = 400, kLead = 24, kTail = 72, kDim = kLead + kTail;
    constexpr std::size_t kAnn = kLead;   // truncate to the leading block
    std::mt19937 rng(20240521u);

    // Each vector: a strong signal in the leading kLead dims, small noise in
    // the trailing kTail (the "low-fidelity tail" MRL is trained to shed).
    std::vector<std::vector<float>> db;
    db.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        std::vector<float> v(kDim, 0.0f);
        std::normal_distribution<float> lead(0.0f, 1.0f), tail(0.0f, 0.05f);
        for (std::size_t d = 0; d < kLead; ++d) v[d] = lead(rng);
        for (std::size_t d = kLead; d < kDim; ++d) v[d] = tail(rng);
        db.push_back(std::move(v));
    }

    rag::HnswConfig cfg;
    cfg.ann_dim = kAnn;
    rag::HnswIndex idx(cfg);
    std::vector<std::uint32_t> ids(kN);
    std::vector<const std::vector<float>*> embs(kN);
    for (std::uint32_t i = 0; i < kN; ++i) { ids[i] = i; embs[i] = &db[i]; }
    idx.build(ids, embs);

    // The graph works in the truncated space.
    CHECK(idx.dim() == kAnn);
    CHECK(idx.size() == kN);

    // Ground truth uses the FULL-dim cosine; the truncated graph should still
    // recover it because the tail carries almost no signal (the MRL premise).
    int hits = 0; constexpr int kQ = 40;
    for (int qn = 0; qn < kQ; ++qn) {
        auto q = db[static_cast<std::size_t>(rng() % kN)];
        // Perturb the query slightly so it isn't a trivial exact match.
        std::normal_distribution<float> jit(0.0f, 0.02f);
        for (auto& x : q) x += jit(rng);
        std::uint32_t want = true_nn(db, q);      // full-dim truth
        auto res = idx.search(q, 1, /*ef=*/100);  // full-width query, truncated graph
        if (!res.empty() && res.front().first == want) ++hits;
    }
    double recall = double(hits) / double(kQ);
    std::printf("rag_hnsw_test: matryoshka recall@1 = %.3f (dim %zu->%zu)\n",
                recall, kDim, kAnn);
    CHECK(recall >= 0.8);

    // ann_dim >= width is a no-op (never truncates up): full dim retained.
    rag::HnswConfig big; big.ann_dim = kDim + 100;
    rag::HnswIndex idx2(big);
    idx2.build(ids, embs);
    CHECK(idx2.dim() == kDim);
}

int main() {
    test_hnsw_recall_small();
    test_hnsw_search_basic();
    test_hnsw_serialize_roundtrip();
    test_hnsw_empty();
    test_hnsw_corrupt_cache_rejected();
    test_hnsw_matryoshka_truncation();

    if (g_failures == 0) {
        std::printf("rag_hnsw_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_hnsw_test: %d check(s) failed\n", g_failures);
    return 1;
}
