# secret_modes.cmake — files under credentials/ are created 0600, structurally.
#
# `~/.agentty/credentials/` is the one directory where a permission audit
# should find no exceptions. It holds OAuth refresh tokens, API keys and the
# saved-account registry; the directory is 0700 and every file in it is 0600.
#
# Except one wasn't. accounts.json sat at 0644 among seven 0600 siblings,
# because it is written with std::ofstream -- which creates at 0666 & ~umask --
# while credentials.json, the provider-key vault and even the auth lockfile all
# pass an explicit mode. Measured on a live install, not hypothesised.
#
# That was contained (the 0700 directory means nothing could read it anyway)
# and it was still worth fixing: the containment is one `chmod 755`, or one
# $AGENTTY_HOME pointed at a pre-existing directory, away from being the only
# thing between a token registry and another local account. Defence in depth
# means the file mode is not load-bearing on the directory mode.
#
# This lint catches the CAUSE rather than the symptom: a writer in the auth or
# accounts layer that creates a file without saying what mode it wants. A
# chmod-after-write is fine here -- those payloads are sealed ciphertext, so
# the create-to-chmod window exposes nothing -- so the rule is "the writer
# names a mode somewhere", not "the writer must use open()".
#
# Usage: cmake -DROOT=<repo root> -P secret_modes.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "secret_modes: -DROOT=<repo root> required")
endif()

# The files that persist secrets. Each must name 0600 (or S_IRUSR|S_IWUSR)
# somewhere, since each creates a file under credentials/.
set(_writers
    "${ROOT}/src/io/accounts.cpp"
    "${ROOT}/src/io/auth.cpp"
    "${ROOT}/src/io/keys.cpp"
    "${ROOT}/src/io/account_switch.cpp")

set(_offenders "")

foreach(_f IN LISTS _writers)
    if(NOT EXISTS "${_f}")
        continue()
    endif()
    file(RELATIVE_PATH _rel "${ROOT}" "${_f}")

    # Does it create a file at all? ofstream / open / atomic_write all count.
    file(STRINGS "${_f}" _creates
         REGEX "std::ofstream|::open\\(|atomic_write")
    if(NOT _creates)
        continue()
    endif()

    # If so, it has to actually CALL something that sets the mode.
    #
    # Matching the mode VALUE is not enough, and the attempts to do so are
    # instructive. Matching `0600` anywhere hits the explanatory comment above
    # the writer. Stripping comment LINES still hits
    # `#include <sys/stat.h>   // chmod -- 0600 ...`, which is code with a
    # trailing comment. Both versions passed a file whose chmod I had deleted.
    #
    # So the check is on the CALL, which is the thing that has to exist:
    #
    #   chmod(...)                  POSIX, after the write
    #   open(..., S_IRUSR...)       POSIX, at create
    #   fs::permissions(...)        C++, used by keys.cpp / account_switch.cpp
    #
    # A value can be mentioned; a call has to be made.
    file(STRINGS "${_f}" _mode_calls
         REGEX "::chmod\\(|[^a-z_]chmod\\(|fs::permissions\\(|O_CREAT[^)]*S_IRUSR")
    set(_modes "")
    foreach(_l IN LISTS _mode_calls)
        string(STRIP "${_l}" _stripped)
        if(NOT _stripped MATCHES "^(//|/\\*|\\*)")
            list(APPEND _modes "${_l}")
        endif()
    endforeach()
    if(NOT _modes)
        list(APPEND _offenders
             "    ${_rel}: creates a file but never sets its mode")
    endif()
endforeach()

if(_offenders)
    string(REPLACE ";" "\n" _report "${_offenders}")
    message(FATAL_ERROR
        "a secrets writer does not set a file mode:\n\n${_report}\n\n"
        "Everything under ~/.agentty/credentials/ is 0600. std::ofstream\n"
        "creates at 0666 & ~umask, so a writer that does not say otherwise\n"
        "lands 0644 -- which is what accounts.json did, among seven 0600\n"
        "siblings. Any of these counts, and all three are already in use:\n"
        "  open(path, O_CREAT, S_IRUSR | S_IWUSR)\n"
        "  chmod(tmp, S_IRUSR | S_IWUSR)        before the rename\n"
        "  fs::permissions(tmp, owner_read | owner_write, replace)\n\n"
        "The check is on the CALL, not the value -- a comment mentioning\n"
        "0600, or an #include whose trailing comment says it, does not count.")
endif()

message(STATUS "secret modes: every credentials/ writer names 0600")
