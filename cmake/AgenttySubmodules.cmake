# AgenttySubmodules.cmake — dependency acquisition phase: the submodule
# auto-pull helper, FetchContent for maya/json/simdjson/doctest/mimalloc, the
# acp/mcp/rag add_subdirectory() calls with their per-submodule toggles, and
# the nghttp2 imported-target discovery. Included as one contiguous block so
# ordering (toggles BEFORE add_subdirectory; deps BEFORE the targets that link
# them) is preserved exactly. include() keeps CMAKE_CURRENT_SOURCE_DIR at the
# top level, so add_subdirectory(maya) etc. resolve as before.

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

