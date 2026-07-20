#pragma once
// agentty::rag — SIMD-optimized vector operations.
//
// Runtime dispatch to SSE4.2 / AVX2 / NEON implementations when available,
// fallback to scalar otherwise. Used by HNSW's hot dot-product loop.
//
// The SIMD paths are 2-4x faster on large vectors (768-dim embeddings),
// which matters when the ANN search issues thousands of dot products per query.

#include <cstdint>
#include <span>
#include <vector>

namespace agentty::rag::simd {

// Detect SIMD capabilities at runtime. Cached after first call.
enum class SimdLevel { Scalar, SSE42, AVX2, NEON };
[[nodiscard]] SimdLevel detect() noexcept;

// Dot product of two float vectors of the same length. Returns 0 for empty.
// Auto-dispatches to the best available SIMD path.
//
// The (ptr, n) form is the low-level kernel entry (used by the AVX2/NEON
// internals). PREFER the std::span overload below at call sites: it carries
// the length WITH the pointer, so the "both buffers are ≥ n" precondition
// that a bare (a, b, n) triple leaves unchecked becomes structural — a
// mismatched embedding dim can't silently read past the end.
[[nodiscard]] float dot(const float* a, const float* b, std::size_t n) noexcept;

// Span overload — the safe surface. Length travels with the data; a size
// mismatch returns 0 instead of running off the shorter buffer.
[[nodiscard]] inline float dot(std::span<const float> a,
                               std::span<const float> b) noexcept {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    return dot(a.data(), b.data(), a.size());
}

// Overload for std::vector (constructs spans; kept for source compatibility).
[[nodiscard]] inline float dot(const std::vector<float>& a,
                               const std::vector<float>& b) noexcept {
    return dot(std::span<const float>(a), std::span<const float>(b));
}

// L2 (Euclidean) distance squared. Useful for some distance metrics.
[[nodiscard]] float l2_sq(const float* a, const float* b, std::size_t n) noexcept;

[[nodiscard]] inline float l2_sq(std::span<const float> a,
                                 std::span<const float> b) noexcept {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    return l2_sq(a.data(), b.data(), a.size());
}

// Normalize a vector in-place to unit length. No-op if zero vector.
void normalize(float* v, std::size_t n) noexcept;

inline void normalize(std::span<float> v) noexcept {
    normalize(v.data(), v.size());
}

} // namespace agentty::rag::simd
