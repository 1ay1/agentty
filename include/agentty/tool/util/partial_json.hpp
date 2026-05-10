#pragma once
// Partial-JSON helpers for the Anthropic `input_json_delta` hot path.
//
// The SSE stream delivers tool args as fragments that form incomplete JSON
// until the tool_use block closes. Two utilities cover the common needs:
//
//   close_partial_json(raw)
//     Walks the buffer and emits a string nlohmann::json can parse. Closes
//     open strings, fills `"key":` with `null`, strips trailing `,`, and
//     closes unbalanced `{` / `[`. C++ equivalent of Zed's
//     `partial-json-fixer` crate.
//
//   sniff_string(raw, key)
//     Hand-walks the buffer for `"key": "<value>"`. Returns the decoded
//     value only once the closing quote has been seen; std::nullopt until
//     then. Useful when the full close+parse is too heavy to run per tick
//     and you only need one scalar.
//
//   sniff_string_progressive(raw, key)
//     Same as sniff_string but returns whatever bytes have accumulated so
//     far, stopping at a half-escape on the buffer edge. Needed for fields
//     whose value dwarfs every other arg (write's `content`, edit's
//     `old_string` / `new_string`) so the UI doesn't wait for the closing
//     quote on an 800-line file to show anything.
//
//   locate_string_value(raw, key)
//     Returns the byte offset of the first char *inside* the value string
//     for `"key":"...`, or nullopt if the prefix isn't complete yet.
//     Used to cache the location once so per-tick previews resume from
//     there instead of re-scanning the buffer from byte 0 every time.
//
//   decode_string_from(raw, offset)
//     Decodes JSON-escaped bytes from `offset` onwards until the closing
//     `"` or end-of-buffer. Mirrors sniff_string_progressive's tail but
//     skips the prefix-walk so the caller can reuse a cached offset.
//
// All three are safe on empty / malformed input.

#include <optional>
#include <string>
#include <string_view>

namespace agentty::tools::util {

std::string close_partial_json(std::string_view raw);

[[nodiscard]] std::optional<std::string>
sniff_string(std::string_view raw, std::string_view key);

[[nodiscard]] std::optional<std::string>
sniff_string_progressive(std::string_view raw, std::string_view key);

// Returns the index of the first byte *inside* the JSON string value
// corresponding to `"key":"...`. nullopt until `"key":"` is fully
// present in the buffer. Append-only streams can cache the result.
[[nodiscard]] std::optional<std::size_t>
locate_string_value(std::string_view raw, std::string_view key);

// Decode JSON-escaped bytes from `offset` to the closing `"` or end
// of buffer. Mirrors sniff_string_progressive's decode tail. `offset`
// must be <= raw.size(); typically the value returned by an earlier
// locate_string_value on a prefix of the same buffer.
[[nodiscard]] std::string
decode_string_from(std::string_view raw, std::size_t offset);

} // namespace agentty::tools::util
