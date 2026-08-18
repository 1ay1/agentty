# CMake best practices for agentty — research + gap analysis

Researched against the canonical modern-CMake sources (HSF training / CLIUtils
"Modern CMake", Daniel Pfeifer's "Effective CMake", the CMake 3.16 PCH/unity
guidance, and the target-based idioms) and mapped onto THIS project: a
header/template-heavy C++26 app whose submodules (maya, mcp-cpp, acp-cpp,
rag-cpp) are structurally similar header-first libraries.

The verdict up front: **agentty already follows the hard-to-get-right modern
practices.** The gaps are targeted, not structural.

---

## 1. The principles (from the research) and how we score

| Principle (modern-CMake consensus) | agentty today |
|---|---|
| **Everything is a target**; usage requirements (`target_*` PUBLIC/PRIVATE/INTERFACE), never global `include_directories`/`link_libraries` | ✅ objlibs + `target_link_libraries` throughout |
| **Namespaced imported targets** (`Foo::Foo`); a `::` in a link name = a real target, fails loudly if missing | ⚠️ mcp exposes `mcp::mcp`/`mcp::tools`; **maya/acp/rag link by bare name** (`maya::maya` used but not all ALIAS'd) — see Gap A |
| **Mark third-party headers `SYSTEM`** so their warnings don't pollute your build | ⚠️ mcp does (`INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`); agentty's own submodule includes not uniformly SYSTEM — Gap B |
| **`PROJECT_IS_TOP_LEVEL` guard** (CMake ≥3.21) — only run app-level setup (CTest, install, docs, `-march`, LTO force) when you ARE the top project, so the lib composes cleanly under `add_subdirectory`/FetchContent | ❌ we use `AGENTTY_BUILD_TESTS` + `FORCE` cache overrides instead — works, but not the idiomatic guard — Gap C |
| **`add_subdirectory(... EXCLUDE_FROM_ALL)`** for vendored deps so their targets/tests don't join your `all` | ✅ rag-cpp uses it; ✅ submodule `*_BUILD_TESTS=OFF` (this week's work) |
| **`BUILD_INTERFACE`/`INSTALL_INTERFACE` generator expressions** on include dirs | ✅ maya uses `$<BUILD_INTERFACE:...>` |
| **Compiled OBJECT library reused across targets** (compile once, link many) — the key lever for a template-heavy tree | ✅ `AGENTTY_SHARED_OBJECTS` = `$<TARGET_OBJECTS:...>`, the single best thing we do |
| **Full git hash pinning** for FetchContent, not tags | ✅ submodules are gitlinks (exact SHA) |
| **Test names declared once**, aggregates derived | ✅ the registry we just built |

## 2. The template/header-heavy angle (the part specific to us)

A C++26 tree this header-heavy pays its cost in **redundant template
instantiation across TUs**. The research's four levers, ranked by fit:

1. **ccache (content-hashed)** — ✅ DONE. The single biggest win for a
   header-heavy tree: a submodule header edit that changes no *preprocessed*
   content is a cache hit across all dependent TUs. Already wired + tuned.
2. **Compile-once object libraries** — ✅ DONE. `$<TARGET_OBJECTS>` means the
   ~120-TU shared set is instantiated once and *linked* (not recompiled) into
   every one of the ~25 test binaries. This is why our 641-edge build is mostly
   cheap link/scan, not compile.
3. **Precompiled headers (`target_precompile_headers`, CMake ≥3.16)** — ❌ NOT
   used. For a tree where nearly every TU includes the same heavy prefix
   (`<maya/...>`, `<nlohmann/json>`, `<string>/<vector>/<expected>`), a PCH on
   the shared object libs is the highest-leverage UNUSED lever. Gap D.
4. **Unity/jumbo builds (`UNITY_BUILD`)** — ⚠️ situational. Great for a clean CI
   from-scratch build, but hurts incremental (one edit rebuilds a whole jumbo
   TU) and can surface ODR/anonymous-namespace clashes. NOT recommended as a
   default here; leave as an opt-in CI-only knob at most.

## 3. Similar-submodule strategy (maya/mcp/acp/rag)

The research favors, for sibling libraries you control and edit in lockstep,
**git submodules + `add_subdirectory`** (source-level, one build graph, edit-
and-rebuild) over FetchContent (which re-downloads and is better for
*third-party* deps you don't touch). agentty already does exactly this. ✅

The refinement the research points to for *similar* submodules: **hoist the
shared conventions to the super-project so each submodule doesn't re-derive
them.** We already do this for the compiler cache and C++ standard (set at
agentty root, inherited by every `add_subdirectory`). The next step is a small
shared include the submodules could `include()` for their common target setup
(warnings, SYSTEM include marking, the `::` ALIAS) — Gap E, phase 2.

## 4. Concrete gaps, ranked by value

- **Gap D — PCH on the shared object libraries.** Highest build-time ROI still
  on the table for this template-heavy tree. Add `target_precompile_headers`
  to the objlib factory with the ~10 ubiquitous headers. Measure; keep only if
  it wins (the research warns PCH can lose on many *small* targets — but our
  objlibs are large, the ideal PCH case). Gate behind an option, default ON for
  the app build, and REUSE_FROM across objlibs so it's compiled once.
- **Gap C — `PROJECT_IS_TOP_LEVEL` guard.** Wrap the app-only setup (LTO force,
  `-march`, install, the submodule `*_BUILD_TESTS` FORCE overrides) so agentty
  composes cleanly if ever consumed as a subdirectory. Low effort, idiomatic,
  future-proofs the "each repo builds standalone" goal.
- **Gap A — uniform `::` ALIAS targets** for maya/acp/rag (mcp already has it).
  Makes a missing/renamed dep fail at configure with a clear error instead of a
  confusing link error, and documents intent.
- **Gap B — `SYSTEM` include marking** for the submodule headers agentty
  consumes, so a submodule's warnings never fail agentty's `-Werror` build.
- **Gap E — shared submodule convention module** (phase 2): a
  `cmake/CommonTargetSetup.cmake` the siblings include for warnings + ALIAS +
  SYSTEM, so all five repos read identically.

## 5. What NOT to do (research cautions that apply here)

- Don't `file(GLOB)` sources without `CONFIGURE_DEPENDS`, and prefer explicit
  lists for correctness — we already use explicit `AGENTTY_*_SOURCES` lists. ✅
- Don't force unity builds globally (incremental + ODR hazards).
- Don't over-extract the root: the target's own definition (objlibs, link
  knobs, hardening, the exe) belongs in one readable place — which is exactly
  where we stopped the extraction. ✅
- Don't use raw `find_package` variables — always the imported target. ✅

## Bottom line

Structurally, agentty is already a **modern, target-based, compile-once**
CMake project — better than most real-world C++ trees. For a header/template-
heavy build the two remaining high-value moves are **PCH on the object libs
(Gap D)** and the **`PROJECT_IS_TOP_LEVEL` guard (Gap C)**; the rest (ALIAS,
SYSTEM, shared submodule module) are polish that also serve the "each repo
builds cleanly on its own" goal.

---

## 6. IMPLEMENTATION OUTCOMES (branch cmake-best-practices)

| Gap | Outcome |
|-----|---------|
| **D — PCH** | Implemented (objlib factory + REUSE_FROM), then **MEASURED net-negative** on 8-core Apple clang: cold objlib compile 89 s → 96-98 s, single-TU incremental 1.70 s → 1.77 s. The libc++ prefix balloons to a ~19 MB PCH whose per-TU load + serial anchor + lost parallelism beat the parse savings. **Defaulted OFF**; machinery kept opt-in (`-DAGENTTY_PCH=ON`) for low-core/cold-IO CI or other toolchains. Data-driven, not cargo-culted. |
| **C — top-level guard** | Applied where it MATTERS — the submodules, not agentty (agentty is the top app; a guard there is always-true). maya/mcp-cpp/acp-cpp now default `*_BUILD_TESTS` to `PROJECT_IS_TOP_LEVEL` (rag-cpp already did). Verified: standalone → tests ON, embedded in agentty → OFF. "Each repo runs its own tests" is now STRUCTURAL, not enforced by agentty's FORCE overrides. |
| **A — `::` ALIAS** | Only maya lacked one (agentty synthesised `maya::maya` as a workaround). maya now owns `maya::maya`; agentty's compensating alias is already guarded (`if(NOT TARGET maya::maya)`) so it's a clean no-op. mcp/acp/rag already had theirs. |
| **B — SYSTEM includes** | Already done before this work — `AgenttySubmodules.cmake` marks maya/mcp/acp/simdjson/nlohmann headers SYSTEM (`SYSTEM TRUE` property). No change needed; the original gap analysis was pessimistic. |
| **E — shared submodule module** | **DELIBERATELY SKIPPED.** The submodules are independent git repos; a shared cmake module would have to be VENDORED into each (sync burden across 4 repos) to remove ~10 lines of stable boilerplate — net-negative coupling. The `PROJECT_IS_TOP_LEVEL` snippet is the right amount of duplication for repos that must each build standalone. |

**Net:** the two genuinely-valuable moves (C structural test isolation, A canonical alias) landed; D was implemented-and-measured to an honest OFF default; B was already done; E was correctly declined. No cargo-culting.

## 8. Deep build-time profiling (why PCH loses, what actually costs)

Ran `-ftime-trace` on the heaviest TUs + parsed the ninja log. The numbers
settle the design questions rigorously:

**Per-TU cost (Release, no ccache):** 124 TUs, 903 TU-seconds, mean 7.3 s; tail
is brutal (server.cpp 26.6 s, bridge.cpp 23 s, external_acp_backend 22.8 s).

**Where the frontend 8 s/TU goes (server.cpp trace):**
- Header PARSING: ~0 s. Headers are cheap; every `Source` event is sub-ms.
- Template INSTANTIATION: 30 s cumulative `InstantiateFunction`, dominated by
  `std::variant` machinery (~11 s: `__union`, `__visitation::__make_fdiagonal`,
  `__dispatcher`, `__ctor`/`__dtor`) + `std::function` + `std::vector`. This is
  maya's wide `Element` variant × `std::visit` everywhere: `std::visit`
  generates O(N²) dispatch matrices clang re-instantiates PER TU.

**Consequence — PCH is the WRONG tool here, rigorously:** a PCH caches parsed
*headers*, but header parsing is ~0 s and the cost is *this-TU* template
instantiation, which a PCH cannot cache. That's why the measured PCH was
net-negative (19 MB load cost, zero instantiation savings). Confirmed OFF.

**What actually makes it fast (all already in place):**
- **ccache**: a warm full rebuild (rm all objects, rebuild) is **2.5 s** — the
  frontend cost is paid ONCE per unique content, never again on pull/switch.
- **Object libraries**: the 124-TU set compiles once, links into ~25 binaries.
- **`dev` preset** (`-O0` Debug): skips the ~10 s/TU `-O3` backend for iteration.

**The one lever deliberately NOT taken:** `extern template` for the hot
`std::variant`/`std::visit` instantiations would cut COLD frontend time, but
(a) it's deep, correctness-risky surgery in maya's core Element type, (b) it
only helps the cold build — which ccache already makes a one-time cost, and
(c) the payoff (~90 s→60 s cold, once) doesn't justify risking a green,
shipping renderer. Data-driven decline, not an oversight. If cold-build time
ever becomes a real pain (e.g. CI without ccache), the highest-ROI move is
`extern template std::visit`/variant dispatch in maya, NOT a PCH.

### 8a. UNITY BUILD — IMPLEMENTED (the fast lever, measured)

Acted on the profiling: added `AGENTTY_UNITY_BUILD` (default OFF) which sets
`UNITY_BUILD` on agentty's object libraries (batch 8). Because the cost is
shared `std::variant`/`std::visit` instantiation, batching TUs so those
instantiate ONCE per batch is a big win:

| metric | non-unity | unity | Δ |
|--------|-----------|-------|---|
| cold full objlib compile (Release, no ccache) | 94.2 s | 43.5 s | **2.2×** |
| largest single objlib (runtime, 63 TUs) | 50.5 s | 15.6 s | 3.2× |
| warm 1-file incremental (ccache) | 0.23 s | 0.27 s | negligible |

Modern C++ was PRESERVED — the only source changes were genuine improvements
that unity surfaced as latent issues:
  * `scope/trust.cpp`: an anonymous-namespace `home_dir()` that duplicated an
    identical one in `scope.cpp` — renamed `trust_home_dir()` (real dedup).
  * `provider/external_acp_backend.cpp`: bare `acp::` was ambiguous with
    agentty's own `agentty::acp` (the ACP server ns) — pinned via
    `namespace acp = ::acp;` (removes a real ambiguity).
  * `src/acp/server.cpp` (the sole file opening `agentty::acp`) is excluded
    from batching via `SKIP_UNITY_BUILD_INCLUSION` — CMake property, no code
    change; the rest of the objlib still batches.
No template/pattern/style changed. 250/250 tests pass under unity; binary
identical behaviour.

DEFAULT OFF (unity penalises the incremental dev loop — editing one file
rebuilds its batch), scoped to agentty's objlibs only (maya builds normally, so
no maya changes needed). CI's Linux gate + the `ci` preset turn it ON — that's
where a cold runner build pays the 2.2×. Local dev keeps the `dev` preset
(non-unity, ccache) for instant incremental.

## 9. CMakePresets.json (modern reproducible configs)

Added `CMakePresets.json` (schema v6): named configure/build/test presets that
replace the `-D` flag soup with `cmake --preset dev|release|ci|sanitizer|
standalone|pch`. Each pins its own `build/<preset>` dir + generator + option
set; buildPresets bind targets (e.g. `ci` → `all;tests_gating`) and testPresets
bind ctest filters (e.g. `ci` excludes the `perf` label, `sanitizer` includes
the `sanitizer` label) so a contributor reproduces any CI/dev config in one
command. Verified all six configure; `dev` is the fast Debug+ccache loop the
workflow already used by hand. CI itself was deliberately LEFT on its explicit
flags (the green cross-platform gates aren't worth risking on a preset
translation that can't be validated locally on Windows) — the presets mirror
them for local use.

