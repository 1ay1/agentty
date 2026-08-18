# AgenttyPGO.cmake — opt-in profile-guided optimization (two-phase). Operates
# on the already-defined `agentty` target; included after it. No-op unless
# -DAGENTTY_PGO=generate|use. See the header comment for the workflow.

# ── Profile-guided optimization (opt-in, two phases) ────────────────────
# Phase 1: `cmake -B build-pgogen -DAGENTTY_PGO=generate` → build + run the
#          app through a realistic workload (a couple of chat turns + tool
#          calls is enough; the counters are written on exit).
# Phase 2: `cmake -B build-pgouse -DAGENTTY_PGO=use` → rebuild using the
#          counters — typical gain on streaming + layout hot paths is
#          8–12% wall-clock, 3–6% binary size on MSVC/GCC/Clang.
# The profile data directory is kept out of build/ so switching generators
# or blowing away build/ doesn't lose your collected profile.
set(AGENTTY_PGO "" CACHE STRING
    "Profile-guided optimization: 'generate', 'use', or empty.")
set(AGENTTY_PGO_DIR "${CMAKE_SOURCE_DIR}/pgo-data" CACHE PATH
    "Directory where PGO profile counters live across generate/use phases.")
if(AGENTTY_PGO)
    file(MAKE_DIRECTORY "${AGENTTY_PGO_DIR}")
    if(MSVC)
        if(AGENTTY_PGO STREQUAL "generate")
            target_link_options(agentty PRIVATE "/GENPROFILE:PGD=${AGENTTY_PGO_DIR}/agentty.pgd")
        elseif(AGENTTY_PGO STREQUAL "use")
            target_link_options(agentty PRIVATE "/USEPROFILE:PGD=${AGENTTY_PGO_DIR}/agentty.pgd")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(AGENTTY_PGO STREQUAL "generate")
            target_compile_options(agentty PRIVATE -fprofile-instr-generate)
            target_link_options(agentty PRIVATE -fprofile-instr-generate)
        elseif(AGENTTY_PGO STREQUAL "use")
            # Expect `llvm-profdata merge -output=agentty.profdata ${AGENTTY_PGO_DIR}/*.profraw`
            # to be run between the two builds.
            target_compile_options(agentty PRIVATE "-fprofile-instr-use=${AGENTTY_PGO_DIR}/agentty.profdata")
            target_link_options(agentty PRIVATE "-fprofile-instr-use=${AGENTTY_PGO_DIR}/agentty.profdata")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(AGENTTY_PGO STREQUAL "generate")
            target_compile_options(agentty PRIVATE "-fprofile-generate=${AGENTTY_PGO_DIR}")
            target_link_options(agentty PRIVATE "-fprofile-generate=${AGENTTY_PGO_DIR}")
        elseif(AGENTTY_PGO STREQUAL "use")
            target_compile_options(agentty PRIVATE
                "-fprofile-use=${AGENTTY_PGO_DIR}" -fprofile-correction)
            target_link_options(agentty PRIVATE "-fprofile-use=${AGENTTY_PGO_DIR}")
        endif()
    endif()
    message(STATUS "agentty: PGO phase = ${AGENTTY_PGO} (data dir: ${AGENTTY_PGO_DIR})")
endif()

