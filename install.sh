#!/bin/sh
# install.sh — agentty universal installer
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --prefix ~/.local
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --version v0.2.0
#
# Detects OS+arch, downloads the matching binary from the GitHub release,
# verifies SHA256, installs to $PREFIX/bin (default /usr/local/bin or ~/.local/bin
# when not root). No build toolchain required.

set -eu

REPO="1ay1/agentty"
VERSION="latest"
PREFIX=""
BIN_NAME="agentty"

err()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m✓\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# fetch URL to stdout, trying curl then wget
fetch() {
    if have curl; then
        curl -fsSL "$1"
    elif have wget; then
        wget -qO- "$1"
    else
        err "need curl or wget installed"
    fi
}

# download URL to a file ($2), trying curl then wget
download() {
    if have curl; then
        curl -fsSL "$1" -o "$2"
    elif have wget; then
        wget -q "$1" -O "$2"
    else
        err "need curl or wget installed"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --prefix)  PREFIX="$2";  shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) err "unknown arg: $1" ;;
    esac
done

# --- detect os/arch -----------------------------------------------------------
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)

case "$os" in
    linux)  os=linux ;;
    darwin) os=darwin ;;
    msys*|mingw*|cygwin*) os=windows ;;
    *) err "unsupported OS: $os" ;;
esac

case "$arch" in
    x86_64|amd64)  arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) err "unsupported arch: $arch" ;;
esac

# --- build candidate asset suffixes -------------------------------------------
# Release assets are named like "agentty-<version>-<os>-<arch>" (and historically
# a few unversioned "agentty-<os>-<arch>"). OS/arch tokens have varied across
# releases (darwin vs macos, aarch64 vs arm64), so we try a list of plausible
# suffixes in priority order and take the first match.
if [ "$os" = "windows" ]; then
    suffixes="windows-${arch}.exe windows-amd64.exe"
    BIN_NAME="agentty.exe"
elif [ "$os" = "darwin" ]; then
    case "$arch" in
        aarch64) suffixes="macos-arm64 darwin-arm64 macos-aarch64 darwin-aarch64" ;;
        *)       suffixes="macos-${arch} darwin-${arch} macos-amd64 darwin-amd64" ;;
    esac
else
    case "$arch" in
        aarch64) suffixes="linux-aarch64 linux-arm64" ;;
        *)       suffixes="linux-${arch} linux-amd64" ;;
    esac
fi

# --- resolve release + asset URL via GitHub API -------------------------------
# The GitHub "latest/download/<name>" redirect only works for assets whose names
# are stable across releases; ours embed the version, so we query the API to get
# the real browser_download_url. Falls back to a constructed URL if the API is
# unavailable (e.g. rate-limited).
if [ "$VERSION" = "latest" ]; then
    api_url="https://api.github.com/repos/$REPO/releases/latest"
else
    api_url="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
fi

info "resolving release ($VERSION) for $os/$arch"
api_json=$(fetch "$api_url" 2>/dev/null) || api_json=""

asset_url=""
sums_url=""
if [ -n "$api_json" ]; then
    # Extract every browser_download_url, then pick the asset whose filename ends
    # in one of our candidate suffixes. The '-' before the suffix keeps
    # "linux-aarch64" from satisfying a "linux-x86_64" request, etc.
    urls=$(printf '%s\n' "$api_json" \
        | grep -o '"browser_download_url": *"[^"]*"' \
        | sed 's/.*": *"//; s/"$//')
    sums_url=$(printf '%s\n' "$urls" | grep -E '/SHA256SUMS\$' | head -n1)
    for sfx in $suffixes; do
        asset_url=$(printf '%s\n' "$urls" | grep -E "/agentty(-[0-9][^/]*)?-${sfx}\$" | head -n1)
        [ -n "$asset_url" ] && break
    done
fi

# Fallback: construct the legacy unversioned URL if the API gave us nothing.
if [ -z "$asset_url" ]; then
    if [ "$VERSION" = "latest" ]; then
        base="https://github.com/$REPO/releases/latest/download"
    else
        base="https://github.com/$REPO/releases/download/$VERSION"
    fi
    set -- $suffixes
    asset_url="$base/agentty-$1"
    sums_url="$base/SHA256SUMS"
    info "GitHub API unavailable — falling back to $asset_url"
fi

asset=$(basename "$asset_url")
# --- pick prefix --------------------------------------------------------------
if [ -z "$PREFIX" ]; then
    if [ "$(id -u)" -eq 0 ]; then
        PREFIX=/usr/local
    else
        PREFIX="$HOME/.local"
    fi
fi
bindir="$PREFIX/bin"
mkdir -p "$bindir"

# --- download + verify --------------------------------------------------------
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

info "downloading $asset"
download "$asset_url" "$tmp/$asset" || err "download failed: $asset_url"
[ -s "$tmp/$asset" ] || err "downloaded file is empty: $asset_url"

if [ -n "$sums_url" ] && download "$sums_url" "$tmp/SHA256SUMS" 2>/dev/null; then
    :
else
    info "no SHA256SUMS published for $VERSION — skipping checksum verification"
    SKIP_SUMS=1
fi

if [ -z "${SKIP_SUMS:-}" ]; then
    info "verifying SHA256"
    expected=$(grep " $asset\$" "$tmp/SHA256SUMS" | awk '{print $1}')
    if [ -z "$expected" ]; then
        info "no checksum line for $asset in SHA256SUMS — skipping verification"
    elif have sha256sum; then
        actual=$(sha256sum "$tmp/$asset" | awk '{print $1}')
        [ "$expected" = "$actual" ] || err "checksum mismatch
  expected $expected
  actual   $actual"
        ok "checksum verified"
    elif have shasum; then
        actual=$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')
        [ "$expected" = "$actual" ] || err "checksum mismatch
  expected $expected
  actual   $actual"
        ok "checksum verified"
    else
        info "no sha256sum/shasum tool — skipping checksum verification"
    fi
fi

# --- detect prior install (so updates announce themselves) -------------------
prior_version=""
if [ -x "$bindir/$BIN_NAME" ]; then
    prior_version=$("$bindir/$BIN_NAME" --version 2>/dev/null | awk '/^agentty / {print $2}')
fi

chmod +x "$tmp/$asset"
mv "$tmp/$asset" "$bindir/$BIN_NAME"

new_version=$("$bindir/$BIN_NAME" --version 2>/dev/null | awk '/^agentty / {print $2}')

if [ -n "$prior_version" ] && [ -n "$new_version" ] && [ "$prior_version" != "$new_version" ]; then
    ok "updated $bindir/$BIN_NAME  $prior_version  →  $new_version"
elif [ -n "$prior_version" ] && [ "$prior_version" = "$new_version" ]; then
    ok "already on $new_version (reinstalled $bindir/$BIN_NAME)"
else
    ok "installed $bindir/$BIN_NAME${new_version:+ ($new_version)}"
fi

# --- PATH hint ----------------------------------------------------------------
case ":$PATH:" in
    *":$bindir:"*) ;;
    *) printf '\n\033[1;33m!\033[0m %s\n' "add $bindir to PATH:"
       printf '    export PATH=\"%s:\$PATH\"\n\n' "$bindir" ;;
esac

ok "run: $BIN_NAME"
