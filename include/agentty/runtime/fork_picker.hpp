#pragma once
// Fork picker — "branch this conversation into a new thread; pick how RAG
// behaves in the fork."
//
// Open it from the command palette (Ctrl+K → "Fork thread"). The fork is
// ALWAYS summarized by the utility model (the cheap compaction summary), so
// the branch starts from a clean, well-written recap rather than the raw
// transcript. One pane, three choices for the new thread's RAG behaviour:
//
//   RAG per turn    fork; proactive retrieval on every turn
//   First-turn RAG  fork; proactive retrieval on the first turn only,
//                   retrieving over the full carried-over thread
//   RAG off         fork; no proactive injection (search tools still work)
//
// Enter forks: the current thread is saved untouched, a COPY becomes a new
// thread (new id, forked_from = parent, "Fork:" title), a utility-model
// summary is kicked on it, and its RAG mode is set to the chosen behaviour.
// The original thread is never modified. Esc closes.
//
// UI-state only; reducer in update/fork.cpp, key dispatch in subscribe.cpp,
// view in view/fork_view.cpp.

#include <variant>

namespace agentty {

namespace fork_picker {

// The three rows, in display order.
enum class Choice {
    RagPerTurn,     // fork (summarized) + RAG every turn
    FirstTurnRag,   // fork (summarized) + RAG first turn only (full thread)
    RagOff,         // fork (summarized), no RAG
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
