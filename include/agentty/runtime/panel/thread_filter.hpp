#pragma once
// agentty::ui::thread_filter — which saved threads the ^J picker shows.
//
// One function the view and the reducer share, so the row you see and the
// thread Enter opens are always the same one.

#include <string_view>
#include <vector>

#include "agentty/domain/conversation.hpp"   // Thread
#include "agentty/runtime/fuzzy.hpp"

namespace agentty::ui {

// Indices into `threads`, in their recency order, whose title matches
// `query`. Empty query = every thread.
[[nodiscard]] inline std::vector<int> visible_threads(const std::vector<Thread>& threads,
                                                      std::string_view query) {
    std::vector<int> out;
    out.reserve(threads.size());
    for (int i = 0; i < static_cast<int>(threads.size()); ++i) {
        const auto& t = threads[static_cast<std::size_t>(i)];
        if (query.empty()
            || fuzzy::score(t.title.empty() ? std::string_view{"(untitled)"}
                                            : std::string_view{t.title}, query).matched())
            out.push_back(i);
    }
    return out;
}

} // namespace agentty::ui
