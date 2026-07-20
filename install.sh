#!/bin/sh
# install.sh — agentty universal installer
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --prefix ~/.local
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --version v0.2.0
#   curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --build --prefix ~/.local
#
# Detects OS+arch, downloads the matching binary from the GitHub release,
# verifies SHA256, installs to $PREFIX/bin (default /usr/local/bin or ~/.local/bin
# when not root). No build toolchain required.
#
# --build compiles from source instead of downloading a prebuilt binary — use
# it when the prebuilt artifact is broken or incompatible with your libc (e.g.
# a musl static binary that segfaults on glibc). Needs a C++26 toolchain
# (GCC 14+ / Clang 18+), CMake 3.28+, git, plus libssl-dev + libnghttp2-dev.
# The installer also auto-falls-back to a source build if the downloaded
# binary won't run on this system.
#
# Source builds default to -j2 (low RAM/CPU footprint — safe on Termux/phones
# where each template-heavy TU peaks near ~1.5 GB). Override with
# AGENTTY_BUILD_JOBS, e.g.
#   AGENTTY_BUILD_JOBS=8 curl -fsSL .../install.sh | sh -s -- --build   # faster, more cores
#   AGENTTY_BUILD_JOBS=1 curl -fsSL .../install.sh | sh -s -- --build   # RAM-starved

set -eu

REPO="1ay1/agentty"
VERSION="latest"
# Capture Termux's exported $PREFIX (…/com.termux/files/usr) BEFORE we clear our
# own PREFIX var below — it's the correct, on-PATH install root on Android.
TERMUX_PREFIX_ENV="${PREFIX:-}"
PREFIX=""
BIN_NAME="agentty"
BUILD=0

err()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m✓\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# build_from_source PREFIX VERSION
# Clones the repo at the requested ref and compiles a binary into $PREFIX/bin.
# Used when --build is given, or as an automatic fallback when a downloaded
# prebuilt binary won't run on this system (wrong libc, no PT_INTERP, etc).
build_from_source() {
    _bfs_prefix="$1"
    _bfs_version="$2"
    _bfs_bindir="$_bfs_prefix/bin"

    have git   || err "--build needs git installed"
    have cmake || err "--build needs cmake (3.28+) installed"
    if ! have g++ && ! have clang++; then
        err "--build needs a C++26 compiler (g++ 14+ or clang++ 18+)"
    fi

    # Resolve the git ref. "latest" builds the default branch tip; a pinned
    # version like v0.2.0 checks out that tag.
    if [ "$_bfs_version" = "latest" ]; then
        _bfs_ref="master"
    else
        _bfs_ref="$_bfs_version"
    fi

    _bfs_src=$(mktemp -d)
    # Don't clobber a $tmp cleanup the download path may have installed — remove
    # both on exit.
    # shellcheck disable=SC2064
    trap "rm -rf '$_bfs_src' \"\${tmp:-}\"" EXIT

    info "cloning $REPO ($_bfs_ref) for source build"
    git clone --recursive --depth 1 --branch "$_bfs_ref" \
        "https://github.com/$REPO.git" "$_bfs_src" 2>/dev/null \
        || git clone --recursive --depth 1 \
             "https://github.com/$REPO.git" "$_bfs_src" \
        || err "git clone failed"

    info "configuring (Release, standalone)"
    cmake -S "$_bfs_src" -B "$_bfs_src/build" \
        -DCMAKE_BUILD_TYPE=Release -DAGENTTY_STANDALONE=ON \
        || err "cmake configure failed - install a C++26 toolchain + libssl-dev + libnghttp2-dev"

    info "compiling (this can take a few minutes)"
    # Default to 2 parallel jobs, not nproc: this tree is template-heavy and
    # each TU peaks at ~1-1.5 GB, so -j$(nproc) OOM-kills phones (Termux) and
    # small VMs. 2 is a safe low-footprint default; override with e.g.
    #   AGENTTY_BUILD_JOBS=8 ...   (more cores)  or  AGENTTY_BUILD_JOBS=1 (RAM-starved)
    _bfs_jobs="${AGENTTY_BUILD_JOBS:-2}"
    cmake --build "$_bfs_src/build" -j"$_bfs_jobs" || err "build failed"

    _bfs_bin="$_bfs_src/build/agentty"
    [ -x "$_bfs_bin" ] || _bfs_bin="$_bfs_src/build/$BIN_NAME"
    [ -x "$_bfs_bin" ] || err "build produced no binary at $_bfs_src/build/agentty"

    mkdir -p "$_bfs_bindir"
    chmod +x "$_bfs_bin"
    mv "$_bfs_bin" "$_bfs_bindir/$BIN_NAME"
    ok "built + installed $_bfs_bindir/$BIN_NAME"
}

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
        --build)   BUILD=1;      shift 1 ;;
        -h|--help)
            sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
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

# --- pick prefix (needed by both the build path and the download path) --------
if [ -z "$PREFIX" ]; then
    if [ -n "${TERMUX_VERSION:-}" ] || [ -d /data/data/com.termux/files/usr ]; then
        # Termux/Android: $PREFIX (…/com.termux/files/usr) is the ONLY dir on
        # the default PATH. /usr/local and ~/.local don't exist / aren't on PATH,
        # so installing there leaves `agentty` uncallable. Prefer the exported
        # $PREFIX we captured at startup; fall back to the canonical path.
        PREFIX="${TERMUX_PREFIX_ENV:-/data/data/com.termux/files/usr}"
    elif [ "$(id -u)" -eq 0 ]; then
        PREFIX=/usr/local
    else
        PREFIX="$HOME/.local"
    fi
fi
bindir="$PREFIX/bin"

# --- explicit source build ----------------------------------------------------
# --build skips the prebuilt artifact entirely. Also the right recovery when a
# release binary is broken for your libc (e.g. v0.2.7's musl static binary
# segfaulting on glibc).
if [ "$BUILD" -eq 1 ]; then
    build_from_source "$PREFIX" "$VERSION"
    case ":$PATH:" in
        *":$bindir:"*) ;;
        *) printf '\n\033[1;33m!\033[0m %s\n' "add $bindir to PATH:"
           printf '    export PATH=\"%s:\$PATH\"\n\n' "$bindir" ;;
    esac
    ok "run: $BIN_NAME"
    exit 0
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
    sums_url=$(printf '%s\n' "$urls" | grep -E '/SHA256SUMS$' | head -n1)
    for sfx in $suffixes; do
        asset_url=$(printf '%s\n' "$urls" | grep -E "/agentty(-[0-9][^/]*)?-${sfx}$" | head -n1)
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

# The downloaded binary may be incompatible with this system — a musl static
# build with no PT_INTERP can segfault on glibc, or an arch/ABI mismatch slips
# past detection. If it won't even print its version, recover by building from
# source rather than leaving a broken binary in place. (Skip on Windows: no
# in-place toolchain assumption, and cross-exec checks don't apply here.)
if [ -z "$new_version" ] && [ "$os" != "windows" ]; then
    info "prebuilt binary won't run on this system — falling back to a source build"
    rm -f "$bindir/$BIN_NAME"
    build_from_source "$PREFIX" "$VERSION"
    new_version=$("$bindir/$BIN_NAME" --version 2>/dev/null | awk '/^agentty / {print $2}')
fi

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
