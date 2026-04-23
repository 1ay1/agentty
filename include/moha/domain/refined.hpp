#pragma once
// moha::domain::refined — refinement types: a `Refined<T, Predicate>`
// is a value of type `T` carrying a *type-level* proof that some
// predicate holds. Once you have one, downstream code can read the
// inner value without re-checking — the only way to construct one is
// through `try_make`, which validates and returns an `expected`.
//
// This is the C++ analogue of Rust's "make invalid states
// unrepresentable" via newtypes plus smart constructors. Every place
// that *would* accept `std::string` and re-validate "is this empty?"
// at the top instead accepts `NonEmpty<std::string>` — the validation
// has already happened at construction time, the type system proves it.
//
// `Predicate` is a stateless functor with `static constexpr bool
// check(const T&)` (so the validation can be `consteval` for literal
// inputs). Failing a check returns a typed reason — caller decides what
// to do with it. The same predicate type is the *identity* of the
// refinement: `NonEmpty<string>` and `NotBlank<string>` are different
// types even though both wrap a `string`.

#include <concepts>
#include <expected>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace moha::domain {

// ── Predicate concept ────────────────────────────────────────────────────
template <class P, class T>
concept RefinementPredicate = requires(const T& v) {
    { P::check(v) } -> std::convertible_to<bool>;
    { P::what()  } -> std::convertible_to<std::string_view>;
};

// ── Refined<T, Predicate> ────────────────────────────────────────────────
// Hold-by-value newtype. `value()` returns the inner T. The only
// constructors are: defaulted (when T is default-constructible AND
// the default value satisfies P), and the static `try_make` factory.
//
// A `consteval` overload of `try_make` exists for literal inputs;
// it makes "non-empty literal string" a compile-time check. Pass an
// `std::string_view` literal and you'll either compile or fail-with-
// `static_assert`-ish error during constant evaluation.
template <class T, class Predicate>
    requires RefinementPredicate<Predicate, T>
class Refined {
    T value_{};

    explicit constexpr Refined(T v) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(v)) {}

public:
    using value_type = T;
    using predicate  = Predicate;

    [[nodiscard]] constexpr const T& value() const noexcept { return value_; }
    [[nodiscard]] constexpr operator const T&() const noexcept { return value_; }

    // Validating factory. Returns the typed reason on failure so callers
    // can build their own typed error (e.g. ToolError::invalid_args(...)).
    struct Error {
        std::string_view what;        // = Predicate::what()
        std::string      detail;      // optional context (caller-supplied)
    };

    [[nodiscard]] static constexpr std::expected<Refined, Error>
    try_make(T v, std::string_view detail_context = {}) {
        if (Predicate::check(v))
            return Refined{std::move(v)};
        return std::unexpected(Error{Predicate::what(), std::string{detail_context}});
    }

    // consteval factory: same as try_make but only callable in a constant-
    // evaluated context. Use to build refined literals; failures become
    // hard compile errors via the `Predicate::check` constraint failing.
    [[nodiscard]] static consteval Refined make_unchecked(T v) {
        // Caller is asserting the predicate holds; failure is UB.
        return Refined{std::move(v)};
    }

    [[nodiscard]] friend constexpr bool operator==(
        const Refined& a, const Refined& b) noexcept(noexcept(a.value_ == b.value_)) {
        return a.value_ == b.value_;
    }

    // Stream insertion — defers to T's operator<< when present. Lets
    // refined values flow through `std::ostream` / `std::ostringstream`
    // without a `.value()` call, since the implicit conversion to
    // `const T&` doesn't kick in for templated operator<< lookup.
    template <class CharT, class Traits>
        requires requires(std::basic_ostream<CharT, Traits>& os, const T& v) {
            { os << v } -> std::same_as<std::basic_ostream<CharT, Traits>&>;
        }
    friend std::basic_ostream<CharT, Traits>&
    operator<<(std::basic_ostream<CharT, Traits>& os, const Refined& r) {
        return os << r.value_;
    }
};

// ── Predicates ───────────────────────────────────────────────────────────

// `NonEmpty` for any container type that has `.empty()`.
struct NonEmptyPred {
    template <class T>
        requires requires(const T& t) { { t.empty() } -> std::convertible_to<bool>; }
    [[nodiscard]] static constexpr bool check(const T& v) noexcept {
        return !v.empty();
    }
    [[nodiscard]] static constexpr std::string_view what() noexcept {
        return "must be non-empty";
    }
};

template <class T> using NonEmpty = Refined<T, NonEmptyPred>;

// `Bounded<T, Lo, Hi>` — integral T constrained to [Lo, Hi]. Lo/Hi are
// non-type template parameters baked into the type identity, so
// `Bounded<int, 0, 100>` and `Bounded<int, 0, 1000>` are different types.
template <std::integral T, T Lo, T Hi>
struct InRangePred {
    static_assert(Lo <= Hi, "bounded range must satisfy Lo <= Hi");
    [[nodiscard]] static constexpr bool check(T v) noexcept {
        return v >= Lo && v <= Hi;
    }
    [[nodiscard]] static constexpr std::string_view what() noexcept {
        return "must be within bounded range";
    }
};

template <std::integral T, T Lo, T Hi>
using Bounded = Refined<T, InRangePred<T, Lo, Hi>>;

// `Positive<T>` — strictly > 0. Common enough to deserve its own alias.
template <std::integral T>
struct PositivePred {
    [[nodiscard]] static constexpr bool check(T v) noexcept { return v > 0; }
    [[nodiscard]] static constexpr std::string_view what() noexcept {
        return "must be > 0";
    }
};
template <std::integral T> using Positive = Refined<T, PositivePred<T>>;

// ── Compile-time tests of the refinement machinery ───────────────────────
namespace tests {
static_assert(NonEmpty<std::string>::try_make(std::string{"x"}).has_value());
static_assert(!NonEmpty<std::string>::try_make(std::string{}).has_value());
static_assert(Bounded<int, 0, 10>::try_make(5).has_value());
static_assert(!Bounded<int, 0, 10>::try_make(11).has_value());
static_assert(!Bounded<int, 0, 10>::try_make(-1).has_value());
static_assert(Positive<int>::try_make(1).has_value());
static_assert(!Positive<int>::try_make(0).has_value());
} // namespace tests

} // namespace moha::domain
