---
title: Contributing
description: How to contribute to agentty — bugs, code, and the maintainer workflow.
slug: contributing
---

agentty is open source under the MIT license. Bug reports, fixes, and well-scoped features are all welcome.

## Reporting bugs

File issues on [GitHub](https://github.com/1ay1/agentty/issues). For a rendering bug, include your `$TERM`, your terminal emulator name, and a screenshot. For a code-path bug, paste the relevant block and your `git rev-parse HEAD` (or the release version from `agentty --version`).

## Setting up

1. Fork and clone with submodules: `git clone --recursive`.
2. Build: `cmake -B build && cmake --build build -j`.
3. Run the tests with `-DAGENTTY_BUILD_TESTS=ON` (needs GCC or stock Clang, not AppleClang).

See [Building from Source](/docs/building) for toolchain requirements.

## Code conventions

- **The reducer stays pure.** Side effects go through `Cmd`, never inline in the update function.
- **The permission matrix is the contract.** New tools declare their effect set; `static_assert` enforces it at compile time.
- **The host builds Configs, maya builds Elements.** Keep rendering logic out of the host.
- Use the strong ID newtypes — don't pass raw strings where a `ThreadId` or `ToolCallId` belongs.

## Pull requests

- Keep PRs focused — one logical change per PR.
- Describe the behavior change and how you verified it.
- Update `CHANGELOG.md` under **Unreleased** for user-visible changes.
- Make sure the build is green on Linux at minimum.

## Releases

Maintainers cut releases with one command: `scripts/cut-release.sh X.Y.Z` bumps the version, promotes the `CHANGELOG.md` **Unreleased** section to a dated entry, commits, tags `vX.Y.Z`, and pushes. The tag push fires GitHub Actions, which builds every artifact (deb/rpm/pkg.tar.zst/tarball/binaries) and auto-submits to Homebrew/Scoop/winget/AUR. Single source of truth: `CMakeLists.txt`.

## Code of Conduct

By participating you agree to uphold our [Code of Conduct](/code-of-conduct). Be kind, be constructive.
