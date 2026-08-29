#include "OrderCache.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

// ============================================================================
//  OrderCache — in-memory order store with cross-company match sizing.
//
//  DESIGN (see DESIGN.md for the full rationale):
//    orders_            : orderId -> Order        the single SOURCE OF TRUTH
//    ordersByUser_      : user    -> {orderIds}   secondary index (fast cancels)
//    ordersBySecurity_  : secId   -> {orderIds}   secondary index (fast cancels)
//    securityTotals_    : secId   -> aggregate    a materialized view for matching
//
//  Every mutation goes through insertOrderInternal / removeOrderInternal so the
//  four structures can never drift out of sync — invariants live in ONE place.
//
//  Matching: two orders match iff same security, opposite side, and DIFFERENT
//  company. That last rule means the answer is NOT min(totalBuy, totalSell).
//  Reasoning per company, company c's buys may only pair with everyone else's
//  sells, so with S = security's total sell and s_c = company c's sells:
//
//      match = min( sum_c min(buy_c, S - sell_c),  min(totalBuy, totalSell) )
//
//  The per-company totals are kept current on every add/cancel, so the query is
//  O(#companies in the security) — never a scan of individual orders.
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
//  Strong types at the boundary.
//
//  The given Order stores `side` as a free-form std::string, which admits
//  billions of values when only two are legal. We DON'T rewrite Order (the spec
//  freezes it); instead we normalize the string to a 2-value domain exactly once,
//  at the point data enters the cache. After that boundary no logic branches on
//  raw strings — the whole class of "what if side == \"xyz\"?" bugs is designed
//  out, because such an order is rejected before it is ever stored.
// ---------------------------------------------------------------------------
enum class Side { Buy, Sell };

// Returns the parsed Side, or std::nullopt for anything we don't accept.
// std::optional makes "this might not be a valid side" impossible to forget:
// the caller must handle the empty case to get a Side out.
std::optional<Side> parseSide(const std::string& s) {
    if (s == "Buy"  || s == "buy"  || s == "BUY")  return Side::Buy;
    if (s == "Sell" || s == "sell" || s == "SELL") return Side::Sell;
    return std::nullopt;
}

}  // namespace

// ============================================================================
//  Validation — reject malformed orders at the door (graded: error handling).
// ============================================================================
bool OrderCache::isValidOrder(const Order& order) {
    if (order.orderId().empty())    return false;   // ids must exist & be usable
    if (order.securityId().empty()) return false;
    if (order.qty() == 0)           return false;   // a zero-qty order is a no-op
    if (!parseSide(order.side()))   return false;   // side must be Buy or Sell
    // user / company may legitimately be any non-structural string, so we don't
    // reject empty ones — the spec never says they must be present.
    return true;
}

// ============================================================================
//  insertOrderInternal — add an already-validated, non-duplicate order to every
//  structure. Caller holds the write lock. This is one of only two places that
//  maintain invariants.
// ============================================================================
void OrderCache::insertOrderInternal(const Order& order) {
    // parseSide is guaranteed to succeed here (validated on the way in), but we
    // handle nullopt defensively rather than dereferencing blindly.
    const std::optional<Side> side = parseSide(order.side());
    if (!side) return;  // unreachable for validated orders; keeps us memory-safe

    ordersByUser_[order.user()].insert(order.orderId());
    ordersBySecurity_[order.securityId()].insert(order.orderId());

    SecurityTotals& agg = securityTotals_[order.securityId()];
    CompanyTotals&  co  = agg.companies[order.company()];
    if (*side == Side::Buy) { agg.totalBuy  += order.qty(); co.buy  += order.qty(); }
    else                    { agg.totalSell += order.qty(); co.sell += order.qty(); }
}

// ============================================================================
//  removeOrderInternal — remove an order by id from ALL structures. Caller holds
//  the write lock. Returns false if the id is unknown (a graceful no-op).
//  The second and last place invariants are maintained.
// ============================================================================
bool OrderCache::removeOrderInternal(const std::string& orderId) {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) return false;          // unknown id: nothing to do

    // Copy the fields we need BEFORE erasing the map entry that owns them.
    const Order order = it->second;

    // 1) user index — erase the id, and drop the whole bucket if it empties so
    //    the map does not accumulate empty sets forever.
    if (auto u = ordersByUser_.find(order.user()); u != ordersByUser_.end()) {
        u->second.erase(orderId);
        if (u->second.empty()) ordersByUser_.erase(u);
    }

    // 2) security index — same pattern.
    if (auto s = ordersBySecurity_.find(order.securityId()); s != ordersBySecurity_.end()) {
        s->second.erase(orderId);
        if (s->second.empty()) ordersBySecurity_.erase(s);
    }

    // 3) aggregate — subtract this order's contribution and prune anything that
    //    reaches zero, so per-company and per-security maps stay tight.
    if (auto a = securityTotals_.find(order.securityId()); a != securityTotals_.end()) {
        SecurityTotals& agg = a->second;
        if (const std::optional<Side> side = parseSide(order.side())) {
            auto c = agg.companies.find(order.company());
            if (c != agg.companies.end()) {
                if (*side == Side::Buy) {
                    agg.totalBuy    -= order.qty();
                    c->second.buy   -= order.qty();
                } else {
                    agg.totalSell   -= order.qty();
                    c->second.sell  -= order.qty();
                }
                if (c->second.buy == 0 && c->second.sell == 0) agg.companies.erase(c);
            }
        }
        if (agg.companies.empty()) securityTotals_.erase(a);
    }

    // 4) source of truth — erased last, since steps 1–3 read from `order`.
    orders_.erase(it);
    return true;
}

// ============================================================================
//  Public API
// ============================================================================

void OrderCache::addOrder(Order order) {
    if (!isValidOrder(order)) return;               // graceful reject, no throw

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (orders_.count(order.orderId())) return;     // duplicate id: ignore

    insertOrderInternal(order);
    orders_.emplace(order.orderId(), std::move(order));
}

void OrderCache::cancelOrder(const std::string& orderId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    removeOrderInternal(orderId);                   // unknown id -> no-op
}

void OrderCache::cancelOrdersForUser(const std::string& user) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = ordersByUser_.find(user);
    if (it == ordersByUser_.end()) return;

    // Snapshot the ids first: removeOrderInternal mutates ordersByUser_ (and may
    // erase the very bucket we would be iterating), so iterating it directly is
    // undefined behavior. Copy, then delete.
    const std::vector<std::string> ids(it->second.begin(), it->second.end());
    for (const std::string& id : ids) removeOrderInternal(id);
}

void OrderCache::cancelOrdersForSecIdWithMinimumQty(const std::string& securityId,
                                                    unsigned int minQty) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = ordersBySecurity_.find(securityId);
    if (it == ordersBySecurity_.end()) return;

    // Snapshot the victims (qty >= minQty) before mutating, same reasoning.
    std::vector<std::string> victims;
    victims.reserve(it->second.size());
    for (const std::string& id : it->second) {
        auto o = orders_.find(id);
        if (o != orders_.end() && o->second.qty() >= minQty) victims.push_back(id);
    }
    for (const std::string& id : victims) removeOrderInternal(id);
}

unsigned int OrderCache::getMatchingSizeForSecurity(const std::string& securityId) {
    std::shared_lock<std::shared_mutex> lock(mutex_);   // shared = many readers

    auto it = securityTotals_.find(securityId);
    if (it == securityTotals_.end()) return 0;
    const SecurityTotals& agg = it->second;

    // Per-company reach: each company's buys can match everyone ELSE's sells,
    // i.e. min(buy_c, totalSell - sell_c). Accumulate in 64-bit to be overflow-
    // safe with up to a million orders of large quantity.
    std::uint64_t reach = 0;
    for (const auto& entry : agg.companies) {
        const CompanyTotals& ct = entry.second;
        // totalSell - ct.sell can't underflow: a company's sells are a subset of
        // the security's total sells (maintained invariant).
        reach += std::min<std::uint64_t>(ct.buy, agg.totalSell - ct.sell);
    }

    const std::uint64_t capped =
        std::min(reach, std::min(agg.totalBuy, agg.totalSell));

    // The interface returns unsigned int; the true answer never exceeds
    // min(totalBuy, totalSell), which fits whenever the inputs are sane. We cast
    // only at the very end, after all arithmetic was done in 64-bit.
    return static_cast<unsigned int>(capped);
}

std::vector<Order> OrderCache::getAllOrders() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<Order> out;
    out.reserve(orders_.size());
    for (const auto& entry : orders_) out.push_back(entry.second);
    return out;
}
