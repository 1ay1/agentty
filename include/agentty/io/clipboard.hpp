#pragma once
// The system clipboard, through richpaste (third_party/richpaste).
//
// Reading: the local display (Wayland data-control, X11, macOS pasteboard,
// Win32), else the richpaste agent on the user's laptop over an ssh forward
// (`agentty paste setup` once there), else the terminal itself (OSC 5522 /
// OSC 52, which the composer drives through maya). A `richpaste:1:` line
// copied as text decodes back into the image or file on every route.
//
// Writing: native tools (pbcopy / wl-copy / xclip / clip.exe), since OSC 52
// writes are off by default in Terminal.app and iTerm2.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/paste_config.hpp"

namespace agentty {

struct ClipboardItem {
    enum class Kind : std::uint8_t { Text, Image, File };
    Kind        kind = Kind::Text;
    std::string mime;   // "image/png", "application/pdf", ...
    std::string name;   // file name for File, else empty
    std::string bytes;  // UTF-8 for Text, raw bytes otherwise
    bool        saved_path = false;  // Text that is the path of a saved file
};

struct ClipboardContents {
    std::vector<ClipboardItem> items;  // copied files, else an image, else text
    std::string source;                // "wayland wayland-1", "agent via ssh", ...
    std::string error;                 // set when items is empty
};

// Installs the paste settings (Settings → Paste / settings.json /
// AGENTTY_PASTE_*). Call at startup and whenever they change; a paste that is
// already running keeps what it read.
void clipboard_configure(const paste_cfg::Config& c);
[[nodiscard]] paste_cfg::Config clipboard_config();
// Where pasted images and files are saved right now.
[[nodiscard]] std::filesystem::path clipboard_save_dir();

// Reads the clipboard. Blocking (a socket round trip, maybe over ssh), so
// call it from a worker. `allow_remote` false skips the ssh agent route.
[[nodiscard]] ClipboardContents read_clipboard(bool allow_remote = true);

// Pulls `richpaste:1:` armored blocks out of pasted text. Returns the items
// in order, with plain text between blocks as Text items. Empty when the
// text has no armored block.
[[nodiscard]] std::vector<ClipboardItem> decode_armored(std::string_view text,
                                                        std::string* error = nullptr);
[[nodiscard]] bool has_armored(std::string_view text) noexcept;
// A terminal clipboard reply (OSC 5522) that reached the input as plain text
// because something stripped its ESC bytes. Never paste that as text:
// salvage_clipboard_reply() returns the clipboard bytes it carried (image
// bytes, or text, possibly itself a richpaste:1: line), or "" if it can't.
[[nodiscard]] bool looks_like_clipboard_reply(std::string_view text) noexcept;
[[nodiscard]] std::string salvage_clipboard_reply(std::string_view text);
// True once `text` holds at least as many bytes as its first armored block
// says it carries (base64 is 4/3 of that). Cheap; lets a paste that arrives in
// pieces skip decoding until it can succeed.
[[nodiscard]] bool armored_complete_enough(std::string_view text) noexcept;

// Turns clipboard items into what the composer can hold: images and text
// pass through, a text file becomes Text, any other file is saved under
// ~/.agentty/paste/pasted and becomes a Text item with its path. Does disk IO,
// so call it from a worker.
void settle_clipboard_items(std::vector<ClipboardItem>& items);

/// Write plain UTF-8 text to the system clipboard with the platform tool.
/// Callers also send OSC 52, which still carries the copy across ssh/tmux.
/// On failure writes a one-line reason to `*error_out`.
bool write_clipboard_text(std::string_view text, std::string* error_out = nullptr);

// `agentty paste …`: richpaste's command line. Its defaults are agentty's
// (re-runs `<this exe> paste agent|ssh-hook`, state in ~/.agentty/paste).
int  clipboard_cli(std::span<const std::string> args);

// The `RemoteForward` pair (remote socket, local endpoint) for an ssh
// session, starting the local agent if needed. Empty pair on failure, with
// the reason in `*error`. Used by `agentty airgap --clipboard-relay`.
struct ClipboardForward { std::string remote, local; };
[[nodiscard]] ClipboardForward clipboard_ssh_forward(std::string* error = nullptr);

} // namespace agentty
