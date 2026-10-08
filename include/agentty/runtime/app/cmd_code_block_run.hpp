#pragma once
// cmd::run_code_block — the effect the Ctrl+G code-block reducer returns.
//
// Runs a fenced block from the latest assistant reply: interactively on the
// real terminal with a tee capture (POSIX), or through the captured
// subprocess runner (Windows). Delivers CodeBlockRunFinished when it ends.
// Implemented in cmd_code_block_run.cpp; the reducer never touches a tty,
// a pid or a clock.

#include <string>

#include "agentty/runtime/cmd.hpp"
#include "agentty/runtime/panel/code_blocks.hpp"

namespace agentty::app::cmd {

[[nodiscard]] Cmd run_code_block(std::string command,
                                 code_blocks::BlockShell shell);

}  // namespace agentty::app::cmd
