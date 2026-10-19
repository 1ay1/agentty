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

// Does this thread belong to `workspace`? A thread from before workspaces
// were recorded has none and only shows under All.
[[nodiscard]] inline bool in_workspace(const Thread& t, std::string_view workspace) {
    return !workspace.empty() && t.workspace == workspace;
}

// How many threads were started in `workspace`.
[[nodiscard]] inline int workspace_thread_count(const std::vector<Thread>& threads,
                                                std::string_view workspace) {
    int n = 0;
    for (const auto& t : threads) n += in_workspace(t, workspace) ? 1 : 0;
    return n;
}

// The scope actually in force: This folder when asked for and it has any
// threads, else All. An empty folder never shows an empty picker.
[[nodiscard]] inline bool effective_all(const std::vector<Thread>& threads,
                                        std::string_view workspace, bool all_workspaces) {
    return all_workspaces || workspace_thread_count(threads, workspace) == 0;
}

// A title the old auto-titler saved from a failed reply. It names nothing,
// so the picker shows what you first asked instead.
[[nodiscard]] inline bool is_broken_title(std::string_view t) {
    return t.starts_with("(the model returned") || t.starts_with("(empty response")
        || t == "New thread";
}

// The title the picker shows, which is also what the query matches first.
[[nodiscard]] inline std::string_view shown_title(const Thread& t) {
    if (t.title.empty() || is_broken_title(t.title))
        return t.preview.empty() ? std::string_view{"(untitled)"} : std::string_view{t.preview};
    return t.title;
}

// Indices into `threads`, in their recency order, that are in scope and
// whose title or first message matches `query`. Empty query = every thread
// in scope.
[[nodiscard]] inline std::vector<int> visible_threads(const std::vector<Thread>& threads,
                                                      std::string_view query,
                                                      std::string_view workspace = {},
                                                      bool all_workspaces = true) {
    const bool all = effective_all(threads, workspace, all_workspaces);
    std::vector<int> out;
    out.reserve(threads.size());
    for (int i = 0; i < static_cast<int>(threads.size()); ++i) {
        const auto& t = threads[static_cast<std::size_t>(i)];
        if (!all && !in_workspace(t, workspace)) continue;
        if (query.empty()
            || fuzzy::score(shown_title(t), query).matched()
            || (!t.preview.empty() && fuzzy::score(t.preview, query).matched()))
            out.push_back(i);
    }
    return out;
}

} // namespace agentty::ui
