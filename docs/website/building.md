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

## Cutting a release (maintainers)

```bash
scripts/cut-release.sh X.Y.Z       # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z       # Windows cmd.exe

scripts/cut-release.sh X.Y.Z --dry-run   # preview the exact diff, write nothing
```

Single source of truth: `CMakeLists.txt`'s `project(agentty VERSION …)` line. `cut-release.sh` bumps it, promotes `CHANGELOG.md`'s `[Unreleased]` section to a dated `[X.Y.Z]`, commits `release: vX.Y.Z`, tags `vX.Y.Z`, and pushes. The tag push fires GitHub Actions, which builds every binary + OS package (Linux x86_64/aarch64 on native runners, macOS Intel/ARM, Windows exe/msi) and auto-submits to winget, Homebrew, Scoop, and the AUR — nix/snap/gentoo manifests are attached to the release. Guards refuse a downgrade, duplicate version, dirty tree, or existing tag.
