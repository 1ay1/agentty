#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <shared_mutex>

class Order
{

 public:

  // do not alter signature of this constructor
  Order(
      const std::string& ordId,
      const std::string& secId,
      const std::string& side,
      const unsigned int qty,
      const std::string& user,
      const std::string& company)
      : m_orderId(ordId),
        m_securityId(secId),
        m_side(side),
        m_qty(qty),
        m_user(user),
        m_company(company) { }

  // do not alter these accessor methods
  std::string orderId() const    { return m_orderId; }
  std::string securityId() const { return m_securityId; }
  std::string side() const       { return m_side; }
  std::string user() const       { return m_user; }
  std::string company() const    { return m_company; }
  unsigned int qty() const       { return m_qty; }

 private:

  // use the below to hold the order data
  // do not remove the these member variables
  std::string m_orderId;     // unique order id
  std::string m_securityId;  // security identifier
  std::string m_side;        // side of the order, eg Buy or Sell
  unsigned int m_qty;        // qty for this order
  std::string m_user;        // user name who owns this order
  std::string m_company;     // company for user

};

// Provide an implementation for the OrderCacheInterface interface class.
// Your implementation class should hold all relevant data structures you think
// are needed.
class OrderCacheInterface
{

 public:

  // implement the 6 methods below, do not alter signatures

  // add order to the cache
  virtual void addOrder(Order order) = 0;

  // remove order with this unique order id from the cache
  virtual void cancelOrder(const std::string& orderId) = 0;

  // remove all orders in the cache for this user
  virtual void cancelOrdersForUser(const std::string& user) = 0;

  // remove all orders in the cache for this security with qty >= minQty
  virtual void cancelOrdersForSecIdWithMinimumQty(const std::string& securityId, unsigned int minQty) = 0;

  // return the total qty that can match for the security id
  virtual unsigned int getMatchingSizeForSecurity(const std::string& securityId) = 0;

  // return all orders in cache in a vector
  virtual std::vector<Order> getAllOrders() const = 0;

};


//──────────────────────────────────────────────────────────────────────────────
// OrderCache implementation
//──────────────────────────────────────────────────────────────────────────────

class OrderCache : public OrderCacheInterface
{

 public:

  void addOrder(Order order) override;

  void cancelOrder(const std::string& orderId) override;

  void cancelOrdersForUser(const std::string& user) override;

  void cancelOrdersForSecIdWithMinimumQty(const std::string& securityId, unsigned int minQty) override;

  unsigned int getMatchingSizeForSecurity(const std::string& securityId) override;

  std::vector<Order> getAllOrders() const override;

 private:

  //────────────────────────────────────────────────────────────────────────────
  // Data structures
  //────────────────────────────────────────────────────────────────────────────

  // Primary storage: order ID → Order
  // This is the source of truth. Each order is stored exactly once here.
  std::unordered_map<std::string, Order> orders_;

  // Index: user → set of order IDs belonging to that user
  // Enables O(k) cancellation by user, where k = number of user's orders.
  std::unordered_map<std::string, std::unordered_set<std::string>> ordersByUser_;

  // Index: security ID → set of order IDs for that security
  // Enables O(m) cancellation by security, where m = orders for that security.
  std::unordered_map<std::string, std::unordered_set<std::string>> ordersBySecurity_;

  //────────────────────────────────────────────────────────────────────────────
  // Aggregated totals for fast matching calculation
  //────────────────────────────────────────────────────────────────────────────

  // Per-company buy and sell totals within a security.
  struct CompanyTotals {
    std::uint64_t buy = 0;
    std::uint64_t sell = 0;
  };

  // Per-security totals: overall buy/sell and per-company breakdown.
  struct SecurityTotals {
    std::uint64_t totalBuy = 0;
    std::uint64_t totalSell = 0;
    std::unordered_map<std::string, CompanyTotals> companies;
  };

  // security ID → SecurityTotals
  std::unordered_map<std::string, SecurityTotals> securityTotals_;

  //────────────────────────────────────────────────────────────────────────────
  // Thread safety (optional but recommended per spec)
  //────────────────────────────────────────────────────────────────────────────

  mutable std::shared_mutex mutex_;

  //────────────────────────────────────────────────────────────────────────────
  // Helpers
  //────────────────────────────────────────────────────────────────────────────

  // Validates order fields. Returns true if order is valid.
  static bool isValidOrder(const Order& order);

  // Inserts an already-validated order into every index + aggregate.
  // Caller must hold the exclusive lock and must have checked for a duplicate id.
  void insertOrderInternal(const Order& order);

  // Removes an order from all indexes and totals. Caller must hold exclusive lock.
  // Returns true if order existed and was removed.
  bool removeOrderInternal(const std::string& orderId);

};
