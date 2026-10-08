#pragma once
// agentty::app::read_launch_env — the process environment, as a value.
//
// Called once, by init(). Reducers never call it: they read Model::env.

#include "agentty/runtime/model.hpp"

namespace agentty::app {

[[nodiscard]] Model::Env read_launch_env() noexcept;

}  // namespace agentty::app
