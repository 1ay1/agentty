# Termux packaging

`packaging/termux/build.sh` is a [termux-packages](https://github.com/termux/termux-packages)
recipe that builds agentty from source on-device (or in the Termux build
container) and installs it to `$PREFIX/bin/agentty`, so it can be installed
with `pkg install agentty` once merged into the Termux repos.

## Why a source build (not the release binary)

Every other channel in `packaging/` installs the prebuilt fully-static GitHub
release artifact. Termux can't reuse those: the Linux release binaries target
glibc/musl on generic Linux, whereas Termux is a Bionic (Android libc)
environment. So the Termux recipe compiles from source against Termux's own
clang + system OpenSSL / nghttp2.

Two source-layout facts shape the recipe:

- **Submodules.** agentty vendors `maya`, `acp-cpp`, and `mcp-cpp` as git
  submodules. A GitHub source *tarball* (`archive/refs/tags/…`) contains none
  of them, so the recipe uses `TERMUX_PKG_SRCURL=git+…` + `TERMUX_PKG_GIT_BRANCH`
  and a `termux_step_post_get_source` hook that runs
  `git submodule update --init --recursive`.
- **FetchContent.** The top-level CMake `FetchContent`s `nlohmann-json` and
  `simdjson` at configure time. That step needs network, which Termux allows
  during the get-source / configure phase. (`nlohmann-json` + `simdjson` are
  also in the Termux repos and listed as build-deps so the container has them
  cached; a future revision could patch the CMake to `find_package` them and
  drop the fetch entirely.)

## Build flags

| Flag | Why |
|------|-----|
| `-DAGENTTY_AUTO_PULL_MAYA=OFF` | The default runs `git reset --hard origin/master` on the maya submodule — a network op that must not happen in a package build. |
| `-DAGENTTY_USE_MIMALLOC=OFF`   | mimalloc isn't packaged for Termux. agentty falls back to the system allocator. |
| `-DAGENTTY_STANDALONE=OFF`     | Produce a normal dynamically-linked Termux (PIE) binary that links Termux's shared OpenSSL/nghttp2 — the fully-static release layout doesn't apply here. |
| `-DAGENTTY_BUILD_TESTS=OFF`    | Ship the binary only. |

## Dependencies

- **Runtime:** `openssl`, `libnghttp2`, `libc++`
- **Build:** `nlohmann-json`, `simdjson` (plus `cmake`, `ninja`, `clang` from the base build image)

All are present in the Termux repositories (verified: OpenSSL 3.6, nghttp2 1.69,
nlohmann-json 3.12, simdjson 4.6). mimalloc is intentionally omitted.

## Submitting to the Termux repos

The recipe lives here in-tree for reference; to make `pkg install agentty`
actually work it must be merged into `termux/termux-packages`:

```sh
git clone https://github.com/termux/termux-packages
cd termux-packages
mkdir packages/agentty
cp /path/to/agentty/packaging/termux/build.sh packages/agentty/build.sh

# Build inside the official container (matches the CI toolchain):
./scripts/run-docker.sh ./build-package.sh agentty

# When it builds clean, open a PR to termux/termux-packages.
```

Until that PR merges, install on-device straight from source:

```sh
pkg install git cmake ninja clang openssl libnghttp2 nlohmann-json simdjson
git clone --recursive https://github.com/1ay1/agentty
cd agentty
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DAGENTTY_AUTO_PULL_MAYA=OFF -DAGENTTY_USE_MIMALLOC=OFF \
      -DAGENTTY_STANDALONE=OFF -DAGENTTY_BUILD_TESTS=OFF
cmake --build build -j4
install -Dm755 build/agentty "$PREFIX/bin/agentty"
```
