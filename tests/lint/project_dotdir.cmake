# project_dotdir.cmake — the project `.agentty` has a KNOWN top level.
#
# The user root sorted itself into cache/ credentials/ logs/ threads/ because a
# flat directory has no answer to "where does this new file go" and files then
# land by coin-flip. The project root never got that treatment, and drifted to
# thirteen entries across four lifecycles -- config, accumulated data, derived
# indexes, and lockfiles -- all side by side.
#
# Two things a flat layout cannot express, both of which cost us:
#
#   * No correct .gitignore rule. `.agentty/` ignores the skills the team wants
#     committed; `!.agentty/skills/` plus a growing deny list is a rule nobody
#     maintains.
#   * No safe "reset the derived state". `rm -rf .agentty` takes hand-authored
#     skills and accumulated feedback along with the rebuildable index.
#
# Every one of those thirteen entries arrived legitimately, one commit at a
# time, which is exactly why a reviewer will not catch the fourteenth. This
# makes the top level a declared set: adding a name is a decision someone makes
# on purpose, with this list as the place to record which category it is.
#
# NOT a style check. The categories are the lifecycle axis from
# docs/design/dot-agentty.md, and the lifecycle is what decides whether a file
# is committed, deletable, or needs an override.
#
# Usage: cmake -DROOT=<repo root> -P project_dotdir.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "project_dotdir: -DROOT=<repo root> required")
endif()

set(_dotdir "${ROOT}/.agentty")
if(NOT IS_DIRECTORY "${_dotdir}")
    message(STATUS "project dotdir: none here, nothing to check")
    return()
endif()

# CONFIG -- hand-authored, committed, the team's.
set(_config
    settings.json mcp.json hooks.json
    skills agents commands knowledge)

# LOCAL CONFIG -- hand-authored, gitignored, yours.
set(_local settings.local.json)

# DATA -- accumulated from use, NOT regenerable. Deleting loses something.
set(_data memory.jsonl rag_feedback.tsv routing_memory.tsv state)

# DERIVED / RUNTIME -- rebuildable or process-scoped. Safe to delete.
# Matched by pattern, since the embedder tag and lock suffixes vary.
set(_derived_globs
    "*.ragdb" "*.ragdb.meta.json" "*.lock" "cache")

set(_known ${_config} ${_local} ${_data})

file(GLOB _entries RELATIVE "${_dotdir}" "${_dotdir}/*")
set(_unknown "")
foreach(_e IN LISTS _entries)
    if(_e IN_LIST _known)
        continue()
    endif()
    set(_matched FALSE)
    foreach(_g IN LISTS _derived_globs)
        # Translate the glob to a regex FIRST. Matching the raw glob would try
        # to compile `^*.ragdb$`, where the leading `*` has no operand.
        string(REPLACE "." "\\." _re "${_g}")
        string(REPLACE "*" ".*" _re "${_re}")
        if(_e MATCHES "^${_re}$")
            set(_matched TRUE)
            break()
        endif()
    endforeach()
    if(NOT _matched)
        list(APPEND _unknown "    .agentty/${_e}")
    endif()
endforeach()

if(_unknown)
    string(REPLACE ";" "\n" _report "${_unknown}")
    message(FATAL_ERROR
        "unrecognised entries in the project .agentty:\n\n${_report}\n\n"
        "Every file here has a LIFECYCLE, and it decides whether the file is\n"
        "committed, whether it is safe to delete, and whether it needs a\n"
        "relocation override. Pick one and add the name to the matching list\n"
        "in tests/lint/project_dotdir.cmake:\n\n"
        "  config   hand-authored, committed        (skills/, mcp.json)\n"
        "  local    hand-authored, gitignored       (settings.local.json)\n"
        "  data     accumulated, NOT regenerable    (memory.jsonl)\n"
        "  derived  rebuildable, safe to delete     (*.ragdb, cache/)\n\n"
        "See docs/design/dot-agentty.md for the decision procedure.")
endif()

list(LENGTH _entries _n)
message(STATUS "project dotdir: ${_n} entr(ies), all categorised")
