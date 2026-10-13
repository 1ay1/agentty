# AgenttyToolchain.cmake — compiler cache, C++ standard + graceful fallback,
# default build type, and MSVC Release flag normalization. Included right after
# project() so it applies to every target (agentty + all submodules).

# ── Compiler cache (ccache / sccache) auto-detect ──────────────────────────
# Placed right after project() so it applies to EVERY target in the tree,
# including the submodules pulled in via add_subdirectory (maya, mcp-cpp,
# rag-cpp, acp-cpp). Those submodules are header-heavy: a synced upstream
# commit that touches one widely-included header (e.g. maya/widget/input.hpp)
# otherwise recompiles all ~41 maya TUs plus the ~64 agentty TUs that include
# a maya header, even when a given TU's preprocessed content didn't actually
# change. ccache keys on CONTENT, so those become instant cache hits — the
# right fix for "submodules are mostly headers, make re-sync rebuilds fast".
#
# Off is impossible to get wrong: if neither tool is installed we simply don't
# set the launcher and the build is exactly as before. Opt out with
# -DAGENTTY_COMPILER_CACHE=OFF. Honours a user-set *_COMPILER_LAUNCHER.
option(AGENTTY_COMPILER_CACHE "Use ccache/sccache as the compiler launcher when available" ON)
if(AGENTTY_COMPILER_CACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(AGENTTY_CCACHE NAMES ccache sccache)
    if(AGENTTY_CCACHE)
        set(CMAKE_C_COMPILER_LAUNCHER   "${AGENTTY_CCACHE}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${AGENTTY_CCACHE}" CACHE STRING "" FORCE)
        message(STATUS "agentty: compiler cache enabled (${AGENTTY_CCACHE}) — "
                       "content-hashed rebuilds; header-only submodule syncs stay fast")
    else()
        message(STATUS "agentty: no ccache/sccache found — install ccache to speed up "
                       "rebuilds after submodule syncs (brew/apt/pacman install ccache)")
    endif()
endif()

# CMake (as of 4.2) has no /std:c++26 mapping for MSVC yet. Ask for C++23
# there and opt into /std:c++latest so MSVC 14.50+ exposes available C++26
# library bits (std::expected, std::format, etc). Other compilers get C++26.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── C++26 → C++23 graceful fallback (Termux / older clang) ─────────────────
# The tree ASKS for C++26 on non-MSVC compilers, but uses NO C++26-only
# LIBRARY facility — every TU compiles cleanly at C++23 (the MSVC leg already
# proves this by building the whole tree at /std:c++23-equivalent). So rather
# than hard-FAIL configure with CMake's raw
#     "Target ... requires the language dialect "CXX26" ... not supported"
# on a compiler that doesn't advertise cxx_std_26 (clang < 18, gcc < 14),
# fall back to C++23 and tell the user exactly what happened. Termux is the
# common case here: a fresh `pkg install clang` is 18+, but older installs
# and other embedded toolchains land at 17.
if(NOT MSVC AND NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    if("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(CMAKE_CXX_STANDARD 23)
        message(STATUS
            "agentty: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
            "does not advertise C++26 — falling back to C++23 (fully supported; "
            "no C++26-only library facility is used).")
    else()
        set(_agentty_is_termux OFF)
        if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android"
                   OR DEFINED ENV{TERMUX_VERSION}
                   OR EXISTS "/data/data/com.termux/files/usr")
            set(_agentty_is_termux ON)
        endif()
        if(_agentty_is_termux)
            message(FATAL_ERROR
                "agentty needs at least C++23, but this Termux clang "
                "(${CMAKE_CXX_COMPILER_VERSION}) is too old. Run "
                "`pkg upgrade clang` (Termux ships clang 18+, which is more "
                "than enough), then re-run cmake.")
        else()
            message(FATAL_ERROR
                "agentty needs a compiler that supports C++23 (clang >= 16, "
                "gcc >= 12). Detected ${CMAKE_CXX_COMPILER_ID} "
                "${CMAKE_CXX_COMPILER_VERSION}, which advertises neither "
                "cxx_std_26 nor cxx_std_23. Please upgrade the compiler.")
        endif()
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── MSVC Release: strip default /Ob2 so our /Ob3 doesn't trigger D9025 ──
#    CMake's default MSVC Release flags are "/MD /O2 /Ob2 /DNDEBUG". We
#    want our per-target /Ob3 (aggressive inlining) instead. We also want
#    to handle /O2 ourselves so the per-target Release flags below are
#    authoritative. /MD vs /MT is handled via CMAKE_MSVC_RUNTIME_LIBRARY
#    at the top of this file — see below.
if(MSVC)
    foreach(flag_var
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL
            CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "/O2" "" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/Ob[0-3]" "" ${flag_var} "${${flag_var}}")
    endforeach()
endif()
