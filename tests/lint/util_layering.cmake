# util_layering.cmake — agentty::util must not depend on agentty::tools::util.
#
# There are two `util` namespaces, and the split is the only thing that makes
# the name collision tolerable:
#
#   agentty::tools::util   the TOOL BOUNDARY. Walls and parsers standing
#                          between a model's request and the machine:
#                          workspace clamping, subprocess spawn, the sandbox,
#                          the trust handoff gate, argument reading.
#   agentty::util          APP-WIDE PLUMBING with no notion of a tool call:
#                          logging, home/root resolution, base64, teardown.
#
# The dependency must run ONE WAY. tools::util may use util (fs_helpers calls
# home_dir()); util must never reach back, because a wall that depends on the
# thing it is protecting is not a wall — and because the cycle is what would
# make the two directories genuinely indistinguishable rather than merely
# similarly named.
#
# Documented in both headers. Documentation decays, so this enforces it.
#
# Usage: cmake -DROOT=<repo root> -P util_layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "util_layering: -DROOT=<repo root> required")
endif()

set(_offenders "")

# Both halves of agentty::util: the sources and the headers.
file(GLOB_RECURSE _util_files
     "${ROOT}/src/util/*.cpp" "${ROOT}/src/util/*.hpp"
     "${ROOT}/include/agentty/util/*.hpp")

foreach(_f IN LISTS _util_files)
    file(STRINGS "${_f}" _hits REGEX "tools::util|agentty/tool/util/")
    if(_hits)
        file(RELATIVE_PATH _rel "${ROOT}" "${_f}")
        foreach(_h IN LISTS _hits)
            # Skip the explanatory notes in both headers, which name the other
            # namespace precisely in order to tell them apart.
            if(NOT _h MATCHES "^[ \t]*//")
                string(STRIP "${_h}" _h)
                list(APPEND _offenders "    ${_rel}: ${_h}")
            endif()
        endforeach()
    endif()
endforeach()

if(_offenders)
    string(REPLACE ";" "\n" _report "${_offenders}")
    message(FATAL_ERROR
        "agentty::util reached into agentty::tools::util:\n\n${_report}\n\n"
        "The dependency runs ONE WAY: tools::util may use util, never the\n"
        "reverse. If this helper genuinely needs the tool boundary, it belongs\n"
        "under src/tool/util/ instead. See the notes in\n"
        "include/agentty/util/home_dir.hpp and\n"
        "include/agentty/tool/util/fs_helpers.hpp.")
endif()

message(STATUS "util layering: agentty::util is free of tools::util")
