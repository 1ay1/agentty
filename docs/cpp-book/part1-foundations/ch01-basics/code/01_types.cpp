// 01_types.cpp — what a type actually is: size, layout, and operations.
//
// Build: make 01_types && ./01_types
//   or:  g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g
//            01_types.cpp -o 01_types && ./01_types

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ── a type is a promise about bytes ────────────────────────────────────
static void sizes() {
    std::puts("-- sizes on this machine --");
    std::printf("char        %2zu byte   align %zu\n", sizeof(char), alignof(char));
    std::printf("short       %2zu bytes  align %zu\n", sizeof(short), alignof(short));
    std::printf("int         %2zu bytes  align %zu\n", sizeof(int), alignof(int));
    std::printf("long        %2zu bytes  align %zu\n", sizeof(long), alignof(long));
    std::printf("float       %2zu bytes  align %zu\n", sizeof(float), alignof(float));
    std::printf("double      %2zu bytes  align %zu\n", sizeof(double), alignof(double));
    std::printf("void*       %2zu bytes  align %zu\n", sizeof(void*), alignof(void*));
    std::printf("std::string %2zu bytes  align %zu\n", sizeof(std::string), alignof(std::string));
}

// ── the same bytes, read as two different types ────────────────────────
static void reinterpretation() {
    std::puts("\n-- same 4 bytes, two types --");
    std::uint32_t bits = 0x41200000u;     // this is 10.0f as IEEE-754
    float f;
    std::memcpy(&f, &bits, sizeof f);     // the legal way to type-pun
    std::printf("as uint32_t: %u\n", bits);
    std::printf("as float   : %.1f\n", static_cast<double>(f));
    std::puts("the bytes never changed. only the type we read them with did.");
}

// ── operations come from the type, not the value ───────────────────────
static void operations() {
    std::puts("\n-- / means different things --");
    int a = 7, b = 2;
    std::printf("7 / 2       = %d      (int division truncates)\n", a / b);
    std::printf("7.0 / 2.0   = %.1f\n", 7.0 / 2.0);
    std::printf("(double)7/2 = %.1f    (one cast fixes the whole expression)\n",
                static_cast<double>(a) / b);
}

// ── the bug this causes in real code ───────────────────────────────────
static void the_progress_bug() {
    std::puts("\n-- a real percentage bug --");
    int done = 3, total = 4;
    std::printf("(done / total) * 100            = %d%%   WRONG\n",
                (done / total) * 100);
    std::printf("(done * 100) / total            = %d%%   ok for ints\n",
                (done * 100) / total);
    std::printf("(double(done) / total) * 100.0  = %.0f%%   ok, and reads right\n",
                (static_cast<double>(done) / total) * 100.0);
}

// ── layout: a struct is its members, in order, with padding ────────────
struct Padded   { char a; int b; char c; };   // 1 + pad + 4 + 1 + pad
struct Packedish { int b; char a; char c; };  // 4 + 1 + 1 + pad

static void layout() {
    std::puts("\n-- member order changes the size --");
    std::printf("struct { char; int; char; } = %zu bytes\n", sizeof(Padded));
    std::printf("struct { int; char; char; } = %zu bytes\n", sizeof(Packedish));
    std::puts("same three members. the compiler pads to keep int aligned.");
}

int main() {
    sizes();
    reinterpretation();
    operations();
    the_progress_bug();
    layout();
}
