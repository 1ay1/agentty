// 12_selftest.cpp — the chapter's claims, as assertions.
//
// Every static_assert here is a claim made somewhere in chapter 1. If
// this file compiles, those claims hold on YOUR compiler and platform.
// If one fails, either your platform differs (interesting) or the
// chapter is wrong (tell me).
//
// Most of the work happens at COMPILE time. The runtime part only covers
// what can't be checked statically.
//
// Build: make 12_selftest && ./12_selftest

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ═══ the types under test ══════════════════════════════════════════════
template <typename Tag>
struct Id {
    std::string value;
    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};
struct ThreadIdTag   {};
struct ToolCallIdTag {};
using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;

// ═══ §1: types ═════════════════════════════════════════════════════════
static_assert(sizeof(char) == 1, "§1: sizeof(char) is 1 by definition");
static_assert(sizeof(short) <= sizeof(int), "§1: the size ordering holds");
static_assert(sizeof(int) <= sizeof(long), "§1: the size ordering holds");
static_assert(sizeof(std::uint8_t) == 1 && sizeof(std::uint32_t) == 4,
              "§1: fixed-width types are fixed");

// §1: member order changes size
struct Padded    { char a; int b; char c; };
struct Packedish { int b; char a; char c; };
static_assert(sizeof(Packedish) < sizeof(Padded),
              "§1: declaring largest-first shrinks the struct");
static_assert(alignof(Padded) == alignof(int),
              "§1: a struct's alignment is its strictest member's");

// ═══ §2: integers ══════════════════════════════════════════════════════
static_assert(std::is_unsigned_v<std::size_t>, "§2: size_t is unsigned");
static_assert(std::size_t{0} - 1 == static_cast<std::size_t>(-1),
              "§2: unsigned subtraction wraps, it does not go negative");
static_assert(!std::cmp_less(1u, -1), "§2: cmp_less compares real values");
static_assert(std::cmp_less(-1, 1u), "§2: and gets -1 < 1u right");
// the built-in comparison gets it WRONG, and that is the point:
static_assert(static_cast<unsigned>(-1) > 1u,
              "§2: -1 converted to unsigned is huge");
static_assert(std::numeric_limits<unsigned>::max() + 1u == 0u,
              "§2: unsigned overflow is DEFINED and wraps");

// ═══ §3: strong types ══════════════════════════════════════════════════
static_assert(sizeof(ThreadId) == sizeof(std::string),
              "§3: the phantom tag adds no byte");
static_assert(alignof(ThreadId) == alignof(std::string),
              "§3: and no alignment either");
static_assert(!std::is_same_v<ThreadId, ToolCallId>,
              "§3: different tags make different types");
static_assert(!std::is_convertible_v<std::string, ThreadId>,
              "§3: explicit blocks the implicit conversion");
static_assert(!std::is_convertible_v<ThreadId, ToolCallId>,
              "§3: and the two ids never convert to each other");
static_assert(std::is_constructible_v<ThreadId, std::string>,
              "§3: but you can still build one when you ask");
static_assert(std::is_nothrow_constructible_v<ThreadId, std::string>,
              "§3: and that construction cannot throw");
static_assert(std::totally_ordered<ThreadId>,
              "§3: defaulted <=> makes it usable in ordered containers");
static_assert(std::equality_comparable<ThreadId>, "§3: and comparable");

// ═══ §4: value categories ══════════════════════════════════════════════
static int  an_int  = 0;
static int& lvalue_fn() { return an_int; }
static int  prvalue_fn() { return 0; }

static_assert(std::is_same_v<decltype(lvalue_fn()), int&>,
              "§4: a function returning T& yields an lvalue");
static_assert(std::is_same_v<decltype(prvalue_fn()), int>,
              "§4: a function returning T yields a prvalue");
static_assert(std::is_same_v<decltype(std::move(an_int)), int&&>,
              "§4: std::move yields an xvalue");
static_assert(std::is_same_v<decltype(an_int), int>,
              "§4: decltype of a NAME gives the declared type");
static_assert(std::is_same_v<decltype((an_int)), int&>,
              "§4: decltype of a parenthesised lvalue gives T&");
static_assert(std::is_lvalue_reference_v<decltype((an_int))>, "§4: ditto");

// §4: the named-rvalue-reference trap, checked statically
static void takes_rref(int&& r) {
    static_assert(std::is_same_v<decltype(r), int&&>,
                  "§4: the DECLARED TYPE is int&&");
    static_assert(std::is_same_v<decltype((r)), int&>,
                  "§4: but the EXPRESSION r is an lvalue");
}

// §4: reference binding table
static_assert(std::is_constructible_v<int&, int&>, "§4: T& <- lvalue ok");
static_assert(!std::is_constructible_v<int&, int&&>, "§4: T& <- rvalue no");
static_assert(std::is_constructible_v<const int&, int&>, "§4: const T& <- lvalue");
static_assert(std::is_constructible_v<const int&, int&&>, "§4: const T& <- rvalue");
static_assert(!std::is_constructible_v<int&&, int&>, "§4: T&& <- lvalue no");
static_assert(std::is_constructible_v<int&&, int&&>, "§4: T&& <- rvalue ok");

// ═══ §5: initialisation ════════════════════════════════════════════════
struct Point { int x; int y; };
static_assert(std::is_aggregate_v<Point>, "§5: Point is an aggregate");
static_assert(Point{}.x == 0 && Point{}.y == 0,
              "§5: value-init zeroes every member");
static_assert(Point{.x = 1, .y = 2}.y == 2,
              "§5: designated initialisers work and keep order");
static_assert(!std::is_aggregate_v<ThreadId>,
              "§5: a user-declared ctor stops it being an aggregate");

// ═══ §6: const and references ══════════════════════════════════════════
static_assert(std::is_same_v<decltype(std::declval<const ThreadId&>().empty()), bool>,
              "§6: empty() is callable on a const object");
// if empty() were not const, the above would not compile at all.

// ═══ §8: copy, move, noexcept ══════════════════════════════════════════
struct RuleOfZero {
    std::string      s;
    std::vector<int> v;
};
static_assert(std::is_nothrow_move_constructible_v<RuleOfZero>,
              "§8: rule of zero gives you a noexcept move for free");
static_assert(std::is_nothrow_move_assignable_v<RuleOfZero>, "§8: and assign");
static_assert(std::is_copy_constructible_v<RuleOfZero>, "§8: and a copy");

static_assert(std::is_nothrow_move_constructible_v<ThreadId>,
              "§8: so vector<ThreadId> MOVES on reallocation, not copies");
static_assert(std::is_nothrow_move_constructible_v<std::string>,
              "§8: std::string's move is noexcept, which is why");

struct HasDtor {
    ~HasDtor() {}
    std::string s;
};
// This is the rule-of-five trap, pinned down exactly. HasDtor still
// SATISFIES is_move_constructible - but only because overload resolution
// falls back to the COPY constructor, which can throw. The implicit move
// was suppressed by the user-declared destructor.
static_assert(std::is_move_constructible_v<HasDtor>,
              "§8: it looks movable...");
static_assert(!std::is_nothrow_move_constructible_v<HasDtor>,
              "§8: ...but the implicit move was SUPPRESSED by the declared "
              "destructor, so this is really the copy ctor, and a vector "
              "holding these will COPY on every reallocation");
static_assert(std::is_nothrow_move_constructible_v<decltype(HasDtor::s)>,
              "§8: even though the only member moves just fine");

// And `= default` does NOT save you. It is DECLARING the destructor that
// suppresses the implicit moves, not what is in its body. This trips up
// people who think they already know the rule.
struct DtorNone     { std::string s; };
struct DtorProvided { std::string s; ~DtorProvided() {} };
struct DtorDefaulted{ std::string s; ~DtorDefaulted() = default; };
static_assert(std::is_nothrow_move_constructible_v<DtorNone>,
              "§8: no declared dtor -> implicit noexcept move");
static_assert(!std::is_nothrow_move_constructible_v<DtorProvided>,
              "§8: a provided dtor suppresses it");
static_assert(!std::is_nothrow_move_constructible_v<DtorDefaulted>,
              "§8: and so does `= default`. tidiness is not free.");

struct MoveOnly {
    std::string s;
    MoveOnly(const MoveOnly&)            = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept        = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};
static_assert(!std::is_copy_constructible_v<MoveOnly>, "§8: = delete works");
static_assert(std::is_nothrow_move_constructible_v<MoveOnly>, "§8: move survives");

// ═══ §9: auto and decltype ═════════════════════════════════════════════
template <typename T> using auto_of = std::remove_cvref_t<T>;
static_assert(std::is_same_v<auto_of<int&>, int>, "§9: auto drops the ref");
static_assert(std::is_same_v<auto_of<const int&>, int>,
              "§9: auto drops ref AND top-level const");
static_assert(std::is_same_v<auto_of<const int*>, const int*>,
              "§9: but NOT the const on a pointee");

// §9: the map trap, as a type identity
static_assert(std::is_same_v<std::map<std::string, int>::value_type,
                             std::pair<const std::string, int>>,
              "§9: map's value_type has a CONST key, which is why "
              "binding to pair<string,int> copies every element");
static_assert(!std::is_same_v<std::map<std::string, int>::value_type,
                              std::pair<std::string, int>>,
              "§9: they are genuinely different types");

// §8: THE RULE-OF-FIVE TRAP, with a price tag.
// Three pairs of near-identical classes. The only difference within each
// pair is a user-declared destructor. (These live at namespace scope
// because a local class cannot have static data members.)
namespace rule_of_five {

inline int copies = 0;
inline int moves  = 0;
inline void reset() { copies = moves = 0; }

// hand-written move: the destructor costs nothing, because you did the work
struct Fine {
    std::string s;
    Fine() = default;
    Fine(const Fine& o) : s(o.s) { ++copies; }
    Fine(Fine&& o) noexcept : s(std::move(o.s)) { ++moves; }
};
struct Trapped {
    std::string s;
    Trapped() = default;
    Trapped(const Trapped& o) : s(o.s) { ++copies; }
    Trapped(Trapped&& o) noexcept : s(std::move(o.s)) { ++moves; }
    ~Trapped() {}                     // <- THE ONLY DIFFERENCE
};

// relying on the IMPLICIT move: this is where the destructor bites
struct ImplicitFine    { std::string s; };
struct ImplicitTrapped { std::string s; ~ImplicitTrapped() {} };

} // namespace rule_of_five

// ═══ the runtime half ══════════════════════════════════════════════════
static int    checks = 0;
static int    fails  = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++fails; std::printf("  FAIL: %s\n", what); }
}

struct Counter {
    static inline int copies = 0;
    static inline int moves  = 0;
    std::string s;
    explicit Counter(const char* c) : s(c) {}
    Counter(const Counter& o) : s(o.s) { ++copies; }
    Counter(Counter&& o) noexcept : s(std::move(o.s)) { ++moves; }
    static void reset() { copies = moves = 0; }
};

static Counter make_rvo()  { return Counter{"x"}; }
static Counter make_nrvo() { Counter c{"x"}; return c; }

int main() {
    std::puts("-- compile-time claims --");
    std::puts("  every static_assert above passed, or this would not have built.");
    std::puts("  that is roughly 45 checks across sections 1, 2, 3, 4, 5, 6, 8, 9.\n");

    std::puts("-- runtime claims --");

    // §4: std::move alone does nothing
    {
        std::string a = "payload";
        (void)std::move(a);
        check(a == "payload", "§4: a bare std::move leaves the object alone");
    }

    // §4: the move happens at the construction, not the cast
    {
        std::string a = "a string long enough to be heap allocated, truly";
        std::string b = std::move(a);
        check(b.size() > 16, "§4: the destination got the bytes");
        check(a.empty(), "§4: libstdc++ leaves a moved-from string empty "
                         "(valid but UNSPECIFIED - do not rely on it)");
    }

    // §8: elision. C++17 guarantees zero copies AND zero moves for a prvalue.
    {
        Counter::reset();
        Counter r = make_rvo();
        check(Counter::copies == 0 && Counter::moves == 0,
              "§8: RVO is guaranteed - no copy, no move");
        (void)r;
    }
    {
        Counter::reset();
        Counter n = make_nrvo();
        check(Counter::copies == 0,
              "§8: NRVO elides the copy (every real compiler does this)");
        (void)n;
    }

    // §8: noexcept move means vector moves instead of copying
    {
        Counter::reset();
        std::vector<Counter> v;
        v.emplace_back("a");
        v.emplace_back("b");   // forces a reallocation
        check(Counter::copies == 0 && Counter::moves >= 1,
              "§8: vector MOVED on growth because the move is noexcept");
    }

    // §8: emplace_back builds in place, push_back moves
    {
        Counter::reset();
        std::vector<Counter> v;
        v.reserve(2);
        v.emplace_back("a");
        check(Counter::moves == 0 && Counter::copies == 0,
              "§8: emplace_back constructs directly, no move");
        v.push_back(Counter{"b"});
        check(Counter::moves == 1, "§8: push_back costs one move");
    }

    // §8: THE RULE-OF-FIVE TRAP, with a price tag.
    {
        using namespace rule_of_five;

        check(std::is_nothrow_move_constructible_v<ImplicitFine>,
              "§8: no declared dtor -> implicit noexcept move");
        check(!std::is_nothrow_move_constructible_v<ImplicitTrapped>,
              "§8: ONE declared dtor -> implicit move gone, silently");

        // with a hand-written move, the destructor costs nothing
        reset();
        { std::vector<Fine> v; for (int i = 0; i < 64; ++i) v.emplace_back(); }
        const int fine_copies = copies, fine_moves = moves;

        reset();
        { std::vector<Trapped> v; for (int i = 0; i < 64; ++i) v.emplace_back(); }
        const int trap_copies = copies, trap_moves = moves;

        check(fine_copies == 0 && trap_copies == 0,
              "§8: with a hand-written noexcept move, a dtor costs nothing");
        std::printf("  growing to 64 elements:\n");
        std::printf("    hand-written move, no dtor : %2d copies, %2d moves\n",
                    fine_copies, fine_moves);
        std::printf("    hand-written move, + dtor  : %2d copies, %2d moves\n",
                    trap_copies, trap_moves);

        // now the version that actually bites: relying on the implicit move
        std::printf("    implicit move, no dtor     : nothrow=%s  -> vector MOVES\n",
                    std::is_nothrow_move_constructible_v<ImplicitFine> ? "yes" : "no");
        std::printf("    implicit move, + dtor      : nothrow=%s  -> vector COPIES\n",
                    std::is_nothrow_move_constructible_v<ImplicitTrapped> ? "yes" : "no");
        std::puts("  one line - a destructor - flipped every reallocation from");
        std::puts("  a pointer swap to a full string copy. no warning, no error.");
        std::puts("  declare one of the five, declare all five.");
    }

    // §2: the loop rewrite is safe on an empty container
    {
        std::vector<int> empty;
        int iterations = 0;
        for (std::size_t i = 0; i + 1 < empty.size(); ++i) ++iterations;
        check(iterations == 0, "§2: `i + 1 < size()` is safe when empty");
    }

    // §3: the strong type actually orders
    {
        std::vector<ThreadId> v{ThreadId{"c"}, ThreadId{"a"}, ThreadId{"b"}};
        std::sort(v.begin(), v.end());
        check(v[0].value == "a" && v[2].value == "c",
              "§3: defaulted <=> makes it sortable");
        std::map<ThreadId, int> m;
        m[ThreadId{"k"}] = 1;
        check(m.size() == 1, "§3: and usable as a map key");
    }

    // §7: destruction order is reverse of construction
    {
        static std::string order;
        struct Rec {
            char c;
            explicit Rec(char ch) : c(ch) {}
            ~Rec() { order += c; }
        };
        { Rec a{'a'}; Rec b{'b'}; Rec c{'c'}; (void)a; (void)b; (void)c; }
        check(order == "cba", "§7: destruction is reverse of construction");
    }

    takes_rref(1);

    std::printf("\n%d runtime checks, %d failures\n", checks, fails);
    if (fails == 0)
        std::puts("everything the chapter claims is true on this machine.");
    else
        std::puts("something the chapter claims is NOT true here. that is worth "
                  "chasing down - read the failing line above.");
    return fails == 0 ? 0 : 1;
}
