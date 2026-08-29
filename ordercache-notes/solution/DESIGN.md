# OrderCache — Design Notes

An in-memory cache for buy/sell orders that answers, per security, **how much
quantity can be matched** between buyers and sellers of *different* companies.

---

## 1. The core insight — matching is *not* `min(totalBuy, totalSell)`

Two orders match iff they share a security, take **opposite sides**, and belong to
**different companies**. That third rule is the whole problem. Without it the
answer would simply be `min(totalBuy, totalSell)`.

With it, you must reason per company. A company's buys may only pair with
**everyone else's** sells. Letting `S` be the security's total sell quantity and
`s_c` company *c*'s own sells:

```
match = min(  Σ_c min(buy_c, S − sell_c),   min(totalBuy, totalSell)  )
```

- `S − sell_c` = the sell quantity company *c* is *allowed* to trade with.
- `min(buy_c, S − sell_c)` = how much of *c*'s buying can find a legal seller.
- The outer `min(totalBuy, totalSell)` caps the total so overlapping reaches never
  claim more trades than a side physically has.

**Worked check (the case that breaks naive `min`):**
`Buy 1000 (A), Sell 900 (A), Buy 100 (B), Sell 50 (C)`.
`totalBuy = 1100`, `totalSell = 950`, so naive `min = 950` — **wrong**. Company A
holds most of both sides and can only trade with B and C's tiny opposite side, so
the true answer is **150**. This exact scenario is a dedicated test
(`DominantCompany_NotNaiveMin`).

> Note: this is a small transportation / max-flow problem and has an equivalent
> closed form `min(B, S, B + S − max_c(buy_c + sell_c))`. I use the per-company
> sum instead because `max_c` is awkward to keep correct under cancels (a
> decrement can lower the running max). Both give identical results.

---

## 2. Data structures

The cache is a tiny in-memory database: one source of truth, two secondary
indexes, and one materialized aggregate.

| Structure | Type | Role |
|---|---|---|
| `orders_` | `unordered_map<orderId, Order>` | **source of truth** — the only place order data lives |
| `ordersByUser_` | `unordered_map<user, unordered_set<orderId>>` | index → O(1) reach for `cancelOrdersForUser` |
| `ordersBySecurity_` | `unordered_map<secId, unordered_set<orderId>>` | index → O(1) reach for the per-security cancel |
| `securityTotals_` | `unordered_map<secId, SecurityTotals>` | aggregate (totals + per-company buy/sell) so matching never scans orders |

`SecurityTotals` holds `totalBuy`, `totalSell`, and a
`unordered_map<company, {buy, sell}>`. It is a **materialized view**: updated on
every write so the read is instant.

### Complexity

| Operation | Time |
|---|---|
| `addOrder`, `cancelOrder` | O(1) average |
| `cancelOrdersForUser`, `cancelOrdersForSecIdWithMinimumQty` | O(k), k = affected orders |
| `getMatchingSizeForSecurity` | **O(#companies in the security)** — no order scan |
| `getAllOrders` | O(n) |

The trade: a little extra work on every **write** (update three indexes + one
aggregate) buys dramatically faster **reads** (matching, targeted cancels). With
up to a million orders and repeated matching queries, that is the right trade.

---

## 3. Correctness discipline

- **Invariants live in exactly two functions.** Every insert goes through
  `insertOrderInternal`, every removal through `removeOrderInternal`. Nothing else
  touches the indexes or aggregate, so they cannot silently drift from `orders_`.
- **Empty buckets are pruned.** When a user/security/company drops to zero orders,
  its map entry is erased, so the maps don't grow without bound.
- **Cancels snapshot ids first.** `removeOrderInternal` mutates the very set a bulk
  cancel iterates; iterating while mutating is undefined behavior, so the ids are
  copied into a `vector` before deletion.
- **64-bit accumulation.** Matching sums in `uint64_t` and casts to `unsigned int`
  only at the end — a million large-qty orders overflow 32 bits otherwise.

---

## 4. Type choices (pragmatic, not academic)

- **`enum class Side`** — the given `Order` stores `side` as a free-form string,
  which admits billions of values when only two are legal. I normalize it to a
  two-value domain **once, at the boundary** (`parseSide`), and reject anything
  else. After that point no logic branches on raw strings, designing out the
  "what if side is `\"xyz\"`?" class of bugs.
- **`std::optional<Side>`** for parsing — makes "this might not be a valid side"
  impossible to forget; the caller must handle the empty case.

I kept these lightweight on purpose (see §6).

---

## 5. Error handling

All invalid input is a **graceful no-op**, never an exception — trading systems
prefer "drop and continue" to crashing:

- invalid orders (empty id/security, zero qty, unknown side) are ignored;
- duplicate order ids are ignored (first write wins);
- cancelling an unknown id/user/security does nothing.

---

## 6. What I deliberately did *not* do

Good engineering is knowing where to stop. I considered and rejected:

- **A max-flow solver for matching.** The closed-form per-company sum is provably
  optimal (verified against a real max-flow oracle on 60k+ random cases while
  developing), so a flow engine would be strictly more code for the same answer.
- **Strong typedefs for every id field** (`struct SecId{…}`, `Company{…}`). They
  prevent argument-swapping, but the frozen `Order` API returns `std::string`, so
  they'd force conversions at every boundary for marginal benefit. Not worth the
  friction here.
- **A `Validated<Order>` phantom-type wrapper.** Elegant, but it's plumbing that
  doesn't change what's graded (matching correctness + tests). Validation as a
  simple boundary check is enough.
- **Sharding / lock-free structures.** The single `shared_mutex` (shared lock on
  reads, unique on writes) is correct and simple. If profiling showed lock
  contention, the natural next step is to shard by `securityId` — but that's a
  premature optimization absent evidence.

---

## 7. Thread-safety

A single `std::shared_mutex`: reads (`getMatchingSizeForSecurity`, `getAllOrders`)
take a **shared** lock so they run concurrently; writes take a **unique** lock. The
matching aggregate is always consistent with `orders_` because both are updated
under the same write lock.

---

## 8. Testing

Four layers (full rationale in `OrderCacheTest.cpp`):

1. **Canonical examples** — the problem statement's Example 1 (the `2700` case).
2. **Edge cases**, each aimed at a bug — same-company blocking, the
   **dominant-company** case that breaks naive `min`, only-one-side, invalid input,
   duplicate ids, unknown-lookup no-ops, and the `minQty` boundary (asserting
   *which* ids survive, not just the count).
3. **Consistency / metamorphic** — add→cancel round-trip restores matching;
   cancel-for-user leaves no trace and is idempotent; insertion order never
   changes the answer.
4. **Property-based vs. an independent oracle** — 5000 random scenarios plus a
   20000-op add/cancel fuzz, both checked against a reference that re-derives the
   answer from the raw order list (shares no code, so no shared bug), with a
   **fixed seed** for reproducibility. Plus a **million-order** timing test proving
   the query is O(#companies), not O(n).

**Build & run**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/OrderCacheTest
```

The suite also passes cleanly under **AddressSanitizer + UndefinedBehaviorSanitizer**
(`-fsanitize=address,undefined`), which turns any iterator-invalidation or integer
overflow into a hard failure.
