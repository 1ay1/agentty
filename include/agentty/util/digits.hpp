// agentty/util/digits.hpp — append one decimal digit without overflowing.
//
// Numbers we parse out of text (tool output, diff headers, git numstat,
// version strings, model ids) come from outside. `v = v * 10 + d` on a long
// digit run overflows a signed int, which is UB. Saturate instead.
#pragma once

#include <limits>
#include <type_traits>

namespace agentty::util {

template <class T>
[[nodiscard]] constexpr T push_digit(T v, char c) noexcept {
    static_assert(std::is_integral_v<T>);
    constexpr T kMax = std::numeric_limits<T>::max();
    const T d = static_cast<T>(c - '0');
    if (v > (kMax - d) / 10) return kMax;
    return static_cast<T>(v * 10 + d);
}

static_assert(push_digit(12, '3') == 123);
static_assert(push_digit(std::numeric_limits<int>::max() / 10, '9') == std::numeric_limits<int>::max());
static_assert(push_digit(std::numeric_limits<int>::max(), '0') == std::numeric_limits<int>::max());

}  // namespace agentty::util
