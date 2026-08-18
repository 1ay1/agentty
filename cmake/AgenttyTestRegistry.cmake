# AgenttyTestRegistry.cmake — one declaration per test, aggregates derived.
#
# Replaces the old 4-list system (agentty_add_full_test + AGENTTY_MIGRATED_TESTS
# + hand-listed `tests`/`sanitizer_tests` aggregates + the explicit agentty_tests
# source list). A test's name is now written EXACTLY ONCE, in an agentty_test()
# call; every downstream set is computed from a global registry at finalize.
#
# ── API ────────────────────────────────────────────────────────────────────
#   agentty_test(<name>
#       [MODE consolidated|standalone]   # default consolidated (folds into agentty_tests)
#       [SRCS <extra .cpp> ...]          # extra TUs beyond tests/<name>.cpp
#       [OBJS <$<TARGET_OBJECTS:x>> ...] # extra object libs (standalone only)
#       [LIBS <lib> ...]                 # extra link libraries
#       [TIMEOUT <seconds>]              # default 60; standalone/ctest entries only
#       [LABELS <label> ...]             # ctest labels; `sanitizer` also joins that aggregate
#       [UNIX_LIBS <lib> ...]            # extra libs only on UNIX AND NOT APPLE (e.g. util for openpty)
#       [ENV "K=V" ...]                  # ctest ENVIRONMENT property
#       [NO_TEST]                        # build but register NO ctest entry (dev probes / capture tools)
#       [GATE <VAR>]                     # only define when ${VAR} is truthy (e.g. AGENTTY_MCP)
#   )
#
#   agentty_add_ctest(<name> COMMAND <argv...> [TIMEOUT n] [LABELS ...])
#       # a raw ctest entry that runs an existing target with args (e.g. the
#       # reveal_stream_gate arms driving anthropic_md_stream). No new binary.
#
#   agentty_finalize_tests()
#       # define agentty_tests from accumulated consolidated sources, apply
#       # timeouts/labels/env to standalone entries, and build the derived
#       # `tests`, `sanitizer_tests`, and `tests_perf` aggregates.
#
# Registry state lives in DIRECTORY properties (list-valued), append-only.

define_property(DIRECTORY PROPERTY AGENTTY_T_CONSOLIDATED_SRCS
    BRIEF_DOCS "sources folded into agentty_tests" FULL_DOCS "see AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_STANDALONE
    BRIEF_DOCS "standalone test target names" FULL_DOCS "see AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_SANITIZER
    BRIEF_DOCS "sanitizer-labelled test names" FULL_DOCS "see AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_PERF
    BRIEF_DOCS "perf/probe/bench test names (skippable on the fast gate)" FULL_DOCS "see AgenttyTestRegistry")

# The link recipe shared by every standalone full-stack test. Set once by the
# caller (AgenttyTests.cmake) into these DIRECTORY-scope vars before any
# agentty_test() call, so the function doesn't hard-code agentty's dep list.
#   AGENTTY_T_SHARED_OBJECTS  — $<TARGET_OBJECTS:...> list (compiled once)
#   AGENTTY_T_BASE_LIBS       — libraries every test links
#   AGENTTY_T_INCLUDE_DIRS    — include dirs every test needs

function(agentty_test name)
    cmake_parse_arguments(T
        "NO_TEST"                                   # options
        "MODE;TIMEOUT;GATE"                         # one-value
        "SRCS;OBJS;LIBS;LABELS;UNIX_LIBS;ENV"       # multi-value
        ${ARGN})

    if(T_GATE AND NOT ${T_GATE})
        return()  # gated out (e.g. GATE AGENTTY_MCP when MCP is off)
    endif()

    if(NOT T_MODE)
        set(T_MODE consolidated)
    endif()

    # The primary source is tests/<name>.cpp unless the caller overrides it
    # entirely via SRCS (some consolidated files have a non-name path).
    set(_primary "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")

    if(T_MODE STREQUAL "consolidated")
        # Just accumulate the source(s) into the agentty_tests bucket.
        set(_srcs "${_primary}" ${T_SRCS})
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_CONSOLIDATED_SRCS ${_srcs})
        return()
    endif()

    if(T_MODE STREQUAL "raw")
        # The caller defines the add_executable + add_test itself (narrow-source
        # sanitizer tests that must NOT link the full shared object set). We only
        # record it in the aggregates + label-derived sets.
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_STANDALONE ${name})
        if("sanitizer" IN_LIST T_LABELS)
            set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_SANITIZER ${name})
        endif()
        if("perf" IN_LIST T_LABELS OR T_NO_TEST)
            set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_PERF ${name})
        endif()
        return()
    endif()

    # ── standalone ──────────────────────────────────────────────────────────
    add_executable(${name} EXCLUDE_FROM_ALL ${_primary} ${T_SRCS}
        ${AGENTTY_T_SHARED_OBJECTS} ${T_OBJS})
    target_include_directories(${name} PRIVATE ${AGENTTY_T_INCLUDE_DIRS})
    target_compile_definitions(${name} PRIVATE AGENTTY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${name} PRIVATE ${AGENTTY_T_BASE_LIBS} ${T_LIBS})
    if(APPLE)
        target_link_libraries(${name} PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()
    if(T_UNIX_LIBS AND UNIX AND NOT APPLE)
        target_link_libraries(${name} PRIVATE ${T_UNIX_LIBS})
    endif()

    set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_STANDALONE ${name})

    if(NOT T_NO_TEST)
        add_test(NAME ${name} COMMAND ${name})
        if(NOT T_TIMEOUT)
            set(T_TIMEOUT 60)
        endif()
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
        if(T_LABELS)
            set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
        endif()
        if(T_ENV)
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "${T_ENV}")
        endif()
    endif()

    # Derived-set membership from labels.
    if("sanitizer" IN_LIST T_LABELS)
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_SANITIZER ${name})
    endif()
    if("perf" IN_LIST T_LABELS OR T_NO_TEST)
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_PERF ${name})
    endif()
endfunction()

# Raw ctest entry that runs an EXISTING target with arguments (no new binary).
function(agentty_add_ctest name)
    cmake_parse_arguments(C "" "TIMEOUT" "COMMAND;LABELS" ${ARGN})
    add_test(NAME ${name} COMMAND ${C_COMMAND})
    if(NOT C_TIMEOUT)
        set(C_TIMEOUT 120)
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT ${C_TIMEOUT})
    if(C_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${C_LABELS}")
    endif()
endfunction()

function(agentty_finalize_tests)
    # ── The consolidated single binary ──────────────────────────────────────
    get_property(_srcs DIRECTORY PROPERTY AGENTTY_T_CONSOLIDATED_SRCS)
    add_executable(agentty_tests EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${_srcs}
        ${AGENTTY_T_SHARED_OBJECTS}
        $<TARGET_OBJECTS:agentty_acp_obj>)   # acp_integration_test needs AgentServer
    target_include_directories(agentty_tests PRIVATE ${AGENTTY_T_INCLUDE_DIRS})
    target_compile_definitions(agentty_tests PRIVATE AGENTTY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(agentty_tests PRIVATE doctest::doctest ${AGENTTY_T_BASE_LIBS})
    if(TARGET acp::acp)
        target_link_libraries(agentty_tests PRIVATE acp::acp)
    endif()
    if(APPLE)
        target_link_libraries(agentty_tests PRIVATE
            "-framework Security" "-framework CoreFoundation")
    endif()
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(agentty_tests)

    # ── Derived aggregates (computed, never hand-listed) ────────────────────
    get_property(_standalone DIRECTORY PROPERTY AGENTTY_T_STANDALONE)
    get_property(_sanitizer  DIRECTORY PROPERTY AGENTTY_T_SANITIZER)
    get_property(_perf       DIRECTORY PROPERTY AGENTTY_T_PERF)

    # Full build target: the consolidated binary + every standalone.
    add_custom_target(tests DEPENDS agentty_tests ${_standalone})

    # Fast/correctness gate: everything EXCEPT perf probes & benches. This is
    # what CI's Linux gate should build to skip the never-gating diagnostics.
    set(_gating ${_standalone})
    if(_perf)
        list(REMOVE_ITEM _gating ${_perf})
    endif()
    add_custom_target(tests_gating DEPENDS agentty_tests ${_gating})

    # Sanitizer gate: the labelled set (must NOT pull maya's un-instrumented
    # renderer — those tests are agentty-logic-only) + the consolidated binary.
    add_custom_target(sanitizer_tests DEPENDS agentty_tests ${_sanitizer})
endfunction()
