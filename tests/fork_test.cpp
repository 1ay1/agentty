// fork_test.cpp — the fork reducer (fork_update / ForkThread).
//
// Pins the two properties the fork redesign fixed:
//   1. A fork ALWAYS summarizes — regardless of the RAG choice — because
//      the point of forking a nearly-full thread is to RECLAIM CONTEXT.
//      (The old behaviour summarized only the RAG-off choice; a RAG-on
//      fork landed verbatim and reclaimed nothing.) So after ForkThread
//      the session is in a compaction (compacting=true, target_index set),
//      and the fork's inherited compaction records are cleared.
//   2. The fork switches to a NEW thread with fresh identity + provenance
//      (forked_from = parent), the parent id unchanged, and the RAG mode
//      override reflecting the picked choice.
//
// The frozen-render bounding (rehydrate_frozen vs a full re-emit) is a
// render-path property covered by the frozen tests; here we assert the
// state-machine contract of the fork.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <optional>
#include <print>
#include <string>
#include <vector>

using namespace agentty;
namespace detail = agentty::app::detail;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::println("  FAIL: {}", msg); ++g_failed; }
}

int g_thread_ids = 0;
void install_stub_deps() {
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"fork-" + std::to_string(++g_thread_ids)}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });
}

// A parent thread with a couple of turns + an inherited compaction record.
Model make_parent() {
    Model m;
    m.d.current.id = ThreadId{"parent"};
    m.d.current.title = "Some conversation";
    for (int i = 0; i < 4; ++i) {
        Message u; u.role = Role::User;      u.text = "q" + std::to_string(i);
        Message a; a.role = Role::Assistant; a.text = "a" + std::to_string(i);
        m.d.current.messages.push_back(std::move(u));
        m.d.current.messages.push_back(std::move(a));
    }
    // Inherited compaction (should be cleared on fork so the fresh summary
    // supersedes it).
    Thread::CompactionRecord rec;
    rec.up_to_index = 2;
    rec.summary = "old recap";
    m.d.current.compactions.push_back(std::move(rec));
    return m;
}

// Open the picker at `choice_index`, then fork.
Model fork_with(Model m, int choice_index) {
    auto s1 = detail::fork_update(std::move(m), OpenForkPicker{});
    Model m1 = std::move(s1.first);
    // Move the picker cursor to the desired choice.
    for (int i = 0; i < choice_index; ++i) {
        auto s = detail::fork_update(std::move(m1), ForkPickerMove{+1});
        m1 = std::move(s.first);
    }
    auto s2 = detail::fork_update(std::move(m1), ForkThread{});
    return std::move(s2.first);
}

void always_summarizes(int choice, const char* name) {
    std::println("--- always_summarizes: {} ---", name);
    Model forked = fork_with(make_parent(), choice);

    // 1. A summarization is in flight regardless of RAG choice.
    check(forked.s.compacting, "fork entered a compaction (summarize)");
    check(forked.s.compaction_target_index == forked.d.current.messages.size(),
          "compaction covers the whole transcript");
    check(forked.d.current.compactions.empty(),
          "inherited compaction records cleared (fresh summary supersedes)");

    // 2. New thread identity + provenance; parent id NOT reused.
    check(forked.d.current.id.value != "parent",
          "fork has a new thread id (got '" + forked.d.current.id.value + "')");
    check(forked.d.current.forked_from == "parent",
          "fork records forked_from=parent");
    check(forked.d.current.title.rfind("Fork: ", 0) == 0,
          "fork title is prefixed (got '" + forked.d.current.title + "')");
    check(forked.d.current.rag_mode_override >= 0,
          "fork carries a RAG-mode override");
    std::println("PASS\n");
}

void distinct_rag_modes() {
    std::println("--- distinct_rag_modes ---");
    // The three choices must yield three distinct rag_mode_override values
    // (On / FirstTurnOnly / Off), all while summarizing.
    int m0 = fork_with(make_parent(), 0).d.current.rag_mode_override;
    int m1 = fork_with(make_parent(), 1).d.current.rag_mode_override;
    int m2 = fork_with(make_parent(), 2).d.current.rag_mode_override;
    check(m0 != m1 && m1 != m2 && m0 != m2,
          "each RAG choice sets a distinct override (" +
          std::to_string(m0) + "," + std::to_string(m1) + "," +
          std::to_string(m2) + ")");
    std::println("PASS\n");
}

void empty_thread_no_fork() {
    std::println("--- empty_thread_no_fork ---");
    Model m;  // no messages
    m.d.current.id = ThreadId{"empty"};
    Model after = fork_with(std::move(m), 0);
    check(!after.s.compacting, "empty thread does not enter a fork");
    check(after.d.current.id.value == "empty", "stays on the same thread");
    std::println("PASS\n");
}

} // namespace

int main() {
    std::println("=== fork_test ===");
    install_stub_deps();
    always_summarizes(0, "RAG per turn");
    always_summarizes(1, "First-turn RAG");
    always_summarizes(2, "RAG off");
    distinct_rag_modes();
    empty_thread_no_fork();
    if (g_failed) { std::println("{} check(s) FAILED", g_failed); return 1; }
    std::println("All fork tests passed.");
    return 0;
}
