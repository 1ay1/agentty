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

#include <functional>
#include <string>
#include <string_view>

namespace agentty::web_search_form {

// Where a service's API key comes from right now: "stored", "env: NAME", or ""
// when none. The pane only ever learns THAT a key exists, never what it is.
// Passed in rather than looked up so building the pane touches no keystore.
using KeyOrigin = std::function<std::string(std::string_view service)>;

// Row ids beyond the registry's. A service's dials and key hang under the
// slot that names it (the first slot only, when two name the same service).
//   web_search.dial.<service>.<dial>   a Choice; value id "" = the service default
//   web_search.key.<service>           a Secret; always empty, see the .cpp
inline constexpr std::string_view kDialPrefix = "web_search.dial.";
inline constexpr std::string_view kKeyPrefix  = "web_search.key.";

// The pane for `c`. Cursor on the first row (the on/off switch). `key_origin`
// may be empty, in which case every key row reads "not set".
[[nodiscard]] form::Form build_form(const web_search_cfg::Config& c,
                                    const KeyOrigin& key_origin = {});

// Write every unlocked row back into `c`. Locked rows (an env override) are
// skipped, so an export is never overwritten by what the pane displayed.
void apply_form(const form::Form& f, web_search_cfg::Config& c);

// The service a key row belongs to ("" if `row_id` is not a key row).
[[nodiscard]] std::string_view key_row_service(std::string_view row_id) noexcept;
// The service a "Remove key" action row belongs to ("" if it is not one).
[[nodiscard]] std::string_view key_clear_service(std::string_view row_id) noexcept;


} // namespace agentty::web_search_form
