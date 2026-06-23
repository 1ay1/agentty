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

int main() {
    test_hnsw_recall_small();
    test_hnsw_search_basic();
    test_hnsw_serialize_roundtrip();
    test_hnsw_empty();

    if (g_failures == 0) {
        std::printf("rag_hnsw_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_hnsw_test: %d check(s) failed\n", g_failures);
    return 1;
}
