#pragma once
// moha::app::cmd — factories for the side-effecting commands the runtime issues.
//
// These wrap maya's Cmd<Msg> with moha-specific glue: kicking off a streaming
// turn, executing a tool, advancing pending tool execution after a turn ends.

#include <maya/maya.hpp>

#include "moha/model.hpp"
#include "moha/msg.hpp"

namespace moha::app::cmd {

[[nodiscard]] maya::Cmd<Msg> launch_stream(const Model& m);

[[nodiscard]] maya::Cmd<Msg> run_tool(ToolCallId id,
                                      ToolName tool_name,
                                      nlohmann::json args);

// Inspect the latest assistant turn and either fire off pending tool calls,
// request permission, or kick the follow-up stream once tool results are in.
// Mutates `m` (sets phase, may push a placeholder assistant message).
[[nodiscard]] maya::Cmd<Msg> kick_pending_tools(Model& m);

} // namespace moha::app::cmd
