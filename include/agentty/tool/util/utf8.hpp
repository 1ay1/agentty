#pragma once
// UTF-8 validation + repair. Subprocess output on Windows is whatever code
// page cmd.exe/PowerShell picked (usually OEM/CP1252), but nlohmann::json
// throws type_error.316 on any non-UTF-8 byte. Every string we hand the API
// must pass through to_valid_utf8 at the capture boundary — and ideally
// once more at serialization (belt-and-suspenders for paths that bypass us).

#include <string>
#include <string_view>

namespace agentty::tools::util {

// RFC 3629 scan. True iff every byte sequence in `s` decodes to a valid
// code point (no overlong, no surrogates, no > U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view s) noexcept;

// Replace every invalid byte sequence in `s` with U+FFFD (0xEF 0xBF 0xBD).
// Valid bytes pass through unchanged.
[[nodiscard]] std::string sanitize_utf8(std::string_view s);

// Return a copy of `s` guaranteed to be valid UTF-8. Try in order: already
// valid → transcode from GetConsoleOutputCP() → transcode from CP_ACP →
// byte-level scrub. On non-Windows only the first and last steps apply.
[[nodiscard]] std::string to_valid_utf8(std::string s);

// Return the largest position `cut` in [0, max_bytes] such that `s[0, cut)`
// does NOT split a multi-byte UTF-8 sequence. When `s.size() <= max_bytes`
// returns `s.size()` (no truncation). Never throws, never allocates. Used
// at every byte-granularity truncation site (size caps on file reads,
// HTTP bodies, captured stdout, thread titles, tool output limits) so
// the resulting prefix is still safe to hand to nlohmann::json::dump()
// without triggering its UTF-8 type_error.316.
[[nodiscard]] std::size_t safe_utf8_cut(std::string_view s, std::size_t max_bytes) noexcept;

// Apply terminal line-discipline to captured subprocess output so raw
// control bytes can never reach a UI cell grid or the model:
//   • ESC sequences are consumed whole — CSI through its final byte
//     (0x40–0x7E), OSC/DCS/SOS/PM/APC through BEL or ST, two-byte ESC+X
//     pairs. A sequence left INCOMPLETE at the end of the buffer (a
//     progress snapshot can cut mid-CSI) is dropped, not passed through
//     as literal parameter bytes.
//   • lone \r applies OVERWRITE semantics (progress bars collapse to
//     their final state); \r\n normalises to \n.
//   • \b erases the previous codepoint on the current line.
//   • every other C0 byte and DEL is dropped; \n and \t are kept.
// Byte-transparent outside control sequences; pair with to_valid_utf8
// for the encoding guarantee. Mirrors mcp-cpp's implementation.
[[nodiscard]] std::string strip_terminal_controls(std::string_view in);

} // namespace agentty::tools::util
