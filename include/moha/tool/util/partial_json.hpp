#pragma once
// Close a possibly-truncated JSON buffer into a parseable string.
//
// The Anthropic SSE stream delivers tool-use input as `input_json_delta`
// fragments. Until the tool_use block closes, the buffer is incomplete —
// open strings, unbalanced braces/brackets, dangling `key:` pairs after a
// colon, and half-escape sequences (`\`) at the tail.
//
// `close_partial_json(raw)` walks the buffer once and emits a string that
// nlohmann::json can parse. Semantics:
//   • open string  → close with `"` (half-escapes trimmed)
//   • `"key":` with no value → append `null`
//   • trailing `,` inside obj/array → strip
//   • unclosed `{` / `[` → append `}` / `]`
//
// Produced text is *minimally* mutated — the parser accepts it; inner
// structure is untouched. Safe to call on empty / malformed input (returns
// `null` or best-effort closure).
//
// This is the C++ equivalent of Zed's `partial-json-fixer` crate usage.

#include <string>
#include <string_view>

namespace moha::tools::util {

std::string close_partial_json(std::string_view raw);

} // namespace moha::tools::util
