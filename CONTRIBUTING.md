# Contributing

Bug reports, fixes and well-scoped features are welcome.

- **Bugs:** open an issue with `agentty --version`, your OS, and for screen
  bugs your terminal and `$TERM`.
- **Questions:** use [Discussions](https://github.com/agentty-org/agentty/discussions)
  or the [Discord](https://discord.gg/qhb9AZ8f3c).
- **Code:** clone with `git clone --recursive`, build with
  `cmake -B build && cmake --build build -j`, and add
  `-DAGENTTY_BUILD_TESTS=ON` for tests. If your change bumps a submodule
  (maya, claybin, jaal), say so in the PR so the submodule commits can be
  merged first.

Code conventions, building and the release flow are on
https://agentty.org/contributing. By taking part you agree to the
[Code of Conduct](CODE_OF_CONDUCT.md).
