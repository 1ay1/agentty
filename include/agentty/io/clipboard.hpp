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
#include <memory>
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

// What the composer does when a clipboard read came back with nothing.
enum class ClipboardMiss : std::uint8_t {
    None,          // it didn't miss
    AskTerminal,   // ask the terminal itself, through our own input, then resume
    Empty,         // the clipboard is empty; final
    TooBig,        // over Settings → Paste → max size; final
    Refused,       // the terminal won't let us read its clipboard; final, `error` says the fix
    NoRoute,       // every route came up empty-handed; final, `error` says what to do
};

// Which terminal read to send for AskTerminal.
enum class TerminalAsk : std::uint8_t { Images, Text };

// A paste in progress (richpaste's route walk), as plain data so it can ride
// a Msg between the reducer and a worker.
struct ClipboardWalk {
    bool ssh = false, tmux = false;
    std::uint8_t images = 1;              // rp::Images
    std::vector<std::uint8_t> routes;     // rp::Route, in order
    std::size_t next = 0;
    struct Cause {
        std::uint8_t route = 0;           // rp::Route
        std::uint8_t code = 0;            // rp::Code
        std::string what;
    };
    std::vector<Cause> causes;
    [[nodiscard]] bool active() const noexcept { return next < routes.size(); }
};

struct ClipboardContents {
    std::vector<ClipboardItem> items;  // copied files, else an image, else text
    std::string source;                // "wayland wayland-1", "agent via ssh", ...
    ClipboardMiss miss = ClipboardMiss::None;
    std::string error;                 // set when items is empty
    TerminalAsk ask = TerminalAsk::Text;   // AskTerminal: which read to send
    ClipboardWalk walk;                    // AskTerminal: hand back to resume
};

// Installs the paste settings (Settings → Paste / settings.json /
// AGENTTY_PASTE_*). Call at startup and whenever they change; a paste that is
// already running keeps what it read.
void clipboard_configure(const paste_cfg::Config& c);
[[nodiscard]] paste_cfg::Config clipboard_config();
// Where pasted images and files are saved right now.
[[nodiscard]] std::filesystem::path clipboard_save_dir();

// Reads the clipboard, trying routes from the cheapest and most faithful
// down, none needing setup except the ssh agent. Blocking (a socket round
// trip, maybe over ssh, or tmux asking the terminal), so call it from a
// worker. When the next route is asking the terminal, it stops with
// AskTerminal: the app asks through its own input, and if that brings
// nothing calls resume_clipboard with the walk.
[[nodiscard]] ClipboardContents read_clipboard();
// The terminal didn't answer (or had nothing): note why and carry on.
// `refused`: it answered no (kitty's EPERM), so routes that ask it again are skipped.
[[nodiscard]] ClipboardContents resume_clipboard(ClipboardWalk walk, std::string why, bool refused);
// Only this machine's own clipboard (no ssh, no terminal).
[[nodiscard]] ClipboardContents read_local_clipboard();

// Pulls `richpaste:1:` armored blocks out of pasted text. Returns the items
// in order, with plain text between blocks as Text items. Empty when the
// text has no armored block.
[[nodiscard]] std::vector<ClipboardItem> decode_armored(std::string_view text,
                                                        std::string* error = nullptr);
[[nodiscard]] bool has_armored(std::string_view text) noexcept;
// True while the text's armored block is still arriving (a paste in pieces).
[[nodiscard]] bool armored_partial(std::string_view text) noexcept;
// Bytes of the arriving block received so far, and its full size (0 while
// its header is still on the way). Both 0 when nothing is arriving.
struct ArmoredProgress { std::uint64_t have = 0, need = 0; bool arriving = false; };
[[nodiscard]] ArmoredProgress armored_progress(std::string_view text) noexcept;

// A terminal clipboard reply (OSC 5522) that reached the input as plain text
// because something stripped its ESC bytes. Never paste that as text.
enum class LeakedReply : std::uint8_t { None, Partial, Complete };
[[nodiscard]] LeakedReply leaked_reply(std::string_view text) noexcept;
// The items a complete leaked reply carried (empty if it can't be read).
[[nodiscard]] std::vector<ClipboardItem> decode_leaked_reply(std::string_view text,
                                                             std::string* error = nullptr);

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
