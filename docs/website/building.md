---
title: Building from Source
description: Compile agentty with CMake, including the standalone static build.
nav_section: Advanced
nav_order: 60
slug: building
---

agentty builds with CMake and a C++26 toolchain. Cutting a release is a single command that tags and pushes; GitHub Actions builds every binary and OS package.

## Requirements

- GCC 14+ / Clang 18+ / MSVC 14.40+ (`/std:c++latest`)
- CMake 3.28+
- OpenSSL and nghttp2 (FetchContent pulls maya automatically)

:::warn
AppleClang tops out at C++23 — building the tests (`AGENTTY_BUILD_TESTS`) requires `g++` or stock LLVM `clang++` on macOS, not Xcode's bundled toolchain.
:::

## Basic build

```bash
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty
cmake -B build
cmake --build build -j
./build/agentty
```

## Standalone (static) build

```bash
cmake -B build -DAGENTTY_STANDALONE=ON
```

Statically links OpenSSL + nghttp2 + libstdc++ + libgcc when their `.a` archives are installed, while libc stays dynamic. For a 100% static binary that runs on any Linux userland, pass `-DAGENTTY_FULLY_STATIC=ON`.

The prebuilt Linux release binaries are **true standalone executables**: linked `-static -no-pie` into a classic `ET_EXEC` with no `NEEDED` entry and no `PT_INTERP`, so one file runs on glibc (Debian/Ubuntu/Fedora), musl (Alpine), and 64-bit Raspberry Pi OS alike. A build-time ELF-shape assertion (`cmake/assert_static_pie.cmake`) hard-fails the compile if the artifact ever regains a dynamic dependency. Termux/Android needs a PIE — build that with the opt-in `-DAGENTTY_STATIC_PIE=ON` on a musl toolchain.

## Optimized builds

Release builds already ship with link-time optimization and a stripped symbol table. Two opt-in levers squeeze out more, for a local build you run yourself:

```bash
# mimalloc allocator — measurable keystroke-latency win on the
# allocation-heavy render/parse paths. Not enabled on the fully-static
# release binaries (allocator override under static musl is a hazard).
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAGENTTY_USE_MIMALLOC=ON

# Profile-guided optimization (two phases). Phase 1 builds an instrumented
# binary and runs a scripted PTY workload over the hot paths; phase 2
# rebuilds using the collected counters.
cmake -B build-pgogen -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=generate
cmake --build build-pgogen -j$(nproc) --target agentty
scripts/pgo-train.sh build-pgogen/agentty
cmake -B build-pgouse -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=use
cmake --build build-pgouse -j$(nproc) --target agentty
```

See [Performance](/docs/performance) for what each buys you.

## Cutting a release (maintainers)

```bash
scripts/cut-release.sh X.Y.Z       # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z       # Windows cmd.exe

scripts/cut-release.sh X.Y.Z --dry-run   # preview the exact diff, write nothing
```

Single source of truth: `CMakeLists.txt`'s `project(agentty VERSION …)` line. `cut-release.sh` bumps it, promotes `CHANGELOG.md`'s `[Unreleased]` section to a dated `[X.Y.Z]`, commits `release: vX.Y.Z`, tags `vX.Y.Z`, and pushes. The tag push fires GitHub Actions, which builds every binary + OS package (Linux x86_64/aarch64 on native runners, macOS Intel/ARM, Windows exe/msi) and auto-submits to winget, Homebrew, Scoop, and the AUR — nix/snap/gentoo manifests are attached to the release. Guards refuse a downgrade, duplicate version, dirty tree, or existing tag.

The pipeline is **fully automatic and self-verifying** — after `cut-release.sh` there is nothing left to do by hand:

- A final `verify-release` job runs dead-last and checks each channel's LIVE state (release assets, Homebrew formula, Scoop manifest, AUR `pkgver`, a winget PR for the version). If any channel whose secret is configured did **not** reach the new version, the run goes **red** and names the channel — so a green release genuinely means every channel is up to date. Channels with no secret set are reported as skipped and never fail the gate.
- A separate `reconcile-manifests` workflow re-pins AUR/Homebrew/Scoop from the release's `SHA256SUMS` (no rebuild) **automatically when the release run completes**, and again **weekly** — so if a build leg was slow/flaky and a publisher was skipped, the package still catches up on its own.
- The winget submission gates on `checksums-final` and verifies the MSI hash against `SHA256SUMS` before opening its PR, so it can never submit a hash that drifted from the released asset.
