#!/bin/sh
# install.sh — agentty universal installer
#
# Usage:
#   curl -fsSL https://agentty.org/install.sh | sh
#   curl -fsSL https://agentty.org/install.sh | sh -s -- --prefix ~/.local
#   curl -fsSL https://agentty.org/install.sh | sh -s -- --version v0.2.0
#   wget -qO- https://agentty.org/install.sh | sh
#
# Detects OS+arch, downloads the matching prebuilt binary from the GitHub
# release, verifies its SHA256, and installs to $PREFIX/bin (default
# /usr/local/bin as root, else ~/.local/bin). If no prebuilt binary matches
# your platform it falls back to building from source automatically.
# Works with either curl or wget. No build toolchain required for prebuilts.

set -eu

REPO="1ay1/agentty"
VERSION="latest"
PREFIX=""
BIN_NAME="agentty"
NO_VERIFY=0
BUILD_FROM_SOURCE=0

# ----------------------------------------------------------------------------- ui
err()  { printf '\033[1;31mx\033[0m install.sh: %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!\033[0m %s\n' "$*" >&2; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }

usage() {
    sed -n '2,15p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || cat <<'EOF'
agentty installer
  --version <tag>   install a specific release (default: latest)
  --prefix <dir>    install under <dir>/bin (default: /usr/local or ~/.local)
  --no-verify       skip SHA256 verification
  --build           build from source instead of downloading a prebuilt binary
  -h, --help        show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version)   VERSION="${2:-}"; [ -n "$VERSION" ] || err "--version needs a value"; shift 2 ;;
        --prefix)    PREFIX="${2:-}";  [ -n "$PREFIX" ]  || err "--prefix needs a value";  shift 2 ;;
        --no-verify) NO_VERIFY=1; shift ;;
        --build)     BUILD_FROM_SOURCE=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) err "unknown arg: $1  (try --help)" ;;
    esac
done

# ----------------------------------------------------------------------------- http helpers
# A single downloader abstraction so we work with curl OR wget.
if command -v curl >/dev/null 2>&1; then
    HTTP=curl
elif command -v wget >/dev/null 2>&1; then
    HTTP=wget
else
    err "need either 'curl' or 'wget' installed to download agentty"
fi

# fetch URL -> stdout (quiet, follow redirects, fail on http error)
fetch_stdout() {
    if [ "$HTTP" = curl ]; then
        curl -fsSL "$1"
    else
        wget -qO- "$1"
    fi
}

# download URL -> file, with up to 3 attempts on transient network errors.
# A definite "not found" (HTTP 404) is not retried — it won't fix itself.
fetch_file() {
    # $1 url  $2 dest
    _i=1
    while [ "$_i" -le 3 ]; do
        if [ "$HTTP" = curl ]; then
            # curl exit 22 = HTTP >=400 (with -f). Capture the HTTP code to
            # decide whether a retry is worthwhile.
            _code=$(curl -fsSL --retry 2 --connect-timeout 15 -w '%{http_code}' "$1" -o "$2" 2>/dev/null) && return 0
            _rc=$?
            case "$_code" in 404|403|410) return 1 ;; esac
            [ "$_rc" -eq 22 ] && case "$_code" in 4??) return 1 ;; esac
        else
            wget -q --tries=2 --timeout=15 "$1" -O "$2" && return 0
            # wget exit 8 = server issued an error response (e.g. 404)
            [ $? -eq 8 ] && return 1
        fi
        _i=$((_i+1))
        [ "$_i" -le 3 ] && { warn "download attempt failed, retrying ($_i/3)"; sleep 2; }
    done
    return 1
}

# ----------------------------------------------------------------------------- detect os/arch
os=$(uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]')
arch=$(uname -m 2>/dev/null)

case "$os" in
    linux)                os=linux ;;
    darwin)               os=macos ;;
    msys*|mingw*|cygwin*) os=windows ;;
    *) err "unsupported OS: '$os' — please open an issue at https://github.com/$REPO/issues" ;;
esac

case "$arch" in
    x86_64|amd64)   arch=x86_64 ;;
    aarch64|arm64)  arch=arm64 ;;
    i?86)           arch=i686 ;;
    *)              arch="$arch" ;;   # leave unknown; resolved below
esac

# ----------------------------------------------------------------------------- pick asset
# Asset names as published in the GitHub release:
#   agentty-linux-x86_64   agentty-linux-i686
#   agentty-macos-arm64    agentty-macos-x86_64
#   agentty-windows-x86_64.exe
asset=""
case "$os" in
    linux)
        case "$arch" in
            x86_64) asset="agentty-linux-x86_64" ;;
            i686)   asset="agentty-linux-i686" ;;
            arm64)  asset="agentty-linux-aarch64" ;;
            *)      asset="" ;;
        esac ;;
    macos)
        case "$arch" in
            arm64)  asset="agentty-macos-arm64" ;;
            x86_64) asset="agentty-macos-x86_64" ;;
            *)      asset="" ;;
        esac ;;
    windows)
        case "$arch" in
            x86_64) asset="agentty-windows-x86_64.exe"; BIN_NAME="agentty.exe" ;;
            *)      asset="" ;;
        esac ;;
esac

[ "$BUILD_FROM_SOURCE" -eq 1 ] && asset=""

# ----------------------------------------------------------------------------- resolve prefix
if [ -z "$PREFIX" ]; then
    if [ "$(id -u 2>/dev/null || echo 1000)" -eq 0 ]; then
        PREFIX=/usr/local
    else
        PREFIX="$HOME/.local"
    fi
fi
bindir="$PREFIX/bin"

# ----------------------------------------------------------------------------- source-build fallback
build_from_source() {
    info "no prebuilt binary for ${os}/${arch} — building from source"
    for tool in git cmake; do
        command -v "$tool" >/dev/null 2>&1 || err "building from source needs '$tool' (and a C++26 compiler). Install it and re-run, or grab a prebuilt binary from https://github.com/$REPO/releases"
    done
    command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 \
        || err "no C++ compiler found (need g++ or clang++ with C++26 support)"

    src=$(mktemp -d)
    trap 'rm -rf "$src"' EXIT
    info "cloning $REPO"
    # Force HTTPS and disable any prompts so a user's url.insteadOf rewrite to
    # SSH (which would trigger host-key verification) can't break the clone.
    GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND='ssh -oBatchMode=yes' \
        git -c 'url.https://github.com/.insteadOf=git@github.com:' \
            -c 'url.https://github.com/.insteadOf=ssh://git@github.com/' \
            -c 'url.https://github.com/.insteadOf=git://github.com/' \
            clone --depth 1 --recursive "https://github.com/$REPO" "$src/agentty" >/dev/null 2>&1 \
        || err "git clone failed. Grab a prebuilt binary instead: https://github.com/$REPO/releases"
    info "configuring (cmake)"
    cmake -S "$src/agentty" -B "$src/agentty/build" -DCMAKE_BUILD_TYPE=Release >/dev/null \
        || err "cmake configure failed"
    info "compiling (this can take a minute)"
    cmake --build "$src/agentty/build" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null \
        || err "build failed"
    built=$(find "$src/agentty/build" -type f -name "$BIN_NAME" -perm -u+x 2>/dev/null | head -n1)
    [ -n "$built" ] || err "build succeeded but couldn't locate the '$BIN_NAME' binary"
    mkdir -p "$bindir"
    chmod +x "$built"
    mv "$built" "$bindir/$BIN_NAME"
    ok "built and installed $bindir/$BIN_NAME"
}

# ----------------------------------------------------------------------------- sha256 helper
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        echo ""   # no tool available
    fi
}

# Resolve the expected sha256 for an asset. Preferred source is the release's
# SHA256SUMS file (standard "<hex>  <name>" format). If that's absent, fall
# back to the GitHub API's per-asset "digest":"sha256:<hex>" field.
expected_sha256_for() {
    # $1 asset name  $2 release base url (.../download)
    name=$1; base=$2
    # 1) SHA256SUMS file
    sums=$(fetch_stdout "$base/SHA256SUMS" 2>/dev/null || true)
    if [ -n "$sums" ]; then
        hex=$(printf '%s\n' "$sums" | awk -v n="$name" '$2==n {print $1; exit}')
        if [ -n "$hex" ]; then printf '%s' "$hex"; return 0; fi
    fi
    # 2) GitHub API digest fallback
    api="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
    [ "$VERSION" = "latest" ] && api="https://api.github.com/repos/$REPO/releases/latest"
    fetch_stdout "$api" 2>/dev/null \
        | awk -v want="$name" '
            /"name"[[:space:]]*:/ {
                line=$0
                sub(/^.*"name"[[:space:]]*:[[:space:]]*"/, "", line)
                sub(/".*$/, "", line)
                match_block = (line == want) ? 1 : 0
            }
            match_block && /"digest"[[:space:]]*:[[:space:]]*"sha256:/ {
                d=$0
                sub(/^.*"sha256:/, "", d)
                sub(/".*$/, "", d)
                print d; exit
            }' 2>/dev/null
}

# ----------------------------------------------------------------------------- install prebuilt
install_prebuilt() {
    if [ "$VERSION" = "latest" ]; then
        base="https://github.com/$REPO/releases/latest/download"
    else
        base="https://github.com/$REPO/releases/download/$VERSION"
    fi

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    info "downloading $asset"
    fetch_file "$base/$asset" "$tmp/$asset" || err "download failed: $base/$asset"
    [ -s "$tmp/$asset" ] || err "downloaded file is empty — release asset '$asset' may not exist for $VERSION"

    # --- verify -------------------------------------------------------------
    if [ "$NO_VERIFY" -eq 1 ]; then
        warn "skipping SHA256 verification (--no-verify)"
    else
        expected=$(expected_sha256_for "$asset" "$base" 2>/dev/null || true)
        actual=$(sha256_of "$tmp/$asset")
        if [ -z "$actual" ]; then
            warn "no sha256 tool (sha256sum/shasum/openssl) — cannot verify checksum"
        elif [ -z "$expected" ]; then
            warn "no published checksum for '$asset' — skipping verification"
        elif [ "$expected" = "$actual" ]; then
            ok "checksum verified"
        else
            err "checksum mismatch for $asset
  expected $expected
  actual   $actual
  refusing to install. Re-run with --no-verify to override (not recommended)."
        fi
    fi

    # --- detect prior install ----------------------------------------------
    prior_version=""
    if [ -x "$bindir/$BIN_NAME" ]; then
        prior_version=$("$bindir/$BIN_NAME" --version 2>/dev/null | awk '/^agentty / {print $2}')
    fi

    mkdir -p "$bindir" || err "cannot create $bindir (try: sudo, or --prefix ~/.local)"
    chmod +x "$tmp/$asset"
    mv "$tmp/$asset" "$bindir/$BIN_NAME" 2>/dev/null \
        || err "cannot write $bindir/$BIN_NAME — no permission. Re-run with sudo or --prefix ~/.local"

    # macOS Gatekeeper: strip the quarantine flag so it runs without a prompt.
    if [ "$os" = macos ] && command -v xattr >/dev/null 2>&1; then
        xattr -d com.apple.quarantine "$bindir/$BIN_NAME" >/dev/null 2>&1 || true
    fi

    new_version=$("$bindir/$BIN_NAME" --version 2>/dev/null | awk '/^agentty / {print $2}')
    if [ -n "$prior_version" ] && [ -n "$new_version" ] && [ "$prior_version" != "$new_version" ]; then
        ok "updated $bindir/$BIN_NAME  $prior_version  ->  $new_version"
    elif [ -n "$prior_version" ] && [ "$prior_version" = "$new_version" ]; then
        ok "reinstalled $bindir/$BIN_NAME ($new_version)"
    else
        ok "installed $bindir/$BIN_NAME${new_version:+ ($new_version)}"
    fi
}

# ----------------------------------------------------------------------------- run
if [ -z "$asset" ]; then
    build_from_source
else
    install_prebuilt
fi

# ----------------------------------------------------------------------------- self-test + PATH hint
if "$bindir/$BIN_NAME" --version >/dev/null 2>&1; then
    ok "agentty is ready"
else
    warn "installed, but '$bindir/$BIN_NAME --version' did not run cleanly. Open an issue if this persists: https://github.com/$REPO/issues"
fi

case ":${PATH:-}:" in
    *":$bindir:"*) ;;
    *)
        printf '\n'
        warn "$bindir is not on your PATH. Add it:"
        case "${SHELL:-}" in
            *fish)
                printf '    fish_add_path %s\n\n' "$bindir" ;;
            *)
                if [ "$os" = windows ]; then
                    printf '    setx PATH "%%PATH%%;%s"\n\n' "$bindir"
                else
                    sh_rc="$HOME/.profile"
                    case "${SHELL:-}" in
                        *zsh)  sh_rc="$HOME/.zshrc" ;;
                        *bash) sh_rc="$HOME/.bashrc" ;;
                    esac
                    printf '    echo '\''export PATH="%s:$PATH"'\'' >> %s && . %s\n\n' "$bindir" "$sh_rc" "$sh_rc"
                fi ;;
        esac
        ;;
esac

ok "run:  $BIN_NAME"
