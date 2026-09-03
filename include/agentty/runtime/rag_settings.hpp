#pragma once
// RAG mode picker — one decision: how proactive (pre-turn) retrieval behaves.
//
// Open it from the command palette (Ctrl+K → "RAG"). Three modes:
//
//   On               inject retrieved context before EVERY turn
//   First turn only  inject only on a thread's first turn (grounding, then off)
//   Off              no proactive injection (search_docs/search_code still work)
//
// ↑↓ move, Enter/Space/← → select, Esc closes. The choice persists to
// settings.json and applies live. The advanced retrieval knobs are no longer
// in the UI — they stay at their defaults (env-tunable for power users).
//
// UI-state only; reducer in update/rag_settings.cpp, view in view/pickers.cpp.

#include <variant>

#include "agentty/store/store.hpp"   // store::RagMode

namespace agentty {

namespace rag_settings {

struct Closed {};
struct Open {
    // The cursor is the MODE itself, not an index into kModes. Storing an
    // index means every read is `kModes[i]` — an unchecked array subscript
    // whose meaning silently changes if the list is reordered or resized.
    store::RagMode cursor = store::RagMode::On;
    store::RagMode active = store::RagMode::On;   // persisted mode (row marker)
};

// The rows, in display order — 1:1 with store::RagMode. The ONE place the
// layout is written down: the view walks it and the cursor moves through it.
inline constexpr store::RagMode kModes[] = {
    store::RagMode::On, store::RagMode::FirstTurnOnly, store::RagMode::Off,
};
inline constexpr int kModeCount =
    static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));

// Cursor movement closed over the enumeration — no call site owns a modulus.
[[nodiscard]] constexpr store::RagMode next_mode(store::RagMode mmode,
                                                 int delta) noexcept {
    int i = 0;
    for (int k = 0; k < kModeCount; ++k)
        if (kModes[k] == mmode) { i = k; break; }
    const int n = kModeCount;
    return kModes[((i + delta) % n + n) % n];
}

} // namespace rag_settings

using RagSettingsState =
    std::variant<rag_settings::Closed, rag_settings::Open>;

[[nodiscard]] inline bool rag_settings_is_open(const RagSettingsState& s) noexcept {
    return std::holds_alternative<rag_settings::Open>(s);
}
[[nodiscard]] inline rag_settings::Open*
rag_settings_opened(RagSettingsState& s) noexcept {
    return std::get_if<rag_settings::Open>(&s);
}
[[nodiscard]] inline const rag_settings::Open*
rag_settings_opened(const RagSettingsState& s) noexcept {
    return std::get_if<rag_settings::Open>(&s);
}

} // namespace agentty
