// fork_update — reducer for the fork picker.
//
// Forking BRANCHES the current thread into a brand-new one (new id,
// forked_from = parent, "Fork:" title) that carries a COPY of the transcript.
// The fork is ALWAYS summarized by the utility model (the cheap compaction
// summary) so the branch starts from a clean recap — there is no verbatim
// option. The picker's only choice is how proactive RAG behaves in the fork,
// stored as the fork's per-thread rag_mode_override:
//
//   RAG per turn   → override = On
//   First-turn RAG → override = FirstTurnOnly
//   RAG off        → override = Off
//
// The original thread is saved and left completely untouched — a fork is
// non-destructive by construction. The summary lands as a wire-only
// CompactionRecord on the fork; its transcript still shows every carried-over
// turn, but the wire prefix collapses to the recap.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"

#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/fork_picker.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/command_palette.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/skills.hpp"

namespace agentty::app::detail {

namespace fp   = agentty::fork_picker;
namespace pick = agentty::ui::pick;
using maya::Cmd;
using maya::overload;

namespace {

// Map a picker Choice to the fork's per-thread RAG override.
store::RagMode rag_mode_of(fp::Choice c) {
    switch (c) {
        case fp::Choice::RagPerTurn:   return store::RagMode::On;
        case fp::Choice::FirstTurnRag: return store::RagMode::FirstTurnOnly;
        case fp::Choice::RagOff:       return store::RagMode::Off;
        case fp::Choice::Count_:       return store::RagMode::Off;
    }
    return store::RagMode::Off;
}

const char* label_of(fp::Choice c) {
    switch (c) {
        case fp::Choice::RagPerTurn:   return "RAG per turn";
        case fp::Choice::FirstTurnRag: return "first-turn RAG";
        case fp::Choice::RagOff:       return "RAG off";
        case fp::Choice::Count_:       return "";
    }
    return "";
}

} // namespace

Step fork_update(Model m, msg::ForkMsg fm) {
    return std::visit(overload{
        [&](OpenForkPicker) -> Step {
            if (m.d.current.messages.empty())
                return {std::move(m), set_status_toast(m, "nothing to fork yet")};
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot fork while the agent is working")};
            m.ui.fork_picker = fp::Open{0};
            m.ui.command_palette = agentty::palette::Closed{};
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](CloseForkPicker) -> Step {
            m.ui.fork_picker = fp::Closed{};
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](ForkPickerMove& e) -> Step {
            if (auto* o = fork_picker_opened(m.ui.fork_picker)) {
                int n = fp::kChoiceCount;
                o->index = ((o->index + e.delta) % n + n) % n;
            }
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](ForkThread&) -> Step {
            const auto* picked = fork_picker_opened(m.ui.fork_picker);
            const fp::Choice choice = picked
                ? static_cast<fp::Choice>(picked->index)
                : fp::Choice::RagOff;
            m.ui.fork_picker = fp::Closed{};
            if (m.d.current.messages.empty())
                return {std::move(m), set_status_toast(m, "nothing to fork yet")};
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot fork while the agent is working")};

            // 1. Persist the parent untouched.
            deps().save_thread(m.d.current);
            const std::string parent_id = m.d.current.id.value;

            // 2. Build the fork: a copy with fresh identity + provenance +
            //    per-thread RAG override.
            Thread fork = m.d.current;
            fork.id = deps().new_thread_id();
            fork.forked_from = parent_id;
            fork.rag_mode_override = static_cast<int>(rag_mode_of(choice));
            fork.created_at = fork.updated_at = std::chrono::system_clock::now();
            if (!fork.title.empty() && fork.title.rfind("Fork: ", 0) != 0)
                fork.title = "Fork: " + fork.title;

            // A fork ALWAYS summarizes — that is the point of forking a
            // nearly-full thread: reclaim context. The picker's choice is
            // ONLY the fork's future RAG behaviour (an independent axis),
            // not whether to summarize. (Previously RAG-on forks landed
            // VERBATIM — they copied the whole transcript and reclaimed
            // nothing, so a user forking to escape a full context got a
            // full context back. That was the bug.) Drop inherited
            // compaction records: the fresh summary supersedes them.
            fork.compactions.clear();

            // 3. Switch to the fork with a BOUNDED render, exactly like
            //    New/Open/Rewind thread: rehydrate_frozen seeds only ~2
            //    screens into the live canvas (older turns live in native
            //    scrollback, never re-emitted), so switching a long thread
            //    repaints ~2 screens instead of the ENTIRE transcript.
            //    (The old path called clear_frozen + reset_inline, which
            //    re-emitted every message live — the slow full repaint.)
            tools::skills::reset_activations();
            m.ui.view_cache.clear();
            m.d.current = std::move(fork);
            deps().save_thread(m.d.current);
            m.ui.thread_list = pick::Closed{};
            rehydrate_frozen(m);
            m.ui.needs_warmup_render = !m.ui.frozen.empty();

            // 4. Summarize on the fork with the utility model (the cheapest-
            //    capable compaction path selects the model). The wire prefix
            //    collapses to the recap — which is what actually reclaims
            //    the context window.
            m.s.compaction_style        = CompactionStyle::Recoverable;
            m.s.compaction_target_index = m.d.current.messages.size();
            m.s.compaction_buffer.clear();
            auto now = std::chrono::steady_clock::now();
            phase::Active ctx;
            ctx.started       = now;
            ctx.last_event_at = now;
            m.s.phase      = phase::Streaming{std::move(ctx)};
            m.s.compacting = true;
            m.s.status = std::string{"forking \xc2\xb7 summarizing… ("} +
                         label_of(choice) + ")";
            m.s.status_until = {};
            return {std::move(m),
                    Cmd<Msg>::batch(cmd::launch_stream(m), Cmd<Msg>::reset_inline())};
        },
    }, fm);
}

} // namespace agentty::app::detail
