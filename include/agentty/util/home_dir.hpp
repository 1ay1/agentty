#pragma once
// agentty::util::home_dir — ONE resolution of the user's home directory,
// shared by every path root (data_dir, config_dir, ~ expansion in tool
// arguments). Before this existed the two roots disagreed under MSYS2/mintty:
// persistence.cpp preferred $USERPROFILE (the native-Windows value, e.g.
// C:\Users\me) while auth.cpp preferred $HOME (the MSYS value, e.g.
// /home/me → C:\msys64\home\me). Under mintty BOTH are set, so credentials
// landed under one root and threads/settings under the other — a silent
// split-brain where a fresh `agentty login` under one shell wouldn't be seen
// by a run under the other.
//
// Precedence (highest first), chosen so a single agentty install behaves the
// same across cmd.exe, PowerShell, and mintty on one machine:
//   1. $HOME            — set by MSYS2/mintty/Cygwin and by every POSIX shell;
//                         honouring it FIRST keeps a `~` the user typed in a
//                         mintty prompt pointing at the same place agentty
//                         writes, and keeps Linux/macOS behaviour unchanged
//                         (there $USERPROFILE is unset anyway).
//   2. $USERPROFILE     — native-Windows home (cmd.exe / PowerShell, where
//                         $HOME is usually unset).
//   3. current_path()   — last-ditch fallback so we never throw.
//
// Returning the SAME directory from every caller is the whole point; do not
// reintroduce a second precedence order in any path root.
//
// ── WHICH `util` IS THIS? ──────────────────────────────────────────
// This is agentty::util — APP-WIDE PLUMBING with no notion of a tool call:
// logging, home/root resolution, base64, teardown ordering, the self-updater,
// the models.dev cache. Callable from anywhere.
//
// The other one is agentty::tools::util (include/agentty/tool/util/), the TOOL
// BOUNDARY: workspace clamping, subprocess spawning, the sandbox, the trust
// handoff gate, argument parsing. Its unifying property is UNTRUSTED INPUT —
// it is the set of walls between a model's request and the machine.
//
// The dependency runs ONE WAY: tools::util may use util (fs_helpers calls
// home_dir() below), never the reverse. Verified: nothing under src/util/ or
// include/agentty/util/ mentions tools::util, and nothing here should start,
// because a wall that depends on the thing it is protecting is not a wall.
// The fuller version of this note lives in tool/util/fs_helpers.hpp.

#include <filesystem>

namespace agentty::util {

// The user's home directory (see header comment for precedence). Never throws;
// falls back to the current working directory if nothing is set.
std::filesystem::path home_dir();

// Same $HOME → $USERPROFILE precedence, but returns an EMPTY path (not the
// cwd) when neither is set. This is the contract for callers that treat "no
// home" as "skip this optional feature" — e.g. loading ~/.agentty/hooks.json
// or writing a user-level plugin config: falling back to the cwd there would
// scatter dotfiles into whatever directory agentty happened to launch from.
// Use home_dir() when you need a guaranteed-usable path; use this when empty
// is a meaningful "unavailable" signal.
std::filesystem::path home_dir_or_empty();

} // namespace agentty::util
