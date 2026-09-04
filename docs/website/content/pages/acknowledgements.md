---
title: Acknowledgements
description: The projects and people that make agentty possible.
slug: acknowledgements
---

agentty stands on the shoulders of excellent open source software. Thank you to the maintainers of every project below.

## Built with

- [**maya**](https://github.com/1ay1/maya) — The sister TUI engine agentty renders through — owns every glyph, layout, and animation.
- [**OpenSSL**](https://www.openssl.org/) — TLS and cryptography for the HTTP/2 transport and OAuth (PKCE).
- [**nghttp2**](https://nghttp2.org/) — The HTTP/2 implementation behind the streaming providers.
- [**nlohmann/json**](https://github.com/nlohmann/json) — JSON parsing and serialization for the API and thread persistence.
- [**Bubblewrap**](https://github.com/containers/bubblewrap) — The Linux sandbox (bwrap) that isolates shell and build calls.
- [**CMake**](https://cmake.org/) — The build system that produces the single static binary across platforms.

## Inspiration

agentty exists because of [Anthropic's](https://www.anthropic.com/) Claude and the `claude-code` experience — the goal was a native, dependency-free client for the same workflow. Backoff and transport details drew on ideas from the Zed editor's Anthropic crate.

## Contributors

agentty is maintained by its contributors. See the full list on [GitHub](https://github.com/1ay1/agentty/graphs/contributors). Want to join them? Start with the [contributing guide](/contributing).

## License

agentty is released under the [MIT License](/license). Bundled third-party components retain their own licenses; see the repository for details.
