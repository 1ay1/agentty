#pragma once
// agentty::rag — SIMD-optimized vector operations.
//
// Runtime dispatch to SSE4.2 / AVX2 / NEON implementations when available,
// fallback to scalar otherwise. Used by HNSW's hot dot-product loop.
//
// The SIMD paths are 2-4x faster on large vectors (768-dim embeddings),
// which matters when the ANN search issues thousands of dot products per query.

#include <cstdint>
#include <vector>

namespace agentty::rag::simd {

// Detect SIMD capabilities at runtime. Cached after first call.
enum class SimdLevel { Scalar, SSE42, AVX2, NEON };
[[nodiscard]] SimdLevel detect() noexcept;

// Dot product of two float vectors of the same length. Returns 0 for empty.
// Auto-dispatches to the best available SIMD path.
[[nodiscard]] float dot(const float* a, const float* b, std::size_t n) noexcept;

// Overload for std::vector.
[[nodiscard]] inline float dot(const std::vector<float>& a,
                               const std::vector<float>& b) noexcept {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    return dot(a.data(), b.data(), a.size());
}

// L2 (Euclidean) distance squared. Useful for some distance metrics.
[[nodiscard]] float l2_sq(const float* a, const float* b, std::size_t n) noexcept;

// Normalize a vector in-place to unit length. No-op if zero vector.
void normalize(float* v, std::size_t n) noexcept;

} // namespace agentty::rag::simd
