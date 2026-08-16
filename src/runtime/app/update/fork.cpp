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
#include "agentty/io/persistence.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include <mcp/tools/util/fs_helpers.hpp>
#include <filesystem>
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

            // 2. Write the parent's transcript to a clean, readable file
            //    the fork can READ ON DEMAND — the whole point of forking:
            //    escape a full context window CHEAPLY. The new thread starts
            //    EMPTY (near-zero tokens); the model pulls earlier context
            //    from disk only if it needs it (exactly like manually
            //    opening a new thread and asking it to read the old one).
            const std::filesystem::path transcript =
                persistence::write_thread_transcript_md(m.d.current);

            // The transcript lives under ~/.agentty/threads — OUTSIDE the
            // workspace the read tool is sandboxed to. Allowlist that dir
            // for reads (both the agentty and mcp-cpp fs layers, since
            // tools are served through mcp-cpp) so the model can actually
            // read the file the note points it at.
            if (!transcript.empty()) {
                const auto dir = persistence::threads_dir();
                tools::util::allow_read_root(dir);
                ::mcp::tools::util::allow_read_root(dir);
            }

            // 3. Build the fork: a FRESH, EMPTY thread with provenance +
            //    per-thread RAG override. No messages carried over.
            Thread fork;
            fork.id = deps().new_thread_id();
            fork.forked_from = parent_id;
            fork.rag_mode_override = static_cast<int>(rag_mode_of(choice));
            fork.created_at = fork.updated_at = std::chrono::system_clock::now();
            fork.title = m.d.current.title.rfind("Fork: ", 0) == 0
                             ? m.d.current.title
                             : ("Fork: " + (m.d.current.title.empty()
                                                ? std::string{"conversation"}
                                                : m.d.current.title));

            // 4. Seed a tiny system note so the model KNOWS the prior
            //    context exists and how to reach it — without paying for it
            //    up front. This is the entire fork mechanism.
            if (!transcript.empty()) {
                Message note;
                note.role = Role::System;
                note.text =
                    "This conversation is a fork of an earlier one. Its full "
                    "transcript is saved at:\n  " + transcript.string() +
                    "\nRead it with the `read` tool (or grep it) ONLY if you "
                    "need earlier context — don't read it pre-emptively. The "
                    "fork starts fresh precisely to reclaim the context "
                    "window; pull just the slice you need.";
                fork.messages.push_back(std::move(note));
            }

            // 5. Switch to the fork — a fresh empty thread, so the render is
            //    trivially cheap (nothing to re-emit). rehydrate_frozen on
            //    an ~empty thread is a no-op; reset_inline paints the clean
            //    composer.
            tools::skills::reset_activations();
            m.ui.view_cache.clear();
            m.d.current = std::move(fork);
            deps().save_thread(m.d.current);
            m.ui.thread_list = pick::Closed{};
            rehydrate_frozen(m);
            m.ui.needs_warmup_render = !m.ui.frozen.empty();

            auto toast = set_status_toast(
                m, std::string{"forked \xc2\xb7 fresh context · "} +
                       label_of(choice) +
                       " · prior transcript readable on demand",
                std::chrono::seconds{5});
            return {std::move(m),
                    Cmd<Msg>::batch(std::move(toast), Cmd<Msg>::reset_inline())};
        },
    }, fm);
}

} // namespace agentty::app::detail
