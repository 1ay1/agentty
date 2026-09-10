#pragma once
// agentty::visual — the STRUCTURAL frame-hash walk.
//
// THE PROBLEM THIS RETIRES. visual_hash (program.hpp) gates repaints: a
// frame whose hash equals the last painted frame's is skipped. It used to
// be a hand-maintained enumeration of every model facet the view renders —
// a PARALLEL DESCRIPTION of the view's dependency set with nothing keeping
// the two equal. Forgetting a facet produced a signature bug class: state
// changes, the view WOULD differ, the hash doesn't move, the frame gate
// eats the repaint (login inputs, both forms, the rag probe verdict, the
// result-card scroll — each found by hand, each fixed by hand).
//
// THE INVERSION. mix_any(h, state) derives the hash FROM THE TYPE:
//
//   • scalars/enums/bools    → mixed directly
//   • strings                → FNV over the bytes + length
//   • variants               → alternative index, then the open alternative
//   • optionals              → presence, then the value
//   • ranges                 → size, then each element
//   • aggregates             → DECOMPOSED member-by-member (P1061), so a
//                              member added tomorrow is hashed tomorrow —
//                              there is nothing to remember
//   • everything else        → does not compile ← the load-bearing arm
//
// A type that cannot be auto-walked (private members, bases + members, or
// a field that must NOT be hashed) must define `visual_parts(t)` beside
// its definition: a tuple of the facets to walk, using
//
//   visual::ref(x)   walk x by reference (no copy)
//   visual::exempt   this facet is deliberately NOT visual
//   any value        walked as itself (lengths, counts, projections)
//
// and `static_assert(visual::parts_cover_all<T>)` proves the tuple names
// EXACTLY as many facets as the type has (bases + members): add a member
// without extending the parts list and the assert fires at the type, not
// as a missed repaint three panels later. Exemption is thereby the
// explicit, reviewable act; coverage is the default — the opposite of the
// old contract.
//
// SECRETS. field::Secret and the embed config define visual_parts that
// digest LENGTH ONLY — credential bytes never reach any hash. This is a
// deliberate, asserted exemption, not an accident of enumeration
// (visual_hash_walk_test pins it: same-length overwrite = same hash).
//
// ELM FIT. subscribe(m) declares the input dependencies; view(m) declares
// the pixels; both are pure over the Model. The frame gate needs "did the
// visible state change" — which is a FUNCTION OF THE STATE TYPES, so it is
// derived from them, the same way the slot's Kind is derived from the
// variant rather than maintained beside it.

#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Scoped opt-out for the ONE C++26 construct this header needs (the P1061
// structured-binding pack below). See the comment at its use site: the
// C++23 fallback is deliberate, so the extension diagnostic is expected
// noise there and only there. Kept as a macro pair so the pragma push/pop
// stays balanced and the compiler-specific spelling lives in one place.
#if defined(__clang__)
#define AGENTTY_BEGIN_ALLOW_CXX26                                              \
    _Pragma("clang diagnostic push")                                           \
    _Pragma("clang diagnostic ignored \"-Wc++26-extensions\"")
#define AGENTTY_END_ALLOW_CXX26 _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define AGENTTY_BEGIN_ALLOW_CXX26                                              \
    _Pragma("GCC diagnostic push")                                             \
    _Pragma("GCC diagnostic ignored \"-Wc++26-extensions\"")
#define AGENTTY_END_ALLOW_CXX26 _Pragma("GCC diagnostic pop")
#else
#define AGENTTY_BEGIN_ALLOW_CXX26
#define AGENTTY_END_ALLOW_CXX26
#endif

namespace agentty::visual {

// ── Facet markers ────────────────────────────────────────────────────────

// Deliberately-not-visual facet. Counts toward parts_cover_all (the author
// ACCOUNTED for the member); contributes nothing to the hash.
struct Exempt {};
inline constexpr Exempt exempt{};

// Walk-by-reference wrapper for parts lists: `visual::ref(m.form)` walks
// the referent without copying it into the tuple. (A plain reference can't
// ride in make_tuple; forward_as_tuple of a computed value would dangle —
// this is the explicit, safe middle.)
template <class T>
struct Ref { const T* p; };
template <class T>
[[nodiscard]] constexpr Ref<T> ref(const T& v) noexcept { return Ref<T>{&v}; }

// ── Aggregate arity (bases + direct members, as brace-init slots) ────────
namespace detail {
template <class Exclude>
struct AnyInit {
    // The Exclude guard stops the copy/move constructor from satisfying
    // T{AnyInit} on non-aggregates, which would report arity 1 for
    // everything copyable.
    //
    // DEFINED, not merely declared, even though it is only ever named in
    // the unevaluated operand of brace_constructible's requires-expression.
    // The classic idiom leaves it undefined; clang then reports
    // -Wundefined-inline for every probed type whose member initialization
    // routes through a constexpr callee it decides to instantiate
    // (std::optional's converting constructor is the usual culprit). The
    // std::unreachable() body is well-formed for ANY return type, costs
    // nothing, and makes the "never called" contract explicit instead of
    // making it the linker's problem.
    template <class T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Exclude>)
    constexpr operator T() const noexcept {
        std::unreachable();
    }
};
template <class T, std::size_t... I>
constexpr bool brace_constructible(std::index_sequence<I...>) {
    return requires { T{((void)I, AnyInit<T>{})...}; };
}
template <class T, std::size_t N = 0>
constexpr std::size_t arity_impl() {
    if constexpr (brace_constructible<T>(std::make_index_sequence<N + 1>{}))
        return arity_impl<T, N + 1>();
    else
        return N;
}
} // namespace detail

template <class T>
inline constexpr std::size_t arity =
    detail::arity_impl<std::remove_cvref_t<T>>();

// ── The customization point ──────────────────────────────────────────────
// Found by ADL: define `auto visual_parts(const T&)` in T's namespace,
// returning a std::tuple of facets (values, visual::ref(...), or
// visual::exempt). Every base and every member must be accounted for —
// prove it with parts_cover_all.
template <class T>
concept HasParts = requires(const T& t) { visual_parts(t); };

// Completeness proof: the parts tuple has EXACTLY one entry per
// brace-init slot (base or member). Adding a member without deciding its
// visibility fails here, at the type.
//
// NON-AGGREGATES (custom ctor/dtor — e.g. maya::ScrollState's
// unregistering destructor) have arity 0: brace-probing cannot count
// their members, so no count can be checked. That used to make the proof
// pass VACUOUSLY, which is the worst possible failure mode for a safety
// net — it is silent, and it fires precisely on the types most likely to
// need it. maya::ScrollState is the case in point: it is the single most
// common piece of view state in the app and the proof could not hold it
// to anything.
//
// So arity 0 now fails CLOSED. A non-aggregate must opt in by
// specialising visual::trusted_parts, which is a deliberate, greppable,
// reviewed statement that its parts list was checked by a human — rather
// than an accident of how the type happens to be declared.
template <class T>
inline constexpr bool trusted_parts = false;

template <class T>
inline constexpr bool parts_cover_all =
    (arity<T> == 0 && trusted_parts<std::remove_cvref_t<T>>)
    || (arity<T> != 0
        && std::tuple_size_v<std::remove_cvref_t<
               decltype(visual_parts(std::declval<const T&>()))>> == arity<T>);

// ── The walk ─────────────────────────────────────────────────────────────
// H is any callable taking std::uint64_t (the accumulating mixer).

template <class H, class T>
constexpr void mix_any(H&& h, const T& v);

namespace detail {
template <class H, class T>
constexpr void mix_decomposed(H&& h, const T& v) {
    // P1061 structured-binding pack. Fails to compile for types with both
    // bases and members, or private members — which is the DESIGN: such a
    // type must declare visual_parts, making its visibility decisions
    // explicit.
    //
    // P1061 is C++26. This tree asks for C++26 but falls back to C++23 on
    // toolchains CMake doesn't yet see as cxx_std_26 (Apple clang), where
    // the pack is accepted as an EXTENSION and diagnosed. The fallback is
    // deliberate and load-bearing (see cmake/AgenttyToolchain.cmake), so
    // the diagnostic is noise at exactly one line — silenced here, not
    // project-wide, so a genuine C++26-ism anywhere else still shows up.
    AGENTTY_BEGIN_ALLOW_CXX26
    auto&& [... xs] = v;
    AGENTTY_END_ALLOW_CXX26
    (mix_any(h, xs), ...);
}
template <class>
inline constexpr bool is_ref = false;
template <class T>
inline constexpr bool is_ref<Ref<T>> = true;
} // namespace detail

template <class H, class T>
constexpr void mix_any(H&& h, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Exempt>) {
        // accounted for, contributes nothing
    } else if constexpr (detail::is_ref<U>) {
        mix_any(h, *v.p);
    } else if constexpr (HasParts<U>) {
        static_assert(parts_cover_all<U>,
                      "visual_parts(T) must account for every base and "
                      "member of T (use visual::exempt for non-visual ones)");
        std::apply([&](const auto&... parts) { (mix_any(h, parts), ...); },
                   visual_parts(v));
    } else if constexpr (std::is_same_v<U, bool>) {
        h(v ? 2ull : 1ull);
    } else if constexpr (std::is_enum_v<U>) {
        h(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)) + 0x9e37ull);
    } else if constexpr (std::is_floating_point_v<U>) {
        // Quantized: sub-1/8192 wiggle is invisible at cell resolution.
        h(static_cast<std::uint64_t>(static_cast<std::int64_t>(v * 8192.0)));
    } else if constexpr (std::is_arithmetic_v<U>) {
        h(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
    } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
        const std::string_view s{v};
        std::uint64_t fnv = 1469598103934665603ull;
        for (const unsigned char c : s) {
            fnv ^= c;
            fnv *= 1099511628211ull;
        }
        h(fnv);
        h(s.size());
    } else if constexpr (requires {
                             v.index();
                             std::visit([](const auto&) {}, v);
                         }) {
        h(static_cast<std::uint64_t>(v.index()) + 0x51ull);
        std::visit([&](const auto& alt) { mix_any(h, alt); }, v);
    } else if constexpr (requires {
                             v.has_value();
                             *v;
                         }) {
        h(v.has_value() ? 2ull : 1ull);
        if (v.has_value()) mix_any(h, *v);
    } else if constexpr (requires {
                             v.begin();
                             v.end();
                             v.size();
                         }) {
        h(static_cast<std::uint64_t>(v.size()) + 0xC0ull);
        for (const auto& e : v) mix_any(h, e);
    } else {
        detail::mix_decomposed(h, v);
    }
}

} // namespace agentty::visual
