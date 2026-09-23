// 08_copy_move.cpp — copy, move, elision, and why noexcept changes vector.
//
// Build: make 08_copy_move && ./08_copy_move

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// A type that reports every single thing that happens to it.
struct Tracked {
    std::string name;

    explicit Tracked(std::string n) : name(std::move(n)) {
        std::printf("  ctor      %s\n", name.c_str());
    }
    Tracked(const Tracked& o) : name(o.name) {
        std::printf("  copy-ctor %s\n", name.c_str());
    }
    Tracked(Tracked&& o) noexcept : name(std::move(o.name)) {
        std::printf("  move-ctor %s\n", name.c_str());
    }
    Tracked& operator=(const Tracked& o) {
        name = o.name;
        std::printf("  copy-asgn %s\n", name.c_str());
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        name = std::move(o.name);
        std::printf("  move-asgn %s\n", name.c_str());
        return *this;
    }
    ~Tracked() {
        std::printf("  dtor      %s\n", name.empty() ? "(moved-from)" : name.c_str());
    }
};

static void copy_vs_move() {
    std::puts("-- copy vs move --");
    Tracked a{"A"};
    std::puts(" Tracked b = a;");
    Tracked b = a;
    std::puts(" Tracked c = std::move(a);");
    Tracked c = std::move(a);
    std::puts(" scope end:");
}

// ── elision: the copy that never happens ───────────────────────────────
static Tracked make_rvo() {
    return Tracked{"RVO"};           // C++17: guaranteed, no move at all
}

static Tracked make_nrvo() {
    Tracked local{"NRVO"};
    return local;                    // named: elision allowed, not guaranteed
}

// INTENTIONAL: -Wpessimizing-move fires here, and that warning IS the
// lesson. `return std::move(local)` turns a free elision into a real move.
// Your compiler will tell you. Listen to it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpessimizing-move"
static Tracked make_pessimised() {
    Tracked local{"pessimised"};
    return std::move(local);         // NEVER do this. it BLOCKS elision.
}
#pragma GCC diagnostic pop

static void elision() {
    std::puts("\n-- elision --");
    std::puts(" auto r = make_rvo();");
    auto r = make_rvo();
    std::puts(" auto n = make_nrvo();");
    auto n = make_nrvo();
    std::puts(" auto p = make_pessimised();   <- extra move appears");
    auto p = make_pessimised();
    std::puts(" scope end:");
    (void)r; (void)n; (void)p;
}

// ── noexcept move decides whether vector copies on growth ──────────────
struct SafeMove {
    std::string s;
    explicit SafeMove(const char* c) : s(c) {}
    SafeMove(const SafeMove& o) : s(o.s) { std::puts("  COPY (safe)"); }
    SafeMove(SafeMove&& o) noexcept : s(std::move(o.s)) { std::puts("  move (safe)"); }
};

struct RiskyMove {
    std::string s;
    explicit RiskyMove(const char* c) : s(c) {}
    RiskyMove(const RiskyMove& o) : s(o.s) { std::puts("  COPY (risky)"); }
    RiskyMove(RiskyMove&& o) : s(std::move(o.s)) { std::puts("  move (risky)"); }
    // ^ no noexcept. vector will not trust it during reallocation.
};

static void noexcept_matters() {
    std::puts("\n-- why noexcept on a move ctor is not optional --");

    std::puts(" SafeMove, growing from 1 to 2:");
    std::vector<SafeMove> a;
    a.emplace_back("1");
    a.emplace_back("2");

    std::puts(" RiskyMove, growing from 1 to 2:");
    std::vector<RiskyMove> b;
    b.emplace_back("1");
    b.emplace_back("2");

    std::printf(" is_nothrow_move_constructible<SafeMove>  = %s\n",
                std::is_nothrow_move_constructible_v<SafeMove> ? "true" : "false");
    std::printf(" is_nothrow_move_constructible<RiskyMove> = %s\n",
                std::is_nothrow_move_constructible_v<RiskyMove> ? "true" : "false");
    std::puts(" vector needs the strong exception guarantee on reallocation.");
    std::puts(" a throwing move could leave half the elements in limbo, and");
    std::puts(" there would be no way back. so it copies instead.");
}

// ── push_back vs emplace_back ──────────────────────────────────────────
static void push_vs_emplace() {
    std::puts("\n-- push_back vs emplace_back --");
    std::vector<Tracked> v;
    v.reserve(4);                    // no reallocation noise

    std::puts(" v.push_back(Tracked{\"P\"}):");
    v.push_back(Tracked{"P"});       // ctor, then move

    std::puts(" v.emplace_back(\"E\"):");
    v.emplace_back("E");             // ctor, in place, done

    std::puts(" scope end:");
}

int main() {
    copy_vs_move();
    elision();
    noexcept_matters();
    push_vs_emplace();
}
