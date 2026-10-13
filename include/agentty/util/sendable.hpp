#pragma once
// agentty/util/sendable.hpp — types jaal can't see inside, marked Sendable.
//
// maya::guarded<T> and task arguments must be Sendable: safe to move to
// another thread. jaal proves that by walking a plain struct's fields; a
// class with private members or constructors is opaque to it and refused
// unless someone checks it by hand and opts it in here. Each opt-in below
// is that check.
//
// Include this (not jaal) wherever a guarded value or a task argument holds
// one of these types.

#include <filesystem>

#include <maya/runtime.hpp>

#include <nlohmann/json.hpp>

#include "agentty/domain/id.hpp"
#include "agentty/tool/effects.hpp"

// nlohmann::json owns its whole tree by value (a variant over string, array,
// object, number, bool, null — every branch an owning container). jaal can't
// walk it because the payload is behind a private union, not because there
// is anything borrowed in there. Tool arguments and results are json, so
// they cross to worker threads constantly.
//
// Sendable, not Frozen: a json is freely mutable through a non-const
// reference, and nothing here pretends otherwise.
MAYA_SENDABLE(nlohmann::json);

// Id<Tag> is a strong newtype around ONE std::string, by value, no views and
// no pointers. jaal can't look inside only because it has user-declared
// constructors. So it is Sendable, and Frozen too: nothing reachable
// through a const Id can change.
template <class Tag>
MAYA_SENDABLE_T(agentty::Id<Tag>);
template <class Tag>
MAYA_FROZEN_T(agentty::Id<Tag>);

// std::filesystem::path owns one string (its native form); a moved path
// shares nothing with the source.
MAYA_SENDABLE(std::filesystem::path);

// EffectSet is one std::uint8_t of flags behind accessors.
MAYA_SENDABLE(agentty::tools::EffectSet);
