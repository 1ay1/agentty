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
    int index = 0;              // cursor over the 3 RagMode rows
    store::RagMode active = store::RagMode::On;   // persisted mode (row marker)
};

// The rows, in display order — 1:1 with store::RagMode.
inline constexpr store::RagMode kModes[] = {
    store::RagMode::On, store::RagMode::FirstTurnOnly, store::RagMode::Off,
};
inline constexpr int kModeCount = 3;

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
