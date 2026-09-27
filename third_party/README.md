# third_party/

Vendored dependencies. Everything in here is a git submodule — code agentty
consumes but does not own. The top level of the repo is agentty's own
(`src/`, `include/`, `tests/`, `cmake/`, `docs/`); this directory is the rest.

| Directory  | Target(s)                  | Role                                          |
|------------|----------------------------|-----------------------------------------------|
| `maya/`    | `maya::maya`, `maya::app`  | TUI framework: rendering, layout, widgets     |
| `mcp-cpp/` | `mcp::mcp`, `mcp::tools`   | MCP protocol **and the entire tool set**      |
| `acp-cpp/` | `acp::acp`                 | Agent Client Protocol (`agentty acp`)         |
| `rag-cpp/` | `ragcpp::ragcpp`           | retrieval for `search_docs` / `search_code`   |

`maya` carries its own submodule at `maya/third_party/jaal` — jaal is the
generic Elm-architecture runtime kernel. So the dependency chain is
**agentty → maya → jaal**.

All four are hard requirements; a missing or empty checkout is a configure
`FATAL_ERROR`, not a silent degrade. In particular mcp-cpp cannot be switched
off (`AGENTTY_MCP=OFF` fails loudly) because there is no native tool fallback,
and rag-cpp must not be quietly replaced with no-op retrieval stubs.

## Getting them

```sh
git submodule update --init --recursive
```

## Updating them

Builds deliberately do **not** touch the submodules: fetching perturbs header
mtimes and needlessly rebuilds the ~64 TUs that include them. Sync on demand:

```sh
cmake --build build --target submodules_sync   # or: ./resync.sh
```

The pull refuses to touch a submodule holding local work — unstaged changes,
staged-uncommitted work, or local commits not on origin — and uses
`merge --ff-only`, so it never discards anything.

## Notes

- Include paths are unaffected by this directory: headers arrive through target
  include dirs, so it is still `#include <maya/widget/composer.hpp>`, never a
  path relative to here.
- Each submodule runs its own test suite. Embedded in agentty their
  `*_BUILD_TESTS` default to `PROJECT_IS_TOP_LEVEL`, i.e. OFF, so a change to a
  maya header can be green here and red in maya's CI. To check maya properly,
  run `cmake -B build -DMAYA_BUILD_TESTS=ON` inside `third_party/maya/` — that
  is what maya's CI builds.
- `add_subdirectory` mirrors these paths into the build tree, so the artifacts
  land at `build/third_party/<name>/` (e.g.
  `build/third_party/mcp-cpp/examples/date-mcp/date_server`).
