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
set(ban_env         "(std::)?(getenv|setenv|unsetenv|secure_getenv)[ \t]*\\(|detect_(tier|polarity)[ \t]*\\(|ui_prefs::detect[ \t]*\\(|terminal_is_dumb[ \t]*\\(|registry::read_env[ \t]*\\(|apply_env[ \t]*\\([^,)]*\\)")
# fileio: an operation that touches the disk. NOT the bare `std::filesystem::`
# prefix: `std::filesystem::path` is a value type (string manipulation, no
# syscall), and flagging it reported a reducer composing a path as IO. Only
# the functions that actually read or write the filesystem count.
set(ban_fileio      "std::(if|of|f)stream|(fs|std::filesystem)::(remove|rename|create_director|copy|exists|is_regular_file|is_directory|file_size|last_write_time|directory_iterator|recursive_directory_iterator|status|read_symlink|canonical|weakly_canonical|temp_directory_path|current_path|space|equivalent)\\b")
set(ban_subprocess  "run_command|::system[ \t]*\\(|popen[ \t]*\\(")
set(ban_net         "prewarm_active_provider|http::default_client|dial_new")
set(ban_global      "provider::select[ \t]*\\(|tools::(subagent::set_|skills::(reset_activations|note_activated)|invalidate_mcp_catalog|plugin::(set_|remove_|approve_|add_|update_)|util::allow_read_root)")
set(ban_auth        "auth::(load_credentials|oauth_proactive_refresh_token|clear_credentials|save_credentials|random_urlsafe|anthropic_signed_in)|credentials::(resolve|add_key|clear_active|needs_login)|(acc|accounts)::(activate|remove|snapshot_active|get|list_for|active_label|derive_current_label)[ \t]*\\(|vault::(sign_out|signed_in)[ \t]*\\(")
set(ban_static      "^[ \t]+static[ \t]+(std::|auto[ \t]|bool[ \t]|int[ \t]|long[ \t]|unsigned[ \t]|double[ \t]|float[ \t]|size_t[ \t])[^(]*[=;{][ \t]*$")
set(ban_thread      "std::j?thread([^_:]|$)|\\.detach\\(\\)|std::async[^_]")
# tty: asking the terminal directly. The size arrives as a Msg
# (TerminalResized, from maya's on_resize) and lives in Model::ui.
set(ban_tty         "platform::(query_terminal_size|stdout_handle|stdin_handle)|isatty[ \t]*\\(|ioctl[ \t]*\\(")
# diskhelper: helpers that LOOK like plain functions but write the disk,
# found only by reading them. A name lint can't see through a call, so the
# known ones are named here.
set(ban_diskhelper  "(add_plugin_from_line|create_starter)[ \t]*\\(")
# disk_lookup: helpers that walk the disk or a secure store to answer a
# lookup. Reducers read the answer from the Model instead: m.ui.library
# (skills, approvals, commands, hooks), m.ui.git_repo, m.env (embed
# defaults, user root). A worker that needs them gets them in a cmd::.
set(ban_disk_lookup "(skills|commands)::(all|find|shadowed|shadowed_within_scope|load_approvals)[ \t]*\\(|skills::trust_of[ \t]*\\([^,)]*\\)|commands::try_expand[ \t]*\\([^,)]*\\)|hooks::(active_file|pending_approval)[ \t]*\\(|workspace::in_git_repo(_if_ready)?[ \t]*\\(|eb::apply_env[ \t]*\\(|skills_panel::scan[ \t]*\\([ \t]*\\)|take_unproven_spec[ \t]*\\(|config_path[ \t]*\\([^,)]*\\)[^,]|util::(user_root|home_dir)[ \t]*\\(")
set(ban_names clock env fileio subprocess net global auth static thread tty diskhelper disk_lookup)

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
        # The reducers and the seam files are where `now` is current.
        # cmd_factory builds Cmds from inside the reducer that called it, so
        # it is in the fold too (a task body it returns must not read m).
        if(f MATCHES "^runtime/app/update/" OR f STREQUAL "runtime/app/update.cpp"
           OR f STREQUAL "runtime/app/init.cpp"
           OR f STREQUAL "runtime/app/cmd_factory.cpp")
            continue()
        endif()
        execute_process(
            COMMAND ${GREP_EXE} -nE "\\b(m|model|mdl)(\\.|->)(now\\b|wall_now\\()" ${VIEW_ROOT}/${f}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        string(ASCII 1 _sc)
        string(REPLACE ";" "${_sc}" hits "${hits}")
        string(REGEX MATCHALL "[^\n]+" hl "${hits}")
        foreach(h IN LISTS hl)
            string(REPLACE "${_sc}" ";" h "${h}")
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${h}")
            # Save the captures: the comment test below is another regex and
            # resets CMAKE_MATCH_*.
            set(lineno "${CMAKE_MATCH_1}")
            set(c "${CMAKE_MATCH_2}")
            if(c MATCHES "^[ \t]*//")
                continue()
            endif()
            string(STRIP "${c}" c)
            list(APPEND stale_reads "${f}:${lineno}: ${c}")
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

    # view() is a function of the Model too: the terminal size reaches it as
    # Model::ui.term_cols/term_rows (installed as the RenderContext), so no
    # view code asks the tty. Same tty pattern as the reducers.
    set(tty_reads "")
    foreach(f IN LISTS vfiles)
        if(NOT f MATCHES "^runtime/(view|panel)/")
            continue()
        endif()
        execute_process(
            COMMAND ${GREP_EXE} -nE "${ban_tty}" ${VIEW_ROOT}/${f}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        # Same `;` guard as the reducer scan: CMake lists split on it.
        string(ASCII 1 _sc)
        string(REPLACE ";" "${_sc}" hits "${hits}")
        string(REGEX MATCHALL "[^\n]+" hl "${hits}")
        foreach(h IN LISTS hl)
            string(REPLACE "${_sc}" ";" h "${h}")
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${h}")
            set(lineno "${CMAKE_MATCH_1}")
            set(c "${CMAKE_MATCH_2}")
            if(c MATCHES "^[ \t]*//")
                continue()
            endif()
            string(STRIP "${c}" c)
            list(APPEND tty_reads "${f}:${lineno}: ${c}")
        endforeach()
    endforeach()
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
    set(clock_reads "")
    foreach(f IN LISTS vfiles)
        if(NOT f MATCHES "^runtime/(view|panel)/")
            continue()
        endif()
        execute_process(
            COMMAND ${GREP_EXE} -nE "${ban_clock}" ${VIEW_ROOT}/${f}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        string(ASCII 1 _sc)
        string(REPLACE ";" "${_sc}" hits "${hits}")
        string(REGEX MATCHALL "[^\n]+" hl "${hits}")
        foreach(h IN LISTS hl)
            string(REPLACE "${_sc}" ";" h "${h}")
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${h}")
            set(lineno "${CMAKE_MATCH_1}")
            set(c "${CMAKE_MATCH_2}")
            if(c MATCHES "^[ \t]*//" OR c MATCHES "// stopwatch:")
                continue()
            endif()
            string(STRIP "${c}" c)
            list(APPEND clock_reads "${f}:${lineno}: ${c}")
        endforeach()
    endforeach()
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
    set(env_reads "")
    foreach(f IN LISTS vfiles)
        if(NOT f MATCHES "^runtime/(view|panel)/")
            continue()
        endif()
        execute_process(
            COMMAND ${GREP_EXE} -nE "${ban_env}" ${VIEW_ROOT}/${f}
            OUTPUT_VARIABLE hits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT hits)
            continue()
        endif()
        string(ASCII 1 _sc)
        string(REPLACE ";" "${_sc}" hits "${hits}")
        string(REGEX MATCHALL "[^\n]+" hl "${hits}")
        foreach(h IN LISTS hl)
            string(REPLACE "${_sc}" ";" h "${h}")
            string(REGEX MATCH "^([0-9]+):(.*)$" _ "${h}")
            set(lineno "${CMAKE_MATCH_1}")
            set(c "${CMAKE_MATCH_2}")
            if(c MATCHES "^[ \t]*//" OR c MATCHES "// launch-env:")
                continue()
            endif()
            string(STRIP "${c}" c)
            list(APPEND env_reads "${f}:${lineno}: ${c}")
        endforeach()
    endforeach()
    if(env_reads)
        list(JOIN env_reads "\n  " msg)
        message(FATAL_ERROR
            "view code reads the environment:\n  ${msg}\n\n"
            "Capture it in read_launch_env (runtime/app/env.cpp) into "
            "Model::env and read that.")
    endif()
    message(STATUS "elm_purity: view reads the environment through Model::env")
endif()
