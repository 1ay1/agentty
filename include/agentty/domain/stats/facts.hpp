#pragma once
// agentty::stats::Facts — every statistic, folded in one incremental pass.
//
// The cost model, which is the design:
//
//   one token streamed          0   — no stats code runs at all
//   frame, panel closed         0   — Facts is never built
//   frame, thread settled       0 folds
//   frame, streaming            1 fold (the live tail)
//   panel opened on 800 turns   one O(n) walk of integer adds
//   fork / rewind / compaction  one O(n) rebuild
//
// ── Why incremental, and why the tail is special ─────────────────────────
//
// Messages [0, size-1) are SEALED: folded exactly once, ever. The last
// message is ALWAYS volatile — it is the one being streamed into — so it
// is folded into a scratch total that is recomputed each refresh and added
// on read. That is what makes the panel live during a stream without
// re-walking history, and it is the bug in the projection this replaces:
// a {message_count, thread_id} stamp cannot see the tail mutating, so a
// token counter under it would freeze mid-turn.
//
// ── Why a full rebuild on three operations ───────────────────────────────
//
// Fork, rewind and compaction MUTATE history rather than appending to it.
// Reconciling that incrementally is the optimisation that produces numbers
// which are wrong and believed. All three are rare and user-initiated, so
// the Epoch below detects them by value and resets the cursor.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/domain/smart_mode.hpp"

namespace agentty::stats {

// ── Tally ────────────────────────────────────────────────────────────────
//
// A small ordered counter. Linear scan, deliberately: distinct models in a
// session is < 20 and distinct tools < 40, and at that size a scan beats a
// hash map on both time and allocation. It also keeps INSERTION order
// stable, so rows do not reshuffle frame to frame while a stream runs —
// which reads as a rendering fault rather than as data.
class Tally {
public:
    void add(std::string_view key, std::uint64_t n = 1) {
        for (auto& [k, v] : rows_)
            if (k == key) { v += n; return; }
        rows_.emplace_back(std::string{key}, n);
    }
    [[nodiscard]] std::uint64_t total() const noexcept {
        std::uint64_t t = 0;
        for (const auto& [k, v] : rows_) t += v;
        return t;
    }
    [[nodiscard]] std::size_t size()  const noexcept { return rows_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return rows_.empty(); }
    [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>&
    rows() const noexcept { return rows_; }

    // Largest first; ties keep insertion order (stable_sort), so equal
    // counts do not swap places between frames.
    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>>
    ranked() const {
        auto out = rows_;
        std::stable_sort(out.begin(), out.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        return out;
    }
    void merge(const Tally& other) {
        for (const auto& [k, v] : other.rows_) add(k, v);
    }
    void clear() { rows_.clear(); }

private:
    std::vector<std::pair<std::string, std::uint64_t>> rows_;
};

// ── Hist ─────────────────────────────────────────────────────────────────
//
// A log2 latency histogram: 24 buckets covering 1 ms … ~4 h in 96 bytes.
// p50/p95 without storing a duration per call — the version that grows
// without bound on a long session, for a number nobody reads to three
// significant figures.
class Hist {
public:
    static constexpr int kBuckets = 24;

    void add(std::uint32_t ms) {
        ++count_;
        sum_ += ms;
        if (ms > max_) max_ = ms;
        int b = 0;
        for (std::uint32_t v = ms; v > 1 && b < kBuckets - 1; v >>= 1) ++b;
        ++bucket_[static_cast<std::size_t>(b)];
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t sum()   const noexcept { return sum_; }
    [[nodiscard]] std::uint32_t max()   const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }
    // Upper edge of the bucket the quantile falls in. Reported as the
    // bucket's ceiling rather than interpolated: the histogram genuinely
    // does not know where inside the bucket the sample sat, and inventing
    // a point estimate would claim precision it does not have.
    [[nodiscard]] double quantile(double q) const noexcept {
        if (!count_) return 0.0;
        const auto want = static_cast<std::uint64_t>(
            static_cast<double>(count_) * q);
        std::uint64_t seen = 0;
        for (int b = 0; b < kBuckets; ++b) {
            seen += bucket_[static_cast<std::size_t>(b)];
            // Strictly greater, not >=. At the boundary — 95 fast samples
            // and 5 slow ones, asked for p95 — `>=` returns the FAST
            // bucket, reporting that the slowest 5% were fast. The tail
            // is exactly what a p95 is asked for, so a rule that hides it
            // makes the number worse than not showing one.
            if (seen > want && bucket_[static_cast<std::size_t>(b)])
                return static_cast<double>(1u << b);
        }
        return static_cast<double>(max_);
    }
    void merge(const Hist& o) {
        count_ += o.count_;
        sum_   += o.sum_;
        if (o.max_ > max_) max_ = o.max_;
        for (int b = 0; b < kBuckets; ++b)
            bucket_[static_cast<std::size_t>(b)] += o.bucket_[static_cast<std::size_t>(b)];
    }
    void clear() { *this = Hist{}; }

    // The buckets themselves. p50/p95 are two readings OF a distribution,
    // and a distribution is not two numbers: 95 fast calls and 5 slow ones
    // has the same mean as 100 medium ones and means something completely
    // different. The shape is already stored — not showing it is throwing
    // away the most informative thing the panel has.
    [[nodiscard]] const std::array<std::uint32_t, kBuckets>& buckets()
        const noexcept { return bucket_; }

    // Inclusive lower edge of bucket b, in the unit that was add()ed.
    [[nodiscard]] static std::uint32_t bucket_floor(int b) noexcept {
        return b <= 0 ? 0u : (1u << b);
    }

    // The occupied range, so a renderer can skip the empty head and tail
    // rather than drawing 24 buckets of which 20 are zero.
    [[nodiscard]] std::pair<int, int> occupied() const noexcept {
        int lo = kBuckets, hi = -1;
        for (int b = 0; b < kBuckets; ++b)
            if (bucket_[static_cast<std::size_t>(b)]) {
                if (b < lo) lo = b;
                hi = b;
            }
        return {lo, hi};
    }

private:
    std::array<std::uint32_t, kBuckets> bucket_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_   = 0;
    std::uint32_t max_   = 0;
};

// ── Facts ────────────────────────────────────────────────────────────────
//
// One struct per tab, so "where does this number live" has an answer that
// does not require reading the fold.
struct Facts {
    struct SessionF {
        std::size_t user_turns      = 0;
        std::size_t assistant_turns = 0;
        std::size_t errors          = 0;
        std::size_t compact_summaries = 0;
        // Turns that carry telemetry at all. The denominator for every
        // average below — averaging over ALL turns would silently count
        // pre-telemetry history as zero-cost.
        std::size_t measured_turns  = 0;
        std::uint64_t wall_ms       = 0;   // summed ttft + stream
    } session;

    struct ModelsF {
        Tally by_model;                    // every assistant turn
        Tally by_provider_hint;            // prefix before the first '-' / '/'
    } models;

    struct SmartF {
        std::size_t routed   = 0;          // served_role present
        std::size_t unrouted = 0;          // Smart Mode was off
        Tally by_role;
        Tally by_model;                    // routed turns only
        // Routed turns that ran BELOW Strategic. Its own counter rather
        // than derived, because the denominator is `routed` and deriving
        // it at the view is how two consumers end up disagreeing.
        std::size_t delegated = 0;
    } smart;

    struct TokensF {
        std::uint64_t input     = 0;
        std::uint64_t output    = 0;
        std::uint64_t reasoning = 0;       // inside output, never added to it
        std::uint64_t cache_read     = 0;
        std::uint64_t cache_creation = 0;
        Hist          per_turn_output;
        // Last N turns of output, for the trend plot. Bounded so a 2000-turn
        // thread cannot grow the projection without limit.
        std::vector<double> output_series;
    } tokens;

    struct CacheF {
        std::size_t   turns_with_cache = 0;
        std::uint64_t hits   = 0;          // cache_read tokens
        std::uint64_t writes = 0;          // cache_creation tokens
        std::uint64_t misses = 0;          // uncached input tokens
    } cache;

    struct ToolsF {
        Tally by_name;
        Tally by_status;                   // done / failed / rejected / pending
        Hist  latency;
        std::size_t total = 0;
    } tools;

    struct ReasoningF {
        std::size_t   turns  = 0;          // turns that thought at all
        std::uint64_t ms     = 0;
        std::uint64_t tokens = 0;
        std::size_t   blocks = 0;
    } reasoning;

    struct StreamF {
        std::size_t   degraded_turns  = 0; // turns that survived something
        std::uint64_t transient       = 0;
        std::uint64_t mid_stream      = 0;
        std::uint64_t stalls          = 0;
        std::uint64_t wire_bytes      = 0;
        Hist          ttft;
        Hist          stream_ms;
    } stream;

    struct ContextF {
        std::size_t compactions = 0;
        std::uint64_t peak_input = 0;      // largest prefix seen
        std::vector<double> prefix_series;
    } context;

    struct RetrievalF {
        std::size_t injections   = 0;
        std::size_t with_confidence = 0;
        double      confidence_sum = 0.0;
    } retrieval;

    // Keep the trend series bounded. A plot is ~70 cells wide, so more
    // samples than this cannot be drawn — carrying them would cost memory
    // to render nothing.
    static constexpr std::size_t kSeriesCap = 256;

    void fold(const Message& msg);
    void merge(const Facts& tail);
    void clear() { *this = Facts{}; }
};

// ── Epoch ────────────────────────────────────────────────────────────────
//
// The three-field guard. Compared by value; anything that does not match
// resets the fold cursor to 0. Nothing here attempts to patch a mutated
// prefix in place.
struct Epoch {
    std::string thread_id;
    std::size_t message_count = 0;
    std::size_t compactions   = 0;

    [[nodiscard]] static Epoch of(const Thread& t) {
        return Epoch{t.id.value, t.messages.size(), t.compactions.size()};
    }
    // True when `next` can be reached from `*this` by APPENDING only.
    [[nodiscard]] bool extends_to(const Epoch& next) const noexcept {
        return thread_id == next.thread_id
            && compactions == next.compactions
            && next.message_count >= message_count;
    }
};

// The incremental projection. Owns its cursor, its epoch and the scratch
// total for the live tail.
class Projection {
public:
    // Amortised O(1) per frame on a settled thread; O(1 message) while
    // streaming; O(n) on fork / rewind / compaction.
    const Facts& refresh(const Thread& t);

    [[nodiscard]] const Facts& facts() const noexcept { return view_; }

private:
    Facts sealed_;      // messages [0, consumed_)
    Facts view_;        // sealed_ + the live tail, what callers read
    Epoch epoch_;
    std::size_t consumed_ = 0;
};

}  // namespace agentty::stats
