// 02_integers.cpp — the four integer traps that bite real code.
//
// Build: make 02_integers && ./02_integers

#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>   // std::cmp_less
#include <vector>

// ── trap 1: unsigned subtraction wraps ─────────────────────────────────
static void unsigned_wrap() {
    std::puts("-- trap 1: size() - 1 on an empty container --");
    std::vector<int> empty;
    std::printf("empty.size()      = %zu\n", empty.size());
    std::printf("empty.size() - 1  = %zu   <- not -1. it wrapped.\n",
                empty.size() - 1);

    std::puts("\nso this loop runs 18 quintillion times:");
    std::puts("  for (size_t i = 0; i < v.size() - 1; ++i)   // BAD");
    std::puts("write it with the subtraction moved to the other side:");
    std::vector<int> v{10, 20, 30};
    std::printf("  pairs: ");
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        std::printf("(%d,%d) ", v[i], v[i + 1]);
    std::puts("   <- i + 1 < size(), always safe");
}

// ── trap 2: signed/unsigned comparison converts the signed one ─────────
static void mixed_compare() {
    std::puts("\n-- trap 2: -1 < 1u is false --");
    int      s = -1;
    unsigned u = 1;

    // INTENTIONAL WARNING. -Wsign-compare fires on the next line and that
    // warning is the entire lesson: the compiler knows this is wrong.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    std::printf("s < u            -> %s   WRONG\n", (s < u) ? "true" : "false");
#pragma GCC diagnostic pop
    std::printf("std::cmp_less    -> %s   right (C++20, compares values)\n",
                std::cmp_less(s, u) ? "true" : "false");
    std::puts("reason: s converts to unsigned first, becoming 4294967295.");
}

// ── trap 3: signed overflow is undefined behaviour ─────────────────────
static void signed_overflow() {
    std::puts("\n-- trap 3: signed overflow is UB, not wrap --");
    std::printf("INT_MAX          = %d\n", std::numeric_limits<int>::max());
    std::puts("INT_MAX + 1 is UNDEFINED. the optimiser is allowed to assume");
    std::puts("it never happens, so `if (x + 1 < x)` can be deleted entirely.");
    std::puts("check before you add:");
    int x = std::numeric_limits<int>::max();
    bool safe = x <= std::numeric_limits<int>::max() - 1;
    std::printf("  can add 1 to INT_MAX? %s\n", safe ? "yes" : "no");

    std::puts("\nunsigned overflow IS defined (it wraps mod 2^N):");
    unsigned umax = std::numeric_limits<unsigned>::max();
    std::printf("  UINT_MAX + 1 = %u\n", umax + 1u);
}

// ── trap 4: narrowing on assignment is silent ──────────────────────────
static void narrowing() {
    std::puts("\n-- trap 4: silent truncation --");
    int big = 300;
    std::uint8_t byte = static_cast<std::uint8_t>(big);
    std::printf("uint8_t(300)     = %u   <- lost the top bits\n",
                static_cast<unsigned>(byte));
    double d = 3.99;
    std::printf("int(3.99)        = %d   <- truncates toward zero\n",
                static_cast<int>(d));
    std::puts("with braces, uint8_t b{300} and int i{3.99} are COMPILE ERRORS.");
    std::puts("that is the main reason to prefer {} init. see 05_init.");
}

int main() {
    unsigned_wrap();
    mixed_compare();
    signed_overflow();
    narrowing();
}
