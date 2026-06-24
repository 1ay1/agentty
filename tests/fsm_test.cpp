// fsm_test — unit coverage for the generic typestate machinery in
// include/agentty/io/fsm.hpp.
//
// The header is the compile-time core of agentty's IO/network typestate
// pattern (the HTTP/2 connection lifecycle in src/io/http.cpp is its first
// real consumer). This test pins the three guarantees the header makes:
//
//   1. Edge legality is a pure compile-time relation derived from each
//      state's in-class `fsm_to` successor list. Declared edges read true;
//      everything else (including skip-ahead and reverse edges) reads false.
//      A terminal state (no `fsm_to`) has no outgoing edges.
//
//   2. `assert_legal_edge<From,To>()` compiles for a legal edge. (An illegal
//      edge would be a hard compile error, so it can't be exercised at
//      runtime — we pin the underlying trait instead, which is what the
//      static_assert keys off.)
//
//   3. States are move-only capability tokens: non-copyable, move-only, and
//      their destructors release owned resources exactly once. A transition
//      that consumes a state and a failure that drops it both run cleanup
//      deterministically — the RAII discipline the dial pipeline relies on.
//
// No transport, no network: this is the machinery in isolation.

#include "agentty/io/fsm.hpp"

#include <cassert>
#include <expected>
#include <string>
#include <type_traits>
#include <vector>

namespace fsm = agentty::io::fsm;

// ---------------------------------------------------------------------------
// A toy 4-state linear machine that mirrors the dial lifecycle shape:
//   A ─▶ B ─▶ C ─▶ D(terminal)
// plus a branch state R ─▶ {B, C} to exercise multi-successor lists.
// ---------------------------------------------------------------------------
struct A;
struct B;
struct C;
struct D;
struct R;

struct A : fsm::State<struct ATag> { using fsm_to = fsm::to<B>; };
struct B : fsm::State<struct BTag> { using fsm_to = fsm::to<C>; };
struct C : fsm::State<struct CTag> { using fsm_to = fsm::to<D>; };
struct D : fsm::State<struct DTag> { /* terminal: no fsm_to */ };
struct R : fsm::State<struct RTag> { using fsm_to = fsm::to<B, C>; };

// ─── 1. Edge legality is a compile-time relation ───────────────────────────
// Legal (declared) edges.
static_assert(fsm::is_legal_edge_v<A, B>);
static_assert(fsm::is_legal_edge_v<B, C>);
static_assert(fsm::is_legal_edge_v<C, D>);
// Branch: both successors legal.
static_assert(fsm::is_legal_edge_v<R, B>);
static_assert(fsm::is_legal_edge_v<R, C>);

// Illegal: skip-ahead.
static_assert(!fsm::is_legal_edge_v<A, C>);
static_assert(!fsm::is_legal_edge_v<A, D>);
static_assert(!fsm::is_legal_edge_v<B, D>);
// Illegal: reverse.
static_assert(!fsm::is_legal_edge_v<B, A>);
static_assert(!fsm::is_legal_edge_v<C, B>);
// Illegal: self-loop (never declared).
static_assert(!fsm::is_legal_edge_v<A, A>);
// Illegal: anything leaving the terminal state.
static_assert(!fsm::is_legal_edge_v<D, A>);
static_assert(!fsm::is_legal_edge_v<D, C>);
// Branch state does NOT gain edges it didn't declare.
static_assert(!fsm::is_legal_edge_v<R, D>);
static_assert(!fsm::is_legal_edge_v<R, A>);

// ─── State-trait sanity ────────────────────────────────────────────────────
static_assert(fsm::is_state_v<A>);
static_assert(fsm::is_state_v<D>);
struct NotAState {};
static_assert(!fsm::is_state_v<NotAState>);

// States are move-only.
static_assert(!std::is_copy_constructible_v<A>);
static_assert(!std::is_copy_assignable_v<A>);
static_assert(std::is_move_constructible_v<A>);

// ─── 2. assert_legal_edge compiles for legal edges ─────────────────────────
// (Calling it for an illegal edge is a hard compile error by design, so the
// negative case is pinned by the !is_legal_edge_v static_asserts above.)
void edge_guards_compile() {
    fsm::assert_legal_edge<A, B>();
    fsm::assert_legal_edge<B, C>();
    fsm::assert_legal_edge<C, D>();
    fsm::assert_legal_edge<R, B>();
    fsm::assert_legal_edge<R, C>();
}

// ---------------------------------------------------------------------------
// 3. Move-only RAII: a resource-owning state runs cleanup exactly once,
//    whether it's consumed by a successful transition or dropped on failure.
// ---------------------------------------------------------------------------
static std::vector<std::string>* g_log = nullptr;

struct Err { std::string why; };

// Owns a named "handle"; logs on destruction unless it was released.
struct Owning : fsm::State<struct OwningTag> {
    using fsm_to = fsm::to<struct Owning2>;
    std::string name;
    bool        live = false;

    explicit Owning(std::string n) : name(std::move(n)), live(true) {}
    Owning(Owning&& o) noexcept
        : name(std::move(o.name)), live(o.live) { o.live = false; }
    Owning& operator=(Owning&&) = delete;
    ~Owning() { if (live && g_log) g_log->push_back("~" + name); }

    void release() noexcept { live = false; }
};
struct Owning2 : fsm::State<struct Owning2Tag> { std::string name; };

static_assert(fsm::is_legal_edge_v<Owning, Owning2>);

// Transition that SUCCEEDS: consumes Owning, hands its handle to Owning2.
fsm::Result<Owning2, Err> advance_ok(Owning&& s) {
    fsm::assert_legal_edge<Owning, Owning2>();
    s.release();                       // ownership transferred forward
    return Owning2{ {}, std::move(s.name) };
}

// Transition that FAILS: returns unexpected; the consumed Owning's dtor
// must fire (cleanup of the partially-built resource).
fsm::Result<Owning2, Err> advance_fail(Owning&& s) {
    fsm::assert_legal_edge<Owning, Owning2>();
    (void)s;                           // not released → ~Owning logs
    return std::unexpected(Err{ "boom" });
}

int main() {
    edge_guards_compile();

    // (a) Successful transition: no cleanup log (handle moved forward, the
    //     source token was released; Owning2 doesn't log).
    {
        std::vector<std::string> log;
        g_log = &log;
        auto r = advance_ok(Owning{ "conn-ok" });
        assert(r.has_value());
        assert(r->name == "conn-ok");
        assert(log.empty() && "released handle must not run cleanup");
        g_log = nullptr;
    }

    // (b) Failed transition: the consumed token's destructor releases the
    //     half-built resource exactly once.
    {
        std::vector<std::string> log;
        g_log = &log;
        auto r = advance_fail(Owning{ "conn-fail" });
        assert(!r.has_value());
        assert(r.error().why == "boom");
        assert(log.size() == 1 && log[0] == "~conn-fail"
               && "dropped token must run cleanup exactly once");
        g_log = nullptr;
    }

    // (c) Pipeline threading: expected's monadic and_then advances through a
    //     legal chain, short-circuiting on the first failure. Models the
    //     dial_new(...).and_then(...).and_then(...) shape.
    {
        std::vector<std::string> log;
        g_log = &log;
        // A→ok→ (then a failing step) leaves no leak: each consumed token is
        // either moved forward or destroyed.
        auto pipeline =
            advance_ok(Owning{ "p" })
                .and_then([](Owning2&& s) -> fsm::Result<Owning2, Err> {
                    // a no-op "transition" that just forwards
                    return Owning2{ {}, std::move(s.name) };
                });
        assert(pipeline.has_value());
        assert(pipeline->name == "p");
        assert(log.empty());
        g_log = nullptr;
    }

    return 0;
}
