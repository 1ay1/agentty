#pragma once
// agentty::paste_cfg — how Ctrl+V and `agentty paste` read the clipboard.
//
// The reading itself is richpaste (third_party/richpaste). agentty owns the
// choices: which routes a paste may take, how big a paste may be, what a
// pasted file turns into. They are settings rows (paste.*), so they live in
// settings.json, show in Settings → Paste, take AGENTTY_PASTE_* overrides and
// work with `agentty config get|set`.
//
// Pure: no IO, no globals. io/clipboard.cpp turns a Config into richpaste's
// rp::Config.

#include <cstdint>
#include <string>

namespace agentty::paste_cfg {

// Largest paste accepted, in MB. Anthropic takes images up to 5 MB, but a
// copied file can be bigger and is saved, not sent.
inline constexpr int kMaxMbMin     = 1;
inline constexpr int kMaxMbMax     = 1024;
inline constexpr int kMaxMbDefault = 32;

// A pasted text file at most this big (KB) goes in as its text; bigger ones
// are saved and their path is typed in.
inline constexpr int kInlineKbMin     = 0;
inline constexpr int kInlineKbMax     = 4096;
inline constexpr int kInlineKbDefault = 256;

// paste.terminal: how agentty asks the terminal for the clipboard.
inline constexpr std::string_view kTerminalModes = "auto|osc5522|osc52|off";

struct Config {
    // Over ssh, read the laptop's clipboard through a forwarded agent
    // (`agentty paste setup` on the laptop).
    bool forwards = true;

    // Read this machine's own clipboard (Wayland, X11, macOS, Windows).
    bool display = true;

    // Over ssh, fall back to the SERVER's own clipboard. Off by default:
    // that is not the clipboard you copied into.
    bool server_display = false;

    // How to ask the terminal (OSC 5522 / OSC 52) when nothing else answered.
    // "auto" asks the terminal what it supports; "osc5522" / "osc52" force one
    // for terminals that misreport; "off" never asks.
    std::string terminal = "auto";

    // Turn `richpaste:1:` lines in pasted text back into the image or file.
    bool decode_text = true;

    // Largest paste accepted (MB).
    int max_mb = kMaxMbDefault;

    // A copied text file up to this size (KB) is pasted as its text. 0 = always save.
    int inline_kb = kInlineKbDefault;

    // Where pasted files are saved. Empty = <state dir>/pasted
    // ($AGENTTY_HOME/paste/pasted or ~/.agentty/paste/pasted).
    std::string save_dir;

    // Ask only this agent socket (a relay, a test). Empty = find them.
    std::string socket;

    bool operator==(const Config&) const = default;
};

} // namespace agentty::paste_cfg
