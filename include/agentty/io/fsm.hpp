#pragma once
// agentty::io::fsm — compile-time typestate machinery for IO/network lifecycles.
//
// This header is the GENERIC, dependency-free core of agentty's typestate
// pattern. It carries no transport types (no OpenSSL, no nghttp2, no sockets)
// so it can be included anywhere — the concrete state structs that drive real
// transports are defined in the translation unit that owns those types
// (src/io/http.cpp for the HTTP/2 connection lifecycle, etc.).
//
// What it gives you
// -----------------
//   • State<Tag>            — a move-only, non-copyable empty marker base a
//                             concrete state struct inherits from. Move-only
//                             ownership is the whole point: a state value is a
//                             *capability token*. You consume it to transition;
//                             once consumed it's gone, so you cannot drive a
//                             transition twice or out of order.
//
//   • transition<From,To>   — declare a legal edge of the machine. Specialize
//                             `legal_edge<From,To>` (via the DECLARE_EDGE macro)
//                             to mark From→To as allowed. A transition method
//                             that returns `Result<To>` is gated by a
//                             `static_assert(is_legal_edge_v<From,To>)`, so an
//                             illegal edge fails to COMPILE — the diagnostic
//                             names the exact From/To pair.
//
//   • Result<S>             — `std::expected<S, E>` for a transition that either
//                             advances to state S or fails with the domain
//                             error E. Defaulted on the error type so each
//                             subsystem plugs in its own (HttpError, etc.).
//
// Why this is zero-cost
// ---------------------
// Every state struct is empty-or-payload-only and move-only; there is no
// vtable, no type erasure, no heap. `legal_edge` is a pure type-level trait
// resolved at compile time — it generates no code. The `consteval`
// `assert_legal_edge` is evaluated and discarded during compilation. At
// runtime the typestate pipeline is exactly the same sequence of moves and
// `std::expected` checks you'd write by hand; the types just make the illegal
// orderings unrepresentable. This preserves agentty's speed/size contract:
// no per-event allocation, no dispatch cost on the hot path.
//
// Usage sketch (concrete states live in the .cpp that owns the transport):
//
//     struct Dialing      : fsm::State<struct DialingTag>      { /* inputs */ };
//     struct TcpConnected : fsm::State<struct TcpConnectedTag> { socket_t fd; };
//     AGENTTY_FSM_EDGE(Dialing, TcpConnected);
//
//     fsm::Result<TcpConnected, HttpError> connect(Dialing&& s) {
//         fsm::assert_legal_edge<Dialing, TcpConnected>();
//         ... // do the real dial; on success:
//         return TcpConnected{ .fd = fd };
//     }
//
// Calling a transition the machine doesn't declare (e.g. Dialing → H2Ready
// skipping TLS) trips the static_assert and never links.

#include <expected>
#include <type_traits>
#include <utility>

namespace agentty::io::fsm {

// ---------------------------------------------------------------------------
// State<Tag> — move-only marker base for a concrete typestate.
// ---------------------------------------------------------------------------
// Tag is a unique (usually incomplete) type that distinguishes one state from
// another even when their payloads coincide. Inheriting from State<Tag> makes
// the concrete struct:
//   • non-copyable        — a capability token can't be duplicated;
//   • move-constructible  — it can be consumed by a transition (passed by &&);
//   • default-constructible from its own aggregate initializer.
//
// The base holds no data and is trivially relocatable, so deriving from it
// costs nothing (empty-base optimization applies).
template <class Tag>
struct State {
    using tag_type = Tag;

    constexpr State() noexcept = default;

    State(const State&)            = delete;
    State& operator=(const State&) = delete;

    State(State&&) noexcept            = default;
    State& operator=(State&&) noexcept = default;

    // Public, non-virtual: states are value types, never deleted through a
    // base pointer (nobody holds a State*). Public so a derived aggregate can
    // be brace-initialized — a protected base dtor would block that.
    ~State() = default;
};

// True iff S is one of our typestates (derives from State<S::tag_type>).
template <class S, class = void>
inline constexpr bool is_state_v = false;
template <class S>
inline constexpr bool is_state_v<
    S, std::void_t<typename S::tag_type>> =
        std::is_base_of_v<State<typename S::tag_type>, S>;

// ---------------------------------------------------------------------------
// legal_edge<From, To> — the type-level adjacency relation of the machine.
// ---------------------------------------------------------------------------
// An edge From→To is legal iff `From` names `To` among its declared
// successors. A state declares its successors with an in-class type list:
//
//     struct TlsUp : fsm::State<TlsUpTag> {
//         using fsm_to = fsm::to<H2Ready>;   // single successor
//         ...
//     };
//     struct Routing : fsm::State<RoutingTag> {
//         using fsm_to = fsm::to<StateA, StateB>;  // a branch
//     };
//
// This keeps the whole declaration IN-CLASS — no out-of-line template
// specialization — so it works even when the state structs live in an
// anonymous namespace (where injecting a specialization into fsm:: would be
// ill-formed). A state with no `fsm_to` is terminal: no edge leaves it.
template <class... To>
struct to {};

namespace detail {
template <class T, class List>
struct list_has : std::false_type {};
template <class T, class... Xs>
struct list_has<T, to<Xs...>>
    : std::bool_constant<(std::is_same_v<T, Xs> || ...)> {};

// Extract From's successor list, or an empty `to<>` if it declares none.
template <class From, class = void>
struct successors { using type = to<>; };
template <class From>
struct successors<From, std::void_t<typename From::fsm_to>> {
    using type = typename From::fsm_to;
};
}  // namespace detail

template <class From, class To>
struct legal_edge
    : detail::list_has<To, typename detail::successors<From>::type> {};

template <class From, class To>
inline constexpr bool is_legal_edge_v = legal_edge<From, To>::value;

// Compile-time guard placed at the top of every transition function body.
// If From→To isn't a declared edge this is ill-formed and the build fails
// with a diagnostic that names both states. Consteval ⇒ evaluated and
// discarded at compile time; emits no code.
template <class From, class To>
consteval void assert_legal_edge() noexcept {
    static_assert(is_state_v<From>, "FSM: From is not an fsm::State");
    static_assert(is_state_v<To>,   "FSM: To is not an fsm::State");
    static_assert(is_legal_edge_v<From, To>,
                  "FSM: illegal state transition — To is not among From's "
                  "declared successors. Add `To` to From's `using fsm_to = "
                  "fsm::to<...>` list if this transition is intended.");
}

// ---------------------------------------------------------------------------
// Result<S, E> — a transition outcome: advance to state S, or fail with E.
// ---------------------------------------------------------------------------
// The error type is a template parameter so each subsystem supplies its own
// typed error (agentty::http::HttpError, a subprocess error, …). A transition
// returns Result<NextState, DomainError>.
template <class S, class E>
using Result = std::expected<S, E>;

}  // namespace agentty::io::fsm

