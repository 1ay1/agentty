#pragma once
// Small, pure helpers shared between the thread renderer and the tool-card
// renderer. Kept separate from helpers.hpp because these all operate on
// ToolUse/args shape — they don't belong with generic color/UTF-8 stuff.

#include <initializer_list>
#include <string>

#include <nlohmann/json_fwd.hpp>  // only const json& params below; full type lives in the .cpp

#include "agentty/domain/conversation.hpp"

namespace agentty::ui {

// Safe string read — empty when the key is missing or non-string.
[[nodiscard]] std::string safe_arg(const nlohmann::json& args, const char* key);

// Pick the first non-empty string under any of the listed keys. Mirrors
// the alias-tolerant parsing the tool implementations do (write/edit accept
// `path | file_path | filepath | filename`, `display_description | description`
// etc.). Without this the view reads one canonical key and a model that
// picks an alias renders as a blank card even though the tool ran fine.
[[nodiscard]] std::string pick_arg(const nlohmann::json& args,
                                   std::initializer_list<const char*> keys);

// The file path a tool is acting on, resolved to be visible AS EARLY AS
// possible. During streaming the parsed `tc.args` lags the wire (it's
// re-parsed from partial JSON on a ~120 ms throttle), so a card can show a
// bare "…" for the first fraction of a second of an edit/write even though
// the path bytes have already arrived. This first tries the parsed args
// (path | file_path | filepath | filename), then falls back to scraping the
// value straight out of tc.args_streaming so the filename appears the instant
// its bytes land. Empty only when no path has been streamed at all yet.
[[nodiscard]] std::string tool_path_arg(const ToolUse& tc);

// Scrape the first string value for `key` out of a (possibly partial) JSON
// document without parsing it — finds `"key"` then the next `: "..."`,
// honouring backslash escapes and stopping at the closing quote (or the end
// of a truncated buffer). Returns "" when the key or its value hasn't
// streamed yet. Cheap: a single left-to-right scan, no allocation until a
// value is found.
[[nodiscard]] std::string pick_streaming_string(std::string_view raw_json,
                                                std::string_view key);

// Int read with default when missing / wrong type.
[[nodiscard]] int safe_int_arg(const nlohmann::json& args, const char* key, int def);

// Newline count + 1 for trailing non-newline line. Zero for empty input.
[[nodiscard]] int count_lines(const std::string& s);

// Seconds spent on this tool call so far. Running → now - started;
// terminal → finished - started; returns 0 when started_at is unset.
// Called every Tick while a tool runs so the card updates live.
[[nodiscard]] float tool_elapsed(const ToolUse& tc);

// Strip the ```…``` fence and trailing metadata bash wraps its captured
// stdout/stderr in. The fence lets the model see an unambiguous "literal
// output" boundary but is visual noise inside the widget's own frame.
[[nodiscard]] std::string strip_bash_output_fence(const std::string& s);

// Pull the exit code out of bash/diagnostics tool output — recognizes
// both "failed with exit code N" (Zed-style) and "[exit code N]" (legacy).
// Returns 124 for a timeout marker, 0 when none of those patterns match.
[[nodiscard]] int parse_exit_code(const std::string& output);

} // namespace agentty::ui
