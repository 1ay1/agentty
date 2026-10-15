#pragma once
// agentty::ui::step — the one rule for moving a list cursor.
//
// A single step (↑/↓, j/k) wraps: ↓ on the last row lands on the first and
// ↑ on the first lands on the last. A bigger step (PgUp/PgDn sent as ±N)
// stops at the ends, because jumping a page past the bottom back to the top
// loses your place. Every list uses this so they all feel the same.

#include <algorithm>

namespace agentty::ui {

[[nodiscard]] constexpr int step(int index, int delta, int n) noexcept {
    if (n <= 0) return 0;
    if (delta == 1 || delta == -1) return ((index + delta) % n + n) % n;
    return std::clamp(index + delta, 0, n - 1);
}

// Same, skipping rows `skip(i)` says can't hold the cursor (section headers,
// info lines). Stays put if no other row qualifies.
template <class Skip>
[[nodiscard]] constexpr int step_over(int index, int delta, int n, Skip skip) noexcept {
    if (n <= 0) return 0;
    if (delta != 1 && delta != -1) {
        int i = std::clamp(index + delta, 0, n - 1);
        const int back = delta > 0 ? -1 : 1;   // land on a real row near the edge
        for (int g = 0; g < n && skip(i); ++g) {
            const int next = i + back;
            if (next < 0 || next >= n) break;
            i = next;
        }
        return skip(i) ? index : i;
    }
    int i = index;
    for (int g = 0; g < n; ++g) {
        i = ((i + delta) % n + n) % n;
        if (!skip(i)) return i;
    }
    return index;
}

} // namespace agentty::ui
