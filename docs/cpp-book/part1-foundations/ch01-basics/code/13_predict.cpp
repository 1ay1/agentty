// 13_predict.cpp — the answer key for exercise 9, computed not asserted.
//
// Covers §2 (integral promotion + usual arithmetic conversions) and §8
// (who copies, who moves). Predict every line on paper FIRST, then run
// this and score yourself. Under 8/10 on the type questions means go
// back to §2's "the machine underneath".
//
// Build: make 13_predict && ./13_predict

#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A type-name printer, so the COMPILER tells us what it deduced rather
// than us guessing. See §11 for the TypeIs<> variant that works for any
// type without needing a specialisation.
template <class T> const char* n();
template <> const char* n<int>(){return "int";}
template <> const char* n<unsigned>(){return "unsigned";}
template <> const char* n<long>(){return "long";}
template <> const char* n<unsigned long>(){return "unsigned long";}
template <> const char* n<bool>(){return "bool";}
#define SHOW(e) std::printf("%-34s -> %-14s\n", #e, n<decltype(e)>())

struct T {
    static inline int copies=0, moves=0;
    std::string s;
    T(const char* c):s(c){}
    T(const T& o):s(o.s){++copies;}
    T(T&& o) noexcept :s(std::move(o.s)){++moves;}
    static void reset(){copies=moves=0;}
};

int main(){
    short s=30000; std::uint8_t b=200; unsigned u=1; int i=-1; long l=1; std::size_t nn=0;
    std::puts("-- types --");
    SHOW(s+s); SHOW(b+b); SHOW(b*2); SHOW(i+u); SHOW(u+l); SHOW(nn-1);
    SHOW(i<u); SHOW((b+b)>255); SHOW(sizeof(b<<1));
    std::puts("\n-- values --");
    std::printf("s+s = %d\n", s+s);
    std::printf("b+b = %d\n", b+b);
    std::printf("b*2 = %d\n", b*2);
    std::printf("nn-1 = %zu\n", nn-1);
    std::printf("(b+b)>255 = %d\n", (b+b)>255);
    // INTENTIONAL WARNING: -Wtype-limits fires on the next line because a
    // uint8_t can never exceed 255, so the comparison is always false.
    // That warning IS the lesson - contrast it with the line above.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
    std::printf("(uint8)(b+b)>255 = %d\n", static_cast<std::uint8_t>(b+b)>255);
#pragma GCC diagnostic pop
    std::printf("sizeof(b<<1) = %zu\n", sizeof(b<<1));

    std::puts("\n-- copies --");
    { std::vector<T> v; v.reserve(3); v.emplace_back("a");v.emplace_back("b");v.emplace_back("c");
      T::reset(); for (auto x : v) (void)x; std::printf("11. for(auto x:v)        copies=%d\n",T::copies);
      T::reset(); for (const auto& x : v) (void)x; std::printf("12. for(const auto&:v)  copies=%d\n",T::copies);
      T::reset(); { std::vector<T> w = v; (void)w; } std::printf("13. vector w = v         copies=%d\n",T::copies);
      T::reset(); { auto w = std::move(v); (void)w; } std::printf("14. auto w = move(v)     copies=%d moves=%d\n",T::copies,T::moves);
    }
    { std::vector<T> v; v.reserve(3); v.emplace_back("a");v.emplace_back("b");v.emplace_back("c");
      T::reset(); v.push_back(T{"d"});
      std::printf("15. push_back at cap 3   copies=%d moves=%d\n",T::copies,T::moves);
    }
}
