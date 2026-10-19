#pragma once
// The ^J peek's content: a thread cut into turns of display lines. Built off
// the reducer when a thread loads, so the cache holds KBs, not transcripts.

#include <cstdint>
#include <string>
#include <vector>

#include "agentty/domain/conversation.hpp"

namespace agentty::ui {

struct PeekLine {
    enum class Kind : std::uint8_t { User, Reply, Tool };
    std::string who;    // speaker; "" on a continuation line
    std::string text;
    Kind        kind = Kind::Reply;
};
using PeekTurn = std::vector<PeekLine>;

// Lines kept per turn: enough to fill any card, so a 2000-line reply costs
// the same as a short one.
inline constexpr std::size_t kPeekLinesPerTurn = 24;

// One message as peek lines: text by line (blank runs collapsed), then
// "▸ name" per tool call. Nothing for injected or routing messages.
inline void peek_lines_of(const Message& msg, PeekTurn& out) {
    const bool user = msg.typed_by_user();
    if (!user && msg.role != Role::Assistant) return;
    if (msg.smart_routing || msg.is_proactive_context()) return;
    const std::string who = user ? "you" : "agentty";
    bool first = true;
    int blank = 0;
    const std::string& s = msg.text;
    std::size_t start = 0;
    while (start < s.size() && out.size() < kPeekLinesPerTurn) {
        const auto nl = s.find('\n', start);
        std::string line = s.substr(start, nl == std::string::npos ? std::string::npos
                                                                   : nl - start);
        start = nl == std::string::npos ? s.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) {
            if (first || ++blank > 1) continue;
            line.clear();
        } else {
            blank = 0;
        }
        out.push_back({first ? who : std::string{}, std::move(line),
                       user ? PeekLine::Kind::User : PeekLine::Kind::Reply});
        first = false;
    }
    for (const auto& tc : msg.tool_calls) {
        if (out.size() >= kPeekLinesPerTurn) break;
        out.push_back({first ? who : std::string{},
                       "\xe2\x96\xb8 " + std::string{tc.name.value}, PeekLine::Kind::Tool});   // ▸
        first = false;
    }
}

// A thread as turns: each typed message starts one.
[[nodiscard]] inline std::vector<PeekTurn> peek_turns_of(const Thread& t) {
    std::vector<PeekTurn> turns;
    for (const auto& msg : t.messages) {
        if (msg.typed_by_user() || turns.empty()) turns.emplace_back();
        peek_lines_of(msg, turns.back());
    }
    std::erase_if(turns, [](const PeekTurn& v) { return v.empty(); });
    return turns;
}

}  // namespace agentty::ui
