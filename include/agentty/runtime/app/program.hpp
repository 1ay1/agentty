#pragma once
// agentty::app::Program — the maya runtime binding.
//
// Forwards to the per-domain reducer / view / subscribe.  The init function
// reads settings + recent threads through the Store seam.

#include <maya/maya.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/view.hpp"

namespace agentty::app {

[[nodiscard]] std::pair<Model, maya::Cmd<Msg>> init();

struct AgenttyApp {
    using Model = ::agentty::Model;
    using Msg   = ::agentty::Msg;

    static std::pair<Model, maya::Cmd<Msg>> init() { return ::agentty::app::init(); }

    static auto update(Model m, Msg msg) -> std::pair<Model, maya::Cmd<Msg>> {
        return ::agentty::app::update(std::move(m), std::move(msg));
    }

    static maya::Element view(const Model& m) {
        return ::agentty::ui::view(m);
    }

    static auto subscribe(const Model& m) -> maya::Sub<Msg> {
        return ::agentty::app::subscribe(m);
    }
};

static_assert(maya::Program<AgenttyApp>);

} // namespace agentty::app
