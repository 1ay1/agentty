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

# --- pick asset ---------------------------------------------------------------
if [ "$os" = "windows" ]; then
    asset="agentty-windows-x86_64.exe"
    BIN_NAME="agentty.exe"
elif [ "$os" = "darwin" ]; then
    err "macOS binaries are not yet pre-built. Build from source:
    git clone --recursive https://github.com/$REPO
    cd agentty && cmake -B build && cmake --build build -j
  Or use Homebrew once the tap lands:  brew install 1ay1/tap/agentty"
else
    asset="agentty-${os}-${arch}"
fi

# --- resolve version ----------------------------------------------------------
if [ "$VERSION" = "latest" ]; then
    base="https://github.com/$REPO/releases/latest/download"
else
    base="https://github.com/$REPO/releases/download/$VERSION"
fi

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

info "downloading $asset from $base"
curl -fsSL "$base/$asset"      -o "$tmp/$asset"
curl -fsSL "$base/SHA256SUMS"  -o "$tmp/SHA256SUMS" 2>/dev/null || {
    info "no SHA256SUMS published for $VERSION — skipping checksum verification"
    SKIP_SUMS=1
}

if [ -z "${SKIP_SUMS:-}" ]; then
    info "verifying SHA256"
    expected=$(grep " $asset\$" "$tmp/SHA256SUMS" | awk '{print $1}')
    [ -n "$expected" ] || err "no checksum line for $asset in SHA256SUMS"
    actual=$(sha256sum "$tmp/$asset" | awk '{print $1}')
    [ "$expected" = "$actual" ] || err "checksum mismatch
  expected $expected
  actual   $actual"
    ok "checksum verified"
fi

chmod +x "$tmp/$asset"
mv "$tmp/$asset" "$bindir/$BIN_NAME"
ok "installed $bindir/$BIN_NAME"

# --- PATH hint ----------------------------------------------------------------
case ":$PATH:" in
    *":$bindir:"*) ;;
    *) printf '\n\033[1;33m!\033[0m %s\n' "add $bindir to PATH:"
       printf '    export PATH=\"%s:\$PATH\"\n\n' "$bindir" ;;
esac

ok "run: $BIN_NAME"
