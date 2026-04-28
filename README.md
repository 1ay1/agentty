# moha

A native terminal client for Claude. C++26, no Electron / Node / Python in the loop.

<p align="center">
  <img src="moha.png" alt="moha" />
</p>

- **One binary.** Statically linked except libc; spawns in milliseconds, no JIT warmup, no GC pauses mid-stream.
- **Read every line.** The reducer is one `std::visit` over a closed event sum. The permission trust matrix is a `constexpr` function with `static_assert`s — change a policy cell and the build breaks, not a test that nobody runs.
- **Sandboxed tools.** `bash` and `diagnostics` execute inside `bwrap` (Linux) / `sandbox-exec` (macOS). Workspace + system libs + network are reachable; `~/.ssh`, `/etc`, other projects are read-only. Even an approved bash call can't `cat ~/.ssh/id_rsa`.
- **Workspace boundary.** Filesystem tools refuse paths outside the directory you launched from. `--workspace /` opts out.
- **Inline render.** Lives at the bottom of your terminal, preserves scrollback, doesn't take over the screen.

## Install

```bash
git clone --recursive git@github.com:1ay1/moha.git
cd moha
cmake -B build && cmake --build build
./build/moha
```

GCC 14+ / Clang 18+, CMake 3.28+. Auth happens in-app on first launch.

## Getting started

```bash
cd path/to/your/project   # cwd is the workspace root
./build/moha              # or `moha` if it's on PATH
```

### Auth

First launch opens an auth modal with two paths:

- **API key.** Paste an Anthropic-issued `sk-ant-…` token. Saved to `~/.config/moha/credentials.json` (POSIX, `0600` perms) or `%USERPROFILE%\.config\moha\credentials.json` (Windows, restrictive ACL).
- **OAuth (Claude Pro/Max).** Opens your browser to the Anthropic consent screen; the callback returns a token stored in the same `credentials.json`. The file records which auth kind it holds, so on relaunch moha picks the right header automatically (`x-api-key:` vs `Authorization: Bearer`).

Override order, highest priority first — useful for ephemeral sessions, CI, or testing alternate accounts without touching the saved creds:

1. `-k <key>` / `--key <key>` — single-session API key, never written to disk.
2. `ANTHROPIC_API_KEY` env var — API-key flow.
3. `CLAUDE_CODE_OAUTH_TOKEN` env var — OAuth flow.
4. The on-disk `credentials.json` from the modal.

CLI subcommands cover the rest:

- `moha status` — prints the resolved auth state: which env vars are set, whether the on-disk creds are present, which one will actually be used.
- `moha login` — runs the auth flow non-interactively (useful for first-time setup over SSH or in scripts).
- `moha logout` — clears `credentials.json`. Next launch returns to the modal.

Then you're in a thread. Type, hit `Enter`. The model has the full tool catalog (read/write/edit/bash/grep/git/web — see below); mid-stream typing queues your next message and lands it when the current turn finishes. `Esc` cancels a streaming response or rejects a permission prompt.

You start in the `Ask` profile — writes, shell calls, and network calls each prompt before running. `S-Tab` cycles to `Write` (autonomous, no prompts) or `Minimal` (prompts for everything but pure reads). Profile choice persists across restarts.

Threads live at `~/.moha/threads/<workspace-hash>/`, one JSON file per thread; safe to inspect, back up, or delete. `^J` opens the thread list — pick an old one, fork it, or hit `^N` for a new thread in the same workspace.

To run against a different workspace without `cd`-ing:

```bash
./build/moha --workspace ~/code/other-project
```

Filesystem tools refuse paths outside the workspace. Pass `--workspace /` to opt out.

## What ships

- **Streaming** with mid-stream input queuing — type while the model answers, your message lands when it's done.
- **Threads** persisted under `~/.moha/`. Browse / fork / delete from `^J`.
- **Markdown** with syntax-highlighted code blocks.
- **Tools** — `read`, `write`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`, `diagnostics`, `git_*`. Each one gets a purpose-built widget: diffs render as diffs, search results group by file with line numbers, bash shows exit codes, todos become checklists.
- **Permission profiles** — `Write` (autonomous), `Ask` (prompt before any Exec/WriteFs/Net call), `Minimal` (prompt for everything except Pure). Cycle with `S-Tab`.
- **Auth, in-app.** Paste an API key (`sk-ant-…`) or OAuth against your Claude Pro/Max subscription. Credentials live at `~/.config/moha/` with `0600` perms (POSIX) / restrictive ACLs (Windows). `ANTHROPIC_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, and `-k` still work.

## Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^T     todo / plan
Esc        cancel / reject        ^/     model picker
S-Tab      cycle profile          ^N     new thread
                                  ^C     quit
```

## How it works

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. Strong ID newtypes (`ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier`) — swapping arguments is a compile error, not a debugging session.

View is a single function `Model -> Element`. Rendering is delegated to [maya](https://github.com/1ay1/maya), a sister header-mostly TUI engine — moha builds widget Configs from `Model` state, maya owns every chrome glyph, layout decision, and breathing animation. The host constructs no Elements.

Subprocess uses `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX, `CreateProcessW` + a reader thread on Windows — no GNU `timeout` dependency, no `popen` quoting hazards. File writes are atomic (`write` + `fsync`/`_commit` + `rename`/`MoveFileExW`).

Deep dive: [`docs/RENDERING.md`](docs/RENDERING.md) walks the view pipeline turn-by-turn; [`docs/UI.md`](docs/UI.md) is the per-widget Config reference.

## Standalone build

```bash
cmake -B build -DMOHA_STANDALONE=ON
```

Statically links OpenSSL + nghttp2 + libstdc++ + libgcc when their `.a` archives are installed. libc stays dynamic on Linux/macOS (fully-static glibc breaks `getaddrinfo` and the NSS resolver). Pass `-DMOHA_FULLY_STATIC=ON` with a musl toolchain for a 100% static binary. Windows: implies `/MT` and pulls third-party libs from the `x64-windows-static` vcpkg triplet.

So the accurate one-liner: **statically linked except libc and (usually) OpenSSL.**

## Status

Pre-1.0. Core loop, tools, streaming, permission profiles, in-app auth, persistence, and cross-platform subprocess all work and are built daily.

Stubbed honestly:
- **Checkpoint restore** — `CheckpointId` + per-message marker exist; `RestoreCheckpoint` currently surfaces "not implemented yet" and does nothing.
- **Diff review pane** — modal renders, but `pending_changes` isn't populated by any tool yet, so review/accept/reject toast "no pending changes".

Linux gets daily smoke testing. macOS + Windows code paths exist (`#ifdef` branches throughout, `posix_spawn` for POSIX, `CreateProcessW` for Windows, `fdatasync`/`fsync` switched per OS); CI for those platforms is next.

File terminal-rendering bugs with `$TERM`, your terminal emulator name, and a screenshot. Code-path bugs welcome too — paste the relevant block and `git rev-parse HEAD`.

## License

MIT.
