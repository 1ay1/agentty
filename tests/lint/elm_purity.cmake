# tests/lint/elm_purity.cmake — update() is a pure function of (Model, Msg).
#
# THE CONTRACT
#
# agentty is an Elm program on jaal. A reducer — anything under
# src/runtime/app/update/ plus update.cpp — reads and writes the Model and
# RETURNS a Cmd. It does not reach the world itself. Every effect is something
# the reducer describes and the runtime performs:
#
#   need                         say it with
#   ───────────────────────────  ────────────────────────────────────────────
#   do IO / network / a dial     return a cmd:: effect (cmd_factory.cpp)
#   know the time                read the Model (stamped by the runtime)
#   know an env var              read the Model (captured once at init)
#   change process-global state  return a cmd:: effect that changes it
#   remember something           put it in the Model, never a static
#
# Without that, replay re-runs effects, two app instances in one process
# share state they shouldn't, and tests have to stub the world instead of
# feeding Msgs. "Pure" also is not a style preference here: a reducer that
# dials, reads disk or blocks stalls the UI thread for the duration.
#
# HOW IT IS ENFORCED
#
# Same shape as jaal's concurrency banlist, deliberately: a list of named
# patterns, a per-file allowlist that says which names a file may still use,
# and a tight-check (elm_purity_tight) that fails when an allowlisted name is
# no longer used. So the allowlist is the REMAINING DEBT, written down. It can
# only shrink: a new violation fails the build, and fixing an old one fails
# the build until its exemption is deleted.
#
# The patterns are names, not semantics. A text lint cannot prove purity; what
# it can do is make every known way of reaching the world a decision someone
# had to write down, which is the part that kept regressing.
#
# Run with: cmake -DROOT=<src/runtime/app> -DALLOW=<allowlist> [-DTIGHT=1]
#                 -P elm_purity.cmake

if(NOT DEFINED ROOT OR NOT DEFINED ALLOW)
    message(FATAL_ERROR "ROOT and ALLOW are required")
endif()

# name → regex. Each one is a way a reducer reaches outside the Model.
set(ban_clock       "(steady_clock|system_clock|high_resolution_clock)::now[ \t]*\\(")
set(ban_env         "(std::)?(getenv|setenv|unsetenv|secure_getenv)[ \t]*\\(")
set(ban_fileio      "std::(if|of|f)stream|fs::(remove|rename|create_director|copy|exists|is_regular_file|file_size|last_write_time|directory_iterator)|std::filesystem::")
set(ban_subprocess  "run_command|::system[ \t]*\\(|popen[ \t]*\\(")
set(ban_net         "prewarm_active_provider|http::default_client|dial_new")
set(ban_global      "provider::select[ \t]*\\(|tools::(subagent::set_|skills::(reset_activations|note_activated)|invalidate_mcp_catalog|plugin::(set_|remove_|approve_|add_|update_)|util::allow_read_root)")
set(ban_auth        "auth::(load_credentials|oauth_proactive_refresh_token|clear_credentials|save_credentials|random_urlsafe)|credentials::(resolve|add_method)")
set(ban_static      "^[ \t]+static[ \t]+(std::|auto[ \t]|bool[ \t]|int[ \t]|long[ \t]|unsigned[ \t]|double[ \t]|float[ \t]|size_t[ \t])[^(]*[=;{][ \t]*$")
set(ban_thread      "std::j?thread([^_:]|$)|\\.detach\\(\\)|std::async[^_]")
set(ban_names clock env fileio subprocess net global auth static thread)

# Parse the allowlist: `path: name name ...`, `#` comments.
file(STRINGS ${ALLOW} allow_lines)
set(allowed_files "")
foreach(line IN LISTS allow_lines)
    string(REGEX REPLACE "#.*" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^([^:]+):(.*)$" _ "${line}")
    set(f "${CMAKE_MATCH_1}")
    string(STRIP "${CMAKE_MATCH_2}" names)
    string(REPLACE " " ";" names "${names}")
    string(MAKE_C_IDENTIFIER "${f}" key)
    set(allow_${key} "${names}")
    list(APPEND allowed_files "${f}")
endforeach()

# The reducer tree, and nothing else under ROOT: cmd_factory.cpp is WHERE
# effects are supposed to live, so scanning it would invert the rule.
file(GLOB_RECURSE files RELATIVE ${ROOT} ${ROOT}/update/*.cpp ${ROOT}/update/*.hpp)
list(APPEND files update.cpp)
find_program(GREP_EXE grep REQUIRED)

set(errors "")
foreach(f IN LISTS files)
    if(NOT EXISTS ${ROOT}/${f})
        continue()
    endif()
    string(MAKE_C_IDENTIFIER "${f}" key)
    set(allowed "${allow_${key}}")
    set(used_${key} "")
    foreach(name IN LISTS ban_names)
        execute_process(
            COMMAND ${GREP_EXE} -nE "${ban_${name}}" ${ROOT}/${f}
            OUTPUT_VARIABLE hits
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        # Same `;` guard as the banlist: CMake lists split on it.
        string(ASCII 1 _sc)
        string(REPLACE ";" "${_sc}" hits "${hits}")
        string(REGEX MATCHALL "[^\n]+" hit_lines "${hits}")
        foreach(hl IN LISTS hit_lines)
            string(REPLACE "${_sc}" ";" hl "${hl}")
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${hl}")
            set(lineno "${CMAKE_MATCH_1}")
            set(content "${CMAKE_MATCH_2}")
            if(content MATCHES "^[ \t]*//")
                continue()
            endif()
            string(REGEX REPLACE "//.*" "" code "${content}")
            if(NOT code MATCHES "${ban_${name}}")
                continue()
            endif()
            # `static constexpr` and `static const` are tables, not state.
            if(name STREQUAL "static" AND code MATCHES "static[ \t]+(constexpr|const)[ \t]")
                continue()
            endif()
            list(APPEND used_${key} "${name}")
            list(FIND allowed "${name}" idx)
            if(idx EQUAL -1)
                string(STRIP "${content}" content)
                list(APPEND errors "${f}:${lineno}: [${name}] ${content}")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH files nfiles)
if(nfiles EQUAL 0)
    message(FATAL_ERROR "elm_purity scanned 0 files under ROOT=${ROOT}: "
                        "nothing was checked, so this is a failure, not a pass.")
endif()

if(errors)
    list(JOIN errors "\n  " msg)
    message(FATAL_ERROR
        "a reducer reaches outside the Model:\n  ${msg}\n\n"
        "update() must be pure. Return a cmd:: effect, read the Model, or move "
        "the state into the Model. If this really must stay for now, add it "
        "to ${ALLOW} with the reason — that file is the debt list.")
endif()

# Tight mode: an exemption nobody uses any more is stale, and a stale entry
# is a hole a regression can walk straight through. Fail on it.
if(TIGHT)
    set(stale "")
    foreach(f IN LISTS allowed_files)
        string(MAKE_C_IDENTIFIER "${f}" key)
        if(NOT EXISTS ${ROOT}/${f})
            list(APPEND stale "${f}: file is gone, drop the entry")
            continue()
        endif()
        foreach(name IN LISTS allow_${key})
            list(FIND used_${key} "${name}" idx)
            if(idx EQUAL -1)
                list(APPEND stale "${f}: '${name}' is no longer used, drop it")
            endif()
        endforeach()
    endforeach()
    if(stale)
        list(JOIN stale "\n  " msg)
        message(FATAL_ERROR "elm_purity allowlist has stale grants:\n  ${msg}")
    endif()
endif()

message(STATUS "elm_purity ok (${nfiles} reducer files)")

# ── Model::now is the FOLD's time — stale anywhere a fold isn't running ─────
#
# The dispatch seam writes Model::now from jaal's step-time argument as the
# first thing each fold does, so inside update/ it is always current. Outside,
# it is the LAST fold's time: view() and subscribe() see a const Model that
# may be seconds old, and a wall_now() derived from it is just as stale. A
# reader there would get a plausible-looking wrong answer with no error.
# So the only readers allowed are the reducers and the seam that writes it.
if(DEFINED VIEW_ROOT)
    file(GLOB_RECURSE vfiles RELATIVE ${VIEW_ROOT} ${VIEW_ROOT}/*.cpp ${VIEW_ROOT}/*.hpp)
    set(stale_reads "")
    foreach(f IN LISTS vfiles)
        # The reducers and the two seam files are where `now` is current.
        if(f MATCHES "^runtime/app/update/" OR f STREQUAL "runtime/app/update.cpp"
           OR f STREQUAL "runtime/app/init.cpp")
            continue()
        endif()
        execute_process(
            COMMAND ${GREP_EXE} -nE "\\b(m|model|mdl)(\\.|->)(now\\b|wall_now\\()" ${VIEW_ROOT}/${f}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        string(REGEX MATCHALL "[^\n]+" hl "${hits}")
        foreach(h IN LISTS hl)
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${h}")
            if(CMAKE_MATCH_2 MATCHES "^[ \t]*//")
                continue()
            endif()
            string(STRIP "${CMAKE_MATCH_2}" c)
            list(APPEND stale_reads "${f}:${CMAKE_MATCH_1}: ${c}")
        endforeach()
    endforeach()
    if(stale_reads)
        list(JOIN stale_reads "\n  " msg)
        message(FATAL_ERROR
            "Model::now read outside a fold (it is stale there):\n  ${msg}\n\n"
            "A view or subscription that needs the current time must get it "
            "from the runtime (an animation clock, a Tick), not from the last "
            "fold's timestamp.")
    endif()
    message(STATUS "elm_purity: Model::now read only inside folds")
endif()
