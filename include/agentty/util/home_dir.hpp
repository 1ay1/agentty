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

#include <filesystem>

namespace agentty::util {

// The user's home directory (see header comment for precedence). Never throws;
// falls back to the current working directory if nothing is set.
std::filesystem::path home_dir();

} // namespace agentty::util
