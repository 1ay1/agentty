#pragma once
// Read an image from the system clipboard via platform-native tooling.
//
// Bracketed paste delivers UTF-8 text only; binary clipboard content
// (a PNG that a screenshot tool put on the clipboard) is dropped or
// mangled by every mainstream terminal. The reliable path is to ASK
// the platform clipboard for its image content out-of-band, via a
// small subprocess:
//
//   Linux/Wayland → wl-paste --type image/png
//   Linux/X11     → xclip -selection clipboard -t image/png -o
//   macOS         → pngpaste -            (brew install pngpaste)
//   Windows       → powershell System.Windows.Forms.Clipboard.GetImage
//
// All capture stdout as raw bytes — `tools::util::Subprocess` runs its
// captured output through to_valid_utf8(), which would replace every
// non-UTF-8 byte with U+FFFD and corrupt the image. So this module
// has its own popen-based capture that doesn't touch the bytes.
//
// The reducer arm in update/composer.cpp calls read_clipboard_image()
// synchronously on Ctrl+V. The subprocesses all exit immediately when
// no image is available (non-zero status, empty stdout) so the worst
// case is the shell-spawn cost (~10-50 ms on Linux, ~100-200 ms on
// macOS via osascript). Acceptable on the UI thread for an explicit
// user action.

#include <optional>
#include <string>

namespace moha {

struct ClipboardImage {
    /// Raw image bytes — PNG / JPEG / GIF / WEBP. Sniffed from the
    /// magic prefix of the captured stdout, so format mismatch
    /// between tool flags and actual content (rare, but possible
    /// under odd compositor configurations) is surfaced cleanly.
    std::string bytes;
    /// MIME type (e.g. "image/png") matching the magic prefix.
    std::string media_type;
};

/// Read an image from the system clipboard. On success returns the
/// raw bytes + sniffed MIME type. On failure (tool missing, clipboard
/// empty, no image type on clipboard, image too large), returns
/// nullopt and writes a one-line human-readable diagnostic to
/// `*error_out` so the caller can surface it as a status toast that
/// names the actual failure (the previous "no image on clipboard"
/// blanket message was unhelpful when the real fix was "install
/// wl-clipboard" or "the clipboard has only text").
[[nodiscard]] std::optional<ClipboardImage>
read_clipboard_image(std::string* error_out = nullptr);

} // namespace moha
