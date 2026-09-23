// ch01_examples.cpp — every runnable example from Chapter 1.
//
// Build:
//   g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g
//       ch01_examples.cpp -o ch01 && ./ch01
//
// TWO WARNINGS ARE INTENTIONAL and marked below: the signed/unsigned
// comparison in 1.2 and the discarded std::move in 1.4 are the bugs those
// sections teach. Seeing your compiler flag them is the point.
//
// Each demo is self-contained and prints a labelled section, so you can
// read the chapter with this running beside you.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ───────────────────────────────────────────────────────────────────────
// §1.1  Types are size + operations + promises
// ───────────────────────────────────────────────────────────────────────
static void sec_1_1() {
    std::puts("\n=== 1.1 what a type is ===");
    std::printf("int %zu  double %zu  char %zu bytes\n",
                sizeof(int), sizeof(double), sizeof(char));

    int a = 7, b = 2;
    std::printf("7 / 2        = %d   <- integer division truncates\n", a / b);
    std::printf("7.0 / 2.0    = %.1f\n", 7.0 / 2.0);
    std::printf("(double)7/2  = %.1f\n", static_cast<double>(a) / b);

    int done = 3, total = 4;
    std::printf("progress wrong: %d%%\n", (done / total) * 100);
    std::printf("progress right: %.0f%%\n",
                (static_cast<double>(done) / total) * 100.0);
}

// ───────────────────────────────────────────────────────────────────────
// §1.2  Integer traps
// ───────────────────────────────────────────────────────────────────────
static void sec_1_2() {
    std::puts("\n=== 1.2 integers ===");

    // Unsigned wrap: v.size() - 1 on an empty vector is astronomically large.
    std::vector<int> empty;
    std::printf("empty.size() - 1 = %zu   <- wrapped!\n", empty.size() - 1);

    // Safe idiom: move the term instead of subtracting.
    std::vector<int> v{10, 20, 30};
    std::printf("adjacent pairs: ");
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        std::printf("(%d,%d) ", v[i], v[i + 1]);
    std::putchar('\n');

    // Signed/unsigned comparison lies.
    int s = -1;
    unsigned u = 1;
    // INTENTIONAL WARNING: -Wsign-compare fires here. That warning IS the
    // lesson — the comparison below is wrong and the compiler knows.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    std::printf("s < u           : %s  <- wrong\n", (s < u) ? "true" : "false");
#pragma GCC diagnostic pop
    std::printf("std::cmp_less   : %s  <- right\n",
                std::cmp_less(s, u) ? "true" : "false");

    std::printf("int max = %d (adding 1 would be UB)\n",
                std::numeric_limits<int>::max());
}

// ───────────────────────────────────────────────────────────────────────
// §1.3  Strong types — agentty's Id<Tag>, simplified
// ───────────────────────────────────────────────────────────────────────
template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};

struct ThreadIdTag {};
struct ToolCallIdTag {};
using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;

static void sec_1_3() {
    std::puts("\n=== 1.3 strong types ===");
    ThreadId   t{"abc123"};
    ToolCallId c{"abc123"};

    std::printf("sizeof(std::string) = %zu\n", sizeof(std::string));
    std::printf("sizeof(ThreadId)    = %zu  <- identical\n", sizeof(ThreadId));
    std::printf("same value, different types; t == c will not compile\n");
    std::printf("t.value = %s   c.value = %s\n", t.value.c_str(), c.value.c_str());
}

// ───────────────────────────────────────────────────────────────────────
// §1.4  Value categories
// ───────────────────────────────────────────────────────────────────────
static void cat(int&)  { std::puts("  lvalue"); }
static void cat(int&&) { std::puts("  rvalue"); }

static void named_rref(int&& r) {
    std::printf("  inside f(int&& r), the expression `r` is an: ");
    cat(r);                       // lvalue — it has a name
    std::printf("  after std::move(r):                          ");
    cat(std::move(r));            // rvalue
}

static void sec_1_4() {
    std::puts("\n=== 1.4 value categories ===");
    int x = 1;
    std::printf("cat(x)             -> "); cat(x);
    std::printf("cat(42)            -> "); cat(42);
    std::printf("cat(x + 1)         -> "); cat(x + 1);
    std::printf("cat(std::move(x))  -> "); cat(std::move(x));
    named_rref(42);

    // std::move does nothing by itself
    std::string a = "data";
    // INTENTIONAL: std::move is [[nodiscard]], and discarding it is exactly
    // the mistake people make when they think it does something.
    (void)std::move(a);                             // no-op
    std::printf("after std::move(a) alone: a='%s'\n", a.c_str());
    std::string b = std::move(a);                   // NOW it moves
    std::printf("after b = std::move(a):   a='%s' b='%s'\n",
                a.c_str(), b.c_str());
}

// ───────────────────────────────────────────────────────────────────────
// §1.5  Initialisation
// ───────────────────────────────────────────────────────────────────────
struct Point { int x, y; };

static void sec_1_5() {
    std::puts("\n=== 1.5 initialisation ===");
    int b{}, c{5}, d(5), e = 5;
    std::printf("int b{}=%d  c{5}=%d  d(5)=%d  e=5 -> %d\n", b, c, d, e);

    Point p1{}, p2{1, 2};
    std::printf("Point p1{}=(%d,%d)  p2{1,2}=(%d,%d)\n", p1.x, p1.y, p2.x, p2.y);

    double dd = 3.99;
    std::printf("int(3.99) via () = %d   <- silent truncation\n", int(dd));
    std::printf("int{3.99} would be a COMPILE ERROR (narrowing)\n");

    int big = 300;
    std::printf("uint8_t = 300 via = : %u   <- silently wrapped\n",
                static_cast<unsigned>(static_cast<std::uint8_t>(big)));

    std::vector<int> va(5, 0);   // five zeros
    std::vector<int> vb{5, 0};   // two elements: 5, 0
    std::printf("vector(5,0).size()=%zu   vector{5,0}.size()=%zu\n",
                va.size(), vb.size());
}

// ───────────────────────────────────────────────────────────────────────
// §1.6  References
// ───────────────────────────────────────────────────────────────────────
static void sec_1_6() {
    std::puts("\n=== 1.6 references ===");
    int x = 1, y = 2;
    int& r = x;
    r = y;                       // assigns through; does NOT rebind
    std::printf("after r = y: x=%d y=%d r=%d\n", x, y, r);
    std::printf("&x == &r ? %s  <- r IS x\n",
                (&x == &r) ? "yes" : "no");
}

// ───────────────────────────────────────────────────────────────────────
// §1.7  const, and agentty's mutable-cache pattern
// ───────────────────────────────────────────────────────────────────────
// Modelled on agentty's LazyBytes: the bytes may live in a blob on disk.
// Materialising them is not a LOGICAL change to the object, so bytes()
// stays const and the cache is `mutable`.
class LazyBytes {
public:
    struct Source { std::string blob; };

    LazyBytes() = default;
    static LazyBytes lazy(Source s) {
        LazyBytes b;
        b.src_ = std::move(s);
        return b;
    }

    [[nodiscard]] bool materialised() const noexcept { return have_; }

    // const, yet it fills the cache — that is what `mutable` is for.
    [[nodiscard]] const std::string& bytes() const {
        if (!have_) {
            std::printf("    (resolving blob '%s' from disk...)\n",
                        src_.blob.c_str());
            cache_ = "BYTES:" + src_.blob;
            have_  = true;
        }
        return cache_;
    }

private:
    Source              src_;
    mutable std::string cache_;
    mutable bool        have_ = false;
};

static void sec_1_7() {
    std::puts("\n=== 1.7 const + mutable cache ===");
    const LazyBytes img = LazyBytes::lazy({"sha256-abc"});
    std::printf("  materialised? %s\n", img.materialised() ? "yes" : "no");
    std::printf("  first  bytes(): %s\n", img.bytes().c_str());
    std::printf("  second bytes(): %s  <- cached, no resolve\n",
                img.bytes().c_str());
    std::printf("  materialised? %s\n", img.materialised() ? "yes" : "no");
}

// ───────────────────────────────────────────────────────────────────────
// §1.8  Lifetime
// ───────────────────────────────────────────────────────────────────────
struct Noisy {
    const char* name;
    explicit Noisy(const char* n) : name(n) { std::printf("  + %s\n", name); }
    ~Noisy() { std::printf("  - %s\n", name); }
};

static void sec_1_8() {
    std::puts("\n=== 1.8 lifetime (reverse destruction order) ===");
    Noisy a{"a"};
    {
        Noisy b{"b"};
        Noisy c{"c"};
        std::puts("  (leaving inner scope)");
    }
    std::puts("  (leaving function)");

    // Iterator invalidation, shown without triggering UB.
    std::vector<int> v{1, 2, 3};
    std::printf("  vector size=%zu cap=%zu\n", v.size(), v.capacity());
    for (int i = 0; i < 4; ++i) {
        v.push_back(i);
        std::printf("  push -> size=%zu cap=%zu%s\n", v.size(), v.capacity(),
                    v.size() > 3 && v.capacity() != 3 ? "" : "");
    }
    std::puts("  (any reference taken before a realloc is now dangling)");
}

// ───────────────────────────────────────────────────────────────────────
// §1.9  Copy, move, elision, noexcept
// ───────────────────────────────────────────────────────────────────────
struct Tracked {
    int id;
    explicit Tracked(int i) : id(i) { std::printf("  ctor %d\n", id); }
    Tracked(const Tracked& o) : id(o.id) { std::printf("  COPY %d\n", id); }
    Tracked(Tracked&& o) noexcept : id(o.id) {
        std::printf("  move %d\n", id);
        o.id = -1;
    }
    ~Tracked() { std::printf("  dtor %d\n", id); }
};

static Tracked make_prvalue() { return Tracked{1}; }
static Tracked make_named()   { Tracked t{2}; return t; }

struct Throwing {
    int v;
    explicit Throwing(int i) : v(i) {}
    Throwing(const Throwing& o) : v(o.v) { std::puts("  COPY"); }
    Throwing(Throwing&& o) : v(o.v) { std::puts("  move"); }      // no noexcept
};

struct Safe {
    int v;
    explicit Safe(int i) : v(i) {}
    Safe(const Safe& o) : v(o.v) { std::puts("  COPY"); }
    Safe(Safe&& o) noexcept : v(o.v) { std::puts("  move"); }      // noexcept
};

static void sec_1_9() {
    std::puts("\n=== 1.9 elision ===");
    std::puts(" make_prvalue():");
    Tracked a = make_prvalue();
    std::puts(" make_named():");
    Tracked b = make_named();
    std::puts(" std::move(b):");
    Tracked c = std::move(b);
    (void)a; (void)c;

    std::puts("\n=== 1.9 noexcept decides copy vs move ===");
    std::puts(" vector<Throwing> reallocating:");
    { std::vector<Throwing> t; t.reserve(1); t.emplace_back(1); t.emplace_back(2); }
    std::puts(" vector<Safe> reallocating:");
    { std::vector<Safe> s; s.reserve(1); s.emplace_back(1); s.emplace_back(2); }
    std::puts(" (destructors of the elision demo follow)");
}

// ───────────────────────────────────────────────────────────────────────
// §1.10  auto and decltype
// ───────────────────────────────────────────────────────────────────────
struct Big {
    std::string data;
    explicit Big(std::string d) : data(std::move(d)) {}
    Big(const Big& o) : data(o.data) { std::puts("  COPY"); }
    Big(Big&&) noexcept = default;
};

static void sec_1_10() {
    std::puts("\n=== 1.10 auto ===");
    int x = 0;
    std::printf("decltype(x)   is reference? %s\n",
                std::is_reference_v<decltype(x)> ? "yes" : "no");
    std::printf("decltype((x)) is reference? %s  <- extra parens!\n",
                std::is_reference_v<decltype((x))> ? "yes" : "no");

    std::vector<Big> v;
    v.emplace_back("one");
    v.emplace_back("two");
    std::puts(" for (auto e : v):");
    for (auto e : v) (void)e;
    std::puts(" for (const auto& e : v):");
    for (const auto& e : v) (void)e;
    std::puts(" (no output above = no copies)");
}

int main() {
    sec_1_1();
    sec_1_2();
    sec_1_3();
    sec_1_4();
    sec_1_5();
    sec_1_6();
    sec_1_7();
    sec_1_8();
    sec_1_9();
    sec_1_10();
    std::puts("\n=== done ===");
}
