#include "OrderCache.h"
#include <algorithm>
#include <limits>
#include <mutex>

//──────────────────────────────────────────────────────────────────────────────
// Validation
//──────────────────────────────────────────────────────────────────────────────

bool OrderCache::isValidOrder(const Order& order)
{
    // Check for empty strings
    if (order.orderId().empty())    return false;
    if (order.securityId().empty()) return false;
    if (order.user().empty())       return false;
    if (order.company().empty())    return false;

    // Side must be "Buy" or "Sell"
    const std::string& side = order.side();
    if (side != "Buy" && side != "Sell") return false;

    // Quantity must be positive
    if (order.qty() == 0) return false;

    return true;
}

//──────────────────────────────────────────────────────────────────────────────
// addOrder
//──────────────────────────────────────────────────────────────────────────────

void OrderCache::addOrder(Order order)
{
    // Validate before acquiring lock
    if (!isValidOrder(order)) {
        return; // Silently reject invalid orders
    }

    std::unique_lock lock(mutex_);

    const std::string& orderId = order.orderId();

    // Reject duplicate order IDs
    if (orders_.find(orderId) != orders_.end()) {
        return;
    }

    // Extract fields for indexing
    const std::string& user       = order.user();
    const std::string& securityId = order.securityId();
    const std::string& company    = order.company();
    const std::string& side       = order.side();
    const std::uint64_t qty       = order.qty();

    // Insert into primary storage
    orders_.emplace(orderId, std::move(order));

    // Update user index
    ordersByUser_[user].insert(orderId);

    // Update security index
    ordersBySecurity_[securityId].insert(orderId);

    // Update aggregated totals for matching
    SecurityTotals& secTotals = securityTotals_[securityId];
    CompanyTotals& compTotals = secTotals.companies[company];

    if (side == "Buy") {
        secTotals.totalBuy += qty;
        compTotals.buy += qty;
    } else {
        secTotals.totalSell += qty;
        compTotals.sell += qty;
    }
}

//──────────────────────────────────────────────────────────────────────────────
// removeOrderInternal (helper)
//──────────────────────────────────────────────────────────────────────────────

bool OrderCache::removeOrderInternal(const std::string& orderId)
{
    // Find the order
    auto orderIt = orders_.find(orderId);
    if (orderIt == orders_.end()) {
        return false;
    }

    const Order& order = orderIt->second;

    // Extract fields before erasing
    const std::string& user       = order.user();
    const std::string& securityId = order.securityId();
    const std::string& company    = order.company();
    const std::string& side       = order.side();
    const std::uint64_t qty       = order.qty();

    // Remove from user index
    auto userIt = ordersByUser_.find(user);
    if (userIt != ordersByUser_.end()) {
        userIt->second.erase(orderId);
        if (userIt->second.empty()) {
            ordersByUser_.erase(userIt);
        }
    }

    // Remove from security index
    auto secIdxIt = ordersBySecurity_.find(securityId);
    if (secIdxIt != ordersBySecurity_.end()) {
        secIdxIt->second.erase(orderId);
        if (secIdxIt->second.empty()) {
            ordersBySecurity_.erase(secIdxIt);
        }
    }

    // Update aggregated totals
    auto secTotalsIt = securityTotals_.find(securityId);
    if (secTotalsIt != securityTotals_.end()) {
        SecurityTotals& secTotals = secTotalsIt->second;

        auto compIt = secTotals.companies.find(company);
        if (compIt != secTotals.companies.end()) {
            CompanyTotals& compTotals = compIt->second;

            if (side == "Buy") {
                secTotals.totalBuy -= qty;
                compTotals.buy -= qty;
            } else {
                secTotals.totalSell -= qty;
                compTotals.sell -= qty;
            }

            // Clean up empty company entry
            if (compTotals.buy == 0 && compTotals.sell == 0) {
                secTotals.companies.erase(compIt);
            }
        }

        // Clean up empty security entry
        if (secTotals.totalBuy == 0 && secTotals.totalSell == 0) {
            securityTotals_.erase(secTotalsIt);
        }
    }

    // Remove from primary storage
    orders_.erase(orderIt);

    return true;
}

//──────────────────────────────────────────────────────────────────────────────
// cancelOrder
//──────────────────────────────────────────────────────────────────────────────

void OrderCache::cancelOrder(const std::string& orderId)
{
    std::unique_lock lock(mutex_);
    removeOrderInternal(orderId);
}

//──────────────────────────────────────────────────────────────────────────────
// cancelOrdersForUser
//──────────────────────────────────────────────────────────────────────────────

void OrderCache::cancelOrdersForUser(const std::string& user)
{
    std::unique_lock lock(mutex_);

    auto it = ordersByUser_.find(user);
    if (it == ordersByUser_.end()) {
        return;
    }

    // Copy the order IDs to avoid iterator invalidation
    std::vector<std::string> orderIds(it->second.begin(), it->second.end());

    for (const std::string& orderId : orderIds) {
        removeOrderInternal(orderId);
    }
}

//──────────────────────────────────────────────────────────────────────────────
// cancelOrdersForSecIdWithMinimumQty
//──────────────────────────────────────────────────────────────────────────────

void OrderCache::cancelOrdersForSecIdWithMinimumQty(
    const std::string& securityId,
    unsigned int minQty)
{
    std::unique_lock lock(mutex_);

    auto it = ordersBySecurity_.find(securityId);
    if (it == ordersBySecurity_.end()) {
        return;
    }

    // Collect order IDs that match the criteria
    std::vector<std::string> toCancel;

    for (const std::string& orderId : it->second) {
        auto orderIt = orders_.find(orderId);
        if (orderIt != orders_.end() && orderIt->second.qty() >= minQty) {
            toCancel.push_back(orderId);
        }
    }

    // Cancel them
    for (const std::string& orderId : toCancel) {
        removeOrderInternal(orderId);
    }
}

//──────────────────────────────────────────────────────────────────────────────
// getMatchingSizeForSecurity
//──────────────────────────────────────────────────────────────────────────────
//
// The maximum matching quantity is:
//
//     M = min(B, S, B + S - L)
//
// Where:
//     B = total buy quantity for this security
//     S = total sell quantity for this security
//     L = largest company volume (buy + sell) for this security
//
// Intuition:
//   - Cannot match more than total buys (each match consumes a buy)
//   - Cannot match more than total sells (each match consumes a sell)
//   - Cannot match more than orders outside the largest company
//     (same-company orders cannot match each other)
//
//──────────────────────────────────────────────────────────────────────────────

unsigned int OrderCache::getMatchingSizeForSecurity(const std::string& securityId)
{
    std::shared_lock lock(mutex_);

    auto it = securityTotals_.find(securityId);
    if (it == securityTotals_.end()) {
        return 0;
    }

    const SecurityTotals& totals = it->second;

    // Find the largest company volume
    std::uint64_t largestCompanyVolume = 0;
    for (const auto& entry : totals.companies) {
        const CompanyTotals& comp = entry.second;
        std::uint64_t companyVolume = comp.buy + comp.sell;
        largestCompanyVolume = std::max(largestCompanyVolume, companyVolume);
    }

    // Calculate the three limits
    const std::uint64_t totalBuy  = totals.totalBuy;
    const std::uint64_t totalSell = totals.totalSell;
    const std::uint64_t outsideLargest = totalBuy + totalSell - largestCompanyVolume;

    // The answer is the minimum of all three
    const std::uint64_t result = std::min({totalBuy, totalSell, outsideLargest});

    // Clamp to unsigned int range (defensive, unlikely with real data)
    if (result > std::numeric_limits<unsigned int>::max()) {
        return std::numeric_limits<unsigned int>::max();
    }

    return static_cast<unsigned int>(result);
}

//──────────────────────────────────────────────────────────────────────────────
// getAllOrders
//──────────────────────────────────────────────────────────────────────────────

std::vector<Order> OrderCache::getAllOrders() const
{
    std::shared_lock lock(mutex_);

    std::vector<Order> result;
    result.reserve(orders_.size());

    for (const auto& entry : orders_) {
        result.push_back(entry.second);
    }

    return result;
}
