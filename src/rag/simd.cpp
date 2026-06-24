// agentty::rag — SIMD-optimized vector operations.
// Runtime dispatch to SSE4.2 / AVX2 / NEON implementations when available.

#include "agentty/rag/simd.hpp"

#include <cmath>
#include <cstring>

// Platform detection for SIMD intrinsics.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define RAG_X86 1
    #include <immintrin.h>
    #if defined(__GNUC__) || defined(__clang__)
        #include <cpuid.h>
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define RAG_ARM64 1
    #include <arm_neon.h>
#endif

namespace agentty::rag::simd {

namespace {

#ifdef RAG_X86
bool has_sse42() noexcept {
    #if defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            return (ecx & (1 << 20)) != 0;  // SSE4.2
        }
        return false;
    #elif defined(_MSC_VER)
        int info[4];
        __cpuid(info, 1);
        return (info[2] & (1 << 20)) != 0;
    #else
        return false;
    #endif
}

bool has_avx2() noexcept {
    #if defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;
        // Check OSXSAVE (bit 27) and AVX (bit 28) in CPUID.1:ECX
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            if ((ecx & (1 << 27)) == 0 || (ecx & (1 << 28)) == 0) return false;
        } else {
            return false;
        }
        // Check AVX2 (bit 5) in CPUID.7:EBX
        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
            return (ebx & (1 << 5)) != 0;
        }
        return false;
    #elif defined(_MSC_VER)
        int info[4];
        __cpuid(info, 1);
        if ((info[2] & (1 << 27)) == 0 || (info[2] & (1 << 28)) == 0) return false;
        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 5)) != 0;
    #else
        return false;
    #endif
}
#endif

// Function-level ISA targeting. GCC/Clang need [[gnu::target("avx2")]] to
// emit AVX2/SSE4.2 inside a binary built for a baseline ISA (function
// multiversioning). MSVC has no equivalent and instead compiles every
// intrinsic unconditionally, so the attribute must be ELIDED there (an
// unknown [[gnu::...]] attribute is at best a warning, at worst an error
// under /permissive-). Define a portable macro that expands to the attribute
// only on the compilers that understand it.
#if defined(__GNUC__) || defined(__clang__)
    #define RAG_TARGET(isa) [[gnu::target(isa)]]
#else
    #define RAG_TARGET(isa)
#endif

// Scalar fallback.
float dot_scalar(const float* a, const float* b, std::size_t n) noexcept {
    float sum = 0.0f;
    for (std::size_t i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

#ifdef RAG_X86
// SSE4.2 implementation (128-bit, 4 floats at a time).
RAG_TARGET("sse4.2")
float dot_sse42(const float* a, const float* b, std::size_t n) noexcept {
    __m128 sum = _mm_setzero_ps();
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        sum = _mm_add_ps(sum, _mm_mul_ps(va, vb));
    }
    // Horizontal sum.
    __m128 shuf = _mm_movehdup_ps(sum);
    sum = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sum);
    sum = _mm_add_ss(sum, shuf);
    float result = _mm_cvtss_f32(sum);
    // Handle remainder.
    for (; i < n; ++i) result += a[i] * b[i];
    return result;
}

// AVX2 implementation (256-bit, 8 floats at a time).
RAG_TARGET("avx2")
float dot_avx2(const float* a, const float* b, std::size_t n) noexcept {
    __m256 sum = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(va, vb));
    }
    // Horizontal sum across 256-bit register.
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    lo = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_movehdup_ps(lo);
    lo = _mm_add_ps(lo, shuf);
    shuf = _mm_movehl_ps(shuf, lo);
    lo = _mm_add_ss(lo, shuf);
    float result = _mm_cvtss_f32(lo);
    // Handle remainder.
    for (; i < n; ++i) result += a[i] * b[i];
    return result;
}
#endif

#ifdef RAG_ARM64
// NEON implementation (128-bit, 4 floats at a time).
float dot_neon(const float* a, const float* b, std::size_t n) noexcept {
    float32x4_t sum = vdupq_n_f32(0.0f);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        sum = vmlaq_f32(sum, va, vb);
    }
    // Horizontal sum.
    float32x2_t lo = vget_low_f32(sum);
    float32x2_t hi = vget_high_f32(sum);
    lo = vadd_f32(lo, hi);
    float result = vget_lane_f32(vpadd_f32(lo, lo), 0);
    // Handle remainder.
    for (; i < n; ++i) result += a[i] * b[i];
    return result;
}
#endif

} // namespace

SimdLevel detect() noexcept {
    static SimdLevel level = [] {
#ifdef RAG_X86
        if (has_avx2()) return SimdLevel::AVX2;
        if (has_sse42()) return SimdLevel::SSE42;
        return SimdLevel::Scalar;
#elif defined(RAG_ARM64)
        return SimdLevel::NEON;  // NEON is always available on ARM64.
#else
        return SimdLevel::Scalar;
#endif
    }();
    return level;
}

float dot(const float* a, const float* b, std::size_t n) noexcept {
    if (n == 0) return 0.0f;
    
    static auto impl = [] {
        switch (detect()) {
#ifdef RAG_X86
            case SimdLevel::AVX2:  return dot_avx2;
            case SimdLevel::SSE42: return dot_sse42;
#endif
#ifdef RAG_ARM64
            case SimdLevel::NEON:  return dot_neon;
#endif
            default:               return dot_scalar;
        }
    }();
    
    return impl(a, b, n);
}

float l2_sq(const float* a, const float* b, std::size_t n) noexcept {
    if (n == 0) return 0.0f;
    
    // For L2 distance, we compute sum of (a[i] - b[i])^2.
    // This is less optimized than dot(), but still benefits from loop unrolling.
    float sum = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

void normalize(float* v, std::size_t n) noexcept {
    if (n == 0) return;
    
    float norm = dot(v, v, n);
    if (norm <= 0.0f) return;
    
    float inv = 1.0f / std::sqrt(norm);
    for (std::size_t i = 0; i < n; ++i) v[i] *= inv;
}

} // namespace agentty::rag::simd
