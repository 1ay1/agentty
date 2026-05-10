#pragma once
// agentty::app::subscribe — input → Msg routing.
//
// Pure function of Model: snapshots which modal (if any) owns the keyboard,
// then routes keys / paste / tick into the right Msg.

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app {

[[nodiscard]] maya::Sub<Msg> subscribe(const Model& m);

} // namespace agentty::app
