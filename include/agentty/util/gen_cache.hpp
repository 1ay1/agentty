#pragma once
// agentty/util/gen_cache.hpp — a cached value that a slow refill can't
// overwrite after it was cleared.
//
// A fetch reads gen() before going to the network and stores with
// store_if(value, gen). If clear() ran in between, the store is dropped, so
// a list fetched before an invalidation never lands after it.

#include <cstdint>
#include <utility>

#include <maya/runtime.hpp>

namespace agentty::util {

template <class T>
class GenCache {
public:
    [[nodiscard]] T get() const { return slot_.load()->value; }
    [[nodiscard]] std::uint64_t gen() const { return slot_.load()->gen; }

    // Store `v` only if nothing cleared the cache since `seen` was read.
    // Returns whether it landed.
    bool store_if(T v, std::uint64_t seen) {
        // update() may retry the body, so the result comes from the cell it
        // finally stored, not from a flag set inside the body.
        auto stored = slot_.update([](const Cell& cur, const T& next, std::uint64_t g) {
            return cur.gen == g ? Cell{next, g, true} : Cell{cur.value, cur.gen, false};
        }, std::move(v), seen);
        return stored->landed;
    }

    // Empty the cache and invalidate every fetch already in flight.
    void clear() {
        slot_.update([](const Cell& cur) { return Cell{T{}, cur.gen + 1, false}; });
    }

private:
    struct Cell { T value{}; std::uint64_t gen = 0; bool landed = false; };
    maya::snapshot<Cell> slot_;
};

}  // namespace agentty::util
