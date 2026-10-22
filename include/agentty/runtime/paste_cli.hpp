#pragma once
// `agentty paste …`: `config` is agentty's (the paste.* settings); every other
// subcommand is richpaste's command line, run with those settings installed.

#include <span>
#include <string>

#include <maya/runtime.hpp>   // maya::unlocked

#include "agentty/util/io.hpp"

namespace agentty::paste_cli {

int run(Io io, const maya::unlocked& cap, std::span<const std::string> args);

}  // namespace agentty::paste_cli
