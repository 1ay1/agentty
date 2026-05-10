# Changelog

All notable changes to agentty. Versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `--version` / `-V` / `version` flag — prints `agentty <PROJECT_VERSION>` and exits. The version is baked at build time from `CMakeLists.txt`'s `project(... VERSION ...)` line, so bumping the project version updates every site that reads `AGENTTY_VERSION`.
- Queued messages render as preview rows in the conversation transcript (above the composer), visually identical to real user turns. Mirrors Claude Code 2.1.119's behaviour at binary offset 80106500.
- `↑` (Up-arrow) on an empty composer recalls every queued message back into the buffer, joined by `\n`, with the cursor at the recalled-text seam. Destructive on the queue — re-submit to re-queue. Mirrors Claude Code's `Lc_` (offset 76303220).
- Composer placeholder gains a `press ↑ to edit queued — type to queue another…` hint when the queue is non-empty and the buffer is empty (and matching variants for awaiting/idle phases). Mirrors Claude Code's hint at offset 84591379.
- Retry status now shows attempt counter: `transient — retrying in 5s (attempt 2/6)…`.

### Changed
- **Transport reliability.** Anthropic's `Retry-After` HTTP header is now parsed on 429 / 529 responses and used as the authoritative backoff delay, clamped to `[1s, 120s]`. Falls back to the existing 500ms→45s ladder when no header is present, with ±20% jitter applied to break thundering-herd retry sync during regional brown-outs. Inspired by Zed's `parse_retry_after` (`crates/anthropic/src/anthropic.rs:574-580`).
- **Cancel cleanup.** `Esc` now does the full teardown synchronously: drains `streaming_text` into `text` (preserves partial reply), marks every non-terminal `tool_call` as `Failed("cancelled")`, pops the assistant placeholder if it produced no content, and resets `pending_permission`. No more orphan `Running` spinners or empty placeholder cards after cancel.
- Status banner row replaced by a notification takeover on the existing shortcut row — when `m.s.status` is active, the keybindings strip swaps in a single banner-style entry (`▎⚠ <text>` for errors, `▎ <text>` for info) and reverts to bindings when the toast expires. No new rows added.
- `submit_message` now queues on any non-Idle phase (`m.s.active()`) instead of just `is_streaming() || is_executing_tool()`. Defensive — the keymap already gated `AwaitingPermission` via the permission modal — but makes the guarantee structural.

### Fixed
- **`agentty gets stuck — nothing works` after Esc.** A worker thread's trailing `StreamError("cancelled")`, dispatched ~200 ms after the cancel-token trip, was running on the runtime's `active_ctx`. If the user submitted a new turn during that window, the handler's `a->cancel.reset()` would null out the *new* turn's cancel token, leaving `Esc` unable to cancel anything until process restart. `launch_stream` now wraps `dispatch` in a `guarded` lambda that captures the cancel token and short-circuits when tripped — no events from a cancelled worker reach the reducer, so the new turn's state is never touched.
- Removed the redundant `N messages queued` line from the shortcut row; the composer's own `❚ N queued` chip is now the single source of truth for queue depth.

## [0.1.0] — Initial public release

Pre-1.0. Core loop, tools, streaming, permission profiles, in-app auth, persistence, and cross-platform subprocess all working. Linux gets daily smoke testing; macOS and Windows code paths exist (`#ifdef` branches throughout, `posix_spawn` for POSIX, `CreateProcessW` for Windows, `fdatasync`/`fsync` switched per OS) but CI for those platforms is next.

### Major surfaces

- **Native C++26 TUI** rendering through the `maya` widget engine (sister project, FetchContent-pulled from `1ay1/maya`). Single ~9 MB static binary, no Node / Python / Electron runtime.
- **Anthropic provider** speaking HTTP/2 + SSE directly via in-house `nghttp2` + OpenSSL stack. OAuth (PKCE) + API key both wired through the same `auth::cmd_login` path.
- **Tools**: `read`, `write`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`, `diagnostics`, `git_*`. Compile-time effect set + permission policy enforced via `static_assert` on a `constexpr` matrix.
- **Permission profiles**: `Write` (autonomous), `Ask` (read-only auto, write/exec/net prompt), `Minimal` (only pure tools auto). Profile cycle on `S-Tab`.
- **Sandboxed bash** by default — `bwrap` on Linux, `sandbox-exec` on macOS. Windows runs unsandboxed (no first-class equivalent yet).
- **Workspace boundary**: filesystem tools refuse paths outside `--workspace`/cwd.
- **SSH air-gap mode** (`agentty airgap …`): wraps agentty on a remote host with SOCKS5 forwarding for TLS / OAuth / chat traffic. Compression off by default (small bursty deltas not worth zlib sync overhead on inline frames); env vars for terminal identification forwarded across the SSH boundary so DEC 2026 sync still applies on the remote side.
- **Persistence**: threads and credentials in `~/.agentty/threads/` and `~/.config/agentty/credentials.json` (mode 0600). Atomic writes (temp + fsync + rename).
- **Streaming smoothing**: SSE deltas drip into `streaming_text` at ⅛ buffer per Tick (clamped 32–256 chars), so server-side batching doesn't translate into chunky on-screen text.
- **Inline rendering** — agentty never takes over the terminal; output flows in scrollback, status bar overlays. `compose_inline_frame` wraps frames in DEC 2026 begin/end-sync where supported.

### Stubbed honestly (not yet implemented)

- **Checkpoint restore** — `CheckpointId` + per-message marker exist; `RestoreCheckpoint` surfaces "not implemented yet" and does nothing.
- **Diff review pane** — modal renders, but `pending_changes` isn't populated by any tool yet, so review/accept/reject toasts "no pending changes".

### Build

- C++26 (GCC 14+ / Clang 18+); MSVC builds against `/std:c++latest`.
- AppleClang tops out at C++23 — `AGENTTY_BUILD_TESTS` requires `g++` or stock LLVM `clang++` on macOS, not Xcode's bundled toolchain.
- `cmake -B build && cmake --build build`. `AGENTTY_STANDALONE=ON` produces a static binary (libc and usually OpenSSL stay dynamic).
