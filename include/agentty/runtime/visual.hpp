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
#include <variant>

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
// P1061's pack structured binding (`auto&& [... xs] = v;`) used to live
// here. It is C++26 — g++-14, this repo's primary CI toolchain, cannot parse
// it, so the linux gcc + sanitizer gates have been red since it landed.
//
// The replacement is a fixed-arity structured-binding ladder, dispatched on
// the `arity<T>` already computed above. Each rung names exactly the members
// a brace-init of T would take, IN BRACE-INIT ORDER — the same order P1061
// decomposed in — so the hash stream is bit-identical to the pack's.
//
// The old form's failure modes are preserved by construction:
//   - a type with bases AND members, or private members, does not satisfy
//     any rung (or fails inside one) — it must declare visual_parts, making
//     its visibility decisions explicit;
//   - the arity-0 rung hard-fails with instructions, so a non-decomposable
//     type (custom ctors) can never silently contribute nothing to the hash.
//     Fail closed, as before — now with a message that says what to do.
//
// A rung per arity is boilerplate but dead obvious; a type larger than the
// ladder trips the static_assert at the dispatch site, not a silent skip.
template <class H, class T>
constexpr void mix_decomposed(H&&, const T&, std::integral_constant<std::size_t, 0>) {
    static_assert(std::is_aggregate_v<T>,   // always false when it fires
                  "type reached the visual hash walk but cannot be brace-"
                  "decomposed (custom ctors, or bases+members / private "
                  "members): declare visual_parts(T) for it — see the "
                  "customization point above");
    // An EMPTY aggregate decomposes to an empty pack: contributes nothing,
    // exactly like the P1061 pack it replaces. (The marker structs —
    // ui::panel::None & co. — live here by design.)
}
#define AGENTTY_VISUAL_MIX_RUNG(n, ...)                                        \
    template <class H, class T>                                                \
    constexpr void mix_decomposed(H&& h, const T& v,                           \
                                  std::integral_constant<std::size_t, n>) {    \
        auto& [__VA_ARGS__] = v;                                               \
        std::apply([&](const auto&... xs) { (mix_any(h, xs), ...); },          \
                   std::forward_as_tuple(__VA_ARGS__));                        \
    }
AGENTTY_VISUAL_MIX_RUNG(1,  m0)
AGENTTY_VISUAL_MIX_RUNG(2,  m0, m1)
AGENTTY_VISUAL_MIX_RUNG(3,  m0, m1, m2)
AGENTTY_VISUAL_MIX_RUNG(4,  m0, m1, m2, m3)
AGENTTY_VISUAL_MIX_RUNG(5,  m0, m1, m2, m3, m4)
AGENTTY_VISUAL_MIX_RUNG(6,  m0, m1, m2, m3, m4, m5)
AGENTTY_VISUAL_MIX_RUNG(7,  m0, m1, m2, m3, m4, m5, m6)
AGENTTY_VISUAL_MIX_RUNG(8,  m0, m1, m2, m3, m4, m5, m6, m7)
AGENTTY_VISUAL_MIX_RUNG(9,  m0, m1, m2, m3, m4, m5, m6, m7, m8)
AGENTTY_VISUAL_MIX_RUNG(10, m0, m1, m2, m3, m4, m5, m6, m7, m8, m9)
AGENTTY_VISUAL_MIX_RUNG(11, m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10)
AGENTTY_VISUAL_MIX_RUNG(12, m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11)
#undef AGENTTY_VISUAL_MIX_RUNG
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
        static_assert(arity<T> <= 12,
                      "aggregate with more than 12 members hit the visual hash "
                      "walk: extend the mix_decomposed ladder in this header, "
                      "or declare visual_parts(T) for the type");
        detail::mix_decomposed(h, v,
                               std::integral_constant<std::size_t, arity<T>>{});
    }
}

} // namespace agentty::visual
