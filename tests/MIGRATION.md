# Test migration: hand-rolled harness → doctest single binary

Goal: collapse the ~70 per-test executables (each statically re-linking the
whole shared object set — the dominant CI cost) into ONE `agentty_tests`
binary that links the object set once. Tests become doctest `TEST_CASE`s,
auto-registered into that binary; `doctest_discover_tests()` still gives each
case its own ctest entry (so `ctest -j` and per-case reporting are preserved).

## The recipe (per test file)

Every test uses one of two legacy harness styles. `tests/agtest.hpp` (include
it instead of `<doctest/doctest.h>`) supports BOTH, so migration is nearly
zero-diff in the test body.

### Style 1 — file has `#define CHECK(...)`
1. Delete the local `#define CHECK(...)` / `#define REQUIRE(...)` and any
   `static int g_failures` / `g_fails` counter.
2. Add `#include "agtest.hpp"` (after std/3rd-party headers, before
   `agentty/...` headers). Drop `#include <cstdio>` **only if** the body no
   longer uses `printf`/`fprintf` (the deleted counter-report tail doesn't
   count).
3. Convert each `static void test_XXX()` that `main()` called into
   `TEST_CASE("readable name") { ...body unchanged... }`.
4. Delete `int main()`. If it had inline asserts (not in a helper), wrap that
   block in its own `TEST_CASE`.

### Style 2 — file has a `void check(bool, ...)` helper + counter
1. Delete the local `void check(...)` AND the counter — the shim provides
   `check(bool)` / `check(bool, const char*)` / `check(bool, std::string)`.
2. Add `#include "agtest.hpp"`; drop `<cstdio>` if now unused.
3. Wrap the whole old `main()` body (minus the trailing counter report/return)
   in ONE `TEST_CASE("<filename without _test>")`, or split by self-contained
   `// ── N. xxx ──` sections into multiple TEST_CASEs.
4. Delete `int main()`.

Then in `CMakeLists.txt`, add the test name to `AGENTTY_MIGRATED_TESTS`. That
single list is the switch: `agentty_add_full_test()` sees the name, appends the
source to the `agentty_tests` binary (via the `AGENTTY_TEST_SOURCES` global
property) and skips the standalone exe. Nothing else in CMake changes for a
normal test.

## Gotchas (each cost a debug cycle — check these)

- **`CHECK(a && b)`** → doctest's expression decomposer rejects logical
  operators ("Expression Too Complex"). The shim already wraps the predicate in
  extra parens, so this is handled — do NOT hand-split these.
- **Local `check` with a DIFFERENT signature** (e.g. `check(id, want)` or a
  3-arg form): consolidating all TUs into one binary makes it collide/ambiguous
  with the shim's `check`. Either delete it (if it just forwards to a doctest
  macro) or RENAME it to something file-local (`expect_label`, `check_edit`).
- **`set_tests_properties(<name> PROPERTIES TIMEOUT n)`** after the helper call
  fails for migrated names (their standalone ctest entry is gone). Replace with
  `agentty_test_timeout(<name> n)` — it no-ops for migrated names.
- **Aggregate lists**: remove the migrated name from the `tests` /
  `sanitizer_tests` `add_custom_target(... DEPENDS ...)` blocks and the
  sanitizer `LABELS` `set_tests_properties` list (those name standalone
  targets/ctest entries that no longer exist).
- **Extra link deps**: if a test's standalone registration passed
  `LIBS acp::acp` or linked `$<TARGET_OBJECTS:agentty_acp_obj>` /
  `ragcpp::ragcpp`, the consolidated binary needs the same (already wired for
  acp/rag/acp_obj; add more `if(TARGET ...)` blocks as needed).
- **`<cstdio>` removal**: keep it if the test BODY still prints diagnostics.
- **Test-body setup that a per-process exe got for free** (e.g. a frozen
  animation clock, a chdir into a temp dir): make sure it runs INSIDE the
  TEST_CASE, not at former file scope, or the shared binary won't set it up.
- **TEST_CASE inside an anonymous namespace does NOT register.** Some files
  put their helpers (and formerly their test fns) in a `namespace { ... }`.
  doctest auto-registration relies on external linkage, so CLOSE the anon
  namespace (`} // namespace`) after the helpers and BEFORE the first
  TEST_CASE. The helpers stay visible (same TU).
- **Scripted main() removal is unreliable** when main() has nested braces (a
  regex like `int main\(\)\s*\{[^}]*\}` stops at the first inner `}` and
  leaves a tail fragment). Delete `int main(){...}` by hand, or verify no
  orphaned `return`/`printf` lines remain after the last TEST_CASE.

## Keep STANDALONE (do NOT consolidate)
Their process/ODR isolation is load-bearing:
- **Cross-test global-state contamination**: a test that `chdir`s into a temp
  dir, sets `HOME`/env, or otherwise mutates process-global state can pass
  alone but CORRUPT later cases in the shared binary (they inherit the changed
  cwd/env). Symptom: cases pass with `--test-case=X` in isolation but fail when
  the whole binary runs. Such e2e/integration tests (toolset_e2e,
  subagent_report, plugin_disabled_tools) stay standalone. ALWAYS run the full
  `agentty_tests` binary after a batch, not just the new cases, to catch this.
- **fork/exec/posix_spawn** tests (cred_crypt, concurrency_primitives,
  cross_process_lock, external_acp_backend, fork_test).
- **PTY / openpty** tests (scrollback_oracle, reveal_scrollback).
- **Sanitizer-isolated** subset (needs ODR-clean single-purpose TUs).
- **Fixture-arg ctest entries** that drive one binary with different args
  (reveal_stream_gate*), and e2e tests that set a per-test `ENVIRONMENT`
  (mcp_bridge_test's AGENTTY_MCP_E2E_SERVER, mcp_http_test).
- **Dev probes / benchmarks / fuzzers** (`*_probe`, `*_bench`, `*_fuzz`) —
  timing loops, not assertions; often `NO_TEST` already.

## Verify each file after converting

`grep` it: has `#include "agtest.hpp"`, has ≥1 `TEST_CASE`, has NO `int main`.
Then build `agentty_tests` and run it — cases must pass and ctest must list
them. Migrate in small batches (3–5) with a build+run gate; consolidation
surfaces cross-file collisions that only appear when linked together.

## Submodules

maya / mcp-cpp / rag-cpp / acp-cpp each get the same treatment: vendor doctest
(or reuse this one), a `<repo>_tests` single binary + `test_main.cpp`, the same
`agtest.hpp` shim, `doctest_discover_tests`. They're separate repos with their
own CI, so they land as follow-up PRs using this identical recipe.
