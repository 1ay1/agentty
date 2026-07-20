---
title: Installation
description: Install agentty in one line, or via your distro's package manager.
nav_section: Getting Started
nav_order: 20
slug: installation
---

One line installs it. The same line updates it. Or use your package manager — deb, rpm, AUR, Homebrew, and Scoop are all published per release.

## Latest release

**{{versionLabel}}** · [release notes & checksums ↗]({{releasesLatest}})

:::release-table

## One-line install (recommended)

```bash
{{installOneLiner}}
```

Detects your OS and arch, downloads the right binary from the latest release, verifies SHA256, and installs to `/usr/local/bin` (if root) or `~/.local/bin`. **Re-running the same command updates** to the newest release. No `apt`, no `brew`, no version drift.

:::note
Flags: `--prefix ~/somewhere`, `--version {{versionLabel}}`, `--no-verify`, `--build` (force a source build). Prebuilt binaries for Linux (x86_64, i686), macOS (Apple Silicon & Intel), and Windows (x86_64). On any other platform the script builds from source automatically. Works with `curl` or `wget`.
:::

## Debian / Ubuntu

```bash
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty_{{version}}_amd64.deb
sudo dpkg -i agentty_{{version}}_amd64.deb       # or agentty_{{version}}_arm64.deb
```

Update: `dpkg -i` the new release's `.deb`.

## Fedora / RHEL / openSUSE

```bash
# Fedora / RHEL / CentOS
sudo dnf install https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.rpm

# openSUSE
sudo zypper install https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.rpm

# or upgrade an existing install:
sudo rpm -Uvh https://github.com/1ay1/agentty/releases/latest/download/agentty-{{version}}-1.x86_64.rpm
```

`-U` is upgrade; works for the first install too.

## Arch Linux

```bash
yay -S agentty-bin       # or paru, pikaur, etc.
yay -Syu agentty-bin     # update
```

Or install the release-page `.pkg.tar.zst` with `sudo pacman -U`.

## Alpine

```bash
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.apk
sudo apk add --allow-untrusted agentty-x86_64.apk
```

## Nix · Snap · Gentoo

Pinned manifests for these are attached to every release. Grab the one for your channel from the [latest release page]({{releasesLatest}}):

```bash
nix-env -iA agentty      # Nix (attached manifest)
snap install agentty     # Snap
emerge agentty           # Gentoo
```

## macOS

The one-line installer above ships prebuilt binaries for both Apple Silicon (`arm64`) and Intel (`x86_64`) and strips the Gatekeeper quarantine flag for you, so it just runs:

```bash
{{installOneLiner}}
```

Homebrew (once the tap lands):

```bash
brew tap 1ay1/tap
brew install agentty
brew upgrade agentty     # update
```

## Windows

The fastest way — one line in PowerShell:

```bash
{{installOneLinerWindows}}
```

Downloads `agentty.exe`, verifies its SHA256, installs to `%LOCALAPPDATA%\agentty`, and adds it to your user `PATH`. Or use a package manager (no SmartScreen prompt):

```bash
winget install agentty
# or
scoop bucket add 1ay1 https://github.com/1ay1/scoop-bucket
scoop install agentty
```

Portable single `.exe` (no installer): `curl -L https://github.com/1ay1/agentty/releases/latest/download/agentty-windows-x86_64.exe -o agentty.exe`

## Termux / Android

agentty builds natively on Termux against its Bionic/libc++ toolchain — no root, no proot. The install script detects Termux and installs into `$PREFIX/bin`, which is on your default `PATH`:

```bash
pkg install git cmake clang openssl libnghttp2
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --build
```

Everything works — file tools, RAG, the agent loop, and shell/build tools — with one caveat: the shell sandbox (Bubblewrap) needs Linux user namespaces that unrooted Android doesn't grant, so `bash`/`diagnostics` run **unsandboxed** (agentty detects this and prints `sandbox: unavailable, running unsandboxed`). Point it at a scoped workspace if that matters to you.

An on-repo [`packaging/termux/build.sh`](https://github.com/1ay1/agentty/blob/master/packaging/termux/build.sh) recipe targets the official Termux repos for a future `pkg install agentty`.

## Raw static binaries

Fully-static, no shared-library dependencies. Drop and run:

```bash
# Linux x86_64
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-linux-x86_64 -o agentty && chmod +x agentty
# Linux i686 (32-bit)
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-linux-i686 -o agentty && chmod +x agentty
# macOS (Apple Silicon / Intel)
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-macos-arm64 -o agentty && chmod +x agentty
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-macos-x86_64 -o agentty && chmod +x agentty
```

Each release asset carries a published SHA256 (shown on the release page and verified automatically by the one-line installer).

:::tip
Building from source? See [Building from Source](/docs/building) for the CMake flags and toolchain requirements.
:::
