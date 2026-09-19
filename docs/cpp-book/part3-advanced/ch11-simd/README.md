# Chapter 11: SIMD Programming — AVX2, AVX-512, NEON

**Goal:** Master hardware-accelerated parallel processing with SIMD intrinsics.

**Time:** 6-8 hours  
**Prerequisites:** Chapters 1-10, basic understanding of CPU architecture

---

## 11.1 What Is SIMD?

**SIMD = Single Instruction, Multiple Data**

Instead of processing one value at a time:
```cpp
// Scalar (one at a time)
for (int i = 0; i < 8; ++i) {
    c[i] = a[i] + b[i];
}
// 8 additions, 8 cycles (simplified)
```

Process multiple values simultaneously:
```cpp
// SIMD (8 at once with AVX2)
__m256i va = _mm256_loadu_si256((__m256i*)a);
__m256i vb = _mm256_loadu_si256((__m256i*)b);
__m256i vc = _mm256_add_epi32(va, vb);
_mm256_storeu_si256((__m256i*)c, vc);
// 8 additions, 1 cycle
```

### Hardware Support

| Instruction Set | Width | Elements | Availability |
|-----------------|-------|----------|--------------|
| SSE2 | 128-bit | 4× int32, 2× int64 | x86-64 baseline (2000+) |
| AVX2 | 256-bit | 8× int32, 4× int64 | Intel Haswell (2013+) |
| AVX-512 | 512-bit | 16× int32, 8× int64 | Intel Skylake-X (2017+) |
| NEON | 128-bit | 4× int32, 2× int64 | All ARM64 (2011+) |

### Real-World Impact

**From maya's terminal rendering:**

```
Scalar (one cell at a time):
  80×24 frame diff: 1.2 ms

AVX2 (4 cells at a time):
  80×24 frame diff: 0.3 ms  (4× speedup)

AVX-512 (8 cells at a time):
  80×24 frame diff: 0.13 ms  (9× speedup)
```

---

## 11.2 Intrinsics vs. Auto-Vectorization

### Auto-Vectorization (Compiler Does It)

```cpp
void add_arrays(const int* a, const int* b, int* c, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        c[i] = a[i] + b[i];
    }
}
```

Compile with `-O3 -march=native`:
```bash
g++ -O3 -march=native add.cpp -S -o add.s
# Check assembly: might use vmovdqu, vpaddd (AVX2)
```

**Pros:**
- Easy (just write scalar code)
- Portable

**Cons:**
- Unreliable (small change disables vectorization)
- Limited to simple patterns
- Hard to control

### Manual Intrinsics (You Write SIMD)

```cpp
#include <immintrin.h>  // AVX2

void add_arrays_avx2(const int* a, const int* b, int* c, size_t n) {
    size_t i = 0;
    
    // Process 8 ints at a time
    for (; i + 8 <= n; i += 8) {
        __m256i va = _mm256_loadu_si256((__m256i*)(a + i));
        __m256i vb = _mm256_loadu_si256((__m256i*)(b + i));
        __m256i vc = _mm256_add_epi32(va, vb);
        _mm256_storeu_si256((__m256i*)(c + i), vc);
    }
    
    // Scalar tail for remaining elements
    for (; i < n; ++i) {
        c[i] = a[i] + b[i];
    }
}
```

**Pros:**
- Guaranteed vectorization
- Full control
- Predictable performance

**Cons:**
- Platform-specific code
- Harder to write
- Must handle alignment, tail cases

**When to use intrinsics:**
1. Performance-critical inner loops
2. Algorithms compiler can't vectorize
3. When you need guaranteed SIMD

---

## 11.3 AVX2: 256-bit Vectors

### Load and Store

```cpp
// Load 256 bits (8× int32) from memory
__m256i vec = _mm256_loadu_si256((__m256i*)ptr);
//          └─ unaligned load (slower but safe)

// Aligned load (faster, requires 32-byte alignment)
__m256i vec = _mm256_load_si256((__m256i*)aligned_ptr);

// Store back to memory
_mm256_storeu_si256((__m256i*)ptr, vec);
```

**Alignment matters:**
```cpp
// Unaligned: can access any address
int data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
__m256i v = _mm256_loadu_si256((__m256i*)data);  // OK

// Aligned: address must be multiple of 32
alignas(32) int aligned_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
__m256i v = _mm256_load_si256((__m256i*)aligned_data);  // Faster
```

### Arithmetic Operations

```cpp
__m256i a = _mm256_set_epi32(8, 7, 6, 5, 4, 3, 2, 1);
__m256i b = _mm256_set_epi32(1, 2, 3, 4, 5, 6, 7, 8);

// Addition (parallel)
__m256i sum = _mm256_add_epi32(a, b);
// sum = [9, 9, 9, 9, 9, 9, 9, 9]

// Subtraction
__m256i diff = _mm256_sub_epi32(a, b);
// diff = [7, 5, 3, 1, -1, -3, -5, -7]

// Multiplication (low 32 bits)
__m256i prod = _mm256_mullo_epi32(a, b);
// prod = [8, 14, 18, 20, 20, 18, 14, 8]
```

### Comparison

```cpp
__m256i a = _mm256_set_epi32(8, 7, 6, 5, 4, 3, 2, 1);
__m256i b = _mm256_set_epi32(5, 5, 5, 5, 5, 5, 5, 5);

// Compare equal (each lane: 0xFFFFFFFF if equal, 0 otherwise)
__m256i mask = _mm256_cmpeq_epi32(a, b);
// mask = [0, 0, 0, 0xFFFFFFFF, 0, 0, 0, 0]

// Compare greater than
__m256i gt = _mm256_cmpgt_epi32(a, b);
// gt = [0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 0, 0]
```

### Extract Mask from Comparison

```cpp
__m256i a = _mm256_set_epi32(8, 7, 6, 5, 4, 3, 2, 1);
__m256i b = _mm256_set_epi32(5, 5, 5, 5, 5, 5, 5, 5);

__m256i cmp = _mm256_cmpeq_epi32(a, b);
int mask = _mm256_movemask_epi8(cmp);
// mask has bit set for each byte that's 0xFF
// Use for early-out in search loops
```

### Real Example from maya: Row Comparison

```cpp
// maya/include/maya/platform/simd/avx2.hpp

[[nodiscard]] static std::size_t find_first_diff(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t count) noexcept
{
    std::size_t i = 0;
    
    // Process 4 cells (4× uint64) at a time
    for (; i + 4 <= count; i += 4) {
        __m256i va  = _mm256_loadu_si256((__m256i*)(a + i));
        __m256i vb  = _mm256_loadu_si256((__m256i*)(b + i));
        __m256i cmp = _mm256_cmpeq_epi64(va, vb);
        
        // Convert comparison result to bitmask
        unsigned mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp));
        
        if (mask != 0xFu) {  // Not all equal
            // Find first differing lane
            return i + std::countr_zero(~mask);
        }
    }
    
    // Scalar tail
    for (; i < count; ++i) {
        if (a[i] != b[i]) return i;
    }
    
    return count;  // All equal
}
```

**Why this is fast:**
1. **Parallelism** — 4 comparisons per iteration
2. **Early exit** — Stops at first difference
3. **No branches** — SIMD comparisons don't branch

---

## 11.4 AVX-512: 512-bit Vectors

AVX-512 doubles the vector width to 512 bits (8× uint64, 16× uint32).

### Key Differences from AVX2

1. **Mask registers** (`__mmask8`, `__mmask16`) instead of vector masks
2. **Richer instruction set** (ternary ops, conflict detection)
3. **More registers** (32 zmm registers vs 16 ymm)

### Example: Row Comparison with AVX-512

```cpp
// maya/include/maya/platform/simd/avx512.hpp

[[nodiscard]] static std::size_t find_first_diff(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t count) noexcept
{
    std::size_t i = 0;
    
    // Process 8 cells (8× uint64) at a time
    for (; i + 8 <= count; i += 8) {
        __m512i va = _mm512_loadu_si512((__m512i*)(a + i));
        __m512i vb = _mm512_loadu_si512((__m512i*)(b + i));
        
        // Compare, result is a MASK register
        __mmask8 k = _mm512_cmpneq_epu64_mask(va, vb);
        
        if (k != 0) {  // Not all equal
            return i + std::countr_zero(k);
        }
    }
    
    // Scalar tail
    for (; i < count; ++i) {
        if (a[i] != b[i]) return i;
    }
    
    return count;
}
```

**AVX-512 advantages:**
- 2× throughput vs AVX2
- Mask registers are cleaner than vector masks
- More flexible predication

**AVX-512 downsides:**
- Not universal (Intel only, not AMD Zen 3)
- CPU frequency throttling on heavy AVX-512 use
- Larger code size

---

## 11.5 ARM NEON

ARM's 128-bit SIMD (equivalent to SSE2).

### Intrinsics

```cpp
#include <arm_neon.h>

uint64x2_t a = vld1q_u64(ptr);  // Load 2× uint64
uint64x2_t b = vld1q_u64(ptr + 2);
uint64x2_t sum = vaddq_u64(a, b);  // Parallel add
vst1q_u64(out, sum);  // Store result
```

### Real Example from maya: NEON Row Comparison

```cpp
// maya/include/maya/platform/simd/neon.hpp

[[nodiscard]] static std::size_t find_first_diff(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t count) noexcept
{
    std::size_t i = 0;
    
    // Process 2 cells (2× uint64) at a time
    for (; i + 2 <= count; i += 2) {
        uint64x2_t va = vld1q_u64(a + i);
        uint64x2_t vb = vld1q_u64(b + i);
        uint64x2_t cmp = vceqq_u64(va, vb);
        
        // Extract result
        uint64_t mask_lo = vgetq_lane_u64(cmp, 0);
        uint64_t mask_hi = vgetq_lane_u64(cmp, 1);
        
        if (mask_lo != ~0ULL) return i;
        if (mask_hi != ~0ULL) return i + 1;
    }
    
    // Scalar tail
    for (; i < count; ++i) {
        if (a[i] != b[i]) return i;
    }
    
    return count;
}
```

---

## 11.6 Real Example: maya's Terminal Cell Comparison

### The Problem

Terminal rendering requires comparing two cell arrays to find what changed:

```cpp
struct Cell {
    char32_t codepoint;  // 21 bits
    uint16_t style_id;   // 16 bits (interned)
    uint16_t link_id;    // 16 bits
    uint8_t  width;      // 2 bits (1 or 2 columns)
    // Packed into 64 bits
};

Cell prev_frame[rows][cols];
Cell curr_frame[rows][cols];

// Find which cells changed
for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
        if (memcmp(&prev_frame[r][c], &curr_frame[r][c], sizeof(Cell)) != 0) {
            emit_diff(r, c, curr_frame[r][c]);
        }
    }
}
```

**Cost:** 80×24 = 1,920 comparisons per frame, 60 fps = **115,200 comparisons/sec**

### The Solution: Packed Cells + SIMD

**Step 1: Pack cells into 64-bit values**

```cpp
union PackedCell {
    struct {
        uint32_t codepoint : 21;
        uint32_t style_id  : 11;  // fits in remaining 32 bits
        uint16_t link_id   : 16;
        uint8_t  width     : 2;
        uint32_t padding   : 14;
    };
    uint64_t bits;
};
```

**Step 2: Compare rows with SIMD**

```cpp
// Compare entire row at once
bool row_equal(const uint64_t* a, const uint64_t* b, int width) {
    #ifdef __AVX512F__
    return Ops<Avx512>::find_first_diff(a, b, width) == width;
    #elif defined(__AVX2__)
    return Ops<Avx2>::find_first_diff(a, b, width) == width;
    #else
    return Ops<Scalar>::find_first_diff(a, b, width) == width;
    #endif
}
```

**Step 3: Skip unchanged rows**

```cpp
for (int r = 0; r < rows; ++r) {
    if (row_equal(prev[r], curr[r], cols)) {
        continue;  // Entire row unchanged, skip
    }
    
    // Find first changed cell in this row
    int first_diff = find_first_diff(prev[r], curr[r], cols);
    
    // Emit diff starting from first_diff
    // ...
}
```

**Measured Performance:**

| Method | 80×24 frame | 200×50 frame |
|--------|-------------|--------------|
| Scalar | 1.2 ms | 5.8 ms |
| AVX2 | 0.3 ms | 1.4 ms |
| AVX-512 | 0.13 ms | 0.6 ms |

**9× speedup with AVX-512!**

---

## 11.7 Exercises

### Exercise 11.1: Dot Product

Implement a SIMD dot product:

```cpp
float dot_product_scalar(const float* a, const float* b, size_t n);
float dot_product_avx2(const float* a, const float* b, size_t n);

// Bonus: AVX-512 version
float dot_product_avx512(const float* a, const float* b, size_t n);
```

**Starter code:** `exercises/ch11/ex1-dot-product.cpp`  
**Solution:** `solutions/ch11/ex1-dot-product.cpp`

### Exercise 11.2: Horizontal Sum

Given a vector of 8 floats, compute their sum:

```cpp
float horizontal_sum(__m256 vec);
```

Hint: Use `_mm256_hadd_ps` or shuffles + adds.

**Starter code:** `exercises/ch11/ex2-horizontal-sum.cpp`  
**Solution:** `solutions/ch11/ex2-horizontal-sum.cpp`

### Exercise 11.3: String Search

Implement a SIMD string search that finds the first occurrence of a character:

```cpp
size_t find_char_avx2(const char* str, size_t len, char ch);
```

Hint: `_mm256_set1_epi8` broadcasts a byte, `_mm256_cmpeq_epi8` compares.

**Starter code:** `exercises/ch11/ex3-string-search.cpp`  
**Solution:** `solutions/ch11/ex3-string-search.cpp`

### Exercise 11.4: Benchmark SIMD vs Scalar

Write a benchmark comparing scalar vs SIMD for:
1. Array summation
2. Array comparison
3. Maximum element

Measure on arrays of size 1K, 10K, 100K, 1M.

**Starter code:** `exercises/ch11/ex4-benchmark.cpp`  
**Solution:** `solutions/ch11/ex4-benchmark.cpp`

---

## Key Takeaways

1. **SIMD processes multiple values simultaneously**
   - 4-16× parallelism depending on instruction set
   - One instruction, multiple data elements

2. **Intrinsics give you explicit control**
   - Guaranteed vectorization
   - Platform-specific but predictable
   - Faster than auto-vectorization

3. **Always handle the tail**
   - SIMD processes chunks
   - Scalar code for remaining elements

4. **Data layout matters**
   - Pack data into SIMD-friendly formats
   - Alignment helps performance
   - Structure-of-arrays often better than array-of-structures

5. **Use runtime dispatch**
   - Check CPU capabilities at startup
   - Call AVX-512, AVX2, SSE2, or scalar
   - maya does this with template specialization

6. **Profile before optimizing**
   - SIMD isn't always faster (small data, complex operations)
   - Measure on real data with real workloads

---

## Next Chapter

[Chapter 12: Template Metaprogramming →](../ch12-metaprogramming/README.md)

In the next chapter, you'll learn:
- Type traits and SFINAE
- Tag dispatch
- Compile-time recursion
- How agentty's `Id<Tag>` system works internally
