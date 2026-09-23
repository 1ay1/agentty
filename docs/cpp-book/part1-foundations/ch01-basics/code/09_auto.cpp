// 09_auto.cpp — auto, decltype, and the range-for copy you didn't ask for.
//
// Build: make 09_auto && ./09_auto

#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

struct Chatty {
    std::string s;
    explicit Chatty(const char* c) : s(c) {}
    Chatty(const Chatty& o) : s(o.s) { std::printf("  COPY %s\n", s.c_str()); }
    Chatty(Chatty&&) noexcept = default;
};

// ── auto drops references and top-level const ──────────────────────────
static void auto_strips() {
    std::puts("-- auto strips ref and top-level const --");
    int              x = 1;
    int&             r = x;
    const int        c = 2;
    const int&       cr = c;

    auto a1 = r;    // int    (not int&)
    auto a2 = c;    // int    (not const int)
    auto a3 = cr;   // int
    a1 = a2 = a3 = 9;   // all writable, none affect x or c

    std::printf("after writing to the autos: x = %d, c = %d\n", x, c);
    std::printf("is_same<decltype(a1), int> = %s\n",
                std::is_same_v<decltype(a1), int> ? "true" : "false");

    auto& keeps_ref = r;            // int&
    keeps_ref = 77;
    std::printf("through auto& : x = %d\n", x);

    const auto& view = x;           // const int&
    std::printf("const auto& view = %d\n", view);
}

// ── the range-for trap ─────────────────────────────────────────────────
static void range_for() {
    std::puts("\n-- range-for --");
    std::vector<Chatty> v;
    v.reserve(2);
    v.emplace_back("one");
    v.emplace_back("two");

    std::puts(" for (auto e : v)        <- copies each element:");
    for (auto e : v) (void)e;

    std::puts(" for (const auto& e : v) <- no copies:");
    for (const auto& e : v) (void)e;
    std::puts(" (nothing printed above means nothing was copied)");

    std::puts(" for (auto& e : v)       <- when you want to modify");
    for (auto& e : v) e.s += "*";
    std::printf(" now: %s %s\n", v[0].s.c_str(), v[1].s.c_str());
}

// ── the map one, which costs a copy for a subtler reason ───────────────
static void map_range_for() {
    std::puts("\n-- the map trap --");
    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    std::puts(" value_type is pair<const string, int>, NOT pair<string,int>.");
    std::puts(" so `for (const std::pair<std::string,int>& kv : m)` copies");
    std::puts(" every element to convert the type. use auto&:");
    for (const auto& [k, val] : m)
        std::printf("   %s -> %d\n", k.c_str(), val);
}

// ── decltype vs decltype(auto) ─────────────────────────────────────────
static int  val() { return 1; }
static int& ref() { static int n = 5; return n; }

static void decltypes() {
    std::puts("\n-- decltype --");
    int x = 0;
    std::printf("decltype(x)    is int   : %s\n",
                std::is_same_v<decltype(x), int> ? "yes" : "no");
    std::printf("decltype((x))  is int&  : %s   <- extra parens matter\n",
                std::is_same_v<decltype((x)), int&> ? "yes" : "no");
    std::printf("decltype(val()) is int  : %s\n",
                std::is_same_v<decltype(val()), int> ? "yes" : "no");
    std::printf("decltype(ref()) is int& : %s\n",
                std::is_same_v<decltype(ref()), int&> ? "yes" : "no");

    auto           a = ref();   // int  — the reference is dropped
    decltype(auto) d = ref();   // int& — the reference is kept
    d = 42;
    std::printf("after d = 42: ref() = %d, a = %d\n", ref(), a);
}

// ── when auto helps and when it hides ──────────────────────────────────
static void style() {
    std::puts("\n-- style --");
    std::vector<std::string> v{"x"};
    for (auto it = v.begin(); it != v.end(); ++it)  // good: name is noise
        std::printf("iterator auto: %s\n", it->c_str());

    auto n = v.size();                              // fine: size_t is obvious
    std::printf("size = %zu\n", n);

    std::puts("use auto when the type is long and obvious from the right side.");
    std::puts("write the type when it is the interesting part of the line.");
}

int main() {
    auto_strips();
    range_for();
    map_range_for();
    decltypes();
    style();
}
