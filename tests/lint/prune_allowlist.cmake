# prune_allowlist.cmake — report allowlist entries that no longer earn their
# keep, using the BANLIST'S OWN matching rules so the answer can't disagree
# with the check it feeds.
#
# Two kinds of rot, both invisible to the banlist itself (it walks the files
# on disk and only ever LOOKS UP the allowlist, so a stale grant is silently
# ignored rather than reported):
#
#   dead   — the entry names a file that no longer exists.
#   stale  — the file exists but no longer uses a primitive it is allowed.
#
# Either way the effect is the same: a hand-checked exemption outliving the
# code that justified it, so the next person to add a std::mutex there
# inherits a pass nobody granted them.
#
# Usage: cmake -DROOT=<src dir> -DALLOW=<allowlist> -P prune_allowlist.cmake

if(NOT DEFINED ROOT OR NOT DEFINED ALLOW)
    message(FATAL_ERROR "usage: -DROOT=<dir> -DALLOW=<file> -P prune_allowlist.cmake")
endif()
# Same trap the banlist has: ROOT is a glob prefix, so a RELATIVE path resolves
# against the cwd (for `cmake -P`, wherever it was invoked, not the source
# dir), silently matches zero files, and every entry then looks dead. Insist on
# absolute so the answer can't be quietly wrong.
if(NOT IS_ABSOLUTE "${ROOT}")
    message(FATAL_ERROR "ROOT must be an ABSOLUTE path, got: ${ROOT}")
endif()
if(NOT IS_DIRECTORY "${ROOT}")
    message(FATAL_ERROR "ROOT is not a directory: ${ROOT}")
endif()

# Verbatim from jaal's banlist.cmake — keep in sync if that file changes.
set(ban_thread       "std::j?thread([^_:]|$)")
set(ban_detach       "\\.detach\\(\\)")
set(ban_async        "std::async[^_]")
set(ban_mutex        "std::(shared_|recursive_|timed_)?mutex[^_]")
set(ban_atomic       "std::atomic[^_]")
set(ban_thread_local "thread_local")
set(ban_const_cast   "const_cast")
set(ban_sink_access  "sink_access")
set(ban_loop_key     "loop_key")

file(STRINGS ${ALLOW} allow_lines)
set(dead "")
set(stale "")

foreach(line IN LISTS allow_lines)
    # Drop comments and skip anything left blank. Two subtleties:
    #
    # 1. file(STRINGS) strips non-ASCII, so the section header
    #    `# ── HTTP: the turn's cancel token ──` arrives as ` HTTP: the turn's
    #    cancel token ` -- the '#' and the box-drawing rule are BOTH gone. So
    #    a leading-'#' test cannot catch it, and it otherwise parses as a
    #    `path: names` entry.
    # 2. Hence the real filter: an entry's left side must look like a source
    #    path (no spaces, ends in .cpp/.hpp). Prose never does.
    if(line MATCHES "^[ \t]*#")
        continue()
    endif()
    string(REGEX REPLACE "#.*" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    if(NOT line MATCHES "^[^ \t]+\\.(cpp|hpp):")
        continue()
    endif()
    string(REGEX MATCH "^([^:]+):(.*)$" _ "${line}")
    set(f "${CMAKE_MATCH_1}")
    string(STRIP "${CMAKE_MATCH_2}" names)
    string(REPLACE " " ";" names "${names}")

    if(NOT EXISTS "${ROOT}/${f}")
        list(APPEND dead "${f}")
        continue()
    endif()

    # Ask grep whether each allowed name appears in this file. Same reason
    # the banlist does: CMake's file(STRINGS) collapses `;`-ending lines into
    # merged blobs and the greedy `//.*` strip below then hides half the
    # file, silently marking real uses as "unused". grep is line-oriented so
    # it can't lose data at a `;`. Kept in sync with the banlist's approach.
    find_program(GREP_EXE grep REQUIRED)
    set(unused "")
    foreach(name IN LISTS names)
        execute_process(
            COMMAND ${GREP_EXE} -E "${ban_${name}}" "${ROOT}/${f}"
            OUTPUT_VARIABLE hits
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE grep_rc
            ERROR_QUIET)
        set(found FALSE)
        if(hits)
            # Filter comment-only lines the same way the banlist does; a
            # match that's ONLY in prose (e.g. "// once used std::mutex")
            # doesn't justify keeping the grant.
            string(ASCII 1 _sc)
            string(REPLACE ";" "${_sc}" hits "${hits}")
            string(REGEX MATCHALL "[^\n]+" hit_lines "${hits}")
            foreach(hl IN LISTS hit_lines)
                string(REPLACE "${_sc}" ";" hl "${hl}")
                if(hl MATCHES "^[ \t]*//")
                    continue()
                endif()
                string(REGEX REPLACE "//.*" "" code "${hl}")
                if(code MATCHES "${ban_${name}}")
                    set(found TRUE)
                    break()
                endif()
            endforeach()
        endif()
        if(NOT found)
            list(APPEND unused "${name}")
        endif()
    endforeach()
    if(NOT unused STREQUAL "")
        string(REPLACE ";" " " unused "${unused}")
        list(APPEND stale "${f}: ${unused}")
    endif()
endforeach()

if(dead STREQUAL "" AND stale STREQUAL "")
    message(STATUS "allowlist is tight: every entry still earns its keep")
    return()
endif()

foreach(d IN LISTS dead)
    message(STATUS "DEAD  ${d}")
endforeach()
foreach(s IN LISTS stale)
    message(STATUS "STALE ${s}")
endforeach()
list(LENGTH dead nd)
list(LENGTH stale ns)
message(FATAL_ERROR "allowlist has ${nd} dead entr(ies) and ${ns} stale grant(s)")
