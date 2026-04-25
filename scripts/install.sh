#!/bin/sh
# moha installer — detect platform, download latest release binary, drop it
# on PATH. POSIX shell so it runs on bare macOS / Alpine / minimal Linux.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/1ay1/moha/master/scripts/install.sh | sh
#
# Env overrides:
#   MOHA_VERSION   release tag to install (default: latest, e.g. "v0.1.0")
#   MOHA_PREFIX    install dir (default: $HOME/.local/bin, falls back to /usr/local/bin
#                  with sudo if the home dir isn't writable)
#   MOHA_REPO      GitHub repo (default: 1ay1/moha) — for forks / mirrors
#
# Exit codes:
#   0   installed successfully
#   1   detection or download failure
#   2   checksum mismatch
#   3   unsupported platform/arch

set -eu

REPO="${MOHA_REPO:-1ay1/moha}"
PREFIX="${MOHA_PREFIX:-$HOME/.local/bin}"

err() { printf 'error: %s\n' "$*" >&2; }
info() { printf '%s\n' "$*" >&2; }

# ── 1. Detect OS + arch ────────────────────────────────────────────────
uname_s="$(uname -s)"
uname_m="$(uname -m)"

case "$uname_s" in
    Linux)   os="linux"  ;;
    Darwin)  os="macos"  ;;
    *)
        err "unsupported OS: $uname_s (Linux and macOS only — Windows users grab the .zip from the releases page)"
        exit 3
        ;;
esac

case "$uname_m" in
    x86_64|amd64) arch="x86_64" ;;
    arm64|aarch64) arch="arm64" ;;
    *)
        err "unsupported arch: $uname_m"
        exit 3
        ;;
esac

target="${os}-${arch}"
info "→ detected: $target"

# ── 2. Resolve version ────────────────────────────────────────────────
if [ -n "${MOHA_VERSION:-}" ]; then
    tag="$MOHA_VERSION"
    info "→ using pinned version: $tag"
else
    # GitHub's "latest" redirect is the canonical way to get the most-
    # recent non-prerelease tag without needing a token.
    tag="$(curl -fsSL -o /dev/null -w '%{url_effective}' \
        "https://github.com/${REPO}/releases/latest" \
        | sed -E 's|.*/tag/||')"
    if [ -z "$tag" ]; then
        err "could not resolve latest release of ${REPO}"
        exit 1
    fi
    info "→ latest release: $tag"
fi

# Asset filename includes the resolved version (sans the leading 'v')
# so it matches what the build pipeline produces — same naming the
# Arch PKGBUILD's source URL uses, so a single asset serves both
# `curl | sh` installs and AUR makepkg consumers.
ver="${tag#v}"
asset="moha-${ver}-${target}.tar.gz"
url="https://github.com/${REPO}/releases/download/${tag}/${asset}"

# ── 3. Download + verify checksum (best-effort) ───────────────────────
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "→ downloading $asset"
if ! curl -fL --progress-bar "$url" -o "$tmp/$asset"; then
    err "download failed: $url"
    exit 1
fi

# SHA256SUMS is published alongside; if it exists and matches, verify.
# Missing or mismatched-format checksum file is non-fatal — install
# proceeds with a warning so a botched checksum upload doesn't block users.
if curl -fsSL "https://github.com/${REPO}/releases/download/${tag}/SHA256SUMS" -o "$tmp/SHA256SUMS" 2>/dev/null; then
    expected="$(awk -v a="$asset" '$2 == a { print $1 }' "$tmp/SHA256SUMS")"
    if [ -n "$expected" ]; then
        if command -v sha256sum >/dev/null 2>&1; then
            actual="$(sha256sum "$tmp/$asset" | awk '{print $1}')"
        elif command -v shasum >/dev/null 2>&1; then
            actual="$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')"
        else
            actual=""
        fi
        if [ -n "$actual" ] && [ "$actual" != "$expected" ]; then
            err "checksum mismatch! expected=$expected got=$actual"
            exit 2
        fi
        [ -n "$actual" ] && info "→ checksum OK"
    fi
fi

# ── 4. Extract ────────────────────────────────────────────────────────
tar xzf "$tmp/$asset" -C "$tmp"
# Contained directory mirrors the archive basename: extracting
# `moha-X.Y.Z-T.tar.gz` produces `moha-X.Y.Z-T/moha`.
extracted="$tmp/moha-${ver}-${target}/moha"
if [ ! -x "$extracted" ]; then
    err "expected binary not in archive: $extracted"
    exit 1
fi

# ── 5. Install ────────────────────────────────────────────────────────
mkdir -p "$PREFIX" 2>/dev/null || true
if [ -w "$PREFIX" ] || [ ! -e "$PREFIX" ]; then
    install -m 0755 "$extracted" "$PREFIX/moha"
    dest="$PREFIX/moha"
else
    info "→ $PREFIX not writable, falling back to /usr/local/bin (sudo)"
    sudo install -m 0755 "$extracted" /usr/local/bin/moha
    dest="/usr/local/bin/moha"
fi

info ""
info "  ✓ moha $tag installed to $dest"
info ""

# ── 6. PATH hint ──────────────────────────────────────────────────────
case ":$PATH:" in
    *":$PREFIX:"*) : ;;   # already on PATH
    *)
        case "$dest" in
            "$PREFIX"/*)
                info "  Heads-up: $PREFIX is not on your PATH."
                info "  Add this to your shell rc (~/.bashrc or ~/.zshrc):"
                info ""
                info "      export PATH=\"$PREFIX:\$PATH\""
                info ""
                ;;
        esac
        ;;
esac

info "  Run 'moha' to start.  First launch opens an in-app login modal."
info ""
