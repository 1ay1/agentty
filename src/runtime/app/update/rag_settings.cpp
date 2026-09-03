// rag_settings_update — reducer for the RAG mode picker.
//
// One decision: how proactive (pre-turn) retrieval behaves — On / First turn
// only / Off. The cursor row IS the choice; selecting it sets store::RagMode,
// derives the proactive gate, persists to settings.json, and live-applies to
// the process-wide retriever. The advanced knobs stay at their defaults.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/rag_settings.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"

namespace ov = agentty::ui::overlay;

namespace agentty::app::detail {

namespace rs = agentty::rag_settings;
using maya::Cmd;
using maya::overload;

namespace {

// Persist + live-apply the chosen mode. `proactive` is derived from the mode
// (Off ⇒ no pre-turn injection; the First-turn gate is enforced in modal.cpp).
void commit_mode(store::RagMode mode) {
    auto s = deps().load_settings();
    s.rag.configured = true;
    s.rag.mode = mode;
    s.rag.proactive = (mode != store::RagMode::Off);
    deps().save_settings(s);
    tools::rag_apply_settings(s.rag);
}

} // namespace

Step rag_settings_update(Model m, msg::RagSettingsMsg rm) {
    return std::visit(overload{
        [&](OpenRagSettings) -> Step {
            const store::RagMode cur = deps().load_settings().rag.mode;
            m.ui.overlay = ov::RagSettings{{cur, cur}};
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](CloseRagSettings) -> Step {
            // Esc backs out to the command palette it was opened from,
            // matching the other Ctrl+K settings pickers. (Selecting a
            // mode, below, commits and drops to the thread — that's "done",
            // not "back".)
            m.ui.overlay.close<ov::RagSettings>();
            m.ui.overlay = ov::CommandPalette{{"", palette_index_of(Command::OpenRagSettings)}};
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagSettingsMove& e) -> Step {
            if (auto* o = m.ui.overlay.get<ov::RagSettings>())
                o->cursor = rs::next_mode(o->cursor, e.delta);
            return {std::move(m), Cmd<Msg>::none()};
        },
        // Select the highlighted mode and close.
        [&](RagSettingsAdjust&) -> Step {
            if (auto* o = m.ui.overlay.get<ov::RagSettings>()) {
                // The cursor IS the mode — no kModes[i] subscript to get
                // wrong if the row list is ever reordered.
                commit_mode(o->cursor);
                std::string label{store::to_string(o->cursor)};
                m.ui.overlay.close<ov::RagSettings>();
                return {std::move(m),
                        set_status_toast(m, "RAG: " + label, std::chrono::seconds{3})};
            }
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagSettingsReset) -> Step {
            commit_mode(store::RagMode::On);
            if (auto* o = m.ui.overlay.get<ov::RagSettings>())
                o->cursor = store::RagMode::On;
            return {std::move(m), Cmd<Msg>::none()};
        },
    }, rm);
}

} // namespace agentty::app::detail
