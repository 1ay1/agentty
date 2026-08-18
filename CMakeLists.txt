cmake_minimum_required(VERSION 3.28)
project(agentty VERSION 0.3.0 LANGUAGES CXX)

# Project CMake modules (test registry, and — as the redesign lands — the
# extracted toolchain/standalone/hardening includes) live under cmake/.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Toolchain: compiler cache, C++ standard + fallback, default build type,
# MSVC Release flag normalization. See cmake/AgenttyToolchain.cmake.
include(AgenttyToolchain)

# ── Standalone binary plumbing ─────────────────────────────────────────
# AGENTTY_STANDALONE=ON produces a binary with no third-party shared-library
# dependencies — drop it on any compatible machine (matching libc
# version) and it runs. On every platform it forces the right static-
# linking knobs:
#
#   Linux      OpenSSL + nghttp2 statically linked. libstdc++ and libgcc
#              folded in via -static-libstdc++ / -static-libgcc. libc
#              stays dynamic (fully-static glibc breaks the NSS resolver
#              and DNS lookups; if you need a 100% static binary, build
#              against musl with -DAGENTTY_FULLY_STATIC=ON).
#   macOS      OpenSSL + nghttp2 statically linked. libSystem stays
#              dynamic (the only ABI Apple supports for distribution).
#   Windows    Forces AGENTTY_STATIC_RUNTIME=ON (/MT) so the MSVC CRT is
#              statically embedded; nghttp2 + OpenSSL come from the
#              x64-windows-static vcpkg triplet.
#
# Build with `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DAGENTTY_STANDALONE=ON`.
option(AGENTTY_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" OFF)
option(AGENTTY_STANDALONE     "Produce a standalone binary with no third-party shared-library deps" OFF)
option(AGENTTY_FULLY_STATIC   "Fully static link (Linux only, requires musl toolchain)" OFF)
# By default a fully-static Linux build is `-static -no-pie` (ET_EXEC): a true
# standalone binary that runs on every Linux userland (glibc, musl, Pi OS).
# Flip this ON only to target Android/Bionic (Termux), which refuses ET_EXEC
# and needs a PIE — but ONLY on a musl toolchain whose -static-pie genuinely
# links libc statically (Alpine's default-PIE GCC does NOT; it produces the
# v0.2.7 crasher, which the build-time guard then rejects).
option(AGENTTY_STATIC_PIE      "Emit a static-PIE (ET_DYN) instead of ET_EXEC for the fully-static build — Termux/Android only" OFF)

# mimalloc override — Microsoft's production allocator, fetched by CMake from
# upstream main. agentty churns std::string everywhere (RenderOp, SSE parsing,
# JSON, and the Element view tree), which benefits from mimalloc's sharded
# free lists and eager page purging. The static library overrides malloc/free
# and global C++ new/delete.
# Default ON; turn OFF to build against the plain system allocator.
option(AGENTTY_USE_MIMALLOC   "Route malloc/free and operator new/delete through CMake-fetched mimalloc" ON)

# CPU ISA baseline. Default avx2 (Haswell+/Zen1+, ~2013), which covers ~every
# desktop/laptop in use. Drop to "avx" for Sandy/Ivy Bridge (2011–2012) or
# older VM hosts, "sse2" for truly ancient, "native" to let the compiler pick
# based on the build machine. MSVC only exposes a handful of /arch values;
# GCC/Clang understand -march= directly.
set(AGENTTY_ARCH "avx2" CACHE STRING
    "CPU baseline: native, avx2 (default), avx, or sse2.")
set_property(CACHE AGENTTY_ARCH PROPERTY STRINGS native avx2 avx sse2)

if(AGENTTY_STANDALONE AND MSVC)
    # On Windows, standalone implies static MSVC runtime — there's no
    # other way to ship a "drop it on any machine" .exe.
    set(AGENTTY_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
if(AGENTTY_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Tell find_package(OpenSSL) to prefer .a over .so/.dylib when standalone
# is requested. We try static first (QUIET so a missing .a doesn't error
# out); if that fails we silently fall back to the dynamic variant later
# and warn the user that one runtime dep slipped through. Most distros
# (Arch, Debian default, Fedora) only package OpenSSL as .so; users on
# Alpine, vcpkg-static, or who installed openssl-static get the full
# standalone build.
set(AGENTTY_STANDALONE_OPENSSL_FALLBACK FALSE)
if(AGENTTY_STANDALONE)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# Link-time optimization — enabled by default in Release/RelWithDebInfo, off
# in Debug (would drag build times without shipping value). Supported by
# GCC, Clang, MSVC, and AppleClang; CMake picks the right flag per toolchain.
# Set before the maya/acp-cpp/mcp-cpp subdirectories so the whole tree LTO-
# links as one unit on the platforms where it pays off.
include(CheckIPOSupported)
check_ipo_supported(RESULT AGENTTY_HAS_IPO OUTPUT AGENTTY_IPO_ERR)

# macOS + GCC is the one toolchain where LTO is all cost and no benefit.
# Apple's ld can't consume GCC's LTO bytecode, so `-flto` does NOT optimize
# across modules — the fat objects' per-TU -O3 code is what actually links —
# yet the GIMPLE IR rides along as a dead __GNU_LTO LOAD segment (tens of MB)
# that `strip`/`strip -x` cannot remove (it's a load command, not symbols).
# check_ipo_supported() passes (its probe link "works"), so without this
# guard the macOS release ships ~3x its real size with zero speed gain.
# Disable IPO for this combo only: the binary drops to its true code size and
# is byte-for-byte as optimized. Linux GCC/Clang (where the shipping static
# binaries are built — full LTO + maya's heavy flag set) and MSVC (/GL+/LTCG)
# are unaffected. macOS's real ceiling is AppleClang+ThinLTO, which can't
# build this tree yet (no cxx_std_26 advertised), so -O3 is the macOS max.
set(AGENTTY_IPO_OK ${AGENTTY_HAS_IPO})
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(AGENTTY_IPO_OK FALSE)
    message(STATUS "agentty: LTO disabled on macOS+GCC "
                   "(Apple ld can't consume GCC LTO IR — would only bloat the binary)")
endif()
# Whole-tree sanitizer builds (-DAGENTTY_SANITIZE_ALL=...) must also disable
# LTO here, BEFORE add_subdirectory(maya) below compiles maya's objects.
# AGENTTY_SANITIZE_ALL is read early on purpose: a `-D` on the cmake command
# line (which is how CI and every documented sanitizer invocation set it) is
# visible from the very start of script evaluation, long before its own
# `set(... CACHE STRING ...)` declaration later in this file runs — so this
# check is not a forward reference, it sees the real value. Without this
# guard, maya compiled here with IPO=TRUE produces GCC slim-LTO bytecode
# objects in libmaya.a; the sanitizer flags added later apply `-fno-lto` at
# LINK time only (with no matching `-flto`), so the linker can't consume
# those bytecode objects: "plugin needed to handle lto object". LTO buys
# sanitizer builds nothing anyway (they're a correctness gate, not a
# release artifact), so turning it off here is strictly a win.
if(AGENTTY_SANITIZE_ALL)
    set(AGENTTY_IPO_OK FALSE)
    message(STATUS "agentty: LTO disabled for sanitizer build "
                   "(AGENTTY_SANITIZE_ALL=${AGENTTY_SANITIZE_ALL} — avoids an "
                   "LTO-bytecode/-fno-lto link mismatch in libmaya.a)")
endif()
if(AGENTTY_IPO_OK AND (CMAKE_BUILD_TYPE STREQUAL "Release"
                     OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
                     OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
elseif(AGENTTY_SANITIZE_ALL)
    # Set a DEFINED false value, not just "leave it unset". maya/CMakeLists.txt
    # has its own `if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION) set(...
    # ON)` fallback (so a bare `add_subdirectory(maya)` from an external
    # project still gets LTO) — if we only skip the TRUE branch above without
    # ever defining the variable, that fallback fires inside
    # add_subdirectory(maya) below and silently re-enables LTO out from under
    # this sanitizer build, producing GCC slim-LTO bytecode objects in
    # libmaya.a that the later `-fno-lto`-only link flags can't consume
    # ("plugin needed to handle lto object"). Defining it FALSE here is the
    # actual override maya's guard is designed to respect.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

include(FetchContent)

set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# STANDALONE builds must be portable across every chip of the target arch.
# Maya's default `-march=native -mtune=native` would otherwise bake the
# producing host's microarchitecture into libmaya.a (most visible on
# aarch64 — a Graviton3-built binary SIGILLs on a Cortex-A72). Force the
# native-tuning gate off so maya falls back to the compiler's default
# baseline (`-march=x86-64` / `armv8-a`).
if(AGENTTY_STANDALONE)
    set(MAYA_NATIVE_TUNING OFF CACHE BOOL "" FORCE)
endif()

# macOS SDK + GCC, catch-all: the macOS 26 SDK's <mach/*.h> arm64
# size-assert macros expand to `_Static_assert(...)`, a C keyword g++
# rejects in C++ mode (only clang accepts it as an extension). ANY TU
# pulling in mach headers (subprocess.cpp via <spawn.h>, tls.cpp, etc.)
# trips it. Alias it directory-wide HERE — before every add_subdirectory
# below (maya, acp-cpp, mcp-cpp) and before the agentty / test targets —
# so all of them, including future submodules, inherit it from one place
# instead of each re-discovering the breakage. AppleClang doesn't need it
# (and can't build this tree — it doesn't advertise cxx_std_26).
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_definitions(_Static_assert=static_assert)
endif()

# ── Android / Termux: raise the deployment API to 28 ───────────────────────
# Two Bionic symbols agentty's dependency graph needs are API-gated:
#   • <spawn.h> / posix_spawn*   → __INTRODUCED_IN(28)
#   • strtod_l / strtof_l        → __INTRODUCED_IN(26)   (libc++ <locale>
#     calls these UNCONDITIONALLY, pulled in transitively via <iostream> /
#     <format>, so the whole C++ standard library fails to compile below 26)
#
# It is NOT enough to -D__ANDROID_API__=28: that only moves the preprocessor
# macro, while clang's __attribute__((availability)) diagnostics compare
# against the DEPLOYMENT TARGET baked into the target triple's API suffix
# (e.g. aarch64-linux-android24). If that stays below the symbol's
# introduction level you get:
#     error: 'strtod_l' is unavailable: introduced in Android 26
#     fatal error: 'spawn.h' file not found
# even with the macro raised. The sanctioned Termux fix (termux-packages
# #23401) is to pass --target=<arch>-linux-android28 so the deployment
# target itself moves up. Modern Termux always runs on API ≥ 28 devices, so
# deploying at 28 is safe.
#
# Derive <arch> from the compiler's own default triple so this works on
# aarch64 / arm / x86_64 without hardcoding. Applied to BOTH compile and link
# (the CRT objects carry the API level too). Directory-wide + before every
# add_subdirectory so maya / acp-cpp / mcp-cpp and the agentty/test targets
# all inherit it. No-op on every non-Android platform.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android"
           OR DEFINED ENV{TERMUX_VERSION}
           OR EXISTS "/data/data/com.termux/files/usr")
    # Ask the compiler for its default target triple (e.g.
    # "aarch64-linux-android24") and rewrite the trailing API number to 28.
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE _agentty_android_triple
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_agentty_android_triple MATCHES "^(.+-linux-android)[0-9]*$")
        set(_agentty_android_target "${CMAKE_MATCH_1}28")
    elseif(_agentty_android_triple MATCHES "android")
        set(_agentty_android_target "${_agentty_android_triple}")
    else()
        # Fallback: derive from the processor if -dumpmachine gave us nothing
        # android-shaped (unusual). aarch64 is the overwhelming Termux case.
        set(_agentty_android_target "aarch64-linux-android28")
    endif()
    message(STATUS "agentty: Android/Termux detected — deploying at "
                   "${_agentty_android_target} (exposes spawn.h + strtod_l)")
    add_compile_options("--target=${_agentty_android_target}")
    add_link_options("--target=${_agentty_android_target}")
    # Keep the preprocessor macro in lock-step for any code that reads it
    # directly (undef first so the toolchain's own -D doesn't conflict).
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-U__ANDROID_API__>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-D__ANDROID_API__=28>")

    # BELT-AND-SUSPENDERS: also bake the target + API macro straight into
    # CMAKE_<LANG>_FLAGS. add_compile_options() only reaches targets defined
    # in THIS directory and subdirectories added AFTER the call; anything
    # that slips in via a different mechanism (a FetchContent project that
    # resets flags, a submodule that calls project() and re-detects the
    # compiler, an out-of-order add_subdirectory) can miss it. CMAKE_*_FLAGS
    # is string-prepended to EVERY compile in EVERY (sub)directory
    # unconditionally, so mcp-cpp / maya / acp-cpp all get the deployment
    # target no matter how their own CMake is structured. Idempotent: guard
    # against double-append on reconfigure.
    set(_agentty_android_flags
        "--target=${_agentty_android_target} -U__ANDROID_API__ -D__ANDROID_API__=28")
    if(NOT CMAKE_CXX_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_agentty_android_flags}")
    endif()
    if(NOT CMAKE_C_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_agentty_android_flags}")
    endif()
    if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} --target=${_agentty_android_target}")
    endif()
    if(NOT CMAKE_SHARED_LINKER_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} --target=${_agentty_android_target}")
    endif()
endif()

# ── Submodule auto-pull ────────────────────────────────────────────────────
# When AGENTTY_AUTO_PULL_SUBMODULES is ON (default), every in-tree submodule
# is synced to the tip of its tracking branch on each build before its library
# target compiles. SAFE: the pull is skipped for any submodule that has
# uncommitted changes, so local edits are never clobbered. Skipped entirely
# if the parent isn't a git checkout (release tarballs / FetchContent paths).
#
# Call AFTER add_subdirectory(<sub>) so the library target exists to depend on.
#   agentty_pull_submodule_latest(<dir> <branch> <library-target>)
option(AGENTTY_AUTO_PULL_SUBMODULES
       "Sync every in-tree submodule to its tracking branch on every build" OFF)
# Back-compat: the old maya-only switch still disables maya's pull when OFF.
option(AGENTTY_AUTO_PULL_MAYA "(deprecated alias) pull maya on every build" OFF)

function(agentty_pull_submodule_latest sub branch lib)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        return()
    endif()
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        return()
    endif()
    set(_sd "${CMAKE_CURRENT_SOURCE_DIR}/${sub}")
    # The guard is deliberately paranoid so a developer's LOCAL work in the
    # submodule is never clobbered by the reset --hard. We refuse to pull if
    # ANY of these hold:
    #   • unstaged changes            (git diff --quiet)
    #   • staged-but-uncommitted work (git diff --cached --quiet)
    #   • local commits not on origin (HEAD != origin/<branch> after fetch,
    #     with local commits reachable only from HEAD)
    # Only a perfectly clean tree sitting on (or behind) origin gets fast-
    # forwarded. `git merge --ff-only` after the clean-tree check does exactly
    # that: it advances to the newest commit but ERRORS OUT (harmlessly, the
    # || branch swallows it) rather than discarding your local history.
    #
    # This target is NEVER part of `all`. It runs only when you ask for it,
    # either via the aggregate `submodules_sync` target (see below) or, when
    # AGENTTY_AUTO_PULL_SUBMODULES=ON, as a per-build dependency of ${lib}.
    add_custom_target(${sub}_pull_latest
        COMMAND ${CMAKE_COMMAND} -E echo "[${sub}] checking origin/${branch}…"
        COMMAND ${GIT_EXECUTABLE} -C ${_sd} diff --quiet
            && ${GIT_EXECUTABLE} -C ${_sd} diff --cached --quiet
            && ${GIT_EXECUTABLE} -C ${_sd} fetch --quiet origin ${branch}
            && ${GIT_EXECUTABLE} -C ${_sd} merge --ff-only --quiet origin/${branch}
            || ${CMAKE_COMMAND} -E echo
               "[${sub}] local changes/commits present — keeping your tree, skipping auto-pull"
        COMMENT "Syncing ${sub}/ to origin/${branch} (safe: never discards local work)"
        VERBATIM
        USES_TERMINAL
    )
    # Aggregate on-demand sync: `cmake --build <dir> --target submodules_sync`
    # (or `ninja submodules_sync`) pulls EVERY submodule at once. Created lazily
    # on the first submodule so we don't need to know the full set up front.
    if(NOT TARGET submodules_sync)
        add_custom_target(submodules_sync
            COMMENT "Fast-forwarding all in-tree submodules to their tracking branches")
    endif()
    add_dependencies(submodules_sync ${sub}_pull_latest)
    # Opt-in per-build auto-pull. OFF by default: a normal build must never
    # fetch or reset a submodule (that perturbs header mtimes and needlessly
    # rebuilds the ~64 TUs that include it). When ON, the pull runs before the
    # library compiles so freshly-pulled sources are picked up the same build.
    # rag-cpp additionally honours its own AGENTTY_AUTO_PULL_RAGCPP override.
    set(_auto_pull ${AGENTTY_AUTO_PULL_SUBMODULES})
    if(sub STREQUAL "rag-cpp" AND AGENTTY_AUTO_PULL_RAGCPP)
        set(_auto_pull ON)
    endif()
    if(sub STREQUAL "maya" AND AGENTTY_AUTO_PULL_MAYA)
        set(_auto_pull ON)
    endif()
    if(_auto_pull AND TARGET ${lib})
        add_dependencies(${lib} ${sub}_pull_latest)
    endif()
endfunction()

# Prefer the in-tree submodule when present; only fall back to fetch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/maya/CMakeLists.txt")
    add_subdirectory(maya)
    # Called unconditionally so maya joins the on-demand `submodules_sync`
    # target; per-build auto-pull stays gated inside the function.
    agentty_pull_submodule_latest(maya master maya)
else()
    FetchContent_Declare(
        maya
        GIT_REPOSITORY https://github.com/1ay1/maya.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(maya)
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# simdjson — used on the SSE hot path (content_block_delta) where we parse
# hundreds of small JSON docs per second during streaming. nlohmann is fine
# for config + cold events; simdjson's ondemand API is 3–5× faster for the
# "open doc, read two fields, throw away" pattern that dominates here.
set(SIMDJSON_DEVELOPER_MODE   OFF CACHE INTERNAL "")
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json simdjson)

# doctest — unit-test framework backing the single-binary test model. Chosen
# for the lowest compile overhead of the mainstream C++ frameworks, and its
# assertion macro is already named CHECK (matching every repo's hand-rolled
# harness), so migration is mechanical. Every test TU becomes a TEST_CASE
# auto-registered into ONE `agentty_tests` executable that links the shared
# object set once — instead of ~70 executables each re-linking it (that link
# fan-out was the dominant CI cost). Fetched only when tests are built.
if(AGENTTY_BUILD_TESTS)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
    )
    set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
    set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
    # doctest 2.4.11 declares cmake_minimum_required(VERSION 3.0), which CMake
    # 4.x rejects. Allow the old floor just for this dependency.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
    FetchContent_MakeAvailable(doctest)
    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
endif()

# Mark third-party headers SYSTEM so their warnings don't pollute our build.
# nlohmann_json v3.11.3's binary_writer.hpp uses std::is_trivial which GCC 15
# deprecates under C++26 — the upstream fix is on master but unreleased, and we
# don't want every TU that includes <nlohmann/json.hpp> to re-emit the warning.
if(TARGET nlohmann_json)
    get_target_property(_nj_iface nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    if(_nj_iface)
        set_target_properties(nlohmann_json PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nj_iface}")
    endif()
endif()
if(TARGET simdjson)
    get_target_property(_sj_iface simdjson INTERFACE_INCLUDE_DIRECTORIES)
    if(_sj_iface)
        set_target_properties(simdjson PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_sj_iface}")
    endif()
endif()

# acp-cpp — header-only Agent Client Protocol library (submodule). Provides
# the wire algebra + JSON-RPC engine + stdio transport for `agentty acp`.
# Tests/examples off; it reuses the nlohmann_json target already populated
# above (its own FetchContent_MakeAvailable is a no-op when already present).
set(ACP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ACP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(acp-cpp)
agentty_pull_submodule_latest(acp-cpp main acp)
if(TARGET acp)
    set_target_properties(acp PROPERTIES SYSTEM TRUE)
endif()

# mcp-cpp — the Model Context Protocol library (submodule), and now the SOLE
# source of agentty's local tool implementations. agentty's entire tool set
# (read/write/edit/list_dir, bash, grep/glob/find_definition, diagnostics,
# git_*, web_*, plus the host-coupled remember/forget/wipe/todo/skill/
# search_docs/task SHELLS) is served by mcp-cpp's batteries-included toolset
# and re-wrapped through src/tool/mcp_tools_bridge.cpp. It is therefore a HARD
# build requirement — there is no native tool path to fall back to.
option(AGENTTY_MCP "Build MCP integration (REQUIRED — agentty's tools live in mcp-cpp)" ON)
if(NOT AGENTTY_MCP)
    message(FATAL_ERROR
        "agentty: AGENTTY_MCP=OFF is no longer supported — the tool set is "
        "served exclusively by the mcp-cpp toolset. Configure with "
        "-DAGENTTY_MCP=ON (the default).")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mcp-cpp/CMakeLists.txt")
    message(FATAL_ERROR
        "agentty: the mcp-cpp submodule is missing but it now provides the "
        "entire tool set. Run `git submodule update --init --recursive`.")
endif()
# mcp-cpp's EXAMPLE server rides along (mcp_bridge_test's real spawn+handshake
# e2e needs it), but its TEST binaries do NOT: each repo runs its own tests.
# mcp-cpp's own CI gates its fs/search/web/toolset/codec/protocol suite. agentty
# only needs the mcp LIBRARY here, not mcp_tests in agentty's ctest run.
set(MCP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_EXAMPLES ON  CACHE BOOL "" FORCE)
add_subdirectory(mcp-cpp)
agentty_pull_submodule_latest(mcp-cpp master mcp)
if(TARGET mcp)
    set_target_properties(mcp PROPERTIES SYSTEM TRUE)
endif()

if(NOT TARGET maya::maya)
    add_library(maya::maya ALIAS maya)
endif()

# Treat maya's headers as system so its warnings don't surface in agentty builds.
set_target_properties(maya PROPERTIES SYSTEM TRUE)

# Same treatment for simdjson's headers — upstream's `operator "" _padded`
# trips -Wdeprecated-literal-operator under C++23, and it isn't our bug to
# fix. SYSTEM suppresses diagnostics from the included headers.
if(TARGET simdjson)
    set_target_properties(simdjson PROPERTIES SYSTEM TRUE)
endif()
if(TARGET simdjson_static)
    set_target_properties(simdjson_static PROPERTIES SYSTEM TRUE)
endif()

find_package(Threads REQUIRED)

# OpenSSL: if AGENTTY_STANDALONE asked for static and the static archive is
# missing, retry with shared libs and flag the fallback so the user sees
# a clear note at the end of configure.
if(AGENTTY_STANDALONE)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        unset(OPENSSL_USE_STATIC_LIBS)
        unset(OPENSSL_LIBRARIES CACHE)
        unset(OPENSSL_CRYPTO_LIBRARY CACHE)
        unset(OPENSSL_SSL_LIBRARY CACHE)
        find_package(OpenSSL REQUIRED)
        set(AGENTTY_STANDALONE_OPENSSL_FALLBACK TRUE)
    endif()
else()
    find_package(OpenSSL REQUIRED)
endif()

# nghttp2 — HTTP/2 protocol engine for the in-house http client. Prefer the
# upstream CMake config (vcpkg / Homebrew / nghttp2's own export); fall back
# to pkg-config (Linux distros), then a manual find_path/find_library scan.
find_package(nghttp2 CONFIG QUIET)
# Skip pkg-config for fully-static builds: pkg-config returns the dynamic
# library path by default (libnghttp2.so), which the `-static` link below
# can't accept ("attempted static link of dynamic object").  The manual
# find_library path further down (gated on AGENTTY_STANDALONE) explicitly
# prefers libnghttp2.a, so let it take over.
if(NOT TARGET nghttp2::nghttp2 AND NOT AGENTTY_FULLY_STATIC)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NGHTTP2 IMPORTED_TARGET libnghttp2)
        if(TARGET PkgConfig::NGHTTP2)
            add_library(nghttp2::nghttp2 ALIAS PkgConfig::NGHTTP2)
        endif()
    endif()
endif()
# mimalloc — Microsoft's general-purpose allocator. Pinned to a STABLE release
# tag (below), fetched by CMake and compiled from its C sources into a static
# library. This is not header-only.
#
# DISABLED ON APPLE (macOS): mimalloc's malloc override on macOS works by
# interposing the system malloc ZONE. On macOS 26 / Apple clang 21 this is
# broken — startup emits `mimalloc: warning: unable to allocate aligned OS
# memory directly` and then the process aborts with `pointer being freed was
# not allocated`: some allocations go through mimalloc while the matching
# operator delete routes to the SYSTEM allocator, corrupting the heap. It
# reproduced 25/25 on a normal Release build (and 25/25 even after pinning
# to the v3.4.5 release, so it is NOT a HEAD-only regression — the zone
# interposition itself is unusable here), and was pinpointed under ASan to a
# std::string grow inside fs::path::operator/= in auth.cpp at startup.
# Disabling mimalloc fixed it 25/25. The allocator only ever bought us page
# reclamation via mi_collect(); on Apple we fall back to the system
# allocator (release_to_kernel() is a no-op on macOS anyway — see mem.hpp).
#
# The tag is pinned rather than tracking `main`/HEAD so a random upstream
# commit can't brick every build on the platforms where mimalloc IS enabled.
set(AGENTTY_MIMALLOC_TAG "v3.4.5" CACHE STRING
    "mimalloc git tag to fetch. Pin to a stable release; do NOT track main/HEAD.")
if(APPLE AND AGENTTY_USE_MIMALLOC)
    message(STATUS "agentty: mimalloc DISABLED on Apple — its macOS malloc-zone "
                   "override corrupts the heap; using the system allocator.")
    set(AGENTTY_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
endif()
set(AGENTTY_HAS_MIMALLOC FALSE)
if(AGENTTY_USE_MIMALLOC)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE     ON CACHE BOOL "" FORCE)
    # Standalone builds target broad CPU compatibility; prevent mimalloc from
    # selecting an armv8.1-a baseline on arm64.
    if(AGENTTY_STANDALONE)
        set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
    endif()
    # We pin to a tag, so it never needs re-fetching; keep FetchContent from
    # re-running the git update step on every configure.
    set(FETCHCONTENT_UPDATES_DISCONNECTED_MIMALLOC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG        ${AGENTTY_MIMALLOC_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(mimalloc)
    set(AGENTTY_HAS_MIMALLOC TRUE)
endif()

# ── rag-cpp: the retrieval (RAG) engine ────────────────────────────────────
# agentty's retrieval is powered by the external rag-cpp library (submodule
# rag-cpp/, target ragcpp::ragcpp) — a production-grade hybrid engine
# (contextual chunking, BM25 + dense/HNSW, RRF fusion, CRAG, HyDE, MMR /
# dartboard rerank, GraphRAG, .ragdb persistence). The thin adapter in
# src/rag/adapter.cpp maps agentty's retrieval boundary onto rag::Engine, so
# the rest of the app never sees a rag:: type. Built from source in-tree so
# agentty carries its RAG engine with it and compiles ANYWHERE.
set(AGENTTY_HAS_RAGCPP FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rag-cpp/CMakeLists.txt")
    # rag-cpp is portable across GCC, Clang, MinGW, and MSVC. Its durability
    # layer uses posix_compat.hpp on Windows, while SIMD/prefetch kernels use
    # compiler-specific wrappers and retain scalar/runtime-dispatched fallbacks.
    # Keep retrieval enabled in the official MSVC release: a platform package
    # must not silently replace search_docs/search_code with no-op stubs.
    set(RAGCPP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(RAGCPP_BUILD_BENCH    OFF CACHE BOOL "" FORCE)
    set(RAGCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RAGCPP_BUILD_CLI      OFF CACHE BOOL "" FORCE)
    set(RAGCPP_WITH_RCP       OFF CACHE BOOL "" FORCE)
    # No GPU backends. agentty's retrieval is CPU HNSW + BM25; it never issues
    # the batch-score call the GPU paths accelerate. Leaving them at rag-cpp's
    # defaults is a liability, not a feature:
    #   * METAL defaults ON on Apple and enable_language(OBJCXX) drags Apple
    #     clang into the build via a try_compile probe. That probe inherits our
    #     CMAKE_EXE_LINKER_FLAGS (the GCC-static release passes -static-libgcc /
    #     -static-libstdc++), and Apple clang rejects -static-libgcc — so the
    #     whole macOS standalone configure died at rag-cpp/CMakeLists.txt.
    #   * OPENCL auto-detects any system libOpenCL and would silently add a
    #     dynamic dependency to a binary that's meant to be standalone.
    set(RAGCPP_WITH_METAL     OFF CACHE BOOL "" FORCE)
    set(RAGCPP_WITH_OPENCL    OFF CACHE BOOL "" FORCE)
    add_subdirectory(rag-cpp EXCLUDE_FROM_ALL)
    set(AGENTTY_HAS_RAGCPP TRUE)
    # Opt-in upstream tracking, off by default so local rag-cpp edits compile
    # without CMake changing the submodule checkout. Called UNCONDITIONALLY so
    # rag-cpp is always part of the on-demand `submodules_sync` target; the
    # per-build auto-pull stays gated inside the function by
    # AGENTTY_AUTO_PULL_SUBMODULES (and the rag-specific override below).
    option(AGENTTY_AUTO_PULL_RAGCPP "Fast-forward rag-cpp/ to origin/master on every build" OFF)
    agentty_pull_submodule_latest(rag-cpp master ragcpp)
else()
    message(FATAL_ERROR "agentty: rag-cpp/ submodule is empty. Run "
                        "`git submodule update --init --recursive` to vendor "
                        "the RAG engine, then reconfigure.")
endif()

if(NOT TARGET nghttp2::nghttp2)
    find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
    # Standalone builds prefer the static archive (libnghttp2.a /
    # nghttp2_static.lib); regular builds prefer the shared library so
    # devs don't need a static archive installed.
    if(AGENTTY_STANDALONE)
        if(MSVC)
            find_library(NGHTTP2_LIBRARY NAMES nghttp2_static nghttp2)
        else()
            find_library(NGHTTP2_LIBRARY NAMES libnghttp2.a nghttp2_static nghttp2)
        endif()
    else()
        find_library(NGHTTP2_LIBRARY NAMES nghttp2 nghttp2_static)
    endif()
    if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
        add_library(nghttp2::nghttp2 UNKNOWN IMPORTED)
        set_target_properties(nghttp2::nghttp2 PROPERTIES
            IMPORTED_LOCATION "${NGHTTP2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "nghttp2 not found — install libnghttp2-dev (Debian/Ubuntu), "
            "nghttp2 (Homebrew/Arch), or vcpkg install nghttp2.")
    endif()
endif()

# ── Source groups, by domain ──────────────────────────────────────────────
# The header tree under include/agentty/ mirrors this grouping one-for-one:
#   domain/    — pure value types (no I/O, no UI). Headers only.
#   io/        — sockets, TLS, HTTP/2, OAuth, on-disk persistence, OS
#                clipboard. Anything that talks to the kernel or the
#                wire and isn't tied to a specific provider.
#   provider/  — wire-format adapters for upstream LLM APIs.
#   diff/      — unified-diff parser used by the edit tool & review modal.
#   tool/      — the agent's capability surface: registry + per-tool impls.
#   workspace/ — pure-I/O scanners over the active workspace root (file
#                enumeration for @mention, symbol enumeration for #).
#                Consumed by the runtime's pickers but separate from the
#                UI state they feed.
#   airgap/    — the `agentty airgap` CLI subcommand (exec's into ssh,
#                never returns).  Not part of the maya runtime.
#   runtime/   — the Elm-style app: model, update, subscriptions, view tree.

set(AGENTTY_IO_SOURCES
    src/io/http.cpp
    src/io/tls.cpp
    src/io/auth.cpp
    src/io/accounts.cpp
    src/io/account_switch.cpp
    src/io/cred_crypt.cpp
    src/io/keystore.cpp
    src/io/persistence.cpp
    src/io/clipboard.cpp
    src/util/base64.cpp
    src/util/dbglog.cpp
    src/domain/complexity.cpp
    src/domain/routing_memory.cpp
    src/domain/decomposition_memory.cpp
)

set(AGENTTY_WORKSPACE_SOURCES
    src/workspace/files.cpp
    src/workspace/symbols.cpp
    src/workspace/checkpoint.cpp
)

set(AGENTTY_AIRGAP_SOURCES
    src/airgap/airgap.cpp
)

set(AGENTTY_PROVIDER_SOURCES
    src/provider/anthropic/transport.cpp
    src/provider/anthropic/sse.cpp
    src/provider/anthropic/wire_body.cpp
    src/provider/anthropic/prompt.cpp
    src/provider/chatgpt/provider.cpp
    src/provider/chatgpt/codex_oauth.cpp
    src/provider/chatgpt/responses.cpp
    src/provider/copilot/provider.cpp
    src/provider/copilot/copilot_oauth.cpp
    src/provider/openai/transport.cpp
    src/provider/ollama/transport.cpp
    src/provider/selection.cpp
    src/provider/prompt_policy.cpp
    # The ONE provider-routing seam — dispatch_stream(). Provider-agnostic
    # (type-erased Routes, incl. the ACP arm), so it has no acp-TU dependency
    # and lives in the provider objlib every test links. main.cpp binds the
    # long-lived + external routes; dispatch just routes on the Selection.
    src/provider/dispatch.cpp
    # ACP agent launch registry (config loader). Pure nlohmann/json, no acp-cpp
    # dep, and referenced by selection.cpp (is_acp_agent_id) — so it lives in
    # the provider objlib every test links, NOT in agentty_acp_obj.
    src/provider/acp_agents.cpp
)

# ACP (Agent Client Protocol) — lets agentty run as a headless agent
# subprocess that Zed (or any ACP client) drives over JSON-RPC on stdio.
# The wire protocol/engine/transport live in the acp-cpp submodule (linked
# as acp::acp); server.cpp is the agentty-specific glue (turn loop + tools).
set(AGENTTY_ACP_SOURCES
    src/acp/server.cpp
    src/provider/external_acp_backend.cpp
    src/provider/acp_provider_adapter.cpp
)

# MCP (Model Context Protocol) client glue — spawns external MCP servers and
# exposes their tools as agentty ToolDefs. The heavy mcp-cpp templates are
# confined to these TUs (linked as mcp::mcp). Only compiled when AGENTTY_MCP.
set(AGENTTY_MCP_SOURCES
    src/mcp/bridge.cpp
    src/mcp/http_server.cpp
    src/mcp/serve.cpp
    src/mcp/oauth.cpp
    src/tool/mcp_tools_bridge.cpp
    src/tool/mcp_tools_backends.cpp
)

set(AGENTTY_DIFF_SOURCES
    src/diff/diff.cpp
)

# The RAG engine is now the external rag-cpp library (submodule rag-cpp/,
# target ragcpp::ragcpp). Only the thin agentty adapter that maps the app's
# retrieval boundary onto rag::Engine remains in-tree.
set(AGENTTY_RAG_SOURCES
    src/rag/adapter.cpp
)

set(AGENTTY_TOOL_SOURCES
    src/tool/registry.cpp
    src/scope/scope.cpp
    src/scope/trust.cpp
    src/tool/progress.cpp
    src/tool/util/utf8.cpp
    src/tool/util/fs_helpers.cpp
    src/tool/util/subprocess.cpp
    src/tool/util/sandbox.cpp
    src/tool/util/arg_reader.cpp
    src/tool/util/partial_json.cpp
    src/tool/subagent.cpp
    src/tool/skills.cpp
    src/tool/commands.cpp
    src/tool/hooks.cpp
    src/tool/plugin.cpp
    src/tool/memory_store.cpp
)

# Everything except the entry point — so tests can link the same runtime
# without fighting main().
set(AGENTTY_RUNTIME_NOMAIN_SOURCES
    src/runtime/composer_attachment.cpp
    src/runtime/app/deps.cpp
    src/runtime/app/init.cpp
    src/runtime/app/cmd_factory.cpp
    src/runtime/app/update.cpp
    src/runtime/app/update/composer.cpp
    src/runtime/app/update/stream.cpp
    src/runtime/app/update/stream_preview.cpp
    src/runtime/app/update/modal.cpp
    src/runtime/app/update/frozen.cpp
    src/runtime/app/update/tool.cpp
    src/runtime/app/update/login.cpp
    src/runtime/app/update/picker.cpp
    src/runtime/app/update/palette.cpp
    src/runtime/app/update/mention.cpp
    src/runtime/app/update/symbol.cpp
    src/runtime/app/update/codeblock.cpp
    src/runtime/app/update/checkpoint.cpp
    src/runtime/app/update/rag_settings.cpp
    src/runtime/app/update/settings_list.cpp
    src/runtime/settings_items.cpp
    src/runtime/app/update/fork.cpp
    src/runtime/app/update/diff.cpp
    src/runtime/app/update/meta.cpp
    src/runtime/app/subscribe.cpp

    src/runtime/view/cache.cpp
    src/runtime/view/helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_args.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_body_common.cpp
    src/runtime/view/thread/turn/agent_timeline/edit_body.cpp
    src/runtime/view/thread/turn/agent_timeline/bash_body.cpp
    src/runtime/view/thread/turn/agent_timeline/write_body.cpp
    src/runtime/view/thread/turn/agent_timeline/git_diff_body.cpp
    src/runtime/view/thread/turn/agent_timeline/read_body.cpp
    src/runtime/view/thread/turn/agent_timeline/web_fetch_body.cpp
    src/runtime/view/thread/turn/agent_timeline/list_body.cpp
    src/runtime/view/thread/turn/agent_timeline/task_body.cpp
    src/runtime/view/thread/turn/agent_timeline/todo_body.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_body_preview.cpp
    src/runtime/view/thread/turn/agent_timeline/agent_timeline.cpp
    src/runtime/view/thread/turn/permission.cpp
    src/runtime/view/thread/turn/turn.cpp
    src/runtime/view/thread/welcome_screen.cpp
    src/runtime/view/thread/activity_indicator.cpp
    src/runtime/view/thread/conversation.cpp
    src/runtime/view/thread/thread.cpp
    src/runtime/view/composer.cpp
    src/runtime/view/status_bar/title_chip.cpp
    src/runtime/view/status_bar/phase_chip.cpp
    src/runtime/view/status_bar/token_stream_sparkline.cpp
    src/runtime/view/status_bar/context_gauge.cpp
    src/runtime/view/status_bar/status_banner.cpp
    src/runtime/view/status_bar/model_badge.cpp
    src/runtime/view/status_bar/status_bar.cpp
    src/runtime/view/changes_strip.cpp
    src/runtime/view/pickers.cpp
    src/runtime/view/rag_settings_view.cpp
    src/runtime/view/settings_list_view.cpp
    src/runtime/view/fork_view.cpp
    src/runtime/view/diff_review.cpp
    src/runtime/view/login.cpp
    src/runtime/view/view.cpp
)

set(AGENTTY_RUNTIME_SOURCES
    src/runtime/main.cpp
    ${AGENTTY_RUNTIME_NOMAIN_SOURCES}
)

# ── Shared compile flags ────────────────────────────────────────────────
# All agentty TUs (the main binary, the shared OBJECT libraries below, and
# every test) MUST compile with byte-identical flags. The arch flag
# (-march / /arch) bakes intrinsic selection and ABI into each .o; mixing
# objects built with different flags is undefined. Centralise here so the
# OBJECT libs and the exe can't drift.
function(agentty_apply_compile_flags tgt)
if(MSVC)
    target_compile_options(${tgt} PRIVATE
        /W4
        /utf-8                 # treat source and exec charsets as UTF-8
        /std:c++latest         # opt into C++26 library bits beyond /std:c++23
        /permissive-           # strict conformance
        /Zc:preprocessor       # conforming preprocessor
        /Zc:__cplusplus        # report real __cplusplus value
        /Zc:inline             # drop unreferenced COMDATs at compile time
        /Zc:throwingNew        # assume ::new never returns null
        /EHsc
        /bigobj                # maya's templates blow past default sections
        /MP                    # parallel compilation across TUs
        /wd4100                # unreferenced formal parameter — common in lambdas
        /wd4127                # conditional expression is constant (if constexpr paths)
        /wd4324                # structure padded due to alignment specifier
        # ── Release-only aggressive optimization ─────────────────────
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:
            /O2                #  max-speed optimization
            /Ob3               #  aggressive inlining beyond /Ob2
            /Oi                #  intrinsic functions
            /Ot                #  favor speed over size
            /Oy                #  omit frame pointer (frees a GPR)
            /GL                #  whole-program optimization (pairs with /LTCG)
            /GF                #  eliminate duplicate strings
            /Gy                #  function-level linking (linker /OPT:ICF/REF fodder)
            /Gw                #  package globals for linker to fold/strip
            /GS-               #  no stack-buffer cookies — TUI, not a daemon
            /GR                #  keep RTTI (context.hpp uses typeid)
            /fp:fast           #  relax FP strictness — no errno / reassociation
            $<$<STREQUAL:${AGENTTY_ARCH},avx2>:/arch:AVX2>    # Haswell+ / Zen1+
            $<$<STREQUAL:${AGENTTY_ARCH},avx>:/arch:AVX>      # Sandy/Ivy Bridge
            # MSVC has no /arch:SSE2 (it's the default on x64) or /arch:native;
            # both map to "no flag" — the default x64 codegen already assumes
            # SSE2. `native` on MSVC degrades to the default baseline.
            /Qpar              #  enable auto-parallelizer for hot loops
        >
    )
    target_compile_definitions(${tgt} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>
    )
else()
    target_compile_options(${tgt} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-deprecated-declarations
        # Designated init like `TextElement{.content=…, .style=…}` is the
        # intentional pattern across the view layer — leaving cache fields
        # default-initialized is correct, not a bug. -Wmissing-field-
        # initializers (a -Wextra default) doesn't model that.
        -Wno-missing-field-initializers
    )
    # Clang 18+ split partial *designated* initializers into their own
    # warning that -Wno-missing-field-initializers no longer covers. Same
    # intentional pattern, same benign default-init — suppress it too.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${tgt} PRIVATE
            -Wno-missing-designated-field-initializers)
    endif()
    # GCC -Wmaybe-uninitialized produces false positives on std::variant moves
    # of designated-initialized aggregates (maya's Element{TextElement{...}} pattern).
    # The warnings escape -isystem because they fire during late optimization.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${tgt} PRIVATE -Wno-maybe-uninitialized)
    endif()

    # macOS SDK + GCC: <mach/port.h> uses `_Static_assert` (C keyword) in
    # arm64 macros that fire from any TU pulling in mach headers (e.g.
    # subprocess.cpp via <spawn.h>). Alias the C spelling to its C++
    # equivalent so the SDK headers parse under GCC's C++ frontend.
    if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_definitions(${tgt} PRIVATE
            _Static_assert=static_assert)
    endif()
endif()
endfunction()

# ── Directory-wide sanitizer (instruments EVERYTHING, incl. tests) ─────────
# The target-scoped AGENTTY_SANITIZE below only instruments the `agentty`
# exe. But the memory-safety bugs a Rust advocate points at (use-after-free,
# buffer overrun, UB) are exercised by the TEST SUITE, which links the shared
# OBJECT libraries — not the exe. To catch them we need the sanitizer on the
# object libs and the test binaries too. AGENTTY_SANITIZE_ALL does that by
# injecting the flags DIRECTORY-WIDE, here, BEFORE any target is defined, so
# every agentty TU (objlibs + exe + tests) and their links carry it.
#
#   cmake -B build-asan -DAGENTTY_SANITIZE_ALL=address,undefined \
#         -DAGENTTY_BUILD_TESTS=ON && cmake --build build-asan --target tests \
#         && ctest --test-dir build-asan
#
# This is agentty's Rust-grade memory-safety GATE: the borrow checker proves
# absence of these bugs at compile time; we prove it by running the whole
# suite under ASan+UBSan. Different mechanism, same guarantee for the paths
# the tests cover.
set(AGENTTY_SANITIZE_ALL "" CACHE STRING
    "Comma-separated sanitizers applied to ALL agentty TUs incl. tests (e.g. address,undefined). Empty to disable.")
if(AGENTTY_SANITIZE_ALL AND NOT MSVC)
    message(STATUS "agentty: WHOLE-TREE sanitizer -fsanitize=${AGENTTY_SANITIZE_ALL} "
                   "(objlibs + exe + tests)")
    add_compile_options(-fsanitize=${AGENTTY_SANITIZE_ALL}
                        -fno-omit-frame-pointer -g -O1 -fno-lto)
    add_link_options(-fsanitize=${AGENTTY_SANITIZE_ALL} -fno-lto)
    # GCC's sanitizers instrument the module-level `constexpr std::array`
    # catalogs (spec.hpp kCatalog, etc.), and that instrumentation leaks
    # poisoned pointer arithmetic into the `consteval` evaluation of the
    # static_assert PROOFS that walk them — they then fail to compile with
    # "not a constant expression". This is a GCC limitation, not a proof bug:
    # the SAME proofs compile and pass in every normal (non-sanitizer) build,
    # which is the primary gate. So we define AGENTTY_SANITIZER_BUILD and let
    # the few catalog-walking proof blocks skip themselves ONLY in the
    # sanitizer build — the sanitizer's job is to check RUNTIME memory safety
    # (UAF / overflow / UB), not to re-run compile-time proofs that already
    # ran green elsewhere.
    add_compile_definitions(AGENTTY_SANITIZER_BUILD=1)
endif()

# ── Shared OBJECT libraries ─────────────────────────────────────────────
# Compile every shared TU EXACTLY ONCE into an OBJECT library, then reuse
# the objects (via $<TARGET_OBJECTS:...>) in the main binary AND every
# test. Before this, each of the ~20 test targets recompiled the full
# provider+tool+runtime source set into its own object dir — a clean
# `--target tests` rebuilt the entire codebase ~20 times. With OBJECT
# libraries the shared cost is paid once; a test rebuild is just its own
# .cpp + a link.
#
# `agentty_objlib(NAME src...)` defines the lib, wires the include dir +
# AGENTTY_VERSION define + the project compile flags, and links the
# header-providing libs as INTERFACE deps so transitive #includes resolve
# during compilation (OBJECT libs don't link, but they DO need the
# headers). maya/json/simdjson/nghttp2/openssl all expose their include
# dirs through their imported targets.
function(agentty_objlib name)
    add_library(${name} OBJECT ${ARGN})
    target_include_directories(${name} PRIVATE include)
    target_compile_definitions(${name} PRIVATE AGENTTY_VERSION="${PROJECT_VERSION}")
    # MCP integration is compile-gated. When ON, every objlib sees the macro
    # (registry.cpp branches on it) and the mcp-cpp INTERFACE include dir (so
    # the agentty-facing header chain resolves); only agentty_mcp_obj actually
    # pulls the heavy <mcp/*.hpp> templates in.
    if(AGENTTY_MCP)
        target_compile_definitions(${name} PRIVATE AGENTTY_MCP=1)
        if(TARGET mcp::mcp)
            target_link_libraries(${name} PRIVATE mcp::mcp)
        endif()
        # The tool set is served by the mcp-cpp toolset; fs_helpers.cpp (in
        # agentty_tool_obj) now mirrors the workspace root into mcp's util
        # layer, so every objlib needs mcp::tools' include dir resolvable.
        if(TARGET mcp::tools)
            target_link_libraries(${name} PRIVATE mcp::tools)
        endif()
    else()
        target_compile_definitions(${name} PRIVATE AGENTTY_MCP=0)
    endif()
    agentty_apply_compile_flags(${name})
    # mem.hpp includes <mimalloc.h> when the allocator is enabled. OBJECT libs
    # need the include path and compile definition even though final linking is
    # performed by the executable target.
    if(AGENTTY_HAS_MIMALLOC)
        target_link_libraries(${name} PRIVATE mimalloc-static)
        target_compile_definitions(${name} PRIVATE AGENTTY_USE_MIMALLOC=1)
    endif()
    # The RAG adapter (agentty_rag_obj) includes <rag/rag.hpp>; other objlibs
    # only need the header path resolvable for transitive includes. Linking
    # the target as an INTERFACE dep gives every objlib ragcpp's include dir.
    if(AGENTTY_HAS_RAGCPP)
        target_link_libraries(${name} PRIVATE ragcpp::ragcpp)
        target_compile_definitions(${name} PRIVATE AGENTTY_HAS_RAGCPP=1)
    else()
        target_compile_definitions(${name} PRIVATE AGENTTY_HAS_RAGCPP=0)
    endif()
    # Header-only access to the linked libs (no actual linking for OBJECT).
    target_link_libraries(${name} PRIVATE
        maya::maya
        nlohmann_json::nlohmann_json
        simdjson::simdjson
        nghttp2::nghttp2
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
endfunction()

agentty_objlib(agentty_io_obj        ${AGENTTY_IO_SOURCES})
agentty_objlib(agentty_workspace_obj ${AGENTTY_WORKSPACE_SOURCES})
agentty_objlib(agentty_airgap_obj    ${AGENTTY_AIRGAP_SOURCES})
agentty_objlib(agentty_provider_obj  ${AGENTTY_PROVIDER_SOURCES})
agentty_objlib(agentty_diff_obj      ${AGENTTY_DIFF_SOURCES})
agentty_objlib(agentty_rag_obj       ${AGENTTY_RAG_SOURCES})
agentty_objlib(agentty_tool_obj      ${AGENTTY_TOOL_SOURCES})
agentty_objlib(agentty_runtime_obj   ${AGENTTY_RUNTIME_NOMAIN_SOURCES})
# ACP glue is only needed by the exe + acp_integration_test. It also
# needs the acp::acp include dir (acp/acp.hpp) on top of the standard set.
agentty_objlib(agentty_acp_obj       ${AGENTTY_ACP_SOURCES})
target_link_libraries(agentty_acp_obj PRIVATE acp::acp)

# MCP glue — only built when AGENTTY_MCP is on. Confines the heavy mcp-cpp
# template instantiation to these two TUs; everything else just sees the
# light agentty-facing header. Added to the shared object set below so the
# exe (and any full test that calls tools::registry()) links it.
if(AGENTTY_MCP)
    agentty_objlib(agentty_mcp_obj   ${AGENTTY_MCP_SOURCES})
    target_link_libraries(agentty_mcp_obj PRIVATE mcp::mcp)
    if(TARGET mcp::tools)
        target_link_libraries(agentty_mcp_obj PRIVATE mcp::tools)
    endif()
endif()

# The shared object set every "full" test (and the exe) consumes — the
# whole runtime minus main.cpp and minus the ACP glue.
set(AGENTTY_SHARED_OBJECTS
    $<TARGET_OBJECTS:agentty_io_obj>
    $<TARGET_OBJECTS:agentty_workspace_obj>
    $<TARGET_OBJECTS:agentty_airgap_obj>
    $<TARGET_OBJECTS:agentty_provider_obj>
    $<TARGET_OBJECTS:agentty_diff_obj>
    $<TARGET_OBJECTS:agentty_rag_obj>
    $<TARGET_OBJECTS:agentty_tool_obj>
    $<TARGET_OBJECTS:agentty_runtime_obj>
)
if(AGENTTY_MCP)
    # registry.cpp (in agentty_tool_obj) references mcp::mcp_tools, so the MCP
    # glue objects must be in the shared set the exe + every full test link.
    list(APPEND AGENTTY_SHARED_OBJECTS $<TARGET_OBJECTS:agentty_mcp_obj>)
endif()

add_executable(agentty
    src/runtime/main.cpp
    ${AGENTTY_SHARED_OBJECTS}
    $<TARGET_OBJECTS:agentty_acp_obj>
)

target_include_directories(agentty PRIVATE include)
# Bake the project version into the binary so `agentty --version` /
# the User-Agent string don't drift from CMakeLists.txt's
# `project(agentty VERSION X.Y.Z)`. Single source of truth — bumping
# the project line bumps every site that reads the macro.
target_compile_definitions(agentty PRIVATE AGENTTY_VERSION="${PROJECT_VERSION}")
if(AGENTTY_MCP)
    # main.cpp branches on AGENTTY_MCP (the `mcp-serve` subcommand). The objlibs
    # get this macro via agentty_objlib(); main.cpp is compiled straight into
    # the exe target, so define it here too.
    target_compile_definitions(agentty PRIVATE AGENTTY_MCP=1)
else()
    target_compile_definitions(agentty PRIVATE AGENTTY_MCP=0)
endif()
target_link_libraries(agentty PRIVATE
    maya::maya
    acp::acp
    nlohmann_json::nlohmann_json
    simdjson::simdjson
    nghttp2::nghttp2
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(AGENTTY_MCP AND TARGET mcp::mcp)
    target_link_libraries(agentty PRIVATE mcp::mcp)
endif()
if(AGENTTY_MCP AND TARGET mcp::tools)
    target_link_libraries(agentty PRIVATE mcp::tools)
endif()
if(AGENTTY_HAS_MIMALLOC)
    # The static target contains both C allocation and global C++ new/delete
    # overrides when MI_OVERRIDE is enabled.
    target_link_libraries(agentty PRIVATE mimalloc-static)
    target_compile_definitions(agentty PRIVATE AGENTTY_USE_MIMALLOC=1)
    message(STATUS "agentty: mimalloc allocator override enabled.")
endif()
if(AGENTTY_HAS_RAGCPP)
    # The RAG engine. The adapter TU (in agentty_rag_obj) is already in the
    # shared object set; the exe links the library so its symbols resolve.
    target_link_libraries(agentty PRIVATE ragcpp::ragcpp)
    message(STATUS "agentty: rag-cpp retrieval engine enabled (vendored).")
endif()
if(WIN32)
    # ws2_32   — Winsock2 (sockets, WSAPoll)
    # crypt32  — CertOpenSystemStoreW for the Windows root cert loader
    # shell32  — already needed by existing code paths
    # winmm    — timeBeginPeriod / timeEndPeriod in main.cpp (the
    #            `#pragma comment(lib, "winmm.lib")` only works on MSVC).
    # user32   — OpenClipboard / GetClipboardData (clipboard image paste)
    # gdi32    — DIB / BITMAPFILEHEADER consumers used in the same path
    # gdiplus  — DIB → PNG re-encoding for clipboard images
    # shlwapi  — SHCreateMemStream backing GDI+'s decode / encode
    # advapi32 — CredWriteW/CredReadW/CredDeleteW (keystore.cpp)
    target_link_libraries(agentty PRIVATE
        ws2_32 crypt32 shell32 winmm
        user32 gdi32 gdiplus shlwapi advapi32)
    # Static OpenSSL on Windows additionally pulls in these system libs
    # (used by libcrypto's CSP / certificate APIs). Innocuous in dynamic
    # builds — the linker dedupes — but required when AGENTTY_STANDALONE=ON.
    if(AGENTTY_STANDALONE)
        target_link_libraries(agentty PRIVATE bcrypt secur32 advapi32 user32)
    endif()
    # CRITICAL (MinGW/UCRT64): fold libstdc++, libgcc and the winpthread
    # runtime INTO the exe. Windows has no ELF-style symbol interposition,
    # so a dynamically-linked libstdc++-6.dll can keep its own allocator
    # bindings. With mimalloc overriding the exe, a std::string or filesystem
    # buffer crossing that DLL boundary could otherwise mismatch allocators
    # and corrupt the heap. Static runtimes collapse the process to one
    # allocator and remove the runtime DLL dependency.
    if(NOT MSVC)
        target_link_options(agentty PRIVATE
            -static-libstdc++ -static-libgcc
            -Wl,-Bstatic,--whole-archive -lwinpthread
            -Wl,--no-whole-archive -Wl,-Bdynamic
            # MinGW's static libstdc++.a ships cow-stdexcept.o defining the
            # legacy-ABI logic_error/runtime_error copy ctors, which collide
            # with the inline SSO-ABI copies the TUs already emitted. The
            # duplicates are ABI-compatible; let the linker keep the first.
            # This is the documented -static-libstdc++ workaround on MinGW.
            -Wl,--allow-multiple-definition)
    endif()
endif()
if(APPLE)
    # macOS root cert loader uses the Security and CoreFoundation frameworks.
    target_link_libraries(agentty PRIVATE
        "-framework Security"
        "-framework CoreFoundation")
endif()

# ── Standalone link knobs (per-platform) ───────────────────────────────
if(AGENTTY_STANDALONE AND NOT MSVC)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Fold libstdc++ and libgcc into the binary so it runs on machines
        # with a different distro / ABI version. libc stays dynamic — a
        # fully-static glibc binary breaks the NSS resolver (DNS, /etc/
        # nsswitch) and getpwuid_r at runtime. For 100%-static, opt into
        # AGENTTY_FULLY_STATIC and build with a musl toolchain.
        target_link_options(agentty PRIVATE
            -static-libstdc++ -static-libgcc)
        if(AGENTTY_FULLY_STATIC)
            # HOW WE LINK A BINARY THAT RUNS EVERYWHERE (and the graveyard of
            # approaches that don't, all confirmed on Alpine 3.21 / GCC 14.2):
            #
            #   * `-static-pie` (alone, or + compile flag): Alpine's
            #     default-PIE musl GCC does NOT pull libc from the archive.
            #     The output is ET_DYN with `NEEDED libc.musl-*.so` and no
            #     PT_INTERP -> runs on the Alpine build image, SIGSEGVs on
            #     glibc/Debian. THIS IS THE v0.2.7 CRASH. (Confirmed twice in
            #     CI: readelf -d shows NEEDED libc.musl-{x86_64,aarch64}.so.)
            #   * `-static-pie -static`: link error - `-static` drags in the
            #     NON-pie CRT (crtbeginT.o) whose R_X86_64_32 reloc "can not
            #     be used when making a PIE object" (collect2: ld returned 1).
            #   * `-Wl,-Bstatic --no-dynamic-linker`: driver re-emits
            #     -Bdynamic for its implicit -lc; NEEDED survives.
            #
            # What DOES work, bulletproof: plain `-static -no-pie`. It links
            # libc from the static archive and emits a classic ET_EXEC with
            # NO NEEDED and NO PT_INTERP - a true standalone binary that runs
            # on every Linux userland (glibc Debian/Ubuntu/Fedora, musl
            # Alpine, 64-bit Raspberry Pi OS). The only thing it does NOT run
            # on is Android/Bionic (Termux), which refuses ET_EXEC and needs
            # a PIE - a niche the honest default trades away so `curl | sh`
            # WORKS for the 99% on normal Linux. Opt into the PIE variant
            # with -DAGENTTY_STATIC_PIE=ON when you specifically target Termux
            # AND have a musl toolchain whose -static-pie truly links libc
            # statically (the POST_BUILD guard enforces it either way).
            if(AGENTTY_STATIC_PIE)
                target_compile_options(agentty PRIVATE -static-pie)
                target_link_options(agentty PRIVATE -static-pie)
            else()
                # -no-pie must reach BOTH compile and link so the driver
                # selects the non-PIE static CRT consistently and emits
                # ET_EXEC (not a half-PIE that keeps a NEEDED).
                target_compile_options(agentty PRIVATE -no-pie)
                target_link_options(agentty PRIVATE -static -no-pie)
            endif()
            # musl's default new-thread stack is 128 KiB, vs glibc's 8 MiB.
            # OpenSSL's SSL_connect (cert-chain verification) plus
            # nghttp2 session init plus the local autos in dial_tcp
            # overruns 128 KiB on long chains, surfacing as a random
            # SIGSEGV in the prewarm thread on the musl-static release
            # binary (the glibc-dynamic dev build never hits it).
            # PT_GNU_STACK p_memsz is what musl reads to size every
            # pthread_create — bumping it via -z stack-size fixes the
            # detached prewarm thread, the grep tool's worker pool, and
            # any pthread created inside statically-linked third-party
            # libs we don't control. 8 MiB matches glibc's default so
            # the musl-static and glibc-dynamic builds behave the same.
            target_link_options(agentty PRIVATE -Wl,-z,stack-size=8388608)
        endif()
        # Static OpenSSL pulls in libdl and libpthread (both already
        # implicit) plus libz (compression) for some builds. Add libz
        # defensively; harmless when not needed.
        find_library(ZLIB_STATIC NAMES libz.a z)
        if(ZLIB_STATIC)
            target_link_libraries(agentty PRIVATE ${ZLIB_STATIC})
        endif()
    elseif(APPLE)
        # macOS doesn't allow truly-static system frameworks (libSystem
        # is required to be dynamic), but third-party libs can be static.
        # OPENSSL_USE_STATIC_LIBS already handled OpenSSL above. Nothing
        # else to add here — the resulting binary depends only on
        # /usr/lib/libSystem.B.dylib which is on every macOS.
    endif()
endif()

# Build-time ELF-shape guard for the fully-static Linux binary. This is the
# LAST line of defence against the v0.2.7 disaster, and unlike the CI readelf
# checks it runs on EVERY build path that produces the shipping binary:
# the release workflow, a local `AGENTTY_FULLY_STATIC=ON` build, AND the
# installer's `--build` / auto-fallback source build. A correct static-PIE is
# ET_DYN with a PT_PHDR, no PT_INTERP, and no NEEDED dynamic library. If the
# musl/Alpine driver silently downgraded the link to a dynamic-PIE (the
# exact v0.2.7 x86_64 failure: NEEDED libc.musl-*.so + no INTERP → SIGSEGV on
# glibc), fail the BUILD here — a broken artifact can never reach a release
# again, no matter which pipeline built it.
if(AGENTTY_STANDALONE AND AGENTTY_FULLY_STATIC
        AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MSVC)
    find_program(READELF_EXE NAMES readelf llvm-readelf)
    if(READELF_EXE)
        add_custom_command(TARGET agentty POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:agentty>
                -DREADELF=${READELF_EXE}
                -P ${CMAKE_SOURCE_DIR}/cmake/assert_static_pie.cmake
            VERBATIM
            COMMENT "Verifying static-PIE ELF shape (no NEEDED / no INTERP)")
    else()
        message(WARNING
            "agentty: readelf not found — cannot verify the static-PIE ELF "
            "shape at build time. Install binutils so a downgraded "
            "dynamic-PIE link (the v0.2.7 crash) fails the build.")
    endif()
endif()

# Diagnostic: print a one-liner at configure time so contributors know
# what they're building. Helps catch the "I forgot the flag" case.
if(AGENTTY_STANDALONE)
    message(STATUS "agentty: STANDALONE build — third-party deps statically linked.")
    if(AGENTTY_STANDALONE_OPENSSL_FALLBACK)
        message(WARNING
            "agentty: STANDALONE requested OpenSSL static libs (.a) but only the "
            "shared library was found — the binary will still depend on libssl/"
            "libcrypto at runtime. To get a fully standalone exe, install the "
            "static OpenSSL package (Alpine: openssl-libs-static; Arch AUR: "
            "openssl-static; Fedora: openssl-static; or build from source).")
    endif()
elseif(AGENTTY_STATIC_RUNTIME AND MSVC)
    message(STATUS "agentty: static MSVC runtime (/MT).")
endif()

# ── Shared compile flags applied to agentty ─────────────────────────
# (agentty_apply_compile_flags is defined up top, before the OBJECT libs.)
agentty_apply_compile_flags(agentty)

# agentty-exe-only MSVC link-time codegen (consumes the /GL objects).
if(MSVC)
    target_link_options(agentty PRIVATE
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:
            /LTCG              # link-time codegen (consumes /GL .obj files)
            /OPT:REF           # remove unreferenced functions/data
            /OPT:ICF=3         # fold identical COMDATs (3 passes)
            /INCREMENTAL:NO    # full link only — smaller exe, better IPO
            /DEBUG:NONE        # no PDB in Release
        >
    )
endif()

# nghttp2.h is a C header whose multi-line function signatures trip MSVC's
# conforming preprocessor (/Zc:preprocessor). Revert to the legacy
# preprocessor for just this TU; the rest of the build keeps /Zc:preprocessor.
# Applied directory-wide so the io OBJECT library's http.cpp gets it too.
if(MSVC)
    set_source_files_properties(src/io/http.cpp PROPERTIES
        COMPILE_FLAGS "/Zc:preprocessor- /DNGHTTP2_STATICLIB")
endif()
# Opt-in sanitizer build: `cmake -B build-san -DAGENTTY_SANITIZE=address,undefined`.
# Separate from CMAKE_BUILD_TYPE so CI / contributors can flip it without
# redefining the whole toolchain.
set(AGENTTY_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers for agentty (e.g. address,undefined or thread). Empty to disable.")
if(AGENTTY_SANITIZE AND NOT MSVC)
    message(STATUS "agentty: building with -fsanitize=${AGENTTY_SANITIZE}")
    target_compile_options(agentty PRIVATE
        -fsanitize=${AGENTTY_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(agentty PRIVATE -fsanitize=${AGENTTY_SANITIZE})
endif()

# ── Security hardening + release strip ──────────────────────────────
# Free defense-in-depth for the SHIPPING binary (SECURITY_AUDIT.md #5: no
# explicit ASLR/DEP/RELRO). All zero-behaviour-change:
#   -fstack-protector-strong   canary on any frame with a local array/aggregate
#   -D_FORTIFY_SOURCE=2         compile-time + runtime bounds checks on libc
#                              str/mem/printf calls (needs an optimizing build)
#   -fPIE / -pie               position-independent exe → full ASLR of .text
#   -Wl,-z,relro,-z,now        GOT mapped read-only after startup (full RELRO)
#   -Wl,-z,noexecstack         non-executable stack marker
# Skipped under sanitizers (they own the runtime + want frame pointers) and on
# MSVC (has its own /GS /DYNAMICBASE /NXCOMPAT defaults). RELRO/noexecstack are
# ELF-only; the linker no-ops them elsewhere, but gate to be explicit.
if(NOT MSVC AND NOT AGENTTY_SANITIZE)
    target_compile_options(agentty PRIVATE -fstack-protector-strong)
    # -fPIE only for builds that actually produce a PIE. The default
    # fully-static build is -static -no-pie (ET_EXEC), where -fPIE would
    # fight -no-pie; skip it there. Every other build (dynamic, or the
    # opt-in AGENTTY_STATIC_PIE Termux variant) gets -fPIE for full ASLR.
    if(NOT (AGENTTY_FULLY_STATIC AND NOT AGENTTY_STATIC_PIE))
        target_compile_options(agentty PRIVATE -fPIE)
    endif()
    # _FORTIFY_SOURCE requires -O1+; only define it for optimizing configs so
    # a Debug build doesn't emit the "requires optimization" warning per TU.
    target_compile_definitions(agentty PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
    # A fully-static build already carries its own PIE decision (-static-pie
    # or -no-pie); adding a bare -pie here would fight it. Only add -pie for
    # the non-static path.
    if(UNIX AND NOT APPLE)
        # RELRO + BIND_NOW + non-exec stack are GNU-ld / lld (ELF) features.
        # Apple ld64 rejects -z; MSVC handled above.
        if(NOT AGENTTY_FULLY_STATIC)
            target_link_options(agentty PRIVATE -pie)
        endif()
        target_link_options(agentty PRIVATE
            -Wl,-z,relro,-z,now
            -Wl,-z,noexecstack)
    elseif(NOT APPLE AND NOT AGENTTY_FULLY_STATIC)
        # Non-ELF, non-Apple UNIX. On Apple, PIE is the linker default and a
        # bare -pie only trips clang's "argument unused during compilation"
        # warning, so skip it there.
        target_link_options(agentty PRIVATE -pie)
    endif()
endif()

# Strip the release binary. LTO + -O3 already shipped ~14 MB with a full
# symbol table the running app never needs; --strip-all drops the .symtab /
# .debug sections (typically >50%). Never strip Debug/RelWithDebInfo (you want
# the symbols) or sanitizer builds (strip breaks ASan symbolization), and skip
# on Apple (macOS+GCC keeps a live LTO LOAD segment strip can't touch — see the
# IPO note above; the release path there strips via a separate step) and MSVC
# (PDBs are external, nothing to strip off the .exe).
if(NOT MSVC AND NOT APPLE AND NOT AGENTTY_SANITIZE
        AND (CMAKE_BUILD_TYPE STREQUAL "Release"
          OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    target_link_options(agentty PRIVATE -Wl,--strip-all)
endif()

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

# ─────────────────────────────────────────────────────────────────────
# Tests / benchmarks (opt-in via -DAGENTTY_BUILD_TESTS=ON, default OFF)
# ─────────────────────────────────────────────────────────────────────
#
# Off by default so plain `cmake -B build` keeps building only the
# shipping binary. Turn on when iterating on perf or running CI.
#
# Each test target links the FULL runtime + tool stack (no main.cpp) so
# the bench is exercising the same code paths the production binary
# would run — not a stripped-down stand-in.
option(AGENTTY_BUILD_TESTS "Build the test + benchmark binaries." OFF)
if(AGENTTY_BUILD_TESTS)
    enable_testing()

    # macOS SDK + GCC: the same `_Static_assert` arm64 macros from
    # <mach/*.h> that the `agentty` target aliases above (see the
    # APPLE+GNU block) also fire in the test/bench TUs — they link the
    # full IO/tool/runtime stack (subprocess.cpp pulls <spawn.h> → mach
    # headers). The agentty-target alias is PRIVATE, so it doesn't reach
    # these separate executables; apply it directory-wide here so every
    # target defined below inherits it. AppleClang doesn't need it (and
    # can't build this tree anyway — it doesn't advertise cxx_std_26).
    if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_definitions(_Static_assert=static_assert)
    endif()

    # ── The test suite lives in cmake/AgenttyTests.cmake ────────────────────
    # One agentty_test() declaration per test; the tests / tests_gating /
    # sanitizer_tests aggregates are DERIVED from the registry (no hand-listed
    # parallel lists). See cmake/AgenttyTestRegistry.cmake + cmake/DESIGN.md.
    include(AgenttyTests)
endif()
