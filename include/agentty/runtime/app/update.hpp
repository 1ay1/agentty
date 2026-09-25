#pragma once
// agentty::app::update — the reducers, as jaal calls them.
//
// jaal dispatches by OVERLOAD, not by a visit we write: it walks the Msg tree
// itself and calls `update(Model&, T)` for whatever it lands on (D36). A
// domain declared with handled_as_group (runtime/cmd.hpp) is one of those
// landings, so there is one overload per domain — 23, not 231.
//
// The shape is jaal's: mutate the model in place, return only the effect.
//
//     Cmd update(Model& m, msg::ComposerMsg cm);
//
// The old shape returned `std::pair<Model, Cmd>` and moved the model through
// every reducer. That cost 172 `return {std::move(m), ...}` sites and, worse,
// allowed `return {std::move(m), f(m)}` — reading m in an argument next to a
// move of it, unspecified evaluation order. There was one of those in
// update/rag.cpp. This signature deletes the whole hazard.
//
// All side effects are returned as Cmds, never executed inline.

#include <utility>

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/cmd.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app {

// One per domain, in the order they appear in Msg. Each forwards to the
// domain's reducer in its own TU, which is what keeps a one-leaf edit to a
// ~1.2 s rebuild instead of ~19 s (jaal D36, measured on this app).
Cmd update(Model& m, msg::ComposerMsg     cm);
Cmd update(Model& m, msg::StreamMsg       sm);
Cmd update(Model& m, msg::ToolMsg         tm);
Cmd update(Model& m, msg::ToolOutputMsg   tm);
Cmd update(Model& m, msg::ProvidersMsg    pm);
Cmd update(Model& m, msg::ModelsMsg       mm);
Cmd update(Model& m, msg::ThreadListMsg   tm);
Cmd update(Model& m, msg::PaletteMsg      pm);
Cmd update(Model& m, msg::MentionMsg      mm);
Cmd update(Model& m, msg::SymbolMsg       sm);
Cmd update(Model& m, msg::CodeBlockMsg    cm);
Cmd update(Model& m, msg::CheckpointMsg   cm);
Cmd update(Model& m, msg::RagMsg          rm);
Cmd update(Model& m, msg::StatsMsg        sm);
Cmd update(Model& m, msg::SettingsListMsg sm);
Cmd update(Model& m, msg::ForkMsg         fm);
Cmd update(Model& m, msg::TodoMsg         tm);
Cmd update(Model& m, msg::LoginMsg        lm);
Cmd update(Model& m, msg::DiffReviewMsg   dm);
Cmd update(Model& m, msg::SmartModeMsg    sm);
Cmd update(Model& m, msg::PluginEditMsg   pm);
Cmd update(Model& m, msg::AppearanceMsg   am);
Cmd update(Model& m, msg::MetaMsg         mm);

// The old entry point, still here while the domain reducers keep their
// `Step`-returning bodies. The overloads above are thin wrappers over it;
// once every reducer takes a Model& this goes away.
[[nodiscard]] std::pair<Model, Cmd> update(Model m, Msg msg);

} // namespace agentty::app
