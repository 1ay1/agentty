#pragma once
// agentty::web_search_form — Settings → Web Search as a form::Form.
//
// The pane is a thin projection of the settings registry: every row is a
// web_search.* registry row, so its label, help, control kind, range, env lock
// and "default" provenance all come from the table. What lives here is only
// the pane's framing and the write-back from rows to config.
//
// Pure: no IO, no globals. The env-lock check inside add_rows reads the
// process environment, which is why this lives under panel/ (with the other
// form builders) and not in a reducer.

#include "agentty/domain/web_search_config.hpp"
#include "agentty/runtime/panel/form.hpp"

namespace agentty::web_search_form {

// The pane for `c`. Cursor on the first row (the on/off switch).
[[nodiscard]] form::Form build_form(const web_search_cfg::Config& c);

// Write every unlocked row back into `c`. Locked rows (an env override) are
// skipped, so an export is never overwritten by what the pane displayed.
void apply_form(const form::Form& f, web_search_cfg::Config& c);


} // namespace agentty::web_search_form
