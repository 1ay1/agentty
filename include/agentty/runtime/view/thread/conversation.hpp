#pragma once
#include <maya/widget/conversation.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the Conversation config — list of Turn::Configs (one per
// visible message in the view window) plus an optional bottom
// in-flight indicator. Called from thread_config() when the thread
// is non-empty.
//
// include_frozen=true (default, classic/test callers): borrows
// &m.ui.frozen so the settled prefix renders via list_ref. false
// (Strata LIVE node): cfg.frozen=nullptr so ONLY the live tail is
// rendered here — the settled prefix is supplied to Strata as separate
// sealed nodes instead.
[[nodiscard]] maya::Conversation::Config conversation_config(
    const Model& m, bool include_frozen = true);

} // namespace agentty::ui
