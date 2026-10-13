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
# The theme detectors read TERM/COLORTERM/COLORFGBG themselves, and the
# settings helpers read AGENTTY_* vars, so they are env reads too: use
# Model::env.terminal / Model::env.settings.
set(ban_env         "(std::)?(getenv|setenv|unsetenv|secure_getenv)[ \t]*\\(|detect_(tier|polarity)[ \t]*\\(|ui_prefs::detect[ \t]*\\(|terminal_is_dumb[ \t]*\\(")
# fileio: an operation that touches the disk. NOT the bare `std::filesystem::`
# prefix: `std::filesystem::path` is a value type (string manipulation, no
# syscall), and flagging it reported a reducer composing a path as IO. Only
# the functions that actually read or write the filesystem count.
set(ban_fileio      "std::(if|of|f)stream|(fs|std::filesystem)::(remove|rename|create_director|copy|exists|is_regular_file|is_directory|file_size|last_write_time|directory_iterator|recursive_directory_iterator|status|read_symlink|canonical|weakly_canonical|temp_directory_path|current_path|space|equivalent)\\b")
set(ban_subprocess  "run_command|::system[ \t]*\\(|popen[ \t]*\\(")
# Not listed: the network, credentials, accounts, persistence, provider and
# subagent setters, plugin edits, the skills/commands/hooks lookups, git
# probing and the env helpers. Those take an agentty::Io, which a reducer
# never has, so the compiler rejects the call (util/io.hpp). What stays here
# is the raw std/OS calls a token can't be attached to.
set(ban_static      "^[ \t]+static[ \t]+(std::|auto[ \t]|bool[ \t]|int[ \t]|long[ \t]|unsigned[ \t]|double[ \t]|float[ \t]|size_t[ \t])[^(]*[=;{][ \t]*$")
set(ban_thread      "std::j?thread([^_:]|$)|\\.detach\\(\\)|std::async[^_]")
# tty: asking the terminal directly. The size arrives as a Msg
# (TerminalResized, from maya's on_resize) and lives in Model::ui.
set(ban_tty         "platform::(query_terminal_size|stdout_handle|stdin_handle)|isatty[ \t]*\\(|ioctl[ \t]*\\(")
# disk_lookup: helpers that walk the disk or a secure store to answer a
# lookup. Reducers read the answer from the Model instead: m.ui.library
# (skills, approvals, commands, hooks), m.ui.git_repo, m.env (embed
# defaults, user root). A worker that needs them gets them in a cmd::.
set(ban_disk_lookup "util::(user_root|home_dir)[ \t]*\\(")
set(ban_names clock env fileio subprocess static thread tty disk_lookup)

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

# One grep over MANY files (relative to `base`) -> list of "file\x02line\x02content"
# hits in `out`, `;` in content preserved via \x01. One process per pattern
# instead of one per file: process creation costs tens of ms on Windows, and
# the per-file form ran ~5000 spawns and blew the ctest timeout.
function(grep_many out base pattern)
    set(rel ${ARGN})
    set(acc "")
    if(rel)
        execute_process(
            COMMAND ${GREP_EXE} -nHE "${pattern}" ${rel}
            WORKING_DIRECTORY ${base}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(hits)
            string(ASCII 1 _sc)
            string(ASCII 2 _fs)
            string(REPLACE ";" "${_sc}" hits "${hits}")
            string(REGEX MATCHALL "[^\n]+" lines "${hits}")
            foreach(l IN LISTS lines)
                string(REGEX MATCH "^([^:]+):([0-9]+):(.*)$" _ "${l}")
                list(APPEND acc "${CMAKE_MATCH_1}${_fs}${CMAKE_MATCH_2}${_fs}${CMAKE_MATCH_3}")
            endforeach()
        endif()
    endif()
    set(${out} "${acc}" PARENT_SCOPE)
endfunction()

# Unpack one grep_many hit into hit_file / hit_line / hit_text.
macro(unpack_hit h)
    string(ASCII 1 _sc)
    string(ASCII 2 _fs)
    string(REPLACE "${_fs}" ";" _parts "${h}")
    list(GET _parts 0 hit_file)
    list(GET _parts 1 hit_line)
    list(SUBLIST _parts 2 -1 _rest)
    list(JOIN _rest "" hit_text)
    string(REPLACE "${_sc}" ";" hit_text "${hit_text}")
endmacro()

set(errors "")
set(present "")
foreach(f IN LISTS files)
    if(EXISTS ${ROOT}/${f})
        list(APPEND present "${f}")
        string(MAKE_C_IDENTIFIER "${f}" key)
        set(used_${key} "")
    endif()
endforeach()
foreach(name IN LISTS ban_names)
    grep_many(hits ${ROOT} "${ban_${name}}" ${present})
    foreach(h IN LISTS hits)
        unpack_hit("${h}")
        set(f "${hit_file}")
        set(lineno "${hit_line}")
        set(content "${hit_text}")
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
        string(MAKE_C_IDENTIFIER "${f}" key)
        set(allowed "${allow_${key}}")
        list(APPEND used_${key} "${name}")
        list(FIND allowed "${name}" idx)
        if(idx EQUAL -1)
            string(STRIP "${content}" content)
            list(APPEND errors "${f}:${lineno}: [${name}] ${content}")
        endif()
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
    # Partition once: files outside a fold, and view/panel files.
    set(nonfold_files "")
    set(view_files "")
    foreach(f IN LISTS vfiles)
        # The reducers and the seam files are where `now` is current.
        # cmd_factory builds Cmds from inside the reducer that called it, so
        # it is in the fold too (a task body it returns must not read m).
        if(NOT (f MATCHES "^runtime/app/update/" OR f STREQUAL "runtime/app/update.cpp"
                OR f STREQUAL "runtime/app/init.cpp"
                OR f STREQUAL "runtime/app/cmd_factory.cpp"))
            list(APPEND nonfold_files "${f}")
        endif()
        if(f MATCHES "^runtime/(view|panel)/")
            list(APPEND view_files "${f}")
        endif()
    endforeach()

    # scan_view(<out> <pattern> <exempt-regex-or-empty> <files...>): one grep
    # over all of <files>, dropping comment-only lines and lines matching the
    # exemption marker; hits as "file:line: text".
    function(scan_view out pattern exempt)
        grep_many(hits ${VIEW_ROOT} "${pattern}" ${ARGN})
        set(acc "")
        foreach(h IN LISTS hits)
            unpack_hit("${h}")
            if(hit_text MATCHES "^[ \t]*//")
                continue()
            endif()
            if(NOT exempt STREQUAL "" AND hit_text MATCHES "${exempt}")
                continue()
            endif()
            string(STRIP "${hit_text}" c)
            list(APPEND acc "${hit_file}:${hit_line}: ${c}")
        endforeach()
        set(${out} "${acc}" PARENT_SCOPE)
    endfunction()

    scan_view(stale_reads "\\b(m|model|mdl)(\\.|->)(now\\b|wall_now\\()" "" ${nonfold_files})
    if(stale_reads)
        list(JOIN stale_reads "\n  " msg)
        message(FATAL_ERROR
            "Model::now read outside a fold (it is stale there):\n  ${msg}\n\n"
            "A view or subscription that needs the current time must get it "
            "from the runtime (an animation clock, a Tick), not from the last "
            "fold's timestamp.")
    endif()
    message(STATUS "elm_purity: Model::now read only inside folds")

    # view() is a function of the Model too: the terminal size reaches it as
    # Model::ui.term_cols/term_rows (installed as the RenderContext), so no
    # view code asks the tty. Same tty pattern as the reducers.
    scan_view(tty_reads "${ban_tty}" "" ${view_files})
    if(tty_reads)
        list(JOIN tty_reads "\n  " msg)
        message(FATAL_ERROR
            "view code asks the terminal directly:\n  ${msg}\n\n"
            "Read the size from the RenderContext view() installs "
            "(maya::available_width/height), which comes from the Model.")
    endif()
    message(STATUS "elm_purity: view reads the terminal size from the Model")

    # ...and its time from the frame clock (maya::anim_now, which tests can
    # freeze and advance), with dates via Model::wall_at. A raw clock read
    # in view code is allowed only as a perf stopwatch whose result goes to a
    # log, never to pixels, and the line has to say so.
    scan_view(clock_reads "${ban_clock}" "// stopwatch:" ${view_files})
    if(clock_reads)
        list(JOIN clock_reads "\n  " msg)
        message(FATAL_ERROR
            "view code reads a clock directly:\n  ${msg}\n\n"
            "Use maya::anim_now() (and Model::wall_at for dates). A perf "
            "stopwatch that only feeds a log may stay; mark the line "
            "`// stopwatch: ...`.")
    endif()
    message(STATUS "elm_purity: view reads time from the frame clock")

    # ...and the environment through Model::env, captured once at launch.
    # A detection helper that happens to live under view/ but only ever runs
    # from read_launch_env marks its line `// launch-env: ...`.
    scan_view(env_reads "${ban_env}" "// launch-env:" ${view_files})
    if(env_reads)
        list(JOIN env_reads "\n  " msg)
        message(FATAL_ERROR
            "view code reads the environment:\n  ${msg}\n\n"
            "Capture it in read_launch_env (runtime/app/env.cpp) into "
            "Model::env and read that.")
    endif()
    message(STATUS "elm_purity: view reads the environment through Model::env")
endif()
