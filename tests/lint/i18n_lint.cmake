# i18n_lint — the three gates that keep the catalog honest.
#
# Run as a ctest entry (see AgenttyTests.cmake). Pure CMake and pure text: no
# build artifacts, so it runs in milliseconds and cannot be skipped by a
# platform that failed to compile something.
#
# WHY A LINT AT ALL. The string sweep is a large mechanical diff -- hundreds
# of literals moving into a catalog -- and "did we drop one" is not a question
# a reviewer can answer by eye. These gates answer it mechanically, which is
# what makes the sweep reviewable at all.
#
#   usage: cmake -DROOT=<src> -P i18n_lint.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "usage: -DROOT=<source dir> -P i18n_lint.cmake")
endif()
if(NOT IS_ABSOLUTE "${ROOT}")
    # Same trap the other lints have: a relative ROOT resolves against the
    # cwd (for `cmake -P`, wherever it was invoked), silently matches zero
    # files, and the whole check passes green having examined nothing.
    message(FATAL_ERROR "ROOT must be ABSOLUTE, got: ${ROOT}")
endif()

set(_fail 0)

# ── Read the English catalog out of startup.cpp ──────────────────────────
#
# The catalog is a raw string literal in the source rather than a data file,
# because it is embedded (see the comment there: a catalog read at runtime
# decides what a permission prompt says). So the lint reads the source.
set(_startup "${ROOT}/src/i18n/startup.cpp")
if(NOT EXISTS "${_startup}")
    message(FATAL_ERROR "i18n: cannot find ${_startup}")
endif()
file(READ "${_startup}" _startup_text)

# Every "id": at the start of an entry. Ids are dotted lower-case by
# convention, which is also what keeps this regex from matching the VALUES.
string(REGEX MATCHALL "\"[a-z][a-z0-9_]*(\\.[a-z0-9_]+)+\"[ \t]*:" _cat_raw
       "${_startup_text}")
set(_catalog_ids "")
foreach(_m IN LISTS _cat_raw)
    string(REGEX REPLACE "^\"(.*)\"[ \t]*:$" "\\1" _id "${_m}")
    list(APPEND _catalog_ids "${_id}")
endforeach()
list(REMOVE_DUPLICATES _catalog_ids)
list(LENGTH _catalog_ids _n_catalog)

# ── Collect every t()/tn()/format() id used in the tree ──────────────────
file(GLOB_RECURSE _sources "${ROOT}/src/*.cpp" "${ROOT}/include/*.hpp")
set(_used_ids "")
set(_model_facing "")

foreach(_f IN LISTS _sources)
    file(READ "${_f}" _text)
    file(RELATIVE_PATH _rel "${ROOT}" "${_f}")

    string(REGEX MATCHALL
           "i18n::(t|tn|format|format_n)\\([ \t\n]*\"[^\"]+\"" _calls "${_text}")
    if(NOT _calls)
        continue()
    endif()

    foreach(_c IN LISTS _calls)
        string(REGEX REPLACE ".*\"([^\"]+)\"$" "\\1" _id "${_c}")
        list(APPEND _used_ids "${_id}")
    endforeach()

    # GATE 3, collected here because we already have the file open.
    #
    # Model-facing code must not translate. Tool descriptions are part of an
    # API contract with the provider; the sandbox denial note exists to
    # change the MODEL's behaviour and instruction-following is strongest in
    # English. Translating either degrades a non-English user's agent while
    # changing nothing they can see. See docs/design/i18n.md §3.
    if(_rel MATCHES "^(src/provider/|src/tool/)")
        list(APPEND _model_facing "${_rel}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _used_ids)
list(REMOVE_DUPLICATES _model_facing)

# ── GATE 1: every id the source uses exists in the catalog ───────────────
#
# A t("foo.bar") with no entry renders the id on screen. That is deliberate
# (visibly wrong beats blank) but it must never SHIP, so it fails the build
# rather than the render.
set(_missing "")
foreach(_id IN LISTS _used_ids)
    if(NOT _id IN_LIST _catalog_ids)
        list(APPEND _missing "${_id}")
    endif()
endforeach()
if(_missing)
    message("")
    message("i18n: ids used in code but ABSENT from the English catalog:")
    foreach(_id IN LISTS _missing)
        message("    ${_id}")
    endforeach()
    message("  Add them to kEnglish in src/i18n/startup.cpp.")
    message("  Without an entry these render as the id itself on screen.")
    set(_fail 1)
endif()

# ── GATE 2: every id in the catalog is used ──────────────────────────────
#
# Dead strings are translator time spent on nothing, and they accumulate
# silently -- a string stays in the catalog long after the row that used it
# was deleted, and twenty translators each render it once.
#
# The lang.* and common.* namespaces are exempt: they are the picker's own
# strings and shared vocabulary, referenced from view code that may build the
# id dynamically.
set(_unused "")
foreach(_id IN LISTS _catalog_ids)
    if(NOT _id IN_LIST _used_ids)
        if(NOT _id MATCHES "^(lang|common)\\.")
            list(APPEND _unused "${_id}")
        endif()
    endif()
endforeach()
if(_unused)
    message("")
    message("i18n: ids in the catalog that NOTHING uses:")
    foreach(_id IN LISTS _unused)
        message("    ${_id}")
    endforeach()
    message("  Delete them, or use them. A dead string is translator time")
    message("  spent on nothing, twenty times over.")
    set(_fail 1)
endif()

# ── GATE 3: no translation in model-facing code ──────────────────────────
if(_model_facing)
    message("")
    message("i18n: t() called from MODEL-FACING code:")
    foreach(_f IN LISTS _model_facing)
        message("    ${_f}")
    endforeach()
    message("  Strings the model reads stay English -- tool descriptions are")
    message("  an API contract, and the sandbox denial note exists to change")
    message("  model behaviour, which works best in English. Translating")
    message("  them degrades a non-English user's agent while changing")
    message("  nothing they can see. docs/design/i18n.md section 3.")
    set(_fail 1)
endif()

if(_fail)
    message(FATAL_ERROR "i18n lint failed")
endif()

message(STATUS
    "i18n: ${_n_catalog} catalog ids, all used, none model-facing")
