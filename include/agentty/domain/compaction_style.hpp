#pragma once
// agentty::CompactionStyle — how a thread's history gets summarized when
// compacting in place OR when forking a thread. Each style maps to a distinct
// summarisation prompt (see cmd_factory.cpp compaction_prompt_for). The set is
// the "all the ways to summarize" the fork picker offers; Recoverable is the
// default (the original recoverable continuation summary) and MUST stay zero
// so a legacy StreamState / persisted default resolves to it.

#include <cstdint>
#include <string_view>

namespace agentty {

enum class CompactionStyle : std::uint8_t {
    Recoverable = 0,  // structured Task/State/Discoveries/Next-Steps handoff
    Brief,            // one short paragraph — TL;DR
    Bullets,         // key points as a bullet outline
    Decisions,       // decisions made + rationale (why, not what)
    Handoff,         // actionable next-steps only, for a fresh agent
    Narrative,       // flowing prose recap of the whole session
};

inline constexpr CompactionStyle kCompactionStyles[] = {
    CompactionStyle::Recoverable, CompactionStyle::Brief,
    CompactionStyle::Bullets,     CompactionStyle::Decisions,
    CompactionStyle::Handoff,     CompactionStyle::Narrative,
};

[[nodiscard]] constexpr std::string_view to_string(CompactionStyle s) noexcept {
    switch (s) {
        case CompactionStyle::Recoverable: return "Recoverable";
        case CompactionStyle::Brief:       return "Brief";
        case CompactionStyle::Bullets:     return "Bullets";
        case CompactionStyle::Decisions:   return "Decisions";
        case CompactionStyle::Handoff:     return "Handoff";
        case CompactionStyle::Narrative:   return "Narrative";
    }
    return "?";
}

// One-line human description for the picker rows.
[[nodiscard]] constexpr std::string_view describe(CompactionStyle s) noexcept {
    switch (s) {
        case CompactionStyle::Recoverable:
            return "structured, recoverable handoff (task · state · next steps)";
        case CompactionStyle::Brief:
            return "one short paragraph — the TL;DR";
        case CompactionStyle::Bullets:
            return "key points as a bullet outline";
        case CompactionStyle::Decisions:
            return "decisions made and why (rationale over mechanics)";
        case CompactionStyle::Handoff:
            return "actionable next steps only, for a fresh agent";
        case CompactionStyle::Narrative:
            return "flowing prose recap of the whole session";
    }
    return "";
}

static_assert(static_cast<std::uint8_t>(CompactionStyle::Recoverable) == 0,
              "Recoverable must stay zero (legacy/default resolves to it).");

} // namespace agentty
