cmake_minimum_required(VERSION 3.28)
project(agentty VERSION 0.2.1 LANGUAGES CXX)

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

# mimalloc override — Microsoft's fast allocator. MSVCRT's malloc is notably
# slow on the small-string churn pattern (std::string everywhere in RenderOp,
# SSE parsing, JSON). Linking mimalloc-static and including mimalloc-new-delete
# in main.cpp routes global operator new/delete through mimalloc; measured
# 15–25% wins on allocator-heavy Windows workloads. Default ON when the
# package is installed; harmless on POSIX (mimalloc works there too but the
# delta vs. glibc/jemalloc is smaller).
option(AGENTTY_USE_MIMALLOC   "Route operator new/delete through mimalloc (big Win32 win)" ON)

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
if(AGENTTY_IPO_OK AND (CMAKE_BUILD_TYPE STREQUAL "Release"
                     OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
                     OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
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

# Prefer the in-tree submodule when present; only fall back to fetch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/maya/CMakeLists.txt")
    # Optional "always pull latest maya before each build" mode. When ON,
    # a custom target syncs the submodule to `origin/master` and forces a
    # rebuild of any maya translation unit that changed. SAFE: refuses to
    # touch the submodule if it has uncommitted changes (so local maya
    # edits never get clobbered). Skipped silently if the repo isn't a
    # git checkout (release tarballs, FetchContent fallback paths).
    option(AGENTTY_AUTO_PULL_MAYA "Pull latest maya from origin/master on every build" ON)
    add_subdirectory(maya)
    if(AGENTTY_AUTO_PULL_MAYA AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        find_package(Git QUIET)
        if(GIT_FOUND)
            add_custom_target(maya_pull_latest ALL
                COMMAND ${CMAKE_COMMAND} -E echo
                    "[maya] checking origin/master…"
                COMMAND ${GIT_EXECUTABLE}
                    -C ${CMAKE_CURRENT_SOURCE_DIR}/maya
                    diff --quiet HEAD
                    && ${GIT_EXECUTABLE}
                       -C ${CMAKE_CURRENT_SOURCE_DIR}/maya
                       fetch --quiet origin master
                    && ${GIT_EXECUTABLE}
                       -C ${CMAKE_CURRENT_SOURCE_DIR}/maya
                       reset --hard --quiet origin/master
                    || ${CMAKE_COMMAND} -E echo
                       "[maya] local changes present — skipping auto-pull"
                COMMENT "Syncing maya/ to origin/master (safe: skips if local changes)"
                VERBATIM
                USES_TERMINAL
            )
            # Force maya the library to be rebuilt AFTER the pull (in case
            # sources changed). add_dependencies wires the ordering;
            # whether anything actually recompiles depends on file
            # timestamps the git reset may have updated.
            add_dependencies(maya maya_pull_latest)
        endif()
    endif()
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
set(MCP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(mcp-cpp)
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
# mimalloc — optional, but default-on when available. find_package(mimalloc)
# comes from the upstream CMake export (vcpkg / brew / apt). If missing, we
# silently continue without it rather than failing — it's a perf knob, not a
# correctness requirement.
set(AGENTTY_HAS_MIMALLOC FALSE)
if(AGENTTY_USE_MIMALLOC)
    find_package(mimalloc CONFIG QUIET)
    if(TARGET mimalloc-static OR TARGET mimalloc)
        set(AGENTTY_HAS_MIMALLOC TRUE)
    else()
        # WARNING (not STATUS) on purpose: this is the difference between
        # a process that returns memory to the kernel promptly (mimalloc)
        # and one that hoards a long session's freed arenas in glibc's
        # free-list until exit (system allocator). On a long-running
        # interactive session that's hundreds of MiB of unreclaimed RSS.
        # The user should see the message during configure, not have it
        # buried in a quiet STATUS line.
        message(WARNING "agentty: AGENTTY_USE_MIMALLOC=ON but mimalloc not found "
                       "— building against the system allocator. On "
                       "Linux/glibc this means freed memory is not returned "
                       "to the kernel until process exit; long sessions will "
                       "look bloated in RSS. Install via `apt install "
                       "libmimalloc-dev` / `pacman -S mimalloc` / `brew "
                       "install mimalloc` / vcpkg `mimalloc` and "
                       "reconfigure to enable it.")
    endif()
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
    src/io/persistence.cpp
    src/io/clipboard.cpp
    src/util/base64.cpp
)

set(AGENTTY_WORKSPACE_SOURCES
    src/workspace/files.cpp
    src/workspace/symbols.cpp
)

set(AGENTTY_AIRGAP_SOURCES
    src/airgap/airgap.cpp
)

set(AGENTTY_PROVIDER_SOURCES
    src/provider/anthropic/transport.cpp
    src/provider/openai/transport.cpp
    src/provider/ollama/transport.cpp
    src/provider/selection.cpp
)

# ACP (Agent Client Protocol) — lets agentty run as a headless agent
# subprocess that Zed (or any ACP client) drives over JSON-RPC on stdio.
# The wire protocol/engine/transport live in the acp-cpp submodule (linked
# as acp::acp); server.cpp is the agentty-specific glue (turn loop + tools).
set(AGENTTY_ACP_SOURCES
    src/acp/server.cpp
)

# MCP (Model Context Protocol) client glue — spawns external MCP servers and
# exposes their tools as agentty ToolDefs. The heavy mcp-cpp templates are
# confined to these TUs (linked as mcp::mcp). Only compiled when AGENTTY_MCP.
set(AGENTTY_MCP_SOURCES
    src/mcp/bridge.cpp
    src/mcp/http_server.cpp
    src/mcp/serve.cpp
    src/tool/mcp_tools_bridge.cpp
    src/tool/mcp_tools_backends.cpp
)

set(AGENTTY_DIFF_SOURCES
    src/diff/diff.cpp
)

set(AGENTTY_RAG_SOURCES
    src/rag/bm25.cpp
    src/rag/corpus.cpp
    src/rag/hnsw.cpp
    src/rag/rerank.cpp
    src/rag/expand.cpp
    src/rag/knowledge.cpp
    src/rag/stemmer.cpp
    src/rag/simd.cpp
)

set(AGENTTY_TOOL_SOURCES
    src/tool/registry.cpp
    src/tool/util/utf8.cpp
    src/tool/util/fs_helpers.cpp
    src/tool/util/subprocess.cpp
    src/tool/util/sandbox.cpp
    src/tool/util/arg_reader.cpp
    src/tool/util/partial_json.cpp
    src/tool/util/fuzzy_match.cpp
    src/tool/subagent.cpp
    src/tool/skills.cpp
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
    src/runtime/app/update/modal.cpp
    src/runtime/app/update/frozen.cpp
    src/runtime/app/update/tool.cpp
    src/runtime/app/update/login.cpp
    src/runtime/app/update/picker.cpp
    src/runtime/app/update/palette.cpp
    src/runtime/app/update/mention.cpp
    src/runtime/app/update/symbol.cpp
    src/runtime/app/update/diff.cpp
    src/runtime/app/update/meta.cpp
    src/runtime/app/subscribe.cpp

    src/runtime/view/cache.cpp
    src/runtime/view/helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_args.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_helpers.cpp
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
    # Prefer the static target so operator new/delete routing is linked
    # into the exe without an accompanying DLL / dylib. BUT: some distros
    # (Arch's `mimalloc` package as of 2024) ship a stub libmimalloc.a
    # that exports no symbols — the real allocator lives in the shared
    # object. Linking against the stub produces hundreds of `undefined
    # reference to mi_*` errors at LTO link time. Probe the archive size
    # and fall back to the shared target when it's clearly a stub.
    set(_agentty_mimalloc_target "")
    if(TARGET mimalloc-static)
        get_target_property(_mi_static_loc mimalloc-static IMPORTED_LOCATION_RELEASE)
        if(NOT _mi_static_loc)
            get_target_property(_mi_static_loc mimalloc-static IMPORTED_LOCATION)
        endif()
        if(_mi_static_loc AND EXISTS "${_mi_static_loc}")
            file(SIZE "${_mi_static_loc}" _mi_static_size)
            # A real mimalloc static archive is ~300 KB+; Arch's stub is
            # under 32 KB. Use 64 KB as the cutoff.
            if(_mi_static_size GREATER 65536)
                set(_agentty_mimalloc_target mimalloc-static)
            else()
                message(STATUS "agentty: mimalloc-static archive at ${_mi_static_loc} "
                              "is a stub (${_mi_static_size} bytes) — using shared mimalloc instead.")
            endif()
        endif()
    endif()
    if(NOT _agentty_mimalloc_target AND TARGET mimalloc)
        set(_agentty_mimalloc_target mimalloc)
    endif()
    if(_agentty_mimalloc_target)
        target_link_libraries(agentty PRIVATE ${_agentty_mimalloc_target})
        target_compile_definitions(agentty PRIVATE AGENTTY_USE_MIMALLOC=1)
        message(STATUS "agentty: mimalloc allocator override enabled (${_agentty_mimalloc_target}).")
    else()
        message(WARNING "agentty: mimalloc package found but no usable target — skipping.")
    endif()
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
    target_link_libraries(agentty PRIVATE
        ws2_32 crypt32 shell32 winmm
        user32 gdi32 gdiplus shlwapi)
    # Static OpenSSL on Windows additionally pulls in these system libs
    # (used by libcrypto's CSP / certificate APIs). Innocuous in dynamic
    # builds — the linker dedupes — but required when AGENTTY_STANDALONE=ON.
    if(AGENTTY_STANDALONE)
        target_link_libraries(agentty PRIVATE bcrypt secur32 advapi32 user32)
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
            target_link_options(agentty PRIVATE -static)
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

    # ── Full-runtime test helper ────────────────────────────────────────
    # agentty_add_full_test(<name> [EXTRA_SRCS...] [EXTRA_LIBS...])
    # Builds a test executable that REUSES the precompiled shared OBJECT
    # libraries (no recompilation of the provider/tool/runtime stack) plus
    # its own .cpp (tests/<name>.cpp) and any EXTRA_SRCS. Links the
    # standard lib set + EXTRA_LIBS, applies the platform link blocks, and
    # registers the ctest entry. Replaces ~40 lines of boilerplate per
    # test with one call. Pass extra source files after the name; pass
    # extra link libs after the keyword LIBS.
    function(agentty_add_full_test name)
        set(_extra_srcs "")
        set(_extra_libs "")
        set(_no_test FALSE)
        set(_mode SRCS)
        foreach(arg IN LISTS ARGN)
            if(arg STREQUAL "LIBS")
                set(_mode LIBS)
            elseif(arg STREQUAL "NO_TEST")
                set(_no_test TRUE)
            elseif(_mode STREQUAL "LIBS")
                list(APPEND _extra_libs ${arg})
            else()
                list(APPEND _extra_srcs ${arg})
            endif()
        endforeach()
        add_executable(${name} EXCLUDE_FROM_ALL
            tests/${name}.cpp
            ${_extra_srcs}
            ${AGENTTY_SHARED_OBJECTS}
        )
        target_include_directories(${name} PRIVATE include)
        target_compile_definitions(${name} PRIVATE
            AGENTTY_VERSION="${PROJECT_VERSION}"
            AGENTTY_MCP=1)
        target_link_libraries(${name} PRIVATE
            maya::maya
            nlohmann_json::nlohmann_json
            simdjson::simdjson
            nghttp2::nghttp2
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
            ${_extra_libs}
        )
        # The shared object set bundles agentty_mcp_obj (registry.cpp now
        # routes the whole tool set through the mcp-cpp bridge), so every
        # full test must link mcp::mcp + mcp::tools to resolve make_provider /
        # effects_for_builtin / the progress sink.
        if(TARGET mcp::mcp)
            target_link_libraries(${name} PRIVATE mcp::mcp)
        endif()
        if(TARGET mcp::tools)
            target_link_libraries(${name} PRIVATE mcp::tools)
        endif()
        if(WIN32)
            target_link_libraries(${name} PRIVATE
                ws2_32 crypt32 shell32 winmm
                user32 gdi32 gdiplus shlwapi)
        endif()
        if(APPLE)
            target_link_libraries(${name} PRIVATE
                "-framework Security"
                "-framework CoreFoundation")
        endif()
        if(NOT _no_test)
            add_test(NAME ${name} COMMAND ${name})
        endif()
    endfunction()

    # long_session_bench — perf stress for the resume/render hot paths
    # exercised by the seven pins in docs/INLINE_SCROLLBACK.md. Builds
    # synthetic tool-heavy Threads of varying shape and times every
    # phase from Thread construction through warm render.
    agentty_add_full_test(long_session_bench)
    # CTest entry — runs the bench with default scenarios. Exits non-zero
    # if any scenario throws; perf regressions are reported via stdout,
    # not via a CTest fail, because absolute timings vary by machine.
    set_tests_properties(long_session_bench PROPERTIES TIMEOUT 600)

    # o1_probe — measures steady-state per-frame warm render AFTER the
    # live-session freeze+trim flow. Build-only probe (not a ctest entry).
    agentty_add_full_test(o1_probe NO_TEST)

    # realthread_probe — loads a real on-disk thread JSON and times the
    # resume hot path (rehydrate_frozen + cold/warm render). Build-only.
    agentty_add_full_test(realthread_probe NO_TEST)

    # tallcard_probe — isolates the tall-card live-tail cache-miss
    # regime (canvas-height vs card-height). Build-only.
    agentty_add_full_test(tallcard_probe NO_TEST)

    # fuzzy_match_smoke — small correctness smoke for the line-DP
    # fuzzy matcher used by the edit tool. Standalone (no maya / no
    # runtime), so it compiles against just the fuzzy_match TU.
    add_executable(fuzzy_match_smoke EXCLUDE_FROM_ALL
        tests/fuzzy_match_smoke.cpp
        src/tool/util/fuzzy_match.cpp
    )
    target_include_directories(fuzzy_match_smoke PRIVATE include)
    target_link_libraries(fuzzy_match_smoke PRIVATE
        nlohmann_json::nlohmann_json)
    add_test(NAME fuzzy_match_smoke COMMAND fuzzy_match_smoke)

    # param_tag_repair_test — regression for the XML-in-JSON tool-call
    # recovery (model leaks `<parameter name="…">` into the JSON tool
    # input). Header-only logic, so it links against just nlohmann + the
    # header-only tool spec catalog.
    add_executable(param_tag_repair_test EXCLUDE_FROM_ALL
        tests/param_tag_repair_test.cpp
    )
    target_include_directories(param_tag_repair_test PRIVATE include)
    target_link_libraries(param_tag_repair_test PRIVATE
        nlohmann_json::nlohmann_json)
    add_test(NAME param_tag_repair_test COMMAND param_tag_repair_test)

    # model_caps_test — ModelCapabilities weak-model inference (header-only).
    add_executable(model_caps_test EXCLUDE_FROM_ALL
        tests/model_caps_test.cpp
    )
    target_include_directories(model_caps_test PRIVATE include)
    target_link_libraries(model_caps_test PRIVATE
        nlohmann_json::nlohmann_json)
    add_test(NAME model_caps_test COMMAND model_caps_test)

    # fsm_test — generic typestate machinery (include/agentty/io/fsm.hpp):
    # compile-time edge legality from in-class `fsm_to` successor lists,
    # the assert_legal_edge guard, and move-only RAII cleanup-exactly-once.
    # Pure header, zero deps — the same machinery that drives the HTTP/2
    # connection lifecycle's Dialing→TcpConnected→TlsUp→H2Ready pipeline.
    add_executable(fsm_test EXCLUDE_FROM_ALL tests/fsm_test.cpp)
    target_include_directories(fsm_test PRIVATE include)
    add_test(NAME fsm_test COMMAND fsm_test)

    # model_label_test — pretty_model_label() turn-header normalization
    # across every provider id shape (Ollama :tag, OpenAI, OpenRouter
    # namespaced, Gemini/xAI, the agentty [1m] marker). Reuses the shared
    # runtime OBJECT lib so helpers.cpp isn't recompiled.
    agentty_add_full_test(model_label_test)

    # skills_engine_test — Agent Skills (agentskills.io) engine: discovery
    # across native + interop roots (.agentty/.agents/.claude, project +
    # user), shadowing precedence, lenient frontmatter, tier-3 resource
    # enumeration, activation payload/dedup/reset, and the read-allowlist
    # gate. Standalone: links just the skills + fs_helpers TUs.
    add_executable(skills_engine_test EXCLUDE_FROM_ALL
        tests/skills_engine_test.cpp
        src/tool/skills.cpp
        src/tool/util/fs_helpers.cpp
    )
    target_include_directories(skills_engine_test PRIVATE include)
    target_link_libraries(skills_engine_test PRIVATE
        maya::maya
        nlohmann_json::nlohmann_json)
    # fs_helpers.cpp mirrors the workspace root into mcp-cpp's util layer.
    if(TARGET mcp::tools)
        target_link_libraries(skills_engine_test PRIVATE mcp::tools)
    endif()
    add_test(NAME skills_engine_test COMMAND skills_engine_test)

    # midrun_freeze_test — correctness for freeze_settled_subturns: the
    # mid-run split (frozen prefix + live continuation tail) must be
    # visually seamless (one header, all content present, no elision).
    agentty_add_full_test(midrun_freeze_test)

    # midrun_seam_test — scrollback-duplication regression: across the
    # incremental mid-run freeze cadence, the committed row PREFIX must
    # stay byte-stable (a rewritten committed row = a duplicated turn in
    # native scrollback).
    agentty_add_full_test(midrun_seam_test)

    # midrun_wire_test — WIRE-LEVEL scrollback-duplication regression.
    # Drives maya's REAL inline compose across the live->freeze handoff
    # and asserts no committed scrollback row is rewritten (the stranded
    # duplicate the seam test's render_tree comparison can't catch).
    agentty_add_full_test(midrun_wire_test)

    # stream_liveness_test — UX-critical caret-liveness regression. The
    # live-edge typewriter caret must stay armed (request_animation_frame
    # every build) for the WHOLE time phase==Streaming — even across a
    # 10 s inter-delta gap — and must disarm after settle + finalize
    # ramp. Locks the phase-authoritative RAF gate in turn.cpp
    # (commit a9760e7); a fixed byte-recency timeout would fail it.
    # stream_liveness_test
    agentty_add_full_test(stream_liveness_test)
    set_tests_properties(stream_liveness_test PROPERTIES TIMEOUT 30)

    # stream_md_lag_test — long-turn markdown LAG regression. Drives maya's
    # StreamingMarkdown exactly as turn.cpp::cached_markdown_for does
    # (reveal_fx ON, live ON, full content resent + build() every frame) and
    # asserts the per-frame build() cost does NOT scale with total turn
    # length: the committed prefix must make progress while live so only a
    # small tail is reprocessed each frame. A deferred-commit O(N)-per-frame
    # renderer (the bug the user feels as streaming stutter on a big reply)
    # fails both the timing-ratio gate and the committed-progress assertion.
    # Standalone against maya only — no agentty sources needed.
    add_executable(stream_md_lag_test EXCLUDE_FROM_ALL tests/stream_md_lag_test.cpp)
    target_link_libraries(stream_md_lag_test PRIVATE maya::maya)
    add_test(NAME stream_md_lag_test COMMAND stream_md_lag_test)
    set_tests_properties(stream_md_lag_test PROPERTIES TIMEOUT 60)

    # composer_edit_test — reducer-level coverage for the composer's
    # word-wise delete keys (Ctrl+W unix-word-rubout / Alt+D kill-word),
    # including the chip-aware boundary that drops a whole attachment
    # token in one stroke and undo integration. Drives
    # detail::composer_update directly (no maya / no I/O).
    agentty_add_full_test(composer_edit_test)

    # scheduler_path_test — path-aware parallel tool scheduling. Pins the
    # refinement of the effect-only gate: disjoint-path writers / a read
    # alongside an unrelated write run concurrently, while overlapping /
    # exec / blind-writer cases serialise. Pure planner
    # (cmd::schedule_parallel_batch) — no maya / no phase machinery.
    agentty_add_full_test(scheduler_path_test)

    # table_render_test — pins maya's GFM table classification: a
    # well-formed table (header line starts at '|', blank line before)
    # is a Table block; a malformed one with lead-in prose on the header
    # line is a Paragraph (the wall-of-pipes a model produced). Standalone
    # against maya only — no agentty sources needed.
    add_executable(table_render_test EXCLUDE_FROM_ALL tests/table_render_test.cpp)
    target_link_libraries(table_render_test PRIVATE maya::maya)
    add_test(NAME table_render_test COMMAND table_render_test)

    # reveal_pacing_test — guards the streaming-reveal CONSTANT-GLIDE
    # invariant against the long-turn BURST regression. Drives
    # maya::anim::RateCursor directly and asserts the auto-tuned cruise speed
    # is clamped to a readable ceiling (no sprint to thousands of cp/s when a
    # fast model outpaces the cruise), that a slow feed still tracks the wire,
    # and that the finalize ramp still bypasses the cap to flush the tail.
    # Header-only against maya — no agentty sources needed.
    add_executable(reveal_pacing_test EXCLUDE_FROM_ALL tests/reveal_pacing_test.cpp)
    target_link_libraries(reveal_pacing_test PRIVATE maya::maya)
    add_test(NAME reveal_pacing_test COMMAND reveal_pacing_test)
    set_tests_properties(reveal_pacing_test PROPERTIES TIMEOUT 60)

    # openai_transport_test — verifies the OpenAI-compatible provider's pure
    # translation layer with NO network: build_tools / build_messages JSON
    # shaping, and a scripted-SSE → agentty-Msg parse (text deltas, streamed
    # tool-call assembly, finish_reason→StopReason, usage, error frames,
    # endpoint presets).
    agentty_add_full_test(openai_transport_test)

    # ollama_transport_test — verifies the DEDICATED Ollama /api/chat
    # transport: native build_messages (object-form tool args + role:tool
    # results + images), NDJSON parse (structured tool_calls, plain text,
    # NO content-salvage, error frames), and the local system prompt shape.
    # Reuses the shared OBJECT libs (no full-stack recompile) like the
    # sibling openai_transport_test.
    agentty_add_full_test(ollama_transport_test)

    # rag_test — document/knowledge RAG unit tests (chunker, BM25 ranking,
    # RRF fusion, cosine, Corpus BM25-only search). No network: the embed
    # path is exercised only with an empty model so no Ollama server is
    # needed. Links the full shared stack (rag + io objs included).
    agentty_add_full_test(rag_test)
    agentty_add_full_test(rag_hnsw_test)
    agentty_add_full_test(rag_rerank_test)
    agentty_add_full_test(rag_expand_test)
    agentty_add_full_test(rag_advanced_test)
    agentty_add_full_test(knowledge_test)

    # visual_hash_coverage_test — proves the render gate
    # (Program::visual_hash) can't silently drop a visible change. Two
    # declarative tables: every view-affecting model axis must advance
    # the hash; every non-visual axis (tick clock, token counters) must
    # not. Catches the "forgot to mix() a new field" footgun at CI time
    # instead of as a silent dead region in production.
    agentty_add_full_test(visual_hash_coverage_test)

    # tool_result_budget_test — wire-only byte-budget cap on tool
    # results. Drives the real messages_json_string serializer and
    # asserts oversized outputs are bounded (head+tail kept, marker
    # spliced, UTF-8 boundary respected) while small outputs ship verbatim.
    agentty_add_full_test(tool_result_budget_test)

    # salvage_dedup_test — re-leaked salvaged-tool-call dedup. Unit-tests
    # dedup_releaked_salvage_calls (cmd_factory.cpp): a weak local model that
    # re-leaks a tool call it already ran this turn must not run it twice.
    agentty_add_full_test(salvage_dedup_test)

    # doom_loop_test — agent_loop_should_break (cmd_factory.cpp): the
    # circuit breaker that stops a non-converging weak-model tool loop
    # (same failing call repeated, or a runaway tool-step count) before it
    # spins until the user hits Esc.
    agentty_add_full_test(doom_loop_test)

    # acp_integration_test — end-to-end ACP agent exercise. Drives a real
    # agentty::acp::AgentServer (on the acp-cpp library) through
    # initialize → session/new → session/prompt over an in-memory transport
    # with a scripted provider. Adds the ACP glue objects + acp::acp on
    # top of the shared stack.
    agentty_add_full_test(acp_integration_test
        $<TARGET_OBJECTS:agentty_acp_obj>
        LIBS acp::acp)
    set_tests_properties(acp_integration_test PROPERTIES TIMEOUT 60)

    # mcp_bridge_test — e2e for the agentty↔mcp-cpp capability bridge. Spawns
    # the mcp-cpp example server and drives mcp::mcp_tools() end to end. The
    # test SKIPS (exit 0) when the example server isn't built. Only registered
    # when MCP is compiled in.
    if(AGENTTY_MCP)
        agentty_add_full_test(mcp_bridge_test LIBS mcp::mcp)
        set_tests_properties(mcp_bridge_test PROPERTIES TIMEOUT 60)
        # Point the e2e at the mcp-cpp example server when it's been built, so
        # ctest exercises the real spawn+handshake path instead of skipping.
        # The test still SKIPs gracefully if the binary isn't there.
        set_tests_properties(mcp_bridge_test PROPERTIES
            ENVIRONMENT "AGENTTY_MCP_E2E_SERVER=${CMAKE_CURRENT_SOURCE_DIR}/mcp-cpp/build/examples/mcp_server_example")

        # mcp_http_test — e2e for the Streamable HTTP transport. Stands up a
        # tiny in-process HTTP/1.1 MCP server and drives make_http_provider()
        # over http://127.0.0.1:PORT/mcp. No external binary needed; SKIPs on
        # Windows (POSIX sockets).
        agentty_add_full_test(mcp_http_test LIBS mcp::mcp)
        set_tests_properties(mcp_http_test PROPERTIES TIMEOUT 60)
    endif()

    # frozen_invariant_fuzz — randomized property test for the frozen-
    # scrollback subsystem. Drives arbitrary sequences of conversation
    # growth + freeze/trim/rehydrate ops and asserts the structural
    # invariants after EVERY op. A failure prints SEED=<n> for repro.
    agentty_add_full_test(frozen_invariant_fuzz)

    # scrollback_wire_fuzz — randomized WIRE-LEVEL anti-corruption fuzz.
    # Drives arbitrary interleavings of streaming / tool cards / settle
    # freeze+trim / picker open-close through the REAL ui::view + maya's
    # REAL inline compose, asserting W1-W5 after EVERY frame. A failure
    # prints SEED + op trace.
    agentty_add_full_test(scrollback_wire_fuzz)
    set_tests_properties(scrollback_wire_fuzz PROPERTIES TIMEOUT 120)

    # reveal_scrollback_test — streaming scrollback DUPLICATION repro with
    # reveal_fx ON. Streams a long body that overflows a small viewport,
    # keeps rendering frames (advancing wall-clock), and asserts no
    # already-committed scrollback row is ever rewritten (R1/R5) across
    # four terminal shapes x five streaming scenarios.
    agentty_add_full_test(reveal_scrollback_test)
    if(UNIX AND NOT APPLE)
        # openpty() lives in libutil on glibc.
        target_link_libraries(reveal_scrollback_test PRIVATE util)
    endif()
    set_tests_properties(reveal_scrollback_test PROPERTIES TIMEOUT 180)

    # anthropic_md_stream — dev tool, NOT a ctest entry. Captures a real
    # Anthropic SSE markdown stream to a .jsonl fixture (network +
    # credentials required), or replays a captured fixture through
    # maya's StreamingMarkdown reveal-fx path.
    agentty_add_full_test(anthropic_md_stream NO_TEST)
    # Aggregate target so CI can build every ctest-registered binary in one
    # shot. The individual test targets are EXCLUDE_FROM_ALL (kept out of the
    # default `cmake --build` so a plain build stays lean), which means
    # `ctest` would otherwise find nothing built. `cmake --build build
    # --target tests` forces them all.
    add_custom_target(tests DEPENDS
        long_session_bench
        fuzzy_match_smoke
        param_tag_repair_test
        model_caps_test
        fsm_test
        model_label_test
        skills_engine_test
        midrun_freeze_test
        midrun_seam_test
        midrun_wire_test
        stream_liveness_test
        stream_md_lag_test
        reveal_pacing_test
        composer_edit_test
        scheduler_path_test
        table_render_test
        openai_transport_test
        ollama_transport_test
        rag_test
        rag_hnsw_test
        rag_rerank_test
        rag_expand_test
        rag_advanced_test
        knowledge_test
        visual_hash_coverage_test
        tool_result_budget_test
        salvage_dedup_test
        doom_loop_test
        acp_integration_test
        frozen_invariant_fuzz
        scrollback_wire_fuzz
        reveal_scrollback_test)
    # mcp_bridge_test / mcp_http_test are only registered as ctest entries
    # when MCP is compiled in (AGENTTY_MCP), so they must be added to the
    # aggregate under the same gate — otherwise `cmake --build --target
    # tests` never builds them and ctest reports them "Not Run".
    if(AGENTTY_MCP)
        add_dependencies(tests mcp_bridge_test mcp_http_test)
    endif()
endif()

