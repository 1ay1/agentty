#pragma once
// Fork picker — "branch this conversation into a fresh thread; pick how RAG
// behaves in the fork."
//
// Open it from the command palette (Ctrl+K → "Fork thread"). A fork ESCAPES
// a full context window: the new thread starts FRESH with near-zero context
// (NOT a copy or a summary of the parent). The parent's full transcript is
// exported to disk and the fork carries a single pointer to it that the
// model READS ON DEMAND — so forking is O(1) in tokens regardless of how
// big the parent got. One pane, three choices for the new thread's RAG
// behaviour:
//
//   RAG per turn    fork; proactive retrieval on every turn
//   First-turn RAG  fork; proactive retrieval on the first turn only
//   RAG off         fork; no proactive injection (search tools still work)
//
// Enter forks: the current thread is saved untouched, a fresh thread is
// created (new id, forked_from = parent), seeded with the fork-note pointer,
// and its RAG mode is set to the chosen behaviour. The original thread is
// never modified. Esc closes. See docs/FORK.md for the full design.
//
// UI-state only; reducer in update/fork.cpp, key dispatch in subscribe.cpp,
// view in view/fork_view.cpp.

#include <variant>

namespace agentty {

namespace fork_picker {

// The three rows, in display order.
enum class Choice {
    RagPerTurn,     // fresh fork + RAG every turn
    FirstTurnRag,   // fresh fork + RAG first turn only
    RagOff,         // fresh fork, no RAG
    Count_,
};

inline constexpr int kChoiceCount = static_cast<int>(Choice::Count_);

struct Closed {};
struct Open { int index = 0; };   // cursor row in [0, kChoiceCount)

} // namespace fork_picker

using ForkPickerState = std::variant<fork_picker::Closed, fork_picker::Open>;

[[nodiscard]] inline bool fork_picker_is_open(const ForkPickerState& s) noexcept {
    return std::holds_alternative<fork_picker::Open>(s);
}
[[nodiscard]] inline fork_picker::Open* fork_picker_opened(ForkPickerState& s) noexcept {
    return std::get_if<fork_picker::Open>(&s);
}
[[nodiscard]] inline const fork_picker::Open*
fork_picker_opened(const ForkPickerState& s) noexcept {
    return std::get_if<fork_picker::Open>(&s);
}

} // namespace agentty
