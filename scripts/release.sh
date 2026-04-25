#!/usr/bin/env bash
# scripts/release.sh — build & ship release artifacts locally, no CI.
#
# Drop-in replacement for the .github/workflows/release.yml pipeline
# when GitHub Actions isn't available. Same outputs, same naming
# (moha-<ver>-<target>.<ext>), same SHA256SUMS layout.
#
# What gets built depends on the host platform:
#
#   Linux x86_64    → .tar.gz, .deb, .rpm (if rpmbuild), .pkg.tar.zst (if Arch)
#   Linux arm64     → .tar.gz, .deb, .rpm (if rpmbuild)
#   macOS Intel     → .tar.gz, .dmg
#   macOS Apple Si. → .tar.gz, .dmg
#   Windows x86_64  → .zip, .exe (NSIS — needs makensis on PATH)
#
# To produce the full multi-platform set you'd run this on three hosts
# (Linux + macOS + Windows) and copy the artifacts together — there's
# no cross-compile shortcut for .dmg or .exe. From any single host the
# script does whatever that host can natively produce.
#
# Usage:
#   scripts/release.sh                       # build only; assets in dist/
#   scripts/release.sh --tag v0.1.0          # build + create GH release
#   scripts/release.sh --tag v0.1.0 --upload # build + upload to existing
#   scripts/release.sh --notes-from CHANGELOG.md --tag v0.1.0
#
# Env overrides:
#   MOHA_BUILD_DIR     where to put the cmake build tree (default: build-rel)
#   MOHA_DIST_DIR      where to stage the artifacts        (default: dist)
#   MOHA_REPO          GitHub repo "owner/name"            (default: 1ay1/moha)
#   MOHA_RELEASE_TAG   release tag (overridden by --tag)
#   MOHA_DRAFT         "1" to create the GH release as draft
#   MOHA_PRERELEASE    "1" to mark as prerelease
#
# Exit codes:
#   0   success
#   1   build / packaging failure
#   2   upload failed (artifacts still in dist/)
#   3   missing prereq (cmake / gh / etc.)

set -euo pipefail

# ── Args ───────────────────────────────────────────────────────────
TAG="${MOHA_RELEASE_TAG:-}"
UPLOAD=0
CREATE=0
NOTES_FILE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)         TAG="$2";       shift 2 ;;
        --upload)      UPLOAD=1;       shift ;;
        --create)      CREATE=1;       shift ;;
        --notes-from)  NOTES_FILE="$2"; shift 2 ;;
        -h|--help)     sed -n '1,/^set -euo pipefail/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'unknown arg: %s\n' "$1" >&2; exit 1 ;;
    esac
done

# Default: when --tag is supplied, both create AND upload (most common
# flow). --upload alone (no --create) assumes the release exists and
# just attaches.
if [ -n "$TAG" ] && [ "$CREATE" -eq 0 ] && [ "$UPLOAD" -eq 0 ]; then
    CREATE=1
    UPLOAD=1
fi

REPO="${MOHA_REPO:-1ay1/moha}"
BUILD_DIR="${MOHA_BUILD_DIR:-build-rel}"
DIST_DIR="${MOHA_DIST_DIR:-dist}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ── Detect host ────────────────────────────────────────────────────
detect_target() {
    local os arch s m
    s="$(uname -s)"
    m="$(uname -m)"
    case "$s" in
        Linux)   os="linux"  ;;
        Darwin)  os="macos"  ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *) printf 'unsupported OS: %s\n' "$s" >&2; exit 3 ;;
    esac
    case "$m" in
        x86_64|amd64) arch="x86_64" ;;
        arm64|aarch64) arch="arm64" ;;
        *) printf 'unsupported arch: %s\n' "$m" >&2; exit 3 ;;
    esac
    printf '%s-%s' "$os" "$arch"
}
TARGET="$(detect_target)"
HOST_OS="${TARGET%-*}"
HOST_ARCH="${TARGET#*-}"

# ── Prereqs ────────────────────────────────────────────────────────
require() { command -v "$1" >/dev/null 2>&1 || { printf 'missing: %s\n' "$1" >&2; exit 3; }; }
require cmake
require git

if [ "$HOST_OS" = "linux" ]; then
    require make
fi

# ── Resolve version (single source of truth: cmake project line) ──
PROJECT_VERSION="$(awk -F'[ )]+' '/^project\(moha VERSION/ { print $3; exit }' CMakeLists.txt)"
if [ -z "$PROJECT_VERSION" ]; then
    printf 'could not parse PROJECT_VERSION from CMakeLists.txt\n' >&2
    exit 1
fi

# Sanity: if --tag is set, it should align with the cmake version (the
# tag canonical form is "v<PROJECT_VERSION>"). Mismatch is almost
# always a forgot-to-bump bug — bail rather than ship a misnamed asset.
if [ -n "$TAG" ]; then
    EXPECTED="v${PROJECT_VERSION}"
    if [ "$TAG" != "$EXPECTED" ]; then
        printf 'tag mismatch: --tag=%s but CMakeLists.txt has VERSION=%s (expected %s)\n' \
               "$TAG" "$PROJECT_VERSION" "$EXPECTED" >&2
        printf 'bump project(moha VERSION %s) to match the tag, or use --tag %s\n' \
               "${TAG#v}" "$EXPECTED" >&2
        exit 1
    fi
fi

printf '\n→ moha %s (%s)\n\n' "$PROJECT_VERSION" "$TARGET"

# ── Configure ──────────────────────────────────────────────────────
CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DMOHA_STANDALONE=ON
    -DMOHA_BUILD_PACKAGES=ON
    -DMOHA_TARGET="$TARGET"
)

# Prefer Ninja when available — fastest build experience on the desktop.
if command -v ninja >/dev/null 2>&1; then
    CMAKE_ARGS+=(-G Ninja)
fi

# Windows-specific: when invoked under MSYS/MinGW, defer to Visual
# Studio + vcpkg (matches what the workflow does). Most users running
# this script on Windows will have already set up vcpkg.
if [ "$HOST_OS" = "windows" ] && [ -n "${VCPKG_INSTALLATION_ROOT:-}" ]; then
    CMAKE_ARGS+=(
        -G "Visual Studio 17 2022" -A x64
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
        -DVCPKG_TARGET_TRIPLET=x64-windows-static
    )
fi

# macOS-specific: pick up Homebrew OpenSSL / nghttp2 prefixes.
if [ "$HOST_OS" = "macos" ] && command -v brew >/dev/null 2>&1; then
    OPENSSL_ROOT="$(brew --prefix openssl@3 2>/dev/null || true)"
    if [ -n "$OPENSSL_ROOT" ]; then
        CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="$OPENSSL_ROOT")
    fi
fi

cmake "${CMAKE_ARGS[@]}"

# ── Build ──────────────────────────────────────────────────────────
cmake --build "$BUILD_DIR" --config Release --parallel

# Locate the built binary — different generators put it in different
# places. Visual Studio drops it under <build>/Release/; Ninja and
# Make put it directly under <build>/.
if [ -x "$BUILD_DIR/moha" ]; then
    MOHA_BIN="$BUILD_DIR/moha"
elif [ -x "$BUILD_DIR/Release/moha.exe" ]; then
    MOHA_BIN="$BUILD_DIR/Release/moha.exe"
elif [ -x "$BUILD_DIR/Release/moha" ]; then
    MOHA_BIN="$BUILD_DIR/Release/moha"
else
    printf 'cannot find moha binary under %s\n' "$BUILD_DIR" >&2
    exit 1
fi
printf '\n→ built: %s (%s)\n' "$MOHA_BIN" "$(du -h "$MOHA_BIN" | awk '{print $1}')"

# ── Stage dist/ ────────────────────────────────────────────────────
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Portable archive (always). The contained directory matches the
# archive filename (sans extension) so extracting `moha-X.Y.Z-T.tar.gz`
# yields `moha-X.Y.Z-T/` — the layout the Arch PKGBUILD's package()
# function expects, and the conventional `tar xf` UX.
PKG_BASE="moha-${PROJECT_VERSION}-${TARGET}"
STAGE="$BUILD_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/$PKG_BASE"
cp "$MOHA_BIN" "$STAGE/$PKG_BASE/"
cp README.md "$STAGE/$PKG_BASE/"
[ -f LICENSE ] && cp LICENSE "$STAGE/$PKG_BASE/" || true

if [ "$HOST_OS" = "windows" ]; then
    ARCHIVE="${PKG_BASE}.zip"
    (cd "$STAGE" && zip -qr "$ROOT/$DIST_DIR/$ARCHIVE" "$PKG_BASE")
else
    ARCHIVE="${PKG_BASE}.tar.gz"
    tar -C "$STAGE" -czf "$DIST_DIR/$ARCHIVE" "$PKG_BASE"
fi
printf '→ archive: %s\n' "$DIST_DIR/$ARCHIVE"

# ── CPack: per-platform installers ─────────────────────────────────
build_installer() {
    local generator="$1"
    local label="$2"
    if (cd "$BUILD_DIR" && cpack -G "$generator" -B cpack-out -C Release 2>&1 | tail -3); then
        find "$BUILD_DIR/cpack-out" -maxdepth 2 -type f \
             \( -name "moha-*.deb" -o -name "moha-*.rpm" \
                -o -name "moha-*.dmg" -o -name "moha-*.exe" \) \
             ! -path '*_CPack_Packages*' \
             -exec mv {} "$DIST_DIR/" \;
        printf '→ %s installer built\n' "$label"
    else
        printf '⚠  %s build failed (skipping)\n' "$label" >&2
    fi
    rm -rf "$BUILD_DIR/cpack-out/_CPack_Packages"
}

case "$HOST_OS" in
    linux)
        build_installer DEB ".deb"
        if command -v rpmbuild >/dev/null 2>&1; then
            build_installer RPM ".rpm"
        else
            printf '⚠  skipping .rpm (rpmbuild not on PATH)\n' >&2
        fi
        ;;
    macos)
        build_installer DragNDrop ".dmg"
        ;;
    windows)
        if command -v makensis >/dev/null 2>&1; then
            build_installer NSIS ".exe (NSIS)"
        else
            printf '⚠  skipping .exe (makensis not on PATH — install NSIS)\n' >&2
        fi
        ;;
esac

# ── Arch package (Linux only, when makepkg is available) ───────────
if [ "$HOST_OS" = "linux" ] && [ "$HOST_ARCH" = "x86_64" ] \
   && command -v makepkg >/dev/null 2>&1; then
    ARCH_DIR="$BUILD_DIR/arch-stage"
    rm -rf "$ARCH_DIR"
    mkdir -p "$ARCH_DIR"
    cp "$BUILD_DIR/packaging/arch/PKGBUILD" "$ARCH_DIR/"
    cp "$DIST_DIR/$ARCHIVE" "$ARCH_DIR/"
    if (cd "$ARCH_DIR" && makepkg --skipchecksums --nodeps --noconfirm 2>&1 | tail -3); then
        mv "$ARCH_DIR"/*.pkg.tar.zst "$DIST_DIR/"
        printf '→ Arch .pkg.tar.zst built\n'
    else
        printf '⚠  makepkg failed (skipping Arch package)\n' >&2
    fi
fi

# ── SHA256SUMS ─────────────────────────────────────────────────────
(cd "$DIST_DIR" && \
 find . -maxdepth 1 -type f \
      \( -name 'moha-*.tar.gz' -o -name 'moha-*.zip' \
         -o -name 'moha-*.deb' -o -name 'moha-*.rpm' \
         -o -name 'moha-*.dmg' -o -name 'moha-*.exe' \
         -o -name 'moha-*.pkg.tar.zst' \) \
      -printf '%f\n' | sort | xargs -r sha256sum > SHA256SUMS)

printf '\n── %s/ ─────────────────────────────\n' "$DIST_DIR"
ls -lh "$DIST_DIR/"
printf '\n'
cat "$DIST_DIR/SHA256SUMS"
printf '\n'

# ── Upload ─────────────────────────────────────────────────────────
if [ "$CREATE" -eq 1 ] || [ "$UPLOAD" -eq 1 ]; then
    require gh

    # gh release operates on whatever repo gh is authed against; pin
    # explicitly via -R so a wrong cwd doesn't silently target a fork.
    GH_OPTS=(-R "$REPO")
    NOTES_OPT=()
    if [ -n "$NOTES_FILE" ]; then
        NOTES_OPT=(--notes-file "$NOTES_FILE")
    else
        NOTES_OPT=(--generate-notes)
    fi
    [ "${MOHA_DRAFT:-0}" = "1" ]      && CREATE_OPTS+=(--draft)
    [ "${MOHA_PRERELEASE:-0}" = "1" ] && CREATE_OPTS+=(--prerelease)

    if [ "$CREATE" -eq 1 ]; then
        printf '→ creating release %s\n' "$TAG"
        if gh release view "$TAG" "${GH_OPTS[@]}" >/dev/null 2>&1; then
            printf '   (release exists — skipping create, will upload-clobber)\n'
        else
            gh release create "$TAG" "${GH_OPTS[@]}" \
                --title "$TAG" \
                "${NOTES_OPT[@]}" \
                ${CREATE_OPTS[@]+"${CREATE_OPTS[@]}"}
        fi
    fi

    printf '→ uploading %d assets to %s\n' "$(ls "$DIST_DIR" | wc -l)" "$TAG"
    if ! gh release upload "$TAG" "${GH_OPTS[@]}" --clobber "$DIST_DIR"/*; then
        printf 'upload failed — assets are in %s/, retry with:\n' "$DIST_DIR" >&2
        printf '   gh release upload %s -R %s --clobber %s/*\n' \
               "$TAG" "$REPO" "$DIST_DIR" >&2
        exit 2
    fi
    printf '\n  ✓ release %s ready: https://github.com/%s/releases/tag/%s\n\n' \
           "$TAG" "$REPO" "$TAG"
else
    printf '\n  ✓ artifacts staged in %s/\n' "$DIST_DIR"
    printf '  Next: scripts/release.sh --tag v%s    (build + create + upload)\n\n' \
           "$PROJECT_VERSION"
fi
