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

#include "agentty/tool/effects.hpp"

// std::filesystem::path owns one string (its native form); a moved path
// shares nothing with the source.
MAYA_SENDABLE(std::filesystem::path);

// EffectSet is one std::uint8_t of flags behind accessors.
MAYA_SENDABLE(agentty::tools::EffectSet);
