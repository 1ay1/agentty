#pragma once
// domain/rag_mode.hpp — the proactive-retrieval mode, as vocabulary.
//
// This enum used to live in store/store.hpp, next to the settings struct
// that persists it. That put a piece of DOMAIN vocabulary inside the storage
// layer, and it only became visible when a Thread needed to name its own
// per-thread override: store/store.hpp includes domain/conversation.hpp, so
// the field could not name its own type without a cycle. The workaround
// would have been a second, identical enum — the exact duplication that lets
// two spellings of the same idea drift apart.
//
// A mode is a fact about a conversation, not about a file format, so it
// belongs here. store:: keeps an alias, so `store::RagMode` still compiles
// everywhere it is already written.

#include <cstdint>
#include <string_view>

namespace agentty {

enum class RagMode : std::uint8_t {
    On = 0,        // proactive pre-turn retrieval on every turn
    FirstTurnOnly, // proactive retrieval only on a thread's first turn
    Off,           // no proactive injection (search_docs/search_code still work)
};

[[nodiscard]] constexpr std::string_view to_string(RagMode m) noexcept {
    switch (m) {
        case RagMode::On:            return "On";
        case RagMode::FirstTurnOnly: return "First turn only";
        case RagMode::Off:           return "Off";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view describe(RagMode m) noexcept {
    switch (m) {
        case RagMode::On:
            return "retrieve context before every turn";
        case RagMode::FirstTurnOnly:
            return "retrieve once, at the start of a thread";
        case RagMode::Off:
            return "no automatic retrieval";
    }
    return "";
}

} // namespace agentty
