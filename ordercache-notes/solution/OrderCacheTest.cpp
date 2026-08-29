#include "OrderCache.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
//  Test strategy (see DESIGN.md). Four layers:
//    1. Canonical examples from the problem statement.
//    2. Edge cases, each aimed at a specific bug.
//    3. Consistency / metamorphic tests (relationships between operations).
//    4. Property-based test vs. an INDEPENDENT oracle over thousands of random
//       scenarios (fixed seed = reproducible), plus a performance smoke test.
// ============================================================================

namespace {

class OrderCacheTest : public ::testing::Test {
 protected:
    OrderCache cache;
};

// The problem statement's Example 1, security "SecId2" — the canonical 2700.
void seedExample1(OrderCache& c) {
    c.addOrder({"o1", "SecId1", "Buy",  1000, "u1", "CompanyA"});
    c.addOrder({"o2", "SecId2", "Sell", 3000, "u2", "CompanyB"});
    c.addOrder({"o3", "SecId1", "Sell",  500, "u3", "CompanyA"});
    c.addOrder({"o4", "SecId2", "Buy",   600, "u4", "CompanyC"});
    c.addOrder({"o5", "SecId2", "Buy",   100, "u5", "CompanyB"});
    c.addOrder({"o6", "SecId3", "Buy",  1000, "u6", "CompanyD"});
    c.addOrder({"o7", "SecId2", "Buy",  2000, "u7", "CompanyE"});
    c.addOrder({"o8", "SecId2", "Sell", 5000, "u8", "CompanyE"});
}

// ---------------------------------------------------------------------------
//  Independent oracle.  Re-derives the match size directly from the raw order
//  list, WITHOUT touching the cache's maintained aggregate. Because it computes
//  the answer a different way, it shares no bug with the implementation — so a
//  disagreement pinpoints a bug in how the aggregate is *maintained* (the most
//  error-prone part). Used only on small random inputs.
// ---------------------------------------------------------------------------
unsigned int oracleMatch(const std::vector<Order>& all, const std::string& sec) {
    std::uint64_t totalBuy = 0, totalSell = 0;
    std::unordered_map<std::string, std::array<std::uint64_t, 2>> perCo;  // [buy, sell]
    for (const auto& o : all) {
        if (o.securityId() != sec) continue;
        const bool isBuy = (o.side() == "Buy" || o.side() == "buy" || o.side() == "BUY");
        if (isBuy) { totalBuy  += o.qty(); perCo[o.company()][0] += o.qty(); }
        else       { totalSell += o.qty(); perCo[o.company()][1] += o.qty(); }
    }
    std::uint64_t reach = 0;
    for (const auto& e : perCo)
        reach += std::min<std::uint64_t>(e.second[0], totalSell - e.second[1]);
    return static_cast<unsigned int>(std::min(reach, std::min(totalBuy, totalSell)));
}

// ==========================================================================
//  Layer 1 — canonical examples
// ==========================================================================

TEST_F(OrderCacheTest, ReadmeExample1_MatchSizes) {
    seedExample1(cache);
    EXPECT_EQ(cache.getMatchingSizeForSecurity("SecId1"), 0u);     // same company only
    EXPECT_EQ(cache.getMatchingSizeForSecurity("SecId2"), 2700u);  // the canonical value
    EXPECT_EQ(cache.getMatchingSizeForSecurity("SecId3"), 0u);     // only a buy
}

// ==========================================================================
//  Layer 2 — edge cases, each targeting a bug
// ==========================================================================

TEST_F(OrderCacheTest, SameCompanyCannotMatch) {
    cache.addOrder({"a", "S", "Buy",  500, "u1", "A"});
    cache.addOrder({"b", "S", "Sell", 500, "u2", "A"});           // same company
    EXPECT_EQ(cache.getMatchingSizeForSecurity("S"), 0u);
}

TEST_F(OrderCacheTest, OneCrossCompanyMatch) {
    cache.addOrder({"a", "S", "Buy",  100, "u1", "A"});
    cache.addOrder({"b", "S", "Sell", 100, "u2", "A"});           // blocked (same co)
    cache.addOrder({"c", "S", "Sell", 100, "u3", "B"});           // allowed
    EXPECT_EQ(cache.getMatchingSizeForSecurity("S"), 100u);
}

// THE distinguishing test: naive min(totalBuy, totalSell) = min(1100, 950) = 950,
// but the correct answer is 150. Company A holds most of both sides and can only
// trade with B and C's small opposite side.
TEST_F(OrderCacheTest, DominantCompany_NotNaiveMin) {
    cache.addOrder({"a", "S", "Buy",  1000, "u1", "A"});
    cache.addOrder({"b", "S", "Sell",  900, "u2", "A"});
    cache.addOrder({"c", "S", "Buy",   100, "u3", "B"});
    cache.addOrder({"d", "S", "Sell",   50, "u4", "C"});
    EXPECT_EQ(cache.getMatchingSizeForSecurity("S"), 150u);
}

TEST_F(OrderCacheTest, OnlyOneSide_Zero) {
    cache.addOrder({"a", "S", "Buy", 100, "u1", "A"});
    cache.addOrder({"b", "S", "Buy", 200, "u2", "B"});
    EXPECT_EQ(cache.getMatchingSizeForSecurity("S"), 0u);
}

TEST_F(OrderCacheTest, PartialMatch_LimitedBySmallerSide) {
    cache.addOrder({"a", "S", "Buy",  100, "u1", "A"});
    cache.addOrder({"b", "S", "Sell", 300, "u2", "B"});
    EXPECT_EQ(cache.getMatchingSizeForSecurity("S"), 100u);
}

TEST_F(OrderCacheTest, InvalidOrdersAreIgnored) {
    cache.addOrder({"",  "S", "Buy", 100, "u", "A"});             // empty id
    cache.addOrder({"x", "",  "Buy", 100, "u", "A"});             // empty security
    cache.addOrder({"y", "S", "Buy",   0, "u", "A"});             // zero qty
    cache.addOrder({"z", "S", "xyz", 100, "u", "A"});             // bad side
    EXPECT_TRUE(cache.getAllOrders().empty());
}

TEST_F(OrderCacheTest, DuplicateOrderIdIsIgnored) {
    cache.addOrder({"dup", "S", "Buy", 100, "u", "A"});
    cache.addOrder({"dup", "S", "Sell", 999, "u", "B"});         // same id -> ignored
    auto all = cache.getAllOrders();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].qty(), 100u);                                // the first one won
}

TEST_F(OrderCacheTest, UnknownLookupsAndCancelsAreGracefulNoOps) {
    EXPECT_EQ(cache.getMatchingSizeForSecurity("nope"), 0u);
    cache.cancelOrder("ghost");                                   // must not throw
    cache.cancelOrdersForUser("nobody");                         // must not throw
    cache.cancelOrdersForSecIdWithMinimumQty("nada", 10);        // must not throw
    SUCCEED();
}

// ==========================================================================
//  cancelOrdersForSecIdWithMinimumQty — assert WHICH orders survive
// ==========================================================================

TEST_F(OrderCacheTest, CancelForSecMinQty_BoundaryIsInclusive) {
    cache.addOrder({"keepLow",  "S", "Buy",   99, "u1", "A"});   // 99  < 100 : kept
    cache.addOrder({"killEq",   "S", "Buy",  100, "u2", "A"});   // 100 == 100: removed
    cache.addOrder({"killHigh", "S", "Sell", 300, "u3", "B"});   // 300 >= 100: removed
    cache.addOrder({"otherSec", "T", "Buy",  500, "u4", "C"});   // different security: untouched

    cache.cancelOrdersForSecIdWithMinimumQty("S", 100);

    std::unordered_set<std::string> ids;
    for (const auto& o : cache.getAllOrders()) ids.insert(o.orderId());
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count("keepLow"));    // strictly-less-than-minQty survives
    EXPECT_TRUE(ids.count("otherSec"));   // other securities untouched
    EXPECT_FALSE(ids.count("killEq"));    // ">=" must include the boundary
    EXPECT_FALSE(ids.count("killHigh"));
}

// ==========================================================================
//  Layer 3 — consistency / metamorphic
// ==========================================================================

TEST_F(OrderCacheTest, AddCancelRoundTrip_RestoresMatching) {
    seedExample1(cache);
    const auto before = cache.getMatchingSizeForSecurity("SecId2");
    cache.addOrder({"tmp", "SecId2", "Sell", 1234, "ux", "CompanyZ"});
    cache.cancelOrder("tmp");
    // If the aggregate is decremented correctly on cancel, we are exactly back.
    EXPECT_EQ(cache.getMatchingSizeForSecurity("SecId2"), before);
}

TEST_F(OrderCacheTest, CancelForUser_LeavesNoTraceAndIsIdempotent) {
    cache.addOrder({"a", "S1", "Buy",  100, "victim", "A"});
    cache.addOrder({"b", "S2", "Sell", 200, "victim", "A"});
    cache.addOrder({"c", "S1", "Sell", 100, "other",  "B"});

    cache.cancelOrdersForUser("victim");
    for (const auto& o : cache.getAllOrders())
        EXPECT_NE(o.user(), "victim");                            // none remain
    EXPECT_EQ(cache.getAllOrders().size(), 1u);                   // only "other" left

    cache.cancelOrdersForUser("victim");                          // second time: no-op
    EXPECT_EQ(cache.getAllOrders().size(), 1u);
}

TEST_F(OrderCacheTest, InsertionOrderDoesNotAffectMatching) {
    OrderCache a, b;
    std::vector<Order> orders = {
        {"1", "S", "Buy",  100, "u", "A"}, {"2", "S", "Sell", 100, "u", "B"},
        {"3", "S", "Buy",  200, "u", "C"}, {"4", "S", "Sell", 300, "u", "A"},
    };
    for (const auto& o : orders) a.addOrder(o);
    for (auto it = orders.rbegin(); it != orders.rend(); ++it) b.addOrder(*it);
    EXPECT_EQ(a.getMatchingSizeForSecurity("S"), b.getMatchingSizeForSecurity("S"));
}

// ==========================================================================
//  Layer 4 — property-based vs. oracle
// ==========================================================================

TEST_F(OrderCacheTest, RandomizedMatchesOracle) {
    std::mt19937 rng(1234567u);                                  // fixed seed
    std::uniform_int_distribution<int> nOrders(0, 10), qty(1, 20), pick3(0, 2), side(0, 1);
    const std::array<const char*, 3> companies = {"A", "B", "C"};

    for (int iter = 0; iter < 5000; ++iter) {
        OrderCache c;
        std::vector<Order> all;
        const int n = nOrders(rng);
        for (int i = 0; i < n; ++i) {
            Order o{"id" + std::to_string(iter) + "_" + std::to_string(i),
                    "S",
                    side(rng) ? "Buy" : "Sell",
                    static_cast<unsigned int>(qty(rng)),
                    "u",
                    companies[pick3(rng)]};
            all.push_back(o);
            c.addOrder(o);
        }
        EXPECT_EQ(c.getMatchingSizeForSecurity("S"), oracleMatch(all, "S"))
            << "mismatch at iter=" << iter << " (fixed seed, reproducible)";
    }
}

TEST_F(OrderCacheTest, RandomizedWithCancelsStaysConsistent) {
    std::mt19937 rng(42u);
    std::uniform_int_distribution<int> qty(1, 20), pick3(0, 2), side(0, 1);
    OrderCache c;
    std::unordered_map<std::string, Order> live;                // mirror of live orders

    for (int i = 0; i < 20000; ++i) {
        // ~70% add, ~30% cancel a random existing order
        if (live.empty() || (rng() % 10) < 7) {
            Order o{"id" + std::to_string(i), "S",
                    side(rng) ? "Buy" : "Sell",
                    static_cast<unsigned int>(qty(rng)), "u",
                    std::array<const char*, 3>{"A", "B", "C"}[pick3(rng)]};
            c.addOrder(o);
            live.emplace(o.orderId(), o);
        } else {
            auto it = std::next(live.begin(), rng() % live.size());
            c.cancelOrder(it->first);
            live.erase(it);
        }
    }
    std::vector<Order> snapshot;
    for (const auto& e : live) snapshot.push_back(e.second);
    EXPECT_EQ(c.getMatchingSizeForSecurity("S"), oracleMatch(snapshot, "S"));
    EXPECT_EQ(c.getAllOrders().size(), live.size());            // no leaks / no ghosts
}

// ==========================================================================
//  Performance smoke test — proves the query is O(#companies), not O(n).
// ==========================================================================

TEST_F(OrderCacheTest, MillionOrders_QueryIsFast) {
    OrderCache c;
    for (int i = 0; i < 1'000'000; ++i) {
        c.addOrder({"id" + std::to_string(i), "HOT",
                    (i & 1) ? "Buy" : "Sell", 1000u, "u",
                    (i % 3) ? "A" : "B"});
    }
    const auto t0 = std::chrono::steady_clock::now();
    volatile unsigned int m = c.getMatchingSizeForSecurity("HOT");
    (void)m;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 50) << "matching query should be O(#companies), got " << ms << " ms";
}

}  // namespace
