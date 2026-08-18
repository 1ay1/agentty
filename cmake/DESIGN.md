# agentty CMake redesign

Goal: turn the 2716-line root `CMakeLists.txt` — whose test section alone is
~1200 lines and whose every test-name lives in up to **four** hand-maintained
lists — into a small, layered, drift-proof build where **one declaration per
test** derives every downstream set, and **each repo runs only its own tests**.

This is a *structural* refactor. The compiler/linker flags, the object-library
layout, the doctest consolidation, ccache, and the per-platform hardening are
all good and are **preserved byte-for-byte in effect**. We are moving code, not
changing what it produces.

---

## 1. What's wrong today (measured)

| Smell | Evidence |
|-------|----------|
| **Multi-list name drift** | A test name appears in up to 4 places: the `agentty_add_full_test()` call (70×), `AGENTTY_MIGRATED_TESTS` (56×), the `add_custom_target(tests DEPENDS)` aggregate (25×), and sometimes the explicit `agentty_tests` source list. Forgetting one silently breaks CI ("Not Run" → exit 8 — hit this session). |
| **God file** | One 2716-line `CMakeLists.txt` mixes toolchain probing, standalone/static plumbing, submodule pulls, source groups, hardening, PGO, and the whole test suite. |
| **Prose bloat** | ~1200 test lines are mostly 5-15 line comments. Valuable, but they bury the 1-2 lines of actual build logic and make the file feel unnavigable. |
| **Cross-repo test bleed** | agentty rebuilds 6 maya-owned tests (`reveal_pacing_test`, …) as its OWN ctest entries. maya already gates them in its own CI. Each repo should run only its own tests. |
| **Two "fast iteration" answers** | maya has `MAYA_FAST_TESTS` (a no-LTO `-O1` test lib). agentty has nothing — you hand-make a Debug `build-dev/`. Should be one consistent knob. |
| **Magic ordering** | `agentty_tests` MUST be defined after every `agentty_add_full_test` call (it reads a GLOBAL property they append to). Nothing enforces it; a misplaced call silently drops a test. |

## 2. Target architecture

Split the root file into focused, `include()`-d modules under `cmake/`. The
root `CMakeLists.txt` becomes a ~120-line *table of contents* that includes
them in order.

```
cmake/
  AgenttyOptions.cmake      # all option()/set(... CACHE ...) — the knobs, in one place
  AgenttyToolchain.cmake    # ccache, C++26→23 fallback, IPO probe, MSVC /Ob tweaks, Android API
  AgenttyStandalone.cmake   # STANDALONE / FULLY_STATIC / STATIC_PIE / arch / OpenSSL fallback
  AgenttySubmodules.cmake   # auto-pull + add_subdirectory(maya/mcp/acp/rag) + toggles
  AgenttyDeps.cmake         # FetchContent: mimalloc, (doctest lives in tests module)
  AgenttySources.cmake      # the AGENTTY_*_SOURCES groups + objlib() + AGENTTY_SHARED_OBJECTS
  AgenttyHardening.cmake    # apply_compile_flags(), sanitizer axes, security flags, strip, PGO
  AgenttyTests.cmake        # the registry (see §3) + agentty_tests + aggregates
  AgenttyTestRegistry.cmake # the reusable registry FUNCTIONS (shared design, see §3/§5)
```

Root `CMakeLists.txt` (sketch):

```cmake
cmake_minimum_required(VERSION 3.28)
project(agentty VERSION 0.3.0 LANGUAGES CXX)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(AgenttyOptions)      # knobs first — everything else reads them
include(AgenttyToolchain)
include(AgenttyStandalone)
include(AgenttySubmodules)
include(AgenttyDeps)
include(AgenttySources)
include(AgenttyHardening)

add_executable(agentty ${AGENTTY_RUNTIME_SOURCES})
# … link + hardening applied via functions from the modules …

if(AGENTTY_BUILD_TESTS)
  include(AgenttyTests)
endif()
```

Nothing about *what gets compiled or how* changes. This is pure relocation +
naming, and it makes each concern independently readable and greppable.

## 3. The test registry (the core win)

Replace the 4 parallel lists + 2 helper functions with **one declaration per
test** that records into a single global registry; the aggregates are then
*computed*, never hand-listed.

### API — one call per test

```cmake
agentty_test(<name>
  MODE     consolidated|standalone       # default consolidated
  [SRCS    extra1.cpp ...]               # extra TUs beyond tests/<name>.cpp
  [LIBS    acp::acp ...]                 # extra link libs
  [OBJS    $<TARGET_OBJECTS:...> ...]    # extra object libs
  [TIMEOUT <seconds>]                    # default 60
  [LABELS  sanitizer perf ...]           # ctest labels; drives derived sets
  [PLATFORM_LIBS_UNIX util]             # e.g. openpty in libutil
  [NO_TEST]                              # build but don't register a ctest entry (probes/benches)
  [ENV     "K=V" ...]
  [GATE    AGENTTY_MCP]                  # only define when this var is truthy
)
```

- `MODE consolidated` → the source is appended to the `agentty_tests` binary
  (the doctest single-binary). No standalone exe. **This replaces
  `AGENTTY_MIGRATED_TESTS` entirely** — membership is a property of the call,
  not a separate list to keep in sync.
- `MODE standalone` → its own `EXCLUDE_FROM_ALL` exe (for forkers, PTY, fuzzers,
  sanitizer-isolated, benches) with the shared-object link boilerplate applied
  ONCE inside the function (today it's copy-pasted).
- Every call appends `<name>` to a `GLOBAL PROPERTY AGENTTY_ALL_TESTS` and, if
  labelled, to `AGENTTY_TESTS_<LABEL>`.

### Derived aggregates (no hand-listing)

```cmake
agentty_finalize_tests()   # called once at the end of AgenttyTests.cmake
```

does all of:
- define `agentty_tests` from the accumulated consolidated sources (kills the
  "must be defined last" foot-gun — finalize is explicitly last),
- `add_custom_target(tests DEPENDS <all standalone> agentty_tests)` — computed
  from the registry, so a new test is in the aggregate the moment it's declared,
- `add_custom_target(sanitizer_tests DEPENDS <LABELS sanitizer set> agentty_tests)`,
- optional `add_custom_target(tests-perf DEPENDS <LABELS perf set>)` so CI's
  fast gate can build only the correctness set and skip probes/benches/sweeps,
- apply `TIMEOUT`/`ENV`/`LABELS` to the standalone ctest entries.

Result: **a test name is written exactly once.** The "Not Run" class of CI bug
becomes structurally impossible.

## 4. Each repo runs only its own tests

- **agentty**: drop the 6 `maya/tests/*.cpp` targets (`reveal_pacing_test`,
  `reveal_resume_test`, `stream_async_freeze_test`, `stream_md_lag_test`,
  `reveal_smoothness_probe`, `reveal_lag_probe`). maya's own CI already gates
  them (they're in `MAYA_TEST_SOURCES`). Verified: zero coverage lost.
- **agentty ctest** should not include mcp/acp/rag/maya cases. Today agentty
  forces `MCP_BUILD_TESTS=ON`, so `mcp_tests` etc. run inside agentty's ctest.
  Change: force those submodule `*_BUILD_TESTS=OFF` from agentty (agentty only
  needs the submodule LIBRARIES, not their test binaries). Each submodule's own
  CI runs its own suite. `MCP_BUILD_EXAMPLES` stays ON (mcp_bridge_test needs
  the example server), but the mcp *test* binaries no longer build here.
- Uniform fast-iteration knob: adopt the same idea maya uses. Not required for
  correctness; can be a follow-up. Minimum: document `build-dev/` (Debug, no
  LTO, ccache) as the standard fast loop for all repos.

## 5. Shared registry across submodules (optional, phase 2)

The registry functions in `AgenttyTestRegistry.cmake` are written repo-agnostic
(they take the target's link set as parameters). maya already has an equivalent
`foreach` pattern; mcp-cpp/acp-cpp can adopt the same `*_test()` + `*_finalize`
shape so all five repos read identically. Phase 1 ships agentty's; phase 2
harmonises the submodules if we want the uniformity.

## 7. Status (what shipped on `cmake-redesign`)

| Step | State | Commit |
|------|-------|--------|
| Design doc | ✅ | 9620835 |
| Each repo runs only its own tests (mcp `*_BUILD_TESTS=OFF`, drop 6 maya tests) | ✅ verified | ab27081 |
| Single-declaration test registry; 4-list system deleted | ✅ verified | c2a9e9c |
| Extract `AgenttyToolchain.cmake` | ✅ verified | b13210d |
| Extract Standalone / Submodules / Sources / Hardening modules | ⏸ deferred | — |
| Point CI at `tests_gating` | ⏸ deferred | — |

**Root `CMakeLists.txt`: 2716 → 1447 lines.** The pain the redesign targeted —
the 4-list drift that caused the "Not Run" CI break — is fully eliminated, and
`tests_gating` exists for a leaner CI gate.

### Why the remaining module extractions are deferred
The toolchain block was self-contained and extracted cleanly (cache vars proven
byte-identical). The Standalone / Submodule / Sources / Hardening blocks are
**interleaved** (maya LTO toggles sit in the standalone section; per-target flag
application is order-sensitive; acp/mcp/rag `add_subdirectory` are threaded
through mimalloc + rag-GPU config). A mechanical cut there risks silently
changing platform behavior (Windows static, Linux musl) that can't be verified
on a macOS dev box. These should be extracted one-at-a-time behind CI on all
three platforms, as a follow-up — the value (readability) is lower and the risk
(cross-platform link regressions) is higher than the registry work already
landed.

---

## Original plan


1. Land this design doc (this commit).
2. Extract modules **mechanically** — cut each section into its `cmake/*.cmake`
   file, `include()` from root, no logic change. Configure + build + full ctest
   after EACH extraction to prove parity (the object graph must be identical).
3. Introduce `AgenttyTestRegistry.cmake` with `agentty_test()`/`agentty_finalize_tests()`.
4. Convert the ~70 `agentty_add_full_test` + bare `add_executable` test blocks
   to `agentty_test()` calls, keeping each block's comment directly above its
   one-line call. Delete `AGENTTY_MIGRATED_TESTS` and the hand-listed aggregates.
5. Drop the 6 maya tests; set submodule `*_BUILD_TESTS=OFF`.
6. Diff `ctest -N` before/after — the set of agentty-OWNED cases must be
   identical (minus the 6 maya cases, which move to maya's suite).
7. Update `.github/workflows/ci.yml` to build `tests` (or `tests` + a leaner
   gate) and confirm green on all platforms before merging to master.

### Parity gate (the definition of done)
- `ctest -N` in a `-DAGENTTY_BUILD_TESTS=ON` build lists the same agentty cases
  as master (minus the 6 maya cases).
- `cmake --build build --target tests` builds the same binary set.
- Full `ctest` passes on macOS locally and all CI platforms.
- Net: root `CMakeLists.txt` ≤ ~150 lines; total `cmake/` ≈ what the test
  section was, but each test declared once.
